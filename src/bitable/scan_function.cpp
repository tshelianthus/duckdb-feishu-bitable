#include "bitable/scan_function.hpp"

#include "auth/token_provider.hpp"
#include "bitable/api_client.hpp"
#include "bitable/api_response.hpp"
#include "bitable/function_docs.hpp"
#include "bitable/record_id_lookup.hpp"
#include "bitable/search_pushdown.hpp"
#include "bitable/type_mapping.hpp"
#include "bitable/url_parser.hpp"
#include "bitable/value_coercion.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/vector_buffer.hpp"
#include "duckdb/common/enums/order_type.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "read_error.hpp"
#include "secrets/feishu_secret.hpp"
#include "yyjson.hpp"

#include <algorithm>
#include <cctype>
#include <deque>
#include <limits>
#include <unordered_set>
#include <vector>

namespace duckdb {
namespace feishu {

using namespace duckdb_yyjson; // NOLINT

namespace {

struct ScanField {
	string field_id;
	string field_name;
	int32_t type_id;
	string type_name;
};

struct ScanBindData : public TableFunctionData {
	string app_token;
	string table_id;
	string secret_name;
	string view_id;
	string api_root = FEISHU_BITABLE_API_ROOT;
	bool is_wiki = false;
	SchemaMode schema_mode = SchemaMode::TYPED;
	idx_t page_size = 500;
	bool include_system_columns = false;
	//! Set by pushdown_complex_filter when the query is solely `record_id = ...`.
	string single_record_id;
	//! Set by pushdown_complex_filter when a verified flat filter/sort subset
	//! can use POST records/search (H-2). Mutually exclusive with single_record_id.
	SearchPushdownPlan search_plan;
	FeishuCredentials credentials;
	vector<ScanField> fields;
	vector<LogicalType> result_types;
	vector<string> result_names;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<ScanBindData>(*this);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ScanBindData>();
		return app_token == other.app_token && table_id == other.table_id && secret_name == other.secret_name &&
		       view_id == other.view_id && schema_mode == other.schema_mode && page_size == other.page_size &&
		       include_system_columns == other.include_system_columns && single_record_id == other.single_record_id &&
		       search_plan.Equals(other.search_plan) && result_names == other.result_names &&
		       result_types == other.result_types && api_root == other.api_root && is_wiki == other.is_wiki;
	}
	bool SupportStatementCache() const override {
		return false;
	}
};

struct ScanGlobalState : public GlobalTableFunctionState {
	ScanGlobalState(ClientContext &context_p, const ScanBindData &bind_data_p)
	    : context(context_p), api(context_p), bind_data(bind_data_p), has_more(true), emitted_rows(0),
	      consecutive_empty_pages(0) {
		if (!bind_data.credentials.user_access_token.empty()) {
			access_token = bind_data.credentials.user_access_token;
		} else {
			TokenProvider provider(context, GetGlobalTokenCache());
			access_token = provider.GetTenantAccessToken(bind_data.credentials.app_id, bind_data.credentials.app_secret,
			                                             bind_data.api_root);
		}
	}
	ClientContext &context;
	ApiClient api;
	const ScanBindData &bind_data;
	string access_token;
	string page_token;
	bool has_more;
	idx_t emitted_rows;
	idx_t consecutive_empty_pages;
	vector<column_t> column_ids;
	vector<string> requested_field_names;
	string single_record_id;
	SearchPushdownPlan search_plan;
	// A full cursor set grows with the number of pages and defeats the
	// constant-memory scan guarantee. Keep a bounded recent history for short
	// cycles and use a hard page limit as the final termination guard.
	std::deque<string> recent_page_tokens;
	idx_t page_count = 0;
};

static string PercentEncode(const string &value) {
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

static string EscapeJsonString(const string &value) {
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

static string FieldNamesJson(const vector<string> &names) {
	string result = "[";
	for (idx_t i = 0; i < names.size(); i++) {
		if (i > 0) {
			result.push_back(',');
		}
		result += EscapeJsonString(names[i]);
	}
	result.push_back(']');
	return result;
}

static void SetInvalid(Vector &vector, idx_t row) {
	FlatVector::Validity(vector).SetInvalid(row);
}

static void WriteString(Vector &vector, idx_t row, const string &value) {
	FlatVector::GetData<string_t>(vector)[row] = StringVector::AddString(vector, value);
}

static void WriteList(Vector &vector, idx_t row, yyjson_val *value) {
	std::vector<CoercedListEntry> entries_json;
	if (!CoerceToStringList(value, entries_json)) {
		SetInvalid(vector, row);
		return;
	}
	auto length = static_cast<idx_t>(entries_json.size());
	auto offset = ListVector::GetListSize(vector);
	ListVector::Reserve(vector, offset + length);
	auto entries = FlatVector::GetData<list_entry_t>(vector);
	entries[row].offset = offset;
	entries[row].length = length;
	auto &child_vector = ListVector::GetEntry(vector);
	auto child_data = FlatVector::GetData<string_t>(child_vector);
	for (idx_t i = 0; i < length; i++) {
		if (entries_json[i].is_null) {
			FlatVector::Validity(child_vector).SetInvalid(offset + i);
			continue;
		}
		FlatVector::Validity(child_vector).SetValid(offset + i);
		child_data[offset + i] = StringVector::AddString(child_vector, entries_json[i].value);
	}
	ListVector::SetListSize(vector, offset + length);
}

static void WriteTypedValue(Vector &vector, idx_t row, yyjson_val *value, const LogicalType &type) {
	if (!value || yyjson_is_null(value)) {
		SetInvalid(vector, row);
		return;
	}
	if (type == LogicalType::JSON()) {
		// JSON mode and every composite field keep the payload exactly as the
		// Feishu API delivered it, structure included.
		WriteString(vector, row, SerializeJsonValue(value));
		return;
	}
	switch (type.id()) {
	case LogicalTypeId::VARCHAR:
		WriteString(vector, row, CoerceToString(value));
		break;
	case LogicalTypeId::DOUBLE: {
		double number;
		if (!CoerceToDouble(value, number)) {
			SetInvalid(vector, row);
			return;
		}
		FlatVector::GetData<double>(vector)[row] = number;
		break;
	}
	case LogicalTypeId::BOOLEAN: {
		bool boolean;
		if (!CoerceToBoolean(value, boolean)) {
			SetInvalid(vector, row);
			return;
		}
		FlatVector::GetData<bool>(vector)[row] = boolean;
		break;
	}
	case LogicalTypeId::TIMESTAMP: {
		int64_t microseconds;
		if (!CoerceToTimestampMicros(value, microseconds)) {
			SetInvalid(vector, row);
			return;
		}
		FlatVector::GetData<timestamp_t>(vector)[row] = timestamp_t(microseconds);
		break;
	}
	case LogicalTypeId::LIST:
		WriteList(vector, row, value);
		break;
	default:
		throw IOException("feishu_bitable: unsupported output type during scan");
	}
}

static HeaderMap AuthorizationHeaders(const string &access_token) {
	HeaderMap headers;
	headers["Authorization"] = "Bearer " + access_token;
	return headers;
}

static string FieldsUrl(const ScanBindData &bind_data, const string &page_token) {
	string url = bind_data.api_root + "/apps/" + PercentEncode(bind_data.app_token) + "/tables/" +
	             PercentEncode(bind_data.table_id) + "/fields?page_size=100";
	if (!bind_data.view_id.empty()) {
		url += "&view_id=" + PercentEncode(bind_data.view_id);
	}
	if (!page_token.empty()) {
		url += "&page_token=" + PercentEncode(page_token);
	}
	return url;
}

static bool RequireHasMore(yyjson_val *data, const string &endpoint_name) {
	auto more = yyjson_obj_get(data, "has_more");
	if (!more || !yyjson_is_bool(more)) {
		throw IOException("feishu_bitable: " + endpoint_name + " response is missing BOOLEAN has_more");
	}
	return yyjson_get_bool(more);
}

constexpr idx_t MAX_RECORD_CURSOR_HISTORY = 64;
constexpr idx_t MAX_RECORD_PAGES = 1000000;

static void ValidateNextRecordPageToken(bool has_more, const string &next_page_token, const string &request_page_token,
                                        std::deque<string> &recent_page_tokens, idx_t &page_count) {
	if (!has_more) {
		return;
	}
	if (next_page_token.empty() || next_page_token == request_page_token ||
	    std::find(recent_page_tokens.begin(), recent_page_tokens.end(), next_page_token) != recent_page_tokens.end()) {
		throw IOException("feishu_bitable: records response returned an invalid pagination cursor");
	}
	if (page_count >= MAX_RECORD_PAGES) {
		throw IOException("feishu_bitable: records pagination exceeded the maximum page limit");
	}
	recent_page_tokens.push_back(next_page_token);
	if (recent_page_tokens.size() > MAX_RECORD_CURSOR_HISTORY) {
		recent_page_tokens.pop_front();
	}
	page_count++;
}

static string RefreshAccessToken(ClientContext &context, const ScanBindData &bind_data) {
	if (!bind_data.credentials.user_access_token.empty()) {
		return bind_data.credentials.user_access_token;
	}
	TokenProvider provider(context, GetGlobalTokenCache());
	return provider.GetTenantAccessToken(bind_data.credentials.app_id, bind_data.credentials.app_secret,
	                                     bind_data.api_root);
}

static void LoadFields(ClientContext &context, ScanBindData &bind_data) {
	ApiClient api(context);
	string page_token;
	bool has_more = true;
	idx_t consecutive_empty_pages = 0;
	std::unordered_set<string> seen_page_tokens;
	idx_t page_count = 0;
	while (has_more) {
		if (page_count++ >= MAX_RECORD_PAGES) {
			throw IOException("feishu_bitable: fields pagination exceeded the maximum page limit");
		}
		auto access_token = RefreshAccessToken(context, bind_data);
		auto response = api.Get(FieldsUrl(bind_data, page_token), AuthorizationHeaders(access_token));
		auto page = ParseFieldsMetadataPage(response.status_code, response.body);
		for (const auto &item : page.rows) {
			if (!item.field_id || item.field_id->empty() || !item.field_name || item.field_name->empty() ||
			    !item.type_id) {
				throw IOException("feishu_bitable: field metadata is missing required values");
			}
			bind_data.fields.push_back(
			    {*item.field_id, *item.field_name, *item.type_id, FeishuFieldTypeName(*item.type_id)});
		}
		AdvanceMetadataPaginationCursor("feishu_bitable", page.has_more, page.page_token, page_token, seen_page_tokens);
		has_more = page.has_more;
		if (!page.rows.empty()) {
			consecutive_empty_pages = 0;
		} else if (has_more && ++consecutive_empty_pages > 32) {
			throw IOException("feishu_bitable: exceeded maximum consecutive empty field pages during pagination");
		}
	}
}

static string GetNamedString(TableFunctionBindInput &input, const string &key, const string &function_name) {
	auto entry = input.named_parameters.find(key);
	if (entry == input.named_parameters.end()) {
		return string();
	}
	if (entry->second.IsNull()) {
		throw InvalidInputException("%s %s must not be NULL", function_name, key);
	}
	auto result = StringValue::Get(entry->second);
	if (result.empty()) {
		throw InvalidInputException("%s %s must not be empty", function_name, key);
	}
	return result;
}

static string TablesUrl(const ScanBindData &bind_data) {
	return bind_data.api_root + "/apps/" + PercentEncode(bind_data.app_token) + "/tables?page_size=1";
}

//! A Wiki URL identifies a node, not a Base. Resolve it before either the
//! first-table lookup or field discovery, including when a table is explicit.
static string ResolveWikiAppToken(ClientContext &context, const ScanBindData &bind_data) {
	ApiClient api(context);
	string access_token;
	if (!bind_data.credentials.user_access_token.empty()) {
		access_token = bind_data.credentials.user_access_token;
	} else {
		TokenProvider provider(context, GetGlobalTokenCache());
		access_token = provider.GetTenantAccessToken(bind_data.credentials.app_id, bind_data.credentials.app_secret,
		                                             bind_data.api_root);
	}
	// api_root is canonical and comes only from the URL parser or the default.
	auto origin = bind_data.api_root.substr(0, bind_data.api_root.find("/open-apis/"));
	auto response = api.Get(origin + "/open-apis/wiki/v2/spaces/get_node?token=" + PercentEncode(bind_data.app_token),
	                        AuthorizationHeaders(access_token));
	EnsureReadHttpSuccess(response.status_code, response.body);
	JsonDocument json(response.body);
	auto root = json.Root();
	auto code = yyjson_is_obj(root) ? yyjson_obj_get(root, "code") : nullptr;
	if (!code || !yyjson_is_int(code)) {
		throw IOException("feishu_bitable: malformed wiki node response: expected integer code");
	}
	EnsureApiSuccess(response.status_code, root);
	auto data = yyjson_obj_get(root, "data");
	auto node = yyjson_is_obj(data) ? yyjson_obj_get(data, "node") : nullptr;
	if (!yyjson_is_obj(node)) {
		throw IOException("feishu_bitable: malformed wiki node response: expected object data.node");
	}
	auto obj_type = JsonString(yyjson_obj_get(node, "obj_type"));
	if (obj_type.empty()) {
		throw IOException("feishu_bitable: malformed wiki node response: missing non-empty obj_type");
	}
	if (obj_type != "bitable") {
		throw InvalidInputException("feishu_bitable: wiki node does not reference a Bitable base");
	}
	auto obj_token = JsonString(yyjson_obj_get(node, "obj_token"));
	if (obj_token.empty()) {
		throw IOException("feishu_bitable: malformed wiki node response: missing non-empty obj_token");
	}
	return obj_token;
}

//! Resolves the default scan target when the caller did not name a table:
//! the first table of the Base, mirroring how a spreadsheet URL without a
//! sheet reference opens the first sheet.
static string ResolveFirstTableId(ClientContext &context, const ScanBindData &bind_data) {
	ApiClient api(context);
	string access_token;
	if (!bind_data.credentials.user_access_token.empty()) {
		access_token = bind_data.credentials.user_access_token;
	} else {
		TokenProvider provider(context, GetGlobalTokenCache());
		access_token = provider.GetTenantAccessToken(bind_data.credentials.app_id, bind_data.credentials.app_secret,
		                                             bind_data.api_root);
	}
	auto response = api.Get(TablesUrl(bind_data), AuthorizationHeaders(access_token));
	return ParseFirstTableId(response.status_code, response.body);
}

//! Shared bind for every scan entry point. `modern_interface` enables the
//! gsheets-style ergonomics: a browser URL as first argument, an optional
//! `table` named parameter, and the default first table fallback.
static unique_ptr<FunctionData> ScanBindInternal(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names,
                                                 bool modern_interface) {
	auto &function_name = input.table_function.name;
	auto result = make_uniq<ScanBindData>();
	if (!modern_interface) {
		if (input.inputs.size() != 2 || input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
			throw InvalidInputException("%s requires non-NULL app_token and table_id", function_name);
		}
		result->app_token = StringValue::Get(input.inputs[0]);
		result->table_id = StringValue::Get(input.inputs[1]);
		if (result->app_token.empty() || result->table_id.empty()) {
			throw InvalidInputException("%s requires non-empty app_token and table_id", function_name);
		}
	} else {
		if (input.inputs.empty() || input.inputs[0].IsNull()) {
			throw InvalidInputException("%s requires a non-NULL url or app_token", function_name);
		}
		auto source = StringValue::Get(input.inputs[0]);
		if (source.empty()) {
			throw InvalidInputException("%s requires a non-empty url or app_token", function_name);
		}
		string explicit_table;
		if (input.inputs.size() > 1) {
			if (input.inputs[1].IsNull()) {
				throw InvalidInputException("%s requires a non-NULL table_id", function_name);
			}
			explicit_table = StringValue::Get(input.inputs[1]);
			if (explicit_table.empty()) {
				throw InvalidInputException("%s requires a non-empty table_id", function_name);
			}
		}
		auto named_table = GetNamedString(input, "table_id", function_name);
		if (!named_table.empty()) {
			explicit_table = named_table;
		}
		auto target = ResolveBitableTarget(source, explicit_table, GetNamedString(input, "view_id", function_name),
		                                   function_name);
		result->app_token = target.app_token;
		result->table_id = target.table_id;
		result->view_id = target.view_id;
		result->api_root = target.api_root;
		result->is_wiki = target.is_wiki;
	}
	result->secret_name = GetNamedString(input, "secret", function_name);
	if (!modern_interface) {
		result->view_id = GetNamedString(input, "view_id", function_name);
	}
	auto schema_mode = StringUtil::Lower(GetNamedString(input, "schema_mode", function_name));
	if (schema_mode.empty() || schema_mode == "typed") {
		result->schema_mode = SchemaMode::TYPED;
	} else if (schema_mode == "json") {
		result->schema_mode = SchemaMode::JSON;
	} else {
		throw InvalidInputException("%s schema_mode must be 'typed' or 'json'", function_name);
	}
	auto page_size_entry = input.named_parameters.find("page_size");
	if (page_size_entry != input.named_parameters.end()) {
		if (page_size_entry->second.IsNull()) {
			throw InvalidInputException("%s page_size must not be NULL", function_name);
		}
		auto page_size = IntegerValue::Get(page_size_entry->second);
		if (page_size < 1 || page_size > 500) {
			throw InvalidInputException("%s page_size must be between 1 and 500", function_name);
		}
		result->page_size = static_cast<idx_t>(page_size);
	}
	auto system_entry = input.named_parameters.find("include_system_columns");
	if (system_entry != input.named_parameters.end()) {
		if (system_entry->second.IsNull()) {
			throw InvalidInputException("%s include_system_columns must not be NULL", function_name);
		}
		result->include_system_columns = BooleanValue::Get(system_entry->second);
	}
	result->credentials = GetFeishuCredentials(context, result->secret_name);
	if (result->is_wiki) {
		result->app_token = ResolveWikiAppToken(context, *result);
	}
	if (result->table_id.empty()) {
		result->table_id = ResolveFirstTableId(context, *result);
	}
	LoadFields(context, *result);
	if (result->fields.empty() && !result->include_system_columns) {
		throw InvalidInputException("feishu_bitable: target table has no fields");
	}
	for (idx_t i = 0; i < result->fields.size(); i++) {
		names.push_back(result->fields[i].field_name);
		return_types.push_back(MapFeishuFieldType(result->fields[i].type_id, result->schema_mode));
	}
	if (result->include_system_columns) {
		names.push_back("record_id");
		return_types.push_back(LogicalType::VARCHAR);
		names.push_back("created_time");
		return_types.push_back(LogicalType::TIMESTAMP);
		names.push_back("updated_time");
		return_types.push_back(LogicalType::TIMESTAMP);
	}
	result->result_names = names;
	result->result_types = return_types;
	return std::move(result);
}

static unique_ptr<FunctionData> ScanBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
	return ScanBindInternal(context, input, return_types, names, false);
}

static unique_ptr<FunctionData> ReadBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
	return ScanBindInternal(context, input, return_types, names, true);
}

//! Projection pushdown: translate the column identifiers the optimizer kept
//! into the `field_names` query parameter, so that Feishu only ships the
//! columns this query reads.
//!
//! `column_ids` mixes three kinds of identifiers: real field offsets into
//! `bind_data.fields`, the trailing system columns when
//! `include_system_columns` is set (they ride on `automatic_fields=true`, not
//! on `field_names`), and the virtual COLUMN_IDENTIFIER_ROW_ID /
//! COLUMN_IDENTIFIER_EMPTY markers, which are far above every real offset and
//! therefore never mistaken for a field.
static unique_ptr<GlobalTableFunctionState> ScanInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<ScanBindData>();
	auto result = make_uniq<ScanGlobalState>(context, bind_data);
	result->column_ids = input.column_ids;
	result->single_record_id = bind_data.single_record_id;
	result->search_plan = bind_data.search_plan;
	for (idx_t output_idx = 0; output_idx < input.column_ids.size(); output_idx++) {
		auto column_id = input.column_ids[output_idx];
		if (column_id < bind_data.fields.size()) {
			auto &name = bind_data.fields[column_id].field_name;
			bool found = false;
			for (idx_t i = 0; i < result->requested_field_names.size(); i++) {
				if (result->requested_field_names[i] == name) {
					found = true;
					break;
				}
			}
			if (!found) {
				result->requested_field_names.push_back(name);
			}
		}
	}
	if (result->requested_field_names.empty() && !bind_data.fields.empty()) {
		// COUNT(*) and system-column-only scans read no field at all. Feishu
		// treats a missing `field_names` as "send everything", so ask for a
		// single field instead: `field_names` is a pure projection filter and
		// never changes which records come back.
		result->requested_field_names.push_back(bind_data.fields[0].field_name);
	}
	return std::move(result);
}

static string RecordsUrl(const ScanGlobalState &state) {
	auto &bind_data = state.bind_data;
	string url = bind_data.api_root + "/apps/" + PercentEncode(bind_data.app_token) + "/tables/" +
	             PercentEncode(bind_data.table_id) + "/records?page_size=" + to_string(bind_data.page_size);
	if (!state.requested_field_names.empty()) {
		url += "&field_names=" + PercentEncode(FieldNamesJson(state.requested_field_names));
	}
	if (!bind_data.view_id.empty()) {
		url += "&view_id=" + PercentEncode(bind_data.view_id);
	}
	if (bind_data.include_system_columns) {
		url += "&automatic_fields=true";
	}
	if (!state.page_token.empty()) {
		url += "&page_token=" + PercentEncode(state.page_token);
	}
	return url;
}

static yyjson_val *SystemValue(yyjson_val *record, idx_t system_index) {
	switch (system_index) {
	case 0:
		return yyjson_obj_get(record, "record_id");
	case 1:
		return yyjson_obj_get(record, "created_time");
	case 2: {
		auto value = yyjson_obj_get(record, "last_modified_time");
		return value ? value : yyjson_obj_get(record, "updated_time");
	}
	default:
		return nullptr;
	}
}

//! Maps a pushed Expression onto the DuckDB-independent predicate vocabulary
//! used by PlanRecordFetch. Only `record_id` equality is recognised for the
//! single-record GET fast path; everything else stays on the list scanner.
static RecordIdPredicate ClassifyRecordIdExpression(Expression &expr, const LogicalGet &get,
                                                    const ScanBindData &bind_data) {
	RecordIdPredicate predicate;
	if (!bind_data.include_system_columns) {
		predicate.kind = RecordIdPredicateKind::OTHER;
		return predicate;
	}
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_COMPARISON ||
	    expr.GetExpressionType() != ExpressionType::COMPARE_EQUAL) {
		// Inequalities / IS NULL / OR / IN on record_id are not H-3 shapes.
		predicate.kind = RecordIdPredicateKind::NON_EQUALITY;
		return predicate;
	}

