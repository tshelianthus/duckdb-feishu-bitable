#include "catch.hpp"

#include "bitable/api_response.hpp"
#include "bitable/value_coercion.hpp"

#include <limits>
#include <vector>

using duckdb::feishu::CoercedListEntry;
using duckdb::feishu::CoerceToBoolean;
using duckdb::feishu::CoerceToDouble;
using duckdb::feishu::CoerceToString;
using duckdb::feishu::CoerceToStringList;
using duckdb::feishu::CoerceToTimestampMicros;
using duckdb::feishu::JsonDocument;
using duckdb::feishu::SerializeJsonValue;

namespace {

//! Coerces the value a Feishu record carries for one field. The literal is the
//! JSON that would sit under `data.items[i].fields["<field name>"]`.
std::string Text(const char *field_value) {
	JsonDocument document(field_value);
	return CoerceToString(document.Root());
}

std::string Json(const char *field_value) {
	JsonDocument document(field_value);
	return SerializeJsonValue(document.Root());
}

std::vector<CoercedListEntry> List(const char *field_value, bool &ok) {
	JsonDocument document(field_value);
	std::vector<CoercedListEntry> entries;
	ok = CoerceToStringList(document.Root(), entries);
	return entries;
}

} // namespace

// =============================================================================
// CoerceToString — VARCHAR fields (Text, Phone, Url, Barcode, SingleSelect,
// Email, AutoNumber)
// =============================================================================

TEST_CASE("Rich text segments are concatenated", "[value_coercion]") {
	REQUIRE(Text(R"([{"type":"text","text":"Hello "},{"type":"text","text":"world"}])") == "Hello world");
	REQUIRE(Text(R"([{"type":"text","text":"only"}])") == "only");
	REQUIRE(Text(R"([])") == "");
}

TEST_CASE("Plain strings pass through unchanged", "[value_coercion]") {
	REQUIRE(Text(R"("13800000000")") == "13800000000");
	REQUIRE(Text(R"("")") == "");
	REQUIRE(Text(R"("中文 / emoji 😀")") == "中文 / emoji 😀");
}

