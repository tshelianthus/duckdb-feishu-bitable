#include "bitable/search_pushdown.hpp"

#include "bitable/field_type.hpp"

#include <cctype>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace duckdb {
namespace feishu {

namespace {

string PercentEncode(const string &value) {
	static const char *HEX = "0123456789ABCDEF";
	string result;
	result.reserve(value.size());
	for (idx_t i = 0; i < value.size(); i++) {
		auto byte = static_cast<unsigned char>(value[i]);
		if (std::isalnum(byte) || byte == '-' || byte == '_' || byte == '.' || byte == '~') {
			result.push_back(static_cast<char>(byte));
		} else {
			result.push_back('%');
			result.push_back(HEX[(byte >> 4) & 0x0F]);
			result.push_back(HEX[byte & 0x0F]);
		}
	}
	return result;
}

string EscapeJsonString(const string &value) {
	string result;
	result.reserve(value.size() + 2);
	result.push_back('"');
	for (idx_t i = 0; i < value.size(); i++) {
		auto ch = static_cast<unsigned char>(value[i]);
		switch (ch) {
		case '"':
			result += "\\\"";
			break;
		case '\\':
			result += "\\\\";
			break;
		case '\b':
			result += "\\b";
			break;
		case '\f':
			result += "\\f";
			break;
		case '\n':
			result += "\\n";
			break;
		case '\r':
			result += "\\r";
			break;
		case '\t':
			result += "\\t";
			break;
		default:
			if (ch < 0x20) {
				static const char *HEX = "0123456789abcdef";
				result += "\\u00";
				result.push_back(HEX[(ch >> 4) & 0x0F]);
				result.push_back(HEX[ch & 0x0F]);
			} else {
				result.push_back(static_cast<char>(ch));
			}
		}
	}
	result.push_back('"');
	return result;
}

string FormatNumber(double value) {
	std::ostringstream stream;
	stream.imbue(std::locale::classic());
	stream << std::setprecision(17) << std::defaultfloat << value;
	return stream.str();
}

bool IsTextLikeField(int32_t type_id) {
	switch (static_cast<FeishuFieldTypeId>(type_id)) {
	case FeishuFieldTypeId::TEXT:
	case FeishuFieldTypeId::SINGLE_SELECT:
	case FeishuFieldTypeId::PHONE:
	case FeishuFieldTypeId::URL:
	case FeishuFieldTypeId::EMAIL:
	case FeishuFieldTypeId::BARCODE:
	case FeishuFieldTypeId::AUTO_NUMBER:
		return true;
	default:
		return false;
	}
}

bool IsNumberLikeField(int32_t type_id) {
	switch (static_cast<FeishuFieldTypeId>(type_id)) {
	case FeishuFieldTypeId::NUMBER:
	case FeishuFieldTypeId::PROGRESS:
	case FeishuFieldTypeId::CURRENCY:
	case FeishuFieldTypeId::RATING:
		return true;
	default:
		return false;
	}
}

//! DateTime / CreatedTime / ModifiedTime share ExactDate filter encoding.
bool IsDateLikeField(int32_t type_id) {
	switch (static_cast<FeishuFieldTypeId>(type_id)) {
	case FeishuFieldTypeId::DATE_TIME:
	case FeishuFieldTypeId::CREATED_TIME:
	case FeishuFieldTypeId::MODIFIED_TIME:
		return true;
	default:
		return false;
	}
}

SearchPushdownPlan Reject(const string &reason) {
	SearchPushdownPlan plan;
	plan.reject_reason = reason;
	return plan;
}

} // namespace

bool IsSearchPushableFieldType(int32_t type_id) {
	return IsTextLikeField(type_id) || IsNumberLikeField(type_id) || IsDateLikeField(type_id) ||
	       type_id == static_cast<int32_t>(FeishuFieldTypeId::CHECKBOX);
}

bool IsSortEquivalentFieldType(int32_t type_id) {
	// I-1: only plain text, number, and datetime matched DuckDB's default
	// ORDER BY (codepoint text, numeric, epoch; NULLS LAST on both directions).
	// SingleSelect follows option order, MultiSelect only the first option,
	// and Formula sorts the computed value while DuckDB sorts the JSON column.
	switch (static_cast<FeishuFieldTypeId>(type_id)) {
	case FeishuFieldTypeId::TEXT:
	case FeishuFieldTypeId::NUMBER:
	case FeishuFieldTypeId::DATE_TIME:
		return true;
	default:
		return false;
	}
}

bool IsSearchPushableOperator(int32_t type_id, SearchOperator op) {
	if (!IsSearchPushableFieldType(type_id)) {
		return false;
	}
	if (IsDateLikeField(type_id)) {
		// Official date filter guide: only is / isGreater / isLess (plus empty
		// checks, which are intentionally not in the H-2 operator set).
		// isNot / isGreaterEqual / isLessEqual are explicitly unsupported.
		return op == SearchOperator::IS || op == SearchOperator::IS_GREATER || op == SearchOperator::IS_LESS;
	}
	if (op == SearchOperator::IS || op == SearchOperator::IS_NOT) {
		return true;
	}
	return IsNumberLikeField(type_id);
}

bool TryEncodeSearchFilterValue(int32_t type_id, SearchScalarKind value_kind, const string &text_value,
                                double number_value, bool bool_value, int64_t timestamp_micros,
                                std::vector<string> &out_values) {
	out_values.clear();
	if (!IsSearchPushableFieldType(type_id)) {
		return false;
	}
	if (IsTextLikeField(type_id)) {
		if (value_kind != SearchScalarKind::TEXT) {
			return false;
		}
		out_values.push_back(text_value);
		return true;
	}
	if (IsNumberLikeField(type_id)) {
		if (value_kind != SearchScalarKind::NUMBER || !std::isfinite(number_value)) {
			return false;
		}
		out_values.push_back(FormatNumber(number_value));
		return true;
	}
	if (type_id == static_cast<int32_t>(FeishuFieldTypeId::CHECKBOX)) {
		if (value_kind != SearchScalarKind::BOOLEAN) {
			return false;
		}
		out_values.emplace_back(bool_value ? "true" : "false");
		return true;
	}
	if (IsDateLikeField(type_id)) {
		// Absolute dates only: ["ExactDate", "<epoch_ms>"]. Relative tokens
		// (Today / CurrentWeek / …) have no unambiguous SQL equivalent.
		if (value_kind != SearchScalarKind::TIMESTAMP_MICROS) {
			return false;
		}
		if (timestamp_micros < 0) {
			return false;
		}
		auto millis = timestamp_micros / 1000;
		if (millis > SEARCH_MAX_TIMESTAMP_MILLIS) {
			return false;
		}
		out_values.emplace_back("ExactDate");
		out_values.emplace_back(std::to_string(millis));
		return true;
	}
	return false;
}

SearchPushdownPlan PlanSearchPushdown(const SearchPushdownInput &input) {
	if (input.view_id_set) {
		return Reject("view_id_set");
	}
	if (input.predicates.empty() && input.sorts.empty()) {
		return Reject("empty");
	}
	if (input.predicates.size() > SEARCH_MAX_CONDITIONS) {
		return Reject("too_many_conditions");
	}
	if (input.sorts.size() > SEARCH_MAX_SORT_KEYS) {
		return Reject("too_many_sorts");
	}

	SearchPushdownPlan plan;
	plan.conjunction = input.conjunction;

	for (idx_t i = 0; i < input.predicates.size(); i++) {
		auto &pred = input.predicates[i];
		if (pred.field_name.empty()) {
			return Reject("empty_field_name");
		}
		if (!IsSearchPushableOperator(pred.type_id, pred.op)) {
			return Reject("unpushable_operator");
		}
		SearchCondition condition;
		condition.field_name = pred.field_name;
		condition.op = pred.op;
		if (!TryEncodeSearchFilterValue(pred.type_id, pred.value_kind, pred.text_value, pred.number_value,
		                                pred.bool_value, pred.timestamp_micros, condition.values)) {
			return Reject("unencodable_value");
		}
		plan.conditions.push_back(std::move(condition));
	}

	for (idx_t i = 0; i < input.sorts.size(); i++) {
		auto &sort = input.sorts[i];
		if (sort.field_name.empty() || !IsSortEquivalentFieldType(sort.type_id)) {
			return Reject("unpushable_sort");
		}
		SearchSortKey key;
		key.field_name = sort.field_name;
		key.descending = sort.descending;
		plan.sorts.push_back(std::move(key));
	}

	plan.mode = SearchPushdownMode::SEARCH;
	return plan;
}

string BuildSearchUrl(const string &api_root, const string &app_token, const string &table_id, idx_t page_size,
                      const string &page_token) {
	string url = api_root + "/apps/" + PercentEncode(app_token) + "/tables/" + PercentEncode(table_id) +
	             "/records/search?page_size=" + std::to_string(page_size);
	if (!page_token.empty()) {
		url += "&page_token=" + PercentEncode(page_token);
	}
	return url;
}

string BuildSearchBody(const SearchPushdownPlan &plan, const std::vector<string> &field_names, bool automatic_fields) {
	string body = "{";
	body += "\"field_names\":[";
	for (idx_t i = 0; i < field_names.size(); i++) {
		if (i > 0) {
			body.push_back(',');
		}
		body += EscapeJsonString(field_names[i]);
	}
	body += "]";
	if (automatic_fields) {
		body += ",\"automatic_fields\":true";
	}
	if (!plan.conditions.empty()) {
		body += ",\"filter\":{\"conjunction\":";
		body += plan.conjunction == SearchConjunction::OR ? "\"or\"" : "\"and\"";
		body += ",\"conditions\":[";
		for (idx_t i = 0; i < plan.conditions.size(); i++) {
			if (i > 0) {
				body.push_back(',');
			}
			auto &condition = plan.conditions[i];
			body += "{\"field_name\":";
			body += EscapeJsonString(condition.field_name);
			body += ",\"operator\":\"";
			body += SearchOperatorWireName(condition.op);
			body += "\",\"value\":[";
			for (idx_t v = 0; v < condition.values.size(); v++) {
				if (v > 0) {
					body.push_back(',');
				}
				body += EscapeJsonString(condition.values[v]);
			}
			body += "]}";
		}
		body += "]}";
	}
	if (!plan.sorts.empty()) {
		body += ",\"sort\":[";
		for (idx_t i = 0; i < plan.sorts.size(); i++) {
			if (i > 0) {
				body.push_back(',');
			}
			auto &key = plan.sorts[i];
			body += "{\"field_name\":";
			body += EscapeJsonString(key.field_name);
			body += ",\"desc\":";
			body += key.descending ? "true" : "false";
			body += "}";
		}
		body += "]";
	}
	body.push_back('}');
	return body;
}

const char *SearchOperatorWireName(SearchOperator op) {
	switch (op) {
	case SearchOperator::IS:
		return "is";
	case SearchOperator::IS_NOT:
		return "isNot";
	case SearchOperator::IS_GREATER:
		return "isGreater";
	case SearchOperator::IS_GREATER_EQUAL:
		return "isGreaterEqual";
	case SearchOperator::IS_LESS:
		return "isLess";
	case SearchOperator::IS_LESS_EQUAL:
		return "isLessEqual";
	}
	return "is";
}

HttpResponse ExecuteSearchFetch(const string &url, const string &body, const SearchFetchPost &post) {
	return post(url, body);
}

} // namespace feishu
} // namespace duckdb