	const idx_t record_id_column = bind_data.fields.size();
	auto &column_ids = get.GetColumnIds();
	auto &cmp = expr.Cast<BoundComparisonExpression>();

	auto resolve_column = [&](Expression &side) -> optional_idx {
		if (side.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return optional_idx();
		}
		auto &ref = side.Cast<BoundColumnRefExpression>();
		if (ref.binding.table_index != get.table_index || ref.binding.column_index >= column_ids.size()) {
			return optional_idx();
		}
		return optional_idx(column_ids[ref.binding.column_index].GetPrimaryIndex());
	};

	optional_ptr<BoundConstantExpression> constant;
	optional_idx column_id;
	if (cmp.left->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
	    cmp.right->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		column_id = resolve_column(*cmp.left);
		constant = &cmp.right->Cast<BoundConstantExpression>();
	} else if (cmp.right->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
	           cmp.left->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		column_id = resolve_column(*cmp.right);
		constant = &cmp.left->Cast<BoundConstantExpression>();
	}
	if (!column_id.IsValid() || !constant) {
		predicate.kind = RecordIdPredicateKind::OTHER;
		return predicate;
	}
	if (column_id.GetIndex() != record_id_column) {
		predicate.kind = RecordIdPredicateKind::OTHER;
		return predicate;
	}
	if (constant->value.IsNull() || constant->value.type().id() != LogicalTypeId::VARCHAR) {
		predicate.kind = RecordIdPredicateKind::NON_EQUALITY;
		return predicate;
	}
	predicate.kind = RecordIdPredicateKind::EQUALITY;
	predicate.equality_value = StringValue::Get(constant->value);
	return predicate;
}

