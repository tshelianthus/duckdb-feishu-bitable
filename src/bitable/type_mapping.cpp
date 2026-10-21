#include "bitable/type_mapping.hpp"

namespace duckdb {
namespace feishu {

LogicalType MapFeishuFieldType(int32_t type_id, SchemaMode mode) {
	switch (FeishuFieldValueKind(type_id, mode)) {
	case FeishuValueKind::VARCHAR:
		return LogicalType::VARCHAR;
	case FeishuValueKind::DOUBLE:
		return LogicalType::DOUBLE;
	case FeishuValueKind::BOOLEAN:
		return LogicalType::BOOLEAN;
	case FeishuValueKind::TIMESTAMP:
		return LogicalType::TIMESTAMP;
	case FeishuValueKind::VARCHAR_LIST:
		return LogicalType::LIST(LogicalType::VARCHAR);
	default:
		return LogicalType::JSON();
	}
}

} // namespace feishu
} // namespace duckdb
