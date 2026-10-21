#include "bitable/record_id_lookup.hpp"
#include "bitable/api_response.hpp"
#include "read_error.hpp"

#include "duckdb/common/exception.hpp"

#include <cctype>
#include <cstdlib>

namespace duckdb {
namespace feishu {

using namespace duckdb_yyjson; // NOLINT

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

bool TryReadBusinessCode(const string &response_body, int64_t &code) {
	auto json = TryReadErrorDocument(response_body);
	auto root = yyjson_doc_get_root(json.get());
	if (!root || !yyjson_is_obj(root)) {
		return false;
	}
	auto code_value = yyjson_obj_get(root, "code");
	return yyjson_is_int(code_value) && JsonInt64(code_value, code);
}

} // namespace

RecordFetchPlan PlanRecordFetch(const std::vector<RecordIdPredicate> &predicates) {
	RecordFetchPlan plan;
	if (predicates.size() != 1) {
		return plan;
	}
	auto &predicate = predicates[0];
	if (predicate.kind != RecordIdPredicateKind::EQUALITY || predicate.equality_value.empty()) {
		return plan;
	}
	plan.mode = RecordFetchMode::SINGLE_RECORD;
	plan.record_id = predicate.equality_value;
	return plan;
}

string BuildSingleRecordUrl(const string &api_root, const string &app_token, const string &table_id,
                            const string &record_id, bool automatic_fields) {
	string url = api_root + "/apps/" + PercentEncode(app_token) + "/tables/" + PercentEncode(table_id) + "/records/" +
	             PercentEncode(record_id);
	if (automatic_fields) {
		url += "?automatic_fields=true";
	}
	return url;
}

bool IsSingleRecordNotFound(uint64_t status_code, const string &response_body) {
	if (status_code == 404) {
		return true;
	}
	int64_t code = 0;
	if (!TryReadBusinessCode(response_body, code)) {
		return false;
	}
	// Feishu OpenAPI: RecordIdNotFound.
	return code == 1254043;
}

string ExtractSingleRecordJson(uint64_t status_code, const string &response_body) {
	EnsureReadHttpSuccess(status_code, response_body);
	JsonDocument json(response_body);
	auto root = json.Root();
	EnsureApiSuccess(status_code, root);
	auto data = yyjson_obj_get(root, "data");
	if (!data || !yyjson_is_obj(data)) {
		throw IOException("feishu_bitable: malformed single-record response from Feishu API");
	}
	auto record = yyjson_obj_get(data, "record");
	if (!record || yyjson_is_null(record)) {
		return string();
	}
	if (!yyjson_is_obj(record)) {
		throw IOException("feishu_bitable: malformed single-record response: 'record' must be an object");
	}
	size_t length = 0;
	std::unique_ptr<char, decltype(&std::free)> serialized(yyjson_val_write(record, 0, &length), std::free);
	if (!serialized) {
		throw IOException("feishu_bitable: failed to serialize single-record payload");
	}
	return string(serialized.get(), length);
}

HttpResponse ExecuteRecordFetch(const RecordFetchPlan &plan, const string &url, const RecordFetchGet &get) {
	(void)plan;
	return get(url);
}

} // namespace feishu
} // namespace duckdb