static optional_idx ResolveScanColumnId(Expression &side, const LogicalGet &get) {
	if (side.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return optional_idx();
	}
	auto &ref = side.Cast<BoundColumnRefExpression>();
	auto &column_ids = get.GetColumnIds();
	if (ref.binding.table_index != get.table_index || ref.binding.column_index >= column_ids.size()) {
		return optional_idx();
	}
	return optional_idx(column_ids[ref.binding.column_index].GetPrimaryIndex());
}

static bool TryMapCompareToSearchOperator(ExpressionType type, bool swapped, SearchOperator &out) {
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
		out = SearchOperator::IS;
		return true;
	case ExpressionType::COMPARE_NOTEQUAL:
		out = SearchOperator::IS_NOT;
		return true;
	case ExpressionType::COMPARE_GREATERTHAN:
		out = swapped ? SearchOperator::IS_LESS : SearchOperator::IS_GREATER;
		return true;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		out = swapped ? SearchOperator::IS_LESS_EQUAL : SearchOperator::IS_GREATER_EQUAL;
		return true;
	case ExpressionType::COMPARE_LESSTHAN:
		out = swapped ? SearchOperator::IS_GREATER : SearchOperator::IS_LESS;
		return true;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		out = swapped ? SearchOperator::IS_GREATER_EQUAL : SearchOperator::IS_LESS_EQUAL;
		return true;
	default:
		return false;
	}
}