TEST_CASE("Url payloads prefer their display text over the raw link", "[value_coercion]") {
	REQUIRE(Text(R"({"link":"https://example.com/a","text":"Example"})") == "Example");
	// A link with no display text still yields the address rather than JSON.
	REQUIRE(Text(R"({"link":"https://example.com/a"})") == "https://example.com/a");
}

TEST_CASE("AutoNumber payloads expose their value", "[value_coercion]") {
	// AutoNumber arrives either as a bare string or wrapped in a value object.
	REQUIRE(Text(R"("AN-000123")") == "AN-000123");
	REQUIRE(Text(R"({"value":"AN-000123"})") == "AN-000123");
}

TEST_CASE("Objects without a display member keep their JSON form", "[value_coercion]") {
	REQUIRE(Text(R"({"unexpected":1})") == R"({"unexpected":1})");
}

TEST_CASE("Numbers and booleans coerced to text keep their JSON spelling", "[value_coercion]") {
	REQUIRE(Text("42") == "42");
	REQUIRE(Text("1.5") == "1.5");
	REQUIRE(Text("true") == "true");
}

TEST_CASE("A JSON null coerces to empty text", "[value_coercion]") {
	REQUIRE(Text("null") == "");
	REQUIRE(CoerceToString(nullptr) == "");
}

// =============================================================================
// SerializeJsonValue — composite fields and SCHEMA_MODE 'json'
// =============================================================================

TEST_CASE("User fields keep their full structure", "[value_coercion]") {
	auto user = R"([{"id":"ou_abc","name":"Ada","en_name":"Ada","email":"ada@example.com"}])";
	REQUIRE(Json(user) == R"([{"id":"ou_abc","name":"Ada","en_name":"Ada","email":"ada@example.com"}])");
}

TEST_CASE("Attachment fields keep every file entry", "[value_coercion]") {
	auto attachment = R"([{"file_token":"boxcn1","name":"a.png","size":12,"type":"image/png","url":"https://x/a"},)"
	                  R"({"file_token":"boxcn2","name":"b.pdf","size":34,"type":"application/pdf"}])";
	REQUIRE(Json(attachment) == attachment);
}

TEST_CASE("Link and DuplexLink fields keep their record ids", "[value_coercion]") {
	auto link = R"({"link_record_ids":["rec1","rec2"],"text":"A, B","text_arr":["A","B"],"type":"text"})";
	REQUIRE(Json(link) == link);
}

TEST_CASE("Location fields keep their coordinates", "[value_coercion]") {
	auto location = R"({"address":"1 Main St","adname":"District","location":"116.39,39.91","name":"HQ"})";
	REQUIRE(Json(location) == location);
}

TEST_CASE("GroupChat fields keep their chat entries", "[value_coercion]") {
	auto group_chat = R"([{"avatar_url":"https://x/g","id":"oc_1","name":"Team"}])";
	REQUIRE(Json(group_chat) == group_chat);
}

TEST_CASE("Formula fields keep the raw result envelope", "[value_coercion]") {
	REQUIRE(Json(R"({"type":2,"value":[3.5]})") == R"({"type":2,"value":[3.5]})");
	REQUIRE(Json(R"([{"text":"computed","type":"text"}])") == R"([{"text":"computed","type":"text"}])");
}

TEST_CASE("CreatedUser and ModifiedUser fields keep their identity object", "[value_coercion]") {
	auto audit_user =
	    R"({"avatar_url":"https://x/u","email":"ada@example.com","en_name":"Ada","id":"ou_abc","name":"Ada"})";
	REQUIRE(Json(audit_user) == audit_user);
}

TEST_CASE("JSON mode keeps scalar fields as valid JSON scalars", "[value_coercion]") {
	// SCHEMA_MODE 'json' routes every column through this path, so scalars must
	// stay quoted / spelled as JSON rather than being flattened to text.
	REQUIRE(Json(R"("plain text")") == R"("plain text")");
	REQUIRE(Json("42") == "42");
	REQUIRE(Json("true") == "true");
	REQUIRE(Json("null") == "null");
	REQUIRE(Json(R"(["a","b"])") == R"(["a","b"])");
	REQUIRE(SerializeJsonValue(nullptr) == "");
}

// =============================================================================
// CoerceToDouble — Number, Progress, Currency, Rating
// =============================================================================

TEST_CASE("Numeric field values coerce to double", "[value_coercion]") {
	double result = 0;
	JsonDocument integer("42");
	REQUIRE(CoerceToDouble(integer.Root(), result));
	REQUIRE(result == 42.0);

	JsonDocument real("0.35");
	REQUIRE(CoerceToDouble(real.Root(), result));
	REQUIRE(result == 0.35);

	JsonDocument negative("-7");
	REQUIRE(CoerceToDouble(negative.Root(), result));
	REQUIRE(result == -7.0);

	JsonDocument numeric_string(R"("12.5")");
	REQUIRE(CoerceToDouble(numeric_string.Root(), result));
	REQUIRE(result == 12.5);
}

TEST_CASE("Non numeric values become SQL NULL instead of a wrong number", "[value_coercion]") {
	double result = 123;
	JsonDocument text(R"("12.5kg")");
	REQUIRE_FALSE(CoerceToDouble(text.Root(), result));

	JsonDocument empty(R"("")");
	REQUIRE_FALSE(CoerceToDouble(empty.Root(), result));

	JsonDocument object(R"({"value":1})");
	REQUIRE_FALSE(CoerceToDouble(object.Root(), result));

	JsonDocument null_value("null");
	REQUIRE_FALSE(CoerceToDouble(null_value.Root(), result));

	// std::stod accepts these spellings, but neither is a finite DuckDB DOUBLE
	// value that a Feishu number field can represent.
	JsonDocument nan_value(R"("nan")");
	REQUIRE_FALSE(CoerceToDouble(nan_value.Root(), result));

	JsonDocument infinity_value(R"("inf")");
	REQUIRE_FALSE(CoerceToDouble(infinity_value.Root(), result));

	REQUIRE_FALSE(CoerceToDouble(nullptr, result));
	REQUIRE(result == 123); // untouched on failure
}

// =============================================================================
// CoerceToBoolean — Checkbox
// =============================================================================

TEST_CASE("Checkbox values coerce to boolean", "[value_coercion]") {
	bool result = false;
	JsonDocument yes("true");
	REQUIRE(CoerceToBoolean(yes.Root(), result));
	REQUIRE(result);

	JsonDocument no("false");
	REQUIRE(CoerceToBoolean(no.Root(), result));
	REQUIRE_FALSE(result);

	JsonDocument one("1");
	REQUIRE(CoerceToBoolean(one.Root(), result));
	REQUIRE(result);

	JsonDocument spelled(R"("TRUE")");
	REQUIRE(CoerceToBoolean(spelled.Root(), result));
	REQUIRE(result);

	JsonDocument spelled_false(R"("False")");
	REQUIRE(CoerceToBoolean(spelled_false.Root(), result));
	REQUIRE_FALSE(result);
}

TEST_CASE("Unrepresentable checkbox values become SQL NULL", "[value_coercion]") {
	bool result = true;
	JsonDocument maybe(R"("maybe")");
	REQUIRE_FALSE(CoerceToBoolean(maybe.Root(), result));

	JsonDocument null_value("null");
	REQUIRE_FALSE(CoerceToBoolean(null_value.Root(), result));

	REQUIRE_FALSE(CoerceToBoolean(nullptr, result));
}

// =============================================================================
// CoerceToTimestampMicros — DateTime, CreatedTime, ModifiedTime
// =============================================================================

TEST_CASE("Millisecond epochs become microsecond timestamps", "[value_coercion]") {
	int64_t result = 0;
	JsonDocument millis("1700000000000");
	REQUIRE(CoerceToTimestampMicros(millis.Root(), result));
	REQUIRE(result == 1700000000000000LL);

	JsonDocument epoch("0");
	REQUIRE(CoerceToTimestampMicros(epoch.Root(), result));
	REQUIRE(result == 0);

	JsonDocument before_epoch("-1000");
	REQUIRE(CoerceToTimestampMicros(before_epoch.Root(), result));
	REQUIRE(result == -1000000);

	JsonDocument as_string(R"("1700000000000")");
	REQUIRE(CoerceToTimestampMicros(as_string.Root(), result));
	REQUIRE(result == 1700000000000000LL);
}

TEST_CASE("Timestamps that would overflow become SQL NULL", "[value_coercion]") {
	int64_t result = 7;
	JsonDocument too_large("9223372036854775807");
	REQUIRE_FALSE(CoerceToTimestampMicros(too_large.Root(), result));

	JsonDocument text(R"("not a date")");
	REQUIRE_FALSE(CoerceToTimestampMicros(text.Root(), result));

	// Epoch milliseconds are integral by contract. Never silently truncate a
	// fractional JSON number or numeric string into a different instant.
	JsonDocument fractional_number("1700000000000.5");
	REQUIRE_FALSE(CoerceToTimestampMicros(fractional_number.Root(), result));

	JsonDocument fractional_string(R"("1700000000000.5")");
	REQUIRE_FALSE(CoerceToTimestampMicros(fractional_string.Root(), result));

	JsonDocument null_value("null");
	REQUIRE_FALSE(CoerceToTimestampMicros(null_value.Root(), result));

	REQUIRE_FALSE(CoerceToTimestampMicros(nullptr, result));
	REQUIRE(result == 7); // untouched on failure
}

// =============================================================================
// CoerceToStringList — MultiSelect
// =============================================================================

TEST_CASE("MultiSelect arrays map element-wise", "[value_coercion]") {
	bool ok = false;
	auto entries = List(R"(["red","green","blue"])", ok);
	REQUIRE(ok);
	REQUIRE(entries.size() == 3);
	REQUIRE(entries[0].value == "red");
	REQUIRE(entries[1].value == "green");
	REQUIRE(entries[2].value == "blue");
	REQUIRE_FALSE(entries[0].is_null);
}

TEST_CASE("An empty MultiSelect is an empty list, not NULL", "[value_coercion]") {
	bool ok = false;
	auto entries = List("[]", ok);
	REQUIRE(ok);
	REQUIRE(entries.empty());
}

TEST_CASE("Option objects contribute their label", "[value_coercion]") {
	bool ok = false;
	auto entries = List(R"([{"id":"opt1","text":"Red"},{"id":"opt2","name":"Green"}])", ok);
	REQUIRE(ok);
	REQUIRE(entries.size() == 2);
	REQUIRE(entries[0].value == "Red");
	REQUIRE(entries[1].value == "Green");
}

TEST_CASE("Null elements stay NULL list entries", "[value_coercion]") {
	bool ok = false;
	auto entries = List(R"(["red",null,"blue"])", ok);
	REQUIRE(ok);
	REQUIRE(entries.size() == 3);
	REQUIRE_FALSE(entries[0].is_null);
	REQUIRE(entries[1].is_null);
	REQUIRE(entries[1].value.empty());
	REQUIRE_FALSE(entries[2].is_null);
}

TEST_CASE("A single scalar still forms a one element list", "[value_coercion]") {
	bool ok = false;
	auto entries = List(R"("red")", ok);
	REQUIRE(ok);
	REQUIRE(entries.size() == 1);
	REQUIRE(entries[0].value == "red");
}

TEST_CASE("An absent MultiSelect value becomes SQL NULL", "[value_coercion]") {
	bool ok = true;
	auto entries = List("null", ok);
	REQUIRE_FALSE(ok);
	REQUIRE(entries.empty());

	std::vector<CoercedListEntry> from_nullptr;
	REQUIRE_FALSE(CoerceToStringList(nullptr, from_nullptr));
	REQUIRE(from_nullptr.empty());
}
