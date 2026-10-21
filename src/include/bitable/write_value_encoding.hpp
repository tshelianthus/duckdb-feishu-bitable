#pragma once

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace duckdb {
namespace feishu {

//! Write-side classification of a Feishu field type.
//! Frozen by .specs/03_API_CONTRACT.md sections 8.3 and 8.5.
enum class WriteTargetKind : uint8_t {
	TEXT,
	//! Feishu URL fields accept VARCHAR in SQL but require a link/text object
	//! on the records API wire format.
	URL,
	NUMBER,
	CHECKBOX,
	DATE_TIME,
	MULTI_SELECT,
	//! LINK(18) / DUPLEX_LINK(21): JSON string array of record_id values.
	LINK,
	//! USER(11): JSON array of {"id":"..."} objects (never a plain string array).
	USER,
	//! Structurally read-only: formula, system timestamps, system users, auto number.
	READ_ONLY,
	//! Writable in Feishu but out of scope for the current append-only writeback
	//! (ATTACHMENT / LOCATION / GROUP_CHAT).
	UNSUPPORTED,
	//! Type id outside the published enumeration; refusing is deliberate.
	UNKNOWN,
};

//! Classifies a Feishu field type id for the write path. Note the deliberate
//! asymmetry with the read path, which degrades unknown ids to JSON.
WriteTargetKind ClassifyWriteTarget(int32_t type_id);

//! Scalar forms the write encoder accepts. They mirror what the read path can
//! extract from a DuckDB vector, minus every type section 8.3 rejects at bind.
enum class WriteScalarKind : uint8_t {
	NULL_VALUE,
	TEXT,
	NUMBER,
	//! Canonical decimal integer text. This also covers DuckDB HUGEINT without
	//! introducing a DuckDB value type into this self-contained translation unit.
	INTEGER,
	BOOLEAN,
	TIMESTAMP_MICROS,
	STRING_LIST
};

struct WriteScalar {
	WriteScalarKind kind = WriteScalarKind::NULL_VALUE;
	string text;
	double number = 0;
	bool boolean = false;
	//! UTC microseconds since the epoch (DuckDB internal timestamp unit).
	int64_t micros = 0;
	std::vector<string> list;
	//! Positionally aligned with `list`; a true entry is SQL NULL.
	std::vector<bool> list_has_null;

	static WriteScalar Null();
	static WriteScalar Text(string value);
	static WriteScalar Number(double value);
	//! `value` must be canonical signed decimal integer text (for example
	//! "-42", "0", or the decimal spelling of a HUGEINT).
	static WriteScalar Integer(string value);
	static WriteScalar Boolean(bool value);
	static WriteScalar TimestampMicros(int64_t value);
	static WriteScalar List(std::vector<string> values, std::vector<bool> has_null);
};

//! One append-only batch ready for a records/batch_create request.
//! `first_row` is zero-based and is retained for the write-side diagnostics
//! required by .specs/02_ARCHITECTURE.md section 2.6.
struct WriteBatch {
	idx_t first_row = 0;
	std::vector<string> rows;
};

//! Pure batch boundary logic shared by the COPY sink and the standalone unit
//! tests. The accumulator owns no network state and never performs I/O.
class WriteBatchAccumulator {
public:
	explicit WriteBatchAccumulator(idx_t batch_size);

	//! Adds one already encoded record object. Returns true exactly when a full
	//! batch was produced and moved into `ready`.
	bool AddRow(string row, WriteBatch &ready);

	//! Moves the final partial batch into `ready`, if one exists.
	bool Flush(WriteBatch &ready);

	idx_t PendingRows() const;
	idx_t RowsSeen() const;

private:
	idx_t batch_size;
	idx_t rows_seen = 0;
	std::vector<string> pending_rows;
};

//! Highest millisecond epoch a Feishu date field accepts
//! (9999-12-31T23:59:59.999Z). Defensive bound: Feishu stores dates as
//! millisecond epochs and no business date exceeds year 9999
//! (.specs/03_API_CONTRACT.md section 8.4).
constexpr int64_t FEISHU_MAX_TIMESTAMP_MILLIS = 253402300799999LL;

//! Escapes `value` as a JSON string literal, quotes included.
string EncodeJsonString(const string &value);

//! Percent-encodes a path or query component (RFC 3986 unreserved set kept).
string PercentEncodeUrlComponent(const string &value);

//! Builds `.../records/batch_create?user_id_type=<value>`. The query parameter
//! is always present, including when `user_id_type` is the default `open_id`.
//! `user_id_type` must already be one of the three legal lowercase values.
string BuildRecordsBatchCreateUrl(const string &api_root, const string &app_token, const string &table_id,
                                  const string &user_id_type);

//! Encodes one scalar into the compact JSON value Feishu expects for a field of
//! `target` kind. Returns false when the value must be omitted entirely (SQL
//! NULL), true when `out` was filled. Throws InvalidInputException, with the
//! message built by string concatenation, for every input section 8.4 forbids.
//! `out` is cleared before encoding, including when the field is omitted.
bool EncodeWriteValue(const WriteScalar &value, WriteTargetKind target, string &out);

//! Renders one `"<field>":<value>` entry, or an empty string when the value is
//! omitted. Callers join the entries of one row into a `fields` object.
string EncodeFieldsEntry(const string &field_name, const WriteScalar &value, WriteTargetKind target);

} // namespace feishu
} // namespace duckdb