static bool TryConstantToSearchAtom(const Value &value, SearchAtomicPredicate &atom) {
	if (value.IsNull()) {
		return false;
	}
	switch (value.type().id()) {
	case LogicalTypeId::VARCHAR:
		atom.value_kind = SearchScalarKind::TEXT;
		atom.text_value = StringValue::Get(value);
		return true;
	case LogicalTypeId::BOOLEAN:
		atom.value_kind = SearchScalarKind::BOOLEAN;
		atom.bool_value = BooleanValue::Get(value);
		return true;
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL: {
		atom.value_kind = SearchScalarKind::NUMBER;
		Value dbl = value.DefaultCastAs(LogicalType::DOUBLE);
		atom.number_value = DoubleValue::Get(dbl);
		return true;
	}
	case LogicalTypeId::DATE: {
		atom.value_kind = SearchScalarKind::TIMESTAMP_MICROS;
		atom.timestamp_micros = Timestamp::FromDatetime(DateValue::Get(value), dtime_t(0)).value;
		return true;
	}
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ: {
		// TIMESTAMP is UTC epoch micros; TIMESTAMPTZ constants are already
		// normalized to the same internal representation by DuckDB.
		atom.value_kind = SearchScalarKind::TIMESTAMP_MICROS;
		atom.timestamp_micros = TimestampValue::Get(value).value;
		return true;
	}
	default:
		return false;
	}
}

