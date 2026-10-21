#pragma once

#include "duckdb/common/string.hpp"
#include "yyjson.hpp"

#include <cstdint>
#include <vector>

namespace duckdb {
namespace feishu {

//! Coercion of raw Feishu record values onto the storage kinds decided by
//! bitable/field_type.hpp. Like api_response.cpp this translation unit avoids
//! DuckDB helpers beyond the exception types so it can be compiled into the
//! standalone unit test binary (see test/unit); writing the coerced results
//! into DataChunk vectors stays in bitable/scan_function.cpp.
//!
//! `std::vector` is used on purpose instead of duckdb::vector for the same
//! reason: the unit test binary never links the DuckDB library.

//! One element of a coerced LIST value. Feishu may deliver JSON nulls inside a
//! multi select array, which become NULL list entries rather than empty text.
struct CoercedListEntry {
	bool is_null = false;
	string value;
};

//! Serializes a value back into compact JSON text. Returns an empty string for
//! a null pointer; throws IOException when serialization fails.
string SerializeJsonValue(duckdb_yyjson::yyjson_val *value);

//! Flattens a value into text. Rich text segment arrays are concatenated,
//! objects prefer their `text` / `name` / `link` / `value` member (this is how
//! URL, Phone and AutoNumber payloads carry their display text), and anything
//! else falls back to its JSON serialization.
string CoerceToString(duckdb_yyjson::yyjson_val *value);

//! Coerces a number or a fully numeric string into a double.
//! Returns false when the value cannot be represented (SQL NULL).
bool CoerceToDouble(duckdb_yyjson::yyjson_val *value, double &result);

//! Coerces a boolean, a number or a "true"/"false"/"1"/"0" string.
//! Returns false when the value cannot be represented (SQL NULL).
bool CoerceToBoolean(duckdb_yyjson::yyjson_val *value, bool &result);

//! Converts a Feishu millisecond epoch into DuckDB microseconds.
//! Returns false when the value is not a timestamp or would overflow.
bool CoerceToTimestampMicros(duckdb_yyjson::yyjson_val *value, int64_t &result);

//! Expands a value into list entries. Arrays map element-wise, a bare scalar
//! becomes a single entry, and anything absent yields false (SQL NULL).
bool CoerceToStringList(duckdb_yyjson::yyjson_val *value, std::vector<CoercedListEntry> &result);

} // namespace feishu
} // namespace duckdb
