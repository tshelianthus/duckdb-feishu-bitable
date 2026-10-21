#pragma once

#include "duckdb/common/string.hpp"
#include "yyjson.hpp"

#include <cstdint>
#include <optional>
#include <unordered_set>
#include <vector>

namespace duckdb {
namespace feishu {

//! Shared parsing helpers for Feishu OpenAPI responses.
//! This translation unit deliberately avoids DuckDB helpers beyond the
//! exception types so that it can be compiled into the standalone unit test
//! binary (see test/unit).

//! RAII wrapper around a parsed Feishu API response body.
struct JsonDocument {
	explicit JsonDocument(const string &json);
	~JsonDocument();
	JsonDocument(const JsonDocument &) = delete;
	JsonDocument &operator=(const JsonDocument &) = delete;

	duckdb_yyjson::yyjson_val *Root() const;

	duckdb_yyjson::yyjson_doc *document;
};

//! Returns the string value, or an empty string when the value is absent or not a string.
string JsonString(duckdb_yyjson::yyjson_val *value);

//! Coerces a JSON number or numeric string into an int64_t.
bool JsonInt64(duckdb_yyjson::yyjson_val *value, int64_t &result);

//! Throws unless the response carries a 2xx status and a Feishu business code of 0.
void EnsureApiSuccess(uint64_t status_code, duckdb_yyjson::yyjson_val *root);

//! Validates the pagination cursor of a Feishu list response. Feishu answers a
//! page with `has_more` plus the cursor for the next request; a truthy
//! `has_more` paired with an empty or unchanged `page_token` would spin the
//! scan forever, so callers must treat `false` as a protocol violation and
//! throw. Returns true whenever pagination may safely continue (including the
//! terminal `has_more == false` case).
bool IsValidPaginationCursor(bool has_more, const string &page_token, const string &previous_page_token);

//! Metadata returned by GET /tables. Missing nullable properties are preserved
//! as std::nullopt so the table function can emit SQL NULL without inferring
//! values from another row.
struct TableMetadata {
	std::optional<string> table_id;
	std::optional<string> name;
	std::optional<int64_t> revision;
};

//! Metadata returned by GET /fields. `property_json` contains the serialized
//! property value exactly as a JSON value when present.
struct FieldMetadata {
	std::optional<string> field_id;
	std::optional<string> field_name;
	std::optional<int32_t> type_id;
	std::optional<string> type_name;
	std::optional<bool> is_primary;
	std::optional<string> description;
	std::optional<string> property_json;
};

template <class T>
struct MetadataPage {
	std::vector<T> rows;
	string page_token;
	bool has_more = false;
};

using TablesMetadataPage = MetadataPage<TableMetadata>;
using FieldsMetadataPage = MetadataPage<FieldMetadata>;

//! Parses one metadata page. A successful response must contain an explicit
//! boolean `has_more`; an absent/null/mistyped value is a protocol error.
//! `items` may be absent, null, or an empty array and then yields zero rows.
TablesMetadataPage ParseTablesMetadataPage(uint64_t status_code, const string &response_body);
FieldsMetadataPage ParseFieldsMetadataPage(uint64_t status_code, const string &response_body);

//! Advances a metadata pagination cursor and remembers every cursor already
//! returned by the endpoint. When `has_more` is true, an absent/empty cursor or
//! any repeated cursor is rejected before another HTTP request can be issued.
//! Terminal pages may omit page_token.
void AdvanceMetadataPaginationCursor(const string &function_name, bool has_more, const string &next_page_token,
                                     string &current_page_token, std::unordered_set<string> &seen_page_tokens);

//! Returns `data.items[0].table_id` from a `GET /apps/{app_token}/tables` response.
//! Throws InvalidInputException("feishu_bitable: base contains no tables") when the
//! base holds no readable table.
string ParseFirstTableId(uint64_t status_code, const string &response_body);

} // namespace feishu
} // namespace duckdb