//! Classifies a single column-vs-constant comparison onto a search atom.
//! System columns (record_id / timestamps) and non-comparison shapes refuse.
static bool TryClassifySearchAtomic(Expression &expr, const LogicalGet &get, const ScanBindData &bind_data,
                                    SearchAtomicPredicate &atom) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
		return false;
	}
	auto &cmp = expr.Cast<BoundComparisonExpression>();
	optional_ptr<BoundConstantExpression> constant;
	optional_idx column_id;
	bool swapped = false;
	if (cmp.left->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
	    cmp.right->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		column_id = ResolveScanColumnId(*cmp.left, get);
		constant = &cmp.right->Cast<BoundConstantExpression>();
	} else if (cmp.right->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
	           cmp.left->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		column_id = ResolveScanColumnId(*cmp.right, get);
		constant = &cmp.left->Cast<BoundConstantExpression>();
		swapped = true;
	}
	if (!column_id.IsValid() || !constant) {
		return false;
	}
	SearchOperator op;
	if (!TryMapCompareToSearchOperator(expr.GetExpressionType(), swapped, op)) {
		return false;
	}
	auto field_idx = column_id.GetIndex();
	if (field_idx >= bind_data.fields.size()) {
		// System columns are not Feishu filter field_name targets.
		return false;
	}
	atom.field_name = bind_data.fields[field_idx].field_name;
	atom.type_id = bind_data.fields[field_idx].type_id;
	atom.op = op;
	return TryConstantToSearchAtom(constant->value, atom);
}

static bool TryCollectSearchAtoms(Expression &expr, const LogicalGet &get, const ScanBindData &bind_data,
                                  SearchConjunction expected_inner, std::vector<SearchAtomicPredicate> &atoms) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
			if (expected_inner != SearchConjunction::AND) {
				return false;
			}
			auto &conj = expr.Cast<BoundConjunctionExpression>();
			for (auto &child : conj.children) {
				if (!TryCollectSearchAtoms(*child, get, bind_data, expected_inner, atoms)) {
					return false;
				}
			}
			return true;
		}
		if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_OR) {
			// Nested OR under an AND (or nested under another OR) is refused —
			// Feishu only accepts one flat conjunction.
			return false;
		}
		return false;
	}
	SearchAtomicPredicate atom;
	if (!TryClassifySearchAtomic(expr, get, bind_data, atom)) {
		return false;
	}
	atoms.push_back(std::move(atom));
	return true;
}

//! Flattens DuckDB filter expressions into a single Feishu conjunction.
//! Top-level filters are ANDed by DuckDB; a sole top-level OR is accepted as
//! SearchConjunction::OR. Mixed AND/OR trees refuse.
static bool TryFlattenSearchFilters(vector<unique_ptr<Expression>> &filters, const LogicalGet &get,
                                    const ScanBindData &bind_data, SearchConjunction &conjunction,
                                    std::vector<SearchAtomicPredicate> &atoms) {
	if (filters.empty()) {
		return false;
	}
	if (filters.size() == 1 && filters[0]->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION &&
	    filters[0]->GetExpressionType() == ExpressionType::CONJUNCTION_OR) {
		conjunction = SearchConjunction::OR;
		auto &conj = filters[0]->Cast<BoundConjunctionExpression>();
		for (auto &child : conj.children) {
			SearchAtomicPredicate atom;
			if (child->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION ||
			    !TryClassifySearchAtomic(*child, get, bind_data, atom)) {
				return false;
			}
			atoms.push_back(std::move(atom));
		}
		return !atoms.empty();
	}

	conjunction = SearchConjunction::AND;
	for (auto &filter : filters) {
		if (!TryCollectSearchAtoms(*filter, get, bind_data, SearchConjunction::AND, atoms)) {
			return false;
		}
	}
	return !atoms.empty();
}

//! H-3 then H-2 then fall through: sole `record_id =` → single GET; verified
//! flat filter subset without VIEW_ID → POST records/search; otherwise leave
//! filters for DuckDB and keep GET list.
static void PushdownScanFilters(ClientContext &, LogicalGet &get, FunctionData *bind_data_p,
                                vector<unique_ptr<Expression>> &filters) {
	if (!bind_data_p || filters.empty()) {
		return;
	}
	auto &bind_data = bind_data_p->Cast<ScanBindData>();
	if (!bind_data.single_record_id.empty() || bind_data.search_plan.enabled()) {
		return;
	}

	// 1) H-3: sole record_id equality → single-record GET (never search).
	if (filters.size() == 1 && bind_data.include_system_columns) {
		std::vector<RecordIdPredicate> predicates;
		predicates.push_back(ClassifyRecordIdExpression(*filters[0], get, bind_data));
		auto plan = PlanRecordFetch(predicates);
		if (plan.mode == RecordFetchMode::SINGLE_RECORD) {
			bind_data.single_record_id = plan.record_id;
			filters.clear();
			return;
		}
	}

	// 2) H-2: flat AND/OR of verified scalar predicates → records/search.
	// ORDER BY is attached later by OptimizeFeishuSortPushdown. This pass only
	// sees filters, so sort keys stay empty until that rewrite.
	SearchConjunction conjunction = SearchConjunction::AND;
	std::vector<SearchAtomicPredicate> atoms;
	if (!TryFlattenSearchFilters(filters, get, bind_data, conjunction, atoms)) {
		return;
	}
	SearchPushdownInput input;
	input.view_id_set = !bind_data.view_id.empty();
	input.conjunction = conjunction;
	input.predicates = std::move(atoms);
	auto plan = PlanSearchPushdown(input);
	if (!plan.enabled()) {
		return;
	}
	bind_data.search_plan = std::move(plan);
	filters.clear();
}

