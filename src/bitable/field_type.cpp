#include "bitable/field_type.hpp"

namespace duckdb {
namespace feishu {

string FeishuFieldTypeName(int32_t type_id) {
	switch (static_cast<FeishuFieldTypeId>(type_id)) {
	case FeishuFieldTypeId::TEXT:
		return "Text";
	case FeishuFieldTypeId::NUMBER:
		return "Number";
	case FeishuFieldTypeId::SINGLE_SELECT:
		return "SingleSelect";
	case FeishuFieldTypeId::MULTI_SELECT:
		return "MultiSelect";
	case FeishuFieldTypeId::DATE_TIME:
		return "DateTime";
	case FeishuFieldTypeId::CHECKBOX:
		return "Checkbox";
	case FeishuFieldTypeId::USER:
		return "User";
	case FeishuFieldTypeId::PHONE:
		return "Phone";
	case FeishuFieldTypeId::URL:
		return "Url";
	case FeishuFieldTypeId::ATTACHMENT:
		return "Attachment";
	case FeishuFieldTypeId::LINK:
		return "Link";
	case FeishuFieldTypeId::FORMULA:
		return "Formula";
	case FeishuFieldTypeId::DUPLEX_LINK:
		return "DuplexLink";
	case FeishuFieldTypeId::LOCATION:
		return "Location";
	case FeishuFieldTypeId::GROUP_CHAT:
		return "GroupChat";
	case FeishuFieldTypeId::CREATED_TIME:
		return "CreatedTime";
	case FeishuFieldTypeId::MODIFIED_TIME:
		return "ModifiedTime";
	case FeishuFieldTypeId::CREATED_USER:
		return "CreatedUser";
	case FeishuFieldTypeId::MODIFIED_USER:
		return "ModifiedUser";
	case FeishuFieldTypeId::AUTO_NUMBER:
		return "AutoNumber";
	case FeishuFieldTypeId::BARCODE:
		return "Barcode";
	case FeishuFieldTypeId::PROGRESS:
		return "Progress";
	case FeishuFieldTypeId::CURRENCY:
		return "Currency";
	case FeishuFieldTypeId::RATING:
		return "Rating";
	case FeishuFieldTypeId::EMAIL:
		return "Email";
	default:
		return "Unknown";
	}
}

FeishuValueKind FeishuFieldValueKind(int32_t type_id, SchemaMode mode) {
	if (mode == SchemaMode::JSON) {
		return FeishuValueKind::JSON;
	}

	switch (static_cast<FeishuFieldTypeId>(type_id)) {
	case FeishuFieldTypeId::TEXT:
	case FeishuFieldTypeId::PHONE:
	case FeishuFieldTypeId::URL:
	case FeishuFieldTypeId::BARCODE:
	case FeishuFieldTypeId::SINGLE_SELECT:
	case FeishuFieldTypeId::EMAIL:
	// AutoNumber is delivered as an opaque string (it may carry a formatted
	// prefix), so it stays text rather than becoming a number.
	case FeishuFieldTypeId::AUTO_NUMBER:
		return FeishuValueKind::VARCHAR;

	case FeishuFieldTypeId::NUMBER:
	case FeishuFieldTypeId::PROGRESS:
	case FeishuFieldTypeId::CURRENCY:
	case FeishuFieldTypeId::RATING:
		return FeishuValueKind::DOUBLE;

	case FeishuFieldTypeId::CHECKBOX:
		return FeishuValueKind::BOOLEAN;

	case FeishuFieldTypeId::DATE_TIME:
	case FeishuFieldTypeId::CREATED_TIME:
	case FeishuFieldTypeId::MODIFIED_TIME:
		return FeishuValueKind::TIMESTAMP;

	case FeishuFieldTypeId::MULTI_SELECT:
		return FeishuValueKind::VARCHAR_LIST;

	// Composite fields keep their full structure so that nothing is lost; the
	// DuckDB JSON operators are the supported way to reach into them.
	case FeishuFieldTypeId::USER:
	case FeishuFieldTypeId::ATTACHMENT:
	case FeishuFieldTypeId::LINK:
	case FeishuFieldTypeId::FORMULA:
	case FeishuFieldTypeId::DUPLEX_LINK:
	case FeishuFieldTypeId::LOCATION:
	case FeishuFieldTypeId::GROUP_CHAT:
	case FeishuFieldTypeId::CREATED_USER:
	case FeishuFieldTypeId::MODIFIED_USER:
		return FeishuValueKind::JSON;

	default:
		// Unknown type: degrade to JSON to avoid data loss.
		return FeishuValueKind::JSON;
	}
}

} // namespace feishu
} // namespace duckdb
