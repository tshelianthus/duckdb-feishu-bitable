#include "catch.hpp"

#include "bitable/field_type.hpp"

using duckdb::feishu::FeishuFieldTypeName;
using duckdb::feishu::FeishuFieldValueKind;
using duckdb::feishu::FeishuValueKind;
using duckdb::feishu::SchemaMode;

namespace {

FeishuValueKind Typed(int32_t type_id) {
	return FeishuFieldValueKind(type_id, SchemaMode::TYPED);
}

FeishuValueKind Json(int32_t type_id) {
	return FeishuFieldValueKind(type_id, SchemaMode::JSON);
}

} // namespace

// =============================================================================
// FeishuFieldTypeName — the readable name exposed by feishu_bitable_fields
// =============================================================================

TEST_CASE("FeishuFieldTypeName covers every published field type", "[field_type]") {
	REQUIRE(FeishuFieldTypeName(1) == "Text");
	REQUIRE(FeishuFieldTypeName(2) == "Number");
	REQUIRE(FeishuFieldTypeName(3) == "SingleSelect");
	REQUIRE(FeishuFieldTypeName(4) == "MultiSelect");
	REQUIRE(FeishuFieldTypeName(5) == "DateTime");
	REQUIRE(FeishuFieldTypeName(7) == "Checkbox");
	REQUIRE(FeishuFieldTypeName(11) == "User");
	REQUIRE(FeishuFieldTypeName(13) == "Phone");
	REQUIRE(FeishuFieldTypeName(15) == "Url");
	REQUIRE(FeishuFieldTypeName(17) == "Attachment");
	REQUIRE(FeishuFieldTypeName(18) == "Link");
	REQUIRE(FeishuFieldTypeName(20) == "Formula");
	REQUIRE(FeishuFieldTypeName(21) == "DuplexLink");
	REQUIRE(FeishuFieldTypeName(22) == "Location");
	REQUIRE(FeishuFieldTypeName(23) == "GroupChat");
	REQUIRE(FeishuFieldTypeName(1001) == "CreatedTime");
	REQUIRE(FeishuFieldTypeName(1002) == "ModifiedTime");
	REQUIRE(FeishuFieldTypeName(1003) == "CreatedUser");
	REQUIRE(FeishuFieldTypeName(1004) == "ModifiedUser");
	REQUIRE(FeishuFieldTypeName(1005) == "AutoNumber");
	REQUIRE(FeishuFieldTypeName(99001) == "Barcode");
	REQUIRE(FeishuFieldTypeName(99002) == "Progress");
	REQUIRE(FeishuFieldTypeName(99003) == "Currency");
	REQUIRE(FeishuFieldTypeName(99004) == "Rating");
	REQUIRE(FeishuFieldTypeName(99005) == "Email");
}

TEST_CASE("FeishuFieldTypeName degrades unknown ids", "[field_type]") {
	REQUIRE(FeishuFieldTypeName(0) == "Unknown");
	REQUIRE(FeishuFieldTypeName(-1) == "Unknown");
	REQUIRE(FeishuFieldTypeName(6) == "Unknown");
	REQUIRE(FeishuFieldTypeName(99999) == "Unknown");
	REQUIRE(FeishuFieldTypeName(2147483647) == "Unknown");
}

// =============================================================================
// FeishuFieldValueKind — .specs/03_API_CONTRACT.md section 4.4
// =============================================================================

TEST_CASE("Text-like fields map to VARCHAR", "[field_type]") {
	REQUIRE(Typed(1) == FeishuValueKind::VARCHAR);     // Text
	REQUIRE(Typed(13) == FeishuValueKind::VARCHAR);    // Phone
	REQUIRE(Typed(15) == FeishuValueKind::VARCHAR);    // Url
	REQUIRE(Typed(99001) == FeishuValueKind::VARCHAR); // Barcode
	REQUIRE(Typed(99005) == FeishuValueKind::VARCHAR); // Email
	REQUIRE(Typed(3) == FeishuValueKind::VARCHAR);     // SingleSelect
}

TEST_CASE("AutoNumber stays text because it may carry a formatted prefix", "[field_type]") {
	REQUIRE(Typed(1005) == FeishuValueKind::VARCHAR);
}

TEST_CASE("Numeric fields map to DOUBLE", "[field_type]") {
	REQUIRE(Typed(2) == FeishuValueKind::DOUBLE);     // Number
	REQUIRE(Typed(99002) == FeishuValueKind::DOUBLE); // Progress
	REQUIRE(Typed(99003) == FeishuValueKind::DOUBLE); // Currency
	REQUIRE(Typed(99004) == FeishuValueKind::DOUBLE); // Rating
}

TEST_CASE("Checkbox maps to BOOLEAN", "[field_type]") {
	REQUIRE(Typed(7) == FeishuValueKind::BOOLEAN);
}

TEST_CASE("Date-like fields map to TIMESTAMP", "[field_type]") {
	REQUIRE(Typed(5) == FeishuValueKind::TIMESTAMP);    // DateTime
	REQUIRE(Typed(1001) == FeishuValueKind::TIMESTAMP); // CreatedTime
	REQUIRE(Typed(1002) == FeishuValueKind::TIMESTAMP); // ModifiedTime
}

TEST_CASE("MultiSelect maps to a VARCHAR list", "[field_type]") {
	REQUIRE(Typed(4) == FeishuValueKind::VARCHAR_LIST);
}

TEST_CASE("Composite fields keep their JSON structure", "[field_type]") {
	REQUIRE(Typed(11) == FeishuValueKind::JSON);   // User
	REQUIRE(Typed(17) == FeishuValueKind::JSON);   // Attachment
	REQUIRE(Typed(18) == FeishuValueKind::JSON);   // Link
	REQUIRE(Typed(20) == FeishuValueKind::JSON);   // Formula
	REQUIRE(Typed(21) == FeishuValueKind::JSON);   // DuplexLink
	REQUIRE(Typed(22) == FeishuValueKind::JSON);   // Location
	REQUIRE(Typed(23) == FeishuValueKind::JSON);   // GroupChat
	REQUIRE(Typed(1003) == FeishuValueKind::JSON); // CreatedUser
	REQUIRE(Typed(1004) == FeishuValueKind::JSON); // ModifiedUser
}

TEST_CASE("Unknown field types degrade to JSON instead of losing data", "[field_type]") {
	REQUIRE(Typed(0) == FeishuValueKind::JSON);
	REQUIRE(Typed(6) == FeishuValueKind::JSON);
	REQUIRE(Typed(-1) == FeishuValueKind::JSON);
	REQUIRE(Typed(99999) == FeishuValueKind::JSON);
}

TEST_CASE("JSON schema mode maps every field type to JSON", "[field_type]") {
	const int32_t every_type[] = {1,    2,    3,    4,    5,    7,     11,    13,    15,    17,    18, 20, 21, 22,   23,
	                              1001, 1002, 1003, 1004, 1005, 99001, 99002, 99003, 99004, 99005, 0,  6,  -1, 99999};
	for (size_t i = 0; i < sizeof(every_type) / sizeof(every_type[0]); i++) {
		REQUIRE(Json(every_type[i]) == FeishuValueKind::JSON);
	}
}
