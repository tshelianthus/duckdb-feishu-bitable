#pragma once

#include "bitable/field_type.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {
namespace feishu {

//! Maps a Feishu field type (by integer ID) to a DuckDB LogicalType.
//! In JSON mode, every field maps to JSON. The mapping decision itself lives in
//! FeishuFieldValueKind (bitable/field_type.hpp) so that it stays unit testable
//! without linking DuckDB.
LogicalType MapFeishuFieldType(int32_t type_id, SchemaMode mode);

} // namespace feishu
} // namespace duckdb
