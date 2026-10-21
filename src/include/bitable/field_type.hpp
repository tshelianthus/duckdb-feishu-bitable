#pragma once

#include "duckdb/common/string.hpp"

#include <cstdint>

namespace duckdb {
namespace feishu {

//! Field type metadata for Feishu Bitable columns.
//! This translation unit deliberately avoids DuckDB helpers (it never mentions
//! LogicalType) so that it can be compiled into the standalone unit test binary
//! (see test/unit). bitable/type_mapping.hpp turns the storage kind decided
//! here into the actual DuckDB LogicalType.

//! Column typing strategy requested through the `schema_mode` named parameter.
enum class SchemaMode : uint8_t { TYPED, JSON };

//! Feishu Bitable field type IDs (from the official OpenAPI spec).
enum class FeishuFieldTypeId : int32_t {
	TEXT = 1,
	NUMBER = 2,
	SINGLE_SELECT = 3,
	MULTI_SELECT = 4,
	DATE_TIME = 5,
	CHECKBOX = 7,
	USER = 11,
	PHONE = 13,
	URL = 15,
	ATTACHMENT = 17,
	LINK = 18,
	FORMULA = 20,
	DUPLEX_LINK = 21,
	LOCATION = 22,
	GROUP_CHAT = 23,
	CREATED_TIME = 1001,
	MODIFIED_TIME = 1002,
	CREATED_USER = 1003,
	MODIFIED_USER = 1004,
	AUTO_NUMBER = 1005,
	BARCODE = 99001,
	PROGRESS = 99002,
	CURRENCY = 99003,
	RATING = 99004,
	EMAIL = 99005,
};

//! USER / LINK / DUPLEX_LINK are writable via COPY per
//! .specs/03_API_CONTRACT.md sections 8.3 and 8.4. USER ids follow the
//! official user_id_type values open_id, union_id, and user_id.
//! ATTACHMENT / LOCATION / GROUP_CHAT are never writable in this extension
//! (ADR-008); COPY bind rejects them on purpose, not as a temporary gap.

//! Storage kind a Feishu field is projected onto, mirroring the mapping table
//! of .specs/03_API_CONTRACT.md section 4.4.
enum class FeishuValueKind : uint8_t { VARCHAR, DOUBLE, BOOLEAN, TIMESTAMP, VARCHAR_LIST, JSON };

//! Returns the human-readable Feishu field type name for a given type ID,
//! or "Unknown" for an ID that is not part of the published enumeration.
string FeishuFieldTypeName(int32_t type_id);

//! Maps a Feishu field type ID onto its storage kind. In JSON mode every field
//! maps to JSON; in typed mode composite fields (User, Attachment, Link,
//! DuplexLink, Location, GroupChat, Formula, CreatedUser, ModifiedUser) and any
//! unrecognized type degrade to JSON so that no data is lost.
FeishuValueKind FeishuFieldValueKind(int32_t type_id, SchemaMode mode);

} // namespace feishu
} // namespace duckdb