static void PrepareOutputVectors(DataChunk &output) {
	for (idx_t output_idx = 0; output_idx < output.ColumnCount(); output_idx++) {
		FlatVector::Validity(output.data[output_idx]).SetAllValid(STANDARD_VECTOR_SIZE);
		if (output.data[output_idx].GetType().id() == LogicalTypeId::LIST) {
			ListVector::SetListSize(output.data[output_idx], 0);
			FlatVector::Validity(ListVector::GetEntry(output.data[output_idx])).Reset();
		}
	}
}

static void EmitRecordRow(ScanGlobalState &state, DataChunk &output, yyjson_val *record, idx_t row) {
	if (!record || !yyjson_is_obj(record)) {
		throw IOException("feishu_bitable: malformed record item from Feishu API");
	}
	auto fields = yyjson_obj_get(record, "fields");
	if (fields && !yyjson_is_null(fields) && !yyjson_is_obj(fields)) {
		throw IOException("feishu_bitable: malformed record item: 'fields' must be an object");
	}
	for (idx_t output_idx = 0; output_idx < state.column_ids.size(); output_idx++) {
		auto column_id = state.column_ids[output_idx];
		if (output_idx >= output.ColumnCount()) {
			continue;
		}
		if (column_id == COLUMN_IDENTIFIER_EMPTY) {
			if (output.data[output_idx].GetType().id() != LogicalTypeId::BOOLEAN) {
				throw IOException("feishu_bitable: invalid empty projection column type");
			}
			FlatVector::GetData<bool>(output.data[output_idx])[row] = true;
			continue;
		}
		if (column_id == COLUMN_IDENTIFIER_ROW_ID) {
			if (output.data[output_idx].GetType().id() != LogicalTypeId::BIGINT) {
				throw IOException("feishu_bitable: invalid row identifier column type");
			}
			if (state.emitted_rows > static_cast<idx_t>(std::numeric_limits<int64_t>::max()) - row) {
				throw IOException("feishu_bitable: row identifier exceeds BIGINT range");
			}
			FlatVector::GetData<int64_t>(output.data[output_idx])[row] = static_cast<int64_t>(state.emitted_rows + row);
			continue;
		}
		yyjson_val *value = nullptr;
		if (column_id < state.bind_data.fields.size()) {
			if (fields && yyjson_is_obj(fields)) {
				value = yyjson_obj_get(fields, state.bind_data.fields[column_id].field_name.c_str());
			}
		} else if (state.bind_data.include_system_columns && (column_id - state.bind_data.fields.size() < 3)) {
			value = SystemValue(record, column_id - state.bind_data.fields.size());
		} else {
			throw IOException("feishu_bitable: unexpected column identifier %llu", static_cast<uint64_t>(column_id));
		}
		WriteTypedValue(output.data[output_idx], row, value, output.data[output_idx].GetType());
	}
}

static void ScanFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<ScanGlobalState>();
	if (!state.has_more) {
		return;
	}

	// H-3: sole `record_id = ...` uses Feishu's single-record GET once.
	if (!state.single_record_id.empty()) {
		state.has_more = false;
		if (state.bind_data.credentials.user_access_token.empty()) {
			TokenProvider provider(context, GetGlobalTokenCache());
			state.access_token = provider.GetTenantAccessToken(
			    state.bind_data.credentials.app_id, state.bind_data.credentials.app_secret, state.bind_data.api_root);
		}
		RecordFetchPlan plan;
		plan.mode = RecordFetchMode::SINGLE_RECORD;
		plan.record_id = state.single_record_id;
		auto url = BuildSingleRecordUrl(state.bind_data.api_root, state.bind_data.app_token, state.bind_data.table_id,
		                                state.single_record_id, state.bind_data.include_system_columns);
		auto response = ExecuteRecordFetch(plan, url, [&](const string &request_url) {
			return state.api.Get(request_url, AuthorizationHeaders(state.access_token));
		});
		if (IsSingleRecordNotFound(response.status_code, response.body)) {
			output.SetCardinality(0);
			return;
		}
		EnsureReadHttpSuccess(response.status_code, response.body);
		JsonDocument json(response.body);
		auto root = json.Root();
		EnsureApiSuccess(response.status_code, root);
		auto data = yyjson_obj_get(root, "data");
		auto record = data && yyjson_is_obj(data) ? yyjson_obj_get(data, "record") : nullptr;
		if (!data || !yyjson_is_obj(data) || (record && !yyjson_is_null(record) && !yyjson_is_obj(record))) {
			throw IOException("feishu_bitable: malformed single-record response from Feishu API");
		}
		if (!record || yyjson_is_null(record)) {
			output.SetCardinality(0);
			return;
		}
		PrepareOutputVectors(output);
		EmitRecordRow(state, output, record, 0);
		state.emitted_rows = 1;
		output.SetCardinality(1);
		return;
	}

	// H-2: verified flat filter subset uses POST records/search with the same
	// pagination cursor rules as GET list.
	if (state.search_plan.enabled()) {
		while (state.has_more) {
			auto previous_page_token = state.page_token;
			if (state.bind_data.credentials.user_access_token.empty()) {
				TokenProvider provider(context, GetGlobalTokenCache());
				state.access_token =
				    provider.GetTenantAccessToken(state.bind_data.credentials.app_id,
				                                  state.bind_data.credentials.app_secret, state.bind_data.api_root);
			}
			auto url = BuildSearchUrl(state.bind_data.api_root, state.bind_data.app_token, state.bind_data.table_id,
			                          state.bind_data.page_size, state.page_token);
			auto body =
			    BuildSearchBody(state.search_plan, state.requested_field_names, state.bind_data.include_system_columns);
			auto response = ExecuteSearchFetch(url, body, [&](const string &request_url, const string &request_body) {
				return state.api.PostJson(request_url, request_body, AuthorizationHeaders(state.access_token));
			});
			EnsureReadHttpSuccess(response.status_code, response.body);
			JsonDocument json(response.body);
			auto root = json.Root();
			EnsureApiSuccess(response.status_code, root);
			auto data = yyjson_obj_get(root, "data");
			auto items = data ? yyjson_obj_get(data, "items") : nullptr;
			if (!data || !yyjson_is_obj(data) || (items && !yyjson_is_null(items) && !yyjson_is_arr(items))) {
				throw IOException("feishu_bitable: malformed records/search response from Feishu API");
			}
			auto row_count = !items || yyjson_is_null(items) ? 0 : static_cast<idx_t>(yyjson_arr_size(items));
			if (row_count > STANDARD_VECTOR_SIZE) {
				throw IOException("feishu_bitable: Feishu API returned more records than one vector can hold");
			}
			PrepareOutputVectors(output);
			for (idx_t row = 0; row < row_count; row++) {
				EmitRecordRow(state, output, yyjson_arr_get(items, row), row);
			}
			auto next_has_more = RequireHasMore(data, "records/search");
			auto next_page_token = JsonString(yyjson_obj_get(data, "page_token"));
			ValidateNextRecordPageToken(next_has_more, next_page_token, previous_page_token, state.recent_page_tokens,
			                            state.page_count);
			state.has_more = next_has_more;
			state.page_token = std::move(next_page_token);
			state.emitted_rows += row_count;
			if (row_count == 0 && state.has_more) {
				state.consecutive_empty_pages++;
				if (state.consecutive_empty_pages > 32) {
					throw IOException("feishu_bitable: exceeded maximum consecutive empty pages during pagination");
				}
			} else {
				state.consecutive_empty_pages = 0;
			}
			if (row_count > 0 || !state.has_more) {
				output.SetCardinality(row_count);
				return;
			}
		}
		return;
	}

	while (state.has_more) {
		auto previous_page_token = state.page_token;
		if (state.bind_data.credentials.user_access_token.empty()) {
			TokenProvider provider(context, GetGlobalTokenCache());
			state.access_token = provider.GetTenantAccessToken(
			    state.bind_data.credentials.app_id, state.bind_data.credentials.app_secret, state.bind_data.api_root);
		}
		auto response = state.api.Get(RecordsUrl(state), AuthorizationHeaders(state.access_token));
		EnsureReadHttpSuccess(response.status_code, response.body);
		JsonDocument json(response.body);
		auto root = json.Root();
		EnsureApiSuccess(response.status_code, root);
		auto data = yyjson_obj_get(root, "data");
		auto items = data ? yyjson_obj_get(data, "items") : nullptr;
		if (!data || !yyjson_is_obj(data) || (items && !yyjson_is_null(items) && !yyjson_is_arr(items))) {
			throw IOException("feishu_bitable: malformed records response from Feishu API");
		}
		auto row_count = !items || yyjson_is_null(items) ? 0 : static_cast<idx_t>(yyjson_arr_size(items));
		if (row_count > STANDARD_VECTOR_SIZE) {
			throw IOException("feishu_bitable: Feishu API returned more records than one vector can hold");
		}
		PrepareOutputVectors(output);
		for (idx_t row = 0; row < row_count; row++) {
			EmitRecordRow(state, output, yyjson_arr_get(items, row), row);
		}
		auto next_has_more = RequireHasMore(data, "records");
		auto next_page_token = JsonString(yyjson_obj_get(data, "page_token"));
		ValidateNextRecordPageToken(next_has_more, next_page_token, previous_page_token, state.recent_page_tokens,
		                            state.page_count);
		state.has_more = next_has_more;
		state.page_token = std::move(next_page_token);
		state.emitted_rows += row_count;
		if (row_count == 0 && state.has_more) {
			state.consecutive_empty_pages++;
			if (state.consecutive_empty_pages > 32) {
				throw IOException("feishu_bitable: exceeded maximum consecutive empty pages during pagination");
			}
		} else {
			state.consecutive_empty_pages = 0;
		}
		if (row_count > 0 || !state.has_more) {
			output.SetCardinality(row_count);
			return;
		}
	}
}

