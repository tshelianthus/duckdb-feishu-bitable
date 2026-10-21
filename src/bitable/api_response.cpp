#include "bitable/api_response.hpp"
#include "read_error.hpp"

#include "common/error.hpp"
#include "duckdb/common/exception.hpp"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>

namespace duckdb {
namespace feishu {

using namespace duckdb_yyjson; // NOLINT

JsonDocument::JsonDocument(const string &json) : document(yyjson_read(json.data(), json.size(), 0)) {
	if (!document) {
		throw IOException("feishu_bitable: invalid JSON returned by Feishu API");
	}
}

JsonDocument::~JsonDocument() {
	yyjson_doc_free(document);
}

yyjson_val *JsonDocument::Root() const {
	return yyjson_doc_get_root(document);
}

string JsonString(yyjson_val *value) {
	if (!value || !yyjson_is_str(value)) {
		return string();
	}
	return string(yyjson_get_str(value), yyjson_get_len(value));
}

bool JsonInt64(yyjson_val *value, int64_t &result) {
	if (!value) {
		return false;
	}
	if (yyjson_is_sint(value)) {
		result = yyjson_get_sint(value);
		return true;
	}
	if (yyjson_is_uint(value)) {
		auto number = yyjson_get_uint(value);
		if (number > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
			return false;
		}
		result = static_cast<int64_t>(number);
		return true;
	}
	if (yyjson_is_real(value)) {
		auto number = yyjson_get_real(value);
		// The largest representable double below INT64_MAX rounds to 2^63.
		// Use a strict 2^63 upper bound so that converting that rounded value
		// can never invoke undefined behavior.
		constexpr double INT64_MAGNITUDE = 9223372036854775808.0; // 2^63
		if (!std::isfinite(number) || number < -INT64_MAGNITUDE || number >= INT64_MAGNITUDE) {
			return false;
		}
		result = static_cast<int64_t>(number);
		return true;
	}
	if (yyjson_is_str(value)) {
		try {
			auto text = JsonString(value);
			size_t consumed = 0;
			auto parsed = std::stoll(text, &consumed);
			if (consumed != text.size()) {
				return false;
			}
			result = parsed;
			return true;
		} catch (...) {
			return false;
		}
	}
	return false;
}

static string ReadBusinessErrorDetails(yyjson_val *root) {
	int64_t code;
	auto value = yyjson_obj_get(root, "code");
	if (!yyjson_is_int(value) || !JsonInt64(value, code) || code == 0) {
		return string();
	}
	auto result = " [code=" + std::to_string(code) + "]";
	auto message = JsonString(yyjson_obj_get(root, "msg"));
	if (!message.empty()) {
		result += ": " + SanitizeErrorMessage(message);
	}
	return result;
}

void EnsureApiSuccess(uint64_t status_code, yyjson_val *root) {
	if (status_code < 200 || status_code >= 300) {
		auto detail = ReadBusinessErrorDetails(root);
		if (status_code >= 400 && status_code < 500 && status_code != 408 && status_code != 429) {
			throw InvalidInputException("feishu_bitable: Feishu API rejected the request (HTTP " +
			                            std::to_string(status_code) + ")" + detail);
		}
		throw IOException("feishu_bitable: Feishu API request failed (HTTP " + std::to_string(status_code) + ")" +
		                  detail);
	}
	if (!root || !yyjson_is_obj(root)) {
		throw IOException("feishu_bitable: malformed Feishu API response");
	}
	int64_t code = -1;
	auto code_value = yyjson_obj_get(root, "code");
	// Business codes are integers, not field values: coercion could turn a
	// malformed nonzero code such as 0.5 or "0invalid" into success.
	if (!yyjson_is_int(code_value) || !JsonInt64(code_value, code)) {
		throw IOException("feishu_bitable: Feishu API response is missing an integer code");
	}
	if (code != 0) {
		throw InvalidInputException("feishu_bitable: Feishu API error" + ReadBusinessErrorDetails(root));
	}
}

bool IsValidPaginationCursor(bool has_more, const string &page_token, const string &previous_page_token) {
	if (!has_more) {
		return true;
	}
	return !page_token.empty() && page_token != previous_page_token;
}

namespace {

void EnsureMetadataHttpSuccess(uint64_t status_code, const string &function_name, const string &resource_name,
                               const string &body) {
	if (status_code >= 200 && status_code < 300) {
		return;
	}
	const auto status = std::to_string(status_code);
	auto json = TryReadErrorDocument(body);
	auto detail = ReadBusinessErrorDetails(yyjson_doc_get_root(json.get()));
	if (status_code == 401) {
		throw InvalidInputException(function_name + ": authentication was rejected (HTTP " + status + ")" + detail);
	}
	if (status_code == 403) {
		throw InvalidInputException(function_name + ": permission denied for " + resource_name + " (HTTP " + status +
		                            ")" + detail);
	}
	if (status_code == 404) {
		throw InvalidInputException(function_name + ": " + resource_name +
		                            " does not exist or is not accessible (HTTP " + status + ")" + detail);
	}
	if (status_code >= 400 && status_code < 500 && status_code != 408 && status_code != 429) {
		throw InvalidInputException(function_name + ": Feishu API rejected the request (HTTP " + status + ")" + detail);
	}
	throw IOException(function_name + ": Feishu API request failed (HTTP " + status + ")" + detail);
}

bool MetadataInt64(yyjson_val *value, int64_t &result) {
	if (yyjson_is_sint(value)) {
		result = yyjson_get_sint(value);
		return true;
	}
	if (yyjson_is_uint(value)) {
		auto number = yyjson_get_uint(value);
		if (number <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
			result = static_cast<int64_t>(number);
			return true;
		}
	}
	return false;
}

yyjson_val *RequireMetadataData(JsonDocument &json, const string &function_name, const string &resource_name) {
	auto root = json.Root();
	if (!root || !yyjson_is_obj(root)) {
		throw IOException(function_name + ": malformed response: root must be an object");
	}
	int64_t code = -1;
	if (!MetadataInt64(yyjson_obj_get(root, "code"), code)) {
		throw IOException(function_name + ": malformed response: missing integer 'code'");
	}
	if (code != 0) {
		string message = function_name + ": Feishu API rejected the " + resource_name + " request (code " +
		                 std::to_string(code) + ")";
		auto api_message = yyjson_obj_get(root, "msg");
		if (api_message && yyjson_is_str(api_message)) {
			message += ": " + SanitizeErrorMessage(string(yyjson_get_str(api_message), yyjson_get_len(api_message)));
		}
		throw InvalidInputException(message);
	}
	auto data = yyjson_obj_get(root, "data");
	if (!data || !yyjson_is_obj(data)) {
		throw IOException(function_name + ": malformed response: missing object 'data'");
	}
	return data;
}

std::optional<string> OptionalMetadataString(yyjson_val *object, const char *key, const string &function_name) {
	auto value = yyjson_obj_get(object, key);
	if (!value || yyjson_is_null(value)) {
		return std::nullopt;
	}
	if (std::string(key) == "description" && yyjson_is_obj(value)) {
		auto text = yyjson_obj_get(value, "text");
		if (text && yyjson_is_str(text)) {
			return string(yyjson_get_str(text), yyjson_get_len(text));
		}
		throw IOException(function_name + ": malformed response: 'description.text' must be a string");
	}
	if (!yyjson_is_str(value)) {
		throw IOException(function_name + ": malformed response: '" + string(key) + "' must be a string");
	}
	return string(yyjson_get_str(value), yyjson_get_len(value));
}

std::optional<int64_t> OptionalMetadataInt64(yyjson_val *object, const char *key, const string &function_name) {
	auto value = yyjson_obj_get(object, key);
	if (!value || yyjson_is_null(value)) {
		return std::nullopt;
	}
	int64_t result;
	if (!MetadataInt64(value, result)) {
		throw IOException(function_name + ": malformed response: '" + string(key) + "' must be a BIGINT");
	}
	return result;
}

std::optional<int32_t> OptionalMetadataInt32(yyjson_val *object, const char *key, const string &function_name) {
	auto value = yyjson_obj_get(object, key);
	if (!value || yyjson_is_null(value)) {
		return std::nullopt;
	}
	int64_t result;
	if (!MetadataInt64(value, result) || result < std::numeric_limits<int32_t>::min() ||
	    result > std::numeric_limits<int32_t>::max()) {
		throw IOException(function_name + ": malformed response: '" + string(key) + "' must be an INTEGER");
	}
	return static_cast<int32_t>(result);
}

std::optional<bool> OptionalMetadataBoolean(yyjson_val *object, const char *key, const string &function_name) {
	auto value = yyjson_obj_get(object, key);
	if (!value || yyjson_is_null(value)) {
		return std::nullopt;
	}
	if (!yyjson_is_bool(value)) {
		throw IOException(function_name + ": malformed response: '" + string(key) + "' must be BOOLEAN");
	}
	return yyjson_get_bool(value);
}

std::optional<string> OptionalMetadataJson(yyjson_val *object, const char *key, const string &function_name) {
	auto value = yyjson_obj_get(object, key);
	if (!value || yyjson_is_null(value)) {
		return std::nullopt;
	}
	size_t json_length = 0;
	auto json = yyjson_val_write(value, YYJSON_WRITE_NOFLAG, &json_length);
	if (!json) {
		throw IOException(function_name + ": failed to serialize field property JSON");
	}
	std::unique_ptr<char, decltype(&std::free)> json_guard(json, std::free);
	return string(json, json_length);
}

void ParseRequiredPagination(yyjson_val *data, const string &function_name, bool &has_more, string &page_token) {
	auto more = yyjson_obj_get(data, "has_more");
	if (!more || !yyjson_is_bool(more)) {
		throw IOException(function_name + ": malformed response: missing BOOLEAN 'has_more'");
	}
	has_more = yyjson_get_bool(more);
	auto next_page_token = OptionalMetadataString(data, "page_token", function_name);
	if (next_page_token) {
		page_token = std::move(*next_page_token);
	}
}

} // namespace

TablesMetadataPage ParseTablesMetadataPage(uint64_t status_code, const string &response_body) {
	const string function_name = "feishu_bitable_tables";
	EnsureMetadataHttpSuccess(status_code, function_name, "base", response_body);
	JsonDocument json(response_body);
	auto data = RequireMetadataData(json, function_name, "base metadata");
	auto items = yyjson_obj_get(data, "items");
	if (items && !yyjson_is_null(items) && !yyjson_is_arr(items)) {
		throw IOException(function_name + ": malformed response: 'items' must be an array");
	}

	TablesMetadataPage result;
	if (yyjson_is_arr(items)) {
		size_t index, max;
		yyjson_val *item;
		yyjson_arr_foreach(items, index, max, item) {
			if (!yyjson_is_obj(item)) {
				throw IOException(function_name + ": malformed response: table item must be an object");
			}
			TableMetadata table;
			table.table_id = OptionalMetadataString(item, "table_id", function_name);
			table.name = OptionalMetadataString(item, "name", function_name);
			table.revision = OptionalMetadataInt64(item, "revision", function_name);
			result.rows.push_back(std::move(table));
		}
	}
	ParseRequiredPagination(data, function_name, result.has_more, result.page_token);
	return result;
}

FieldsMetadataPage ParseFieldsMetadataPage(uint64_t status_code, const string &response_body) {
	const string function_name = "feishu_bitable_fields";
	EnsureMetadataHttpSuccess(status_code, function_name, "table", response_body);
	JsonDocument json(response_body);
	auto data = RequireMetadataData(json, function_name, "table metadata");
	auto items = yyjson_obj_get(data, "items");
	if (items && !yyjson_is_null(items) && !yyjson_is_arr(items)) {
		throw IOException(function_name + ": malformed response: 'items' must be an array");
	}

	FieldsMetadataPage result;
	if (yyjson_is_arr(items)) {
		size_t index, max;
		yyjson_val *item;
		yyjson_arr_foreach(items, index, max, item) {
			if (!yyjson_is_obj(item)) {
				throw IOException(function_name + ": malformed response: field item must be an object");
			}
			FieldMetadata field;
			field.field_id = OptionalMetadataString(item, "field_id", function_name);
			field.field_name = OptionalMetadataString(item, "field_name", function_name);
			field.type_id = OptionalMetadataInt32(item, "type", function_name);
			field.type_name = OptionalMetadataString(item, "type_name", function_name);
			field.is_primary = OptionalMetadataBoolean(item, "is_primary", function_name);
			field.description = OptionalMetadataString(item, "description", function_name);
			field.property_json = OptionalMetadataJson(item, "property", function_name);
			result.rows.push_back(std::move(field));
		}
	}
	ParseRequiredPagination(data, function_name, result.has_more, result.page_token);
	return result;
}

void AdvanceMetadataPaginationCursor(const string &function_name, bool has_more, const string &next_page_token,
                                     string &current_page_token, std::unordered_set<string> &seen_page_tokens) {
	if (!has_more) {
		current_page_token.clear();
		return;
	}
	if (next_page_token.empty()) {
		throw IOException(function_name + ": has_more is true but page_token is missing");
	}
	if (next_page_token == current_page_token || seen_page_tokens.find(next_page_token) != seen_page_tokens.end()) {
		throw IOException(function_name + ": repeated page_token returned by Feishu API");
	}
	seen_page_tokens.insert(next_page_token);
	current_page_token = next_page_token;
}

string ParseFirstTableId(uint64_t status_code, const string &response_body) {
	EnsureReadHttpSuccess(status_code, response_body);
	JsonDocument json(response_body);
	auto root = json.Root();
	EnsureApiSuccess(status_code, root);
	auto data = yyjson_obj_get(root, "data");
	auto items = data ? yyjson_obj_get(data, "items") : nullptr;
	if (!data || !yyjson_is_obj(data) || (items && !yyjson_is_null(items) && !yyjson_is_arr(items))) {
		throw IOException("feishu_bitable: malformed tables response from Feishu API");
	}
	bool has_more;
	string page_token;
	ParseRequiredPagination(data, "feishu_bitable", has_more, page_token);
	if (has_more && page_token.empty()) {
		throw IOException("feishu_bitable: tables response returned an invalid pagination cursor");
	}
	if (has_more && (!items || yyjson_is_null(items) || yyjson_arr_size(items) == 0)) {
		throw IOException("feishu_bitable: first tables page is empty but has_more is true");
	}
	if (!items || yyjson_is_null(items) || yyjson_arr_size(items) == 0) {
		throw InvalidInputException("feishu_bitable: base contains no tables");
	}
	auto first = yyjson_arr_get(items, 0);
	auto table_id = first && yyjson_is_obj(first) ? JsonString(yyjson_obj_get(first, "table_id")) : string();
	if (table_id.empty()) {
		throw InvalidInputException("feishu_bitable: base contains no tables");
	}
	return table_id;
}

} // namespace feishu
} // namespace duckdb