static bool IsFeishuScan(const LogicalGet &get) {
	auto &name = get.function.name;
	return name == "feishu_bitable_scan" || name == "read_feishu_bitable" || name == "read_bitable";
}

static bool NullOrderMatchesDuckDBDefault(OrderByNullType null_order) {
	// Measured on this DuckDB: ORDER_DEFAULT is NULLS LAST for both ASC and DESC.
	return null_order == OrderByNullType::ORDER_DEFAULT || null_order == OrderByNullType::NULLS_LAST;
}

static optional_idx ResolveOrderColumn(Expression &expr, LogicalOperator &source, const LogicalGet &get) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return optional_idx();
	}
	auto &ref = expr.Cast<BoundColumnRefExpression>();
	if (ref.depth != 0 || !StringType::GetCollation(ref.return_type).empty()) {
		return optional_idx();
	}
	if (source.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &projection = source.Cast<LogicalProjection>();
		if (ref.binding.table_index != projection.table_index ||
		    ref.binding.column_index >= projection.expressions.size()) {
			return optional_idx();
		}
		auto &inner = *projection.expressions[ref.binding.column_index];
		if (inner.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
		    inner.Cast<BoundColumnRefExpression>().depth != 0 || !StringType::GetCollation(inner.return_type).empty()) {
			return optional_idx();
		}
		return ResolveScanColumnId(inner, get);
	}
	return ResolveScanColumnId(expr, get);
}

static bool TryCollectEquivalentSorts(const LogicalOrder &order, LogicalOperator &source, const LogicalGet &get,
                                      const ScanBindData &bind_data, vector<SearchSortSpec> &sorts) {
	sorts.clear();
	if (order.orders.empty() || order.orders.size() > SEARCH_MAX_SORT_KEYS) {
		return false;
	}
	if (!bind_data.view_id.empty() || !bind_data.single_record_id.empty() ||
	    bind_data.schema_mode != SchemaMode::TYPED || !bind_data.search_plan.sorts.empty()) {
		return false;
	}
	if (!get.table_filters.filters.empty() || (get.dynamic_filters && get.dynamic_filters->HasFilters())) {
		return false;
	}
	for (auto &node : order.orders) {
		if (!node.expression || !NullOrderMatchesDuckDBDefault(node.null_order)) {
			return false;
		}
		if (node.type != OrderType::ORDER_DEFAULT && node.type != OrderType::ASCENDING &&
		    node.type != OrderType::DESCENDING) {
			return false;
		}
		auto column_id = ResolveOrderColumn(*node.expression, source, get);
		if (!column_id.IsValid() || column_id.GetIndex() >= bind_data.fields.size()) {
			return false;
		}
		auto &field = bind_data.fields[column_id.GetIndex()];
		if (!IsSortEquivalentFieldType(field.type_id)) {
			return false;
		}
		SearchSortSpec spec;
		spec.field_name = field.field_name;
		spec.type_id = field.type_id;
		spec.descending = node.type == OrderType::DESCENDING;
		sorts.push_back(std::move(spec));
	}
	return true;
}

static bool AttachEquivalentSearchSort(ScanBindData &bind_data, vector<SearchSortSpec> sorts) {
	if (bind_data.search_plan.enabled()) {
		for (auto &spec : sorts) {
			SearchSortKey key;
			key.field_name = spec.field_name;
			key.descending = spec.descending;
			bind_data.search_plan.sorts.push_back(std::move(key));
		}
		return true;
	}
	SearchPushdownInput input;
	input.sorts = std::move(sorts);
	auto plan = PlanSearchPushdown(input);
	if (!plan.enabled()) {
		return false;
	}
	bind_data.search_plan = std::move(plan);
	return true;
}

static bool TryPushOrderIntoScan(LogicalOrder &order) {
	if (order.children.size() != 1) {
		return false;
	}
	auto &child = *order.children[0];
	LogicalGet *get = nullptr;
	LogicalOperator *source = nullptr;
	if (child.type == LogicalOperatorType::LOGICAL_GET) {
		source = &child;
		get = &child.Cast<LogicalGet>();
	} else if (child.type == LogicalOperatorType::LOGICAL_PROJECTION && child.children.size() == 1 &&
	           child.children[0]->type == LogicalOperatorType::LOGICAL_GET) {
		source = &child;
		get = &child.children[0]->Cast<LogicalGet>();
	}
	if (!get || !source || !IsFeishuScan(*get) || !get->bind_data) {
		return false;
	}
	auto &bind_data = get->bind_data->Cast<ScanBindData>();
	vector<SearchSortSpec> sorts;
	if (!TryCollectEquivalentSorts(order, *source, *get, bind_data, sorts)) {
		return false;
	}
	return AttachEquivalentSearchSort(bind_data, std::move(sorts));
}

static void RewriteFeishuSort(unique_ptr<LogicalOperator> &op) {
	if (!op) {
		return;
	}
	for (auto &child : op->children) {
		RewriteFeishuSort(child);
	}
	if (op->type != LogicalOperatorType::LOGICAL_ORDER_BY) {
		return;
	}
	auto &order = op->Cast<LogicalOrder>();
	if (!TryPushOrderIntoScan(order)) {
		return;
	}
	// The scan now returns rows in the requested order. Drop the local sort.
	// LogicalOrder's bindings are the child's bindings (a subset when a
	// projection map is set), so parents keep resolving those same bindings.
	auto child = std::move(order.children[0]);
	op = std::move(child);
}

static void OptimizeFeishuSortPushdown(OptimizerExtensionInput &, unique_ptr<LogicalOperator> &plan) {
	RewriteFeishuSort(plan);
}

static void AddSharedNamedParameters(TableFunction &function) {
	function.named_parameters["secret"] = LogicalType::VARCHAR;
	function.named_parameters["view_id"] = LogicalType::VARCHAR;
	function.named_parameters["schema_mode"] = LogicalType::VARCHAR;
	function.named_parameters["page_size"] = LogicalType::INTEGER;
	function.named_parameters["include_system_columns"] = LogicalType::BOOLEAN;
}

} // namespace

void RegisterScanFunction(ExtensionLoader &loader) {
	// Classic, fully backwards compatible entry point.
	TableFunction scan("feishu_bitable_scan", {LogicalType::VARCHAR, LogicalType::VARCHAR}, ScanFunction, ScanBind,
	                   ScanInitGlobal);
	AddSharedNamedParameters(scan);
	scan.projection_pushdown = true;
	scan.pushdown_complex_filter = PushdownScanFilters;
	CreateTableFunctionInfo scan_info(std::move(scan));
	scan_info.descriptions.push_back(MakeFunctionDescription(
	    "Scan Feishu/Lark Bitable records with explicit app_token and table_id (classic interface).",
	    {"SELECT * FROM feishu_bitable_scan('<app_token>', '<table_id>');",
	     "SELECT * FROM feishu_bitable_scan('<app_token>', '<table_id>', include_system_columns => true);"},
	    {LogicalType::VARCHAR, LogicalType::VARCHAR}, {}, {"feishu", "bitable"}));
	loader.RegisterFunction(std::move(scan_info));

	// Modern read_* entry points: read_bitable is a plain alias.
	for (auto &name : {"read_feishu_bitable", "read_bitable"}) {
		TableFunctionSet function_set(name);
		for (auto &arguments : {vector<LogicalType> {LogicalType::VARCHAR},
		                        vector<LogicalType> {LogicalType::VARCHAR, LogicalType::VARCHAR}}) {
			TableFunction read(name, arguments, ScanFunction, ReadBind, ScanInitGlobal);
			AddSharedNamedParameters(read);
			read.named_parameters["table_id"] = LogicalType::VARCHAR;
			read.projection_pushdown = true;
			read.pushdown_complex_filter = PushdownScanFilters;
			function_set.AddFunction(std::move(read));
		}
		CreateTableFunctionInfo read_info(std::move(function_set));
		const bool is_alias = string(name) == "read_bitable";
		const string display_name = is_alias ? "read_bitable" : "read_feishu_bitable";
		read_info.descriptions.push_back(MakeFunctionDescription(
		    is_alias ? "Short alias of read_feishu_bitable: read Feishu/Lark Bitable records from a URL or "
		               "app_token (first table when table_id is omitted)."
		             : "Read Feishu/Lark Bitable records from a browser URL or app_token (first table when "
		               "table_id is omitted).",
		    {StringUtil::Format("SELECT * FROM %s('https://<tenant>.feishu.cn/base/<app_token>?table=<table_id>');",
		                        display_name),
		     StringUtil::Format("SELECT * FROM %s('<app_token>', table_id => '<table_id>');", display_name)},
		    {LogicalType::VARCHAR}, {}, {"feishu", "bitable"}));
		read_info.descriptions.push_back(MakeFunctionDescription(
		    is_alias ? "Short alias of read_feishu_bitable: read records with explicit app_token and table_id."
		             : "Read Feishu/Lark Bitable records with explicit app_token and table_id.",
		    {StringUtil::Format("SELECT * FROM %s('<app_token>', '<table_id>');", display_name),
		     StringUtil::Format("SELECT * FROM %s('<app_token>', '<table_id>', schema_mode => 'json');", display_name)},
		    {LogicalType::VARCHAR, LogicalType::VARCHAR}, {}, {"feishu", "bitable"}));
		loader.RegisterFunction(std::move(read_info));
	}

	OptimizerExtension sort_pushdown;
	sort_pushdown.optimize_function = OptimizeFeishuSortPushdown;
	OptimizerExtension::Register(DBConfig::GetConfig(loader.GetDatabaseInstance()), std::move(sort_pushdown));
}

} // namespace feishu
} // namespace duckdb
