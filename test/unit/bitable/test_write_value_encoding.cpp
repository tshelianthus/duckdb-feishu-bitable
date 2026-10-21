#include "catch.hpp"

#include "bitable/field_type.hpp"
#include "bitable/write_value_encoding.hpp"

#include <limits>
#include <locale>
#include <string>
#include <vector>

using duckdb::idx_t;
using duckdb::InvalidInputException;
using duckdb::feishu::BuildRecordsBatchCreateUrl;
using duckdb::feishu::ClassifyWriteTarget;
using duckdb::feishu::EncodeFieldsEntry;
using duckdb::feishu::EncodeJsonString;
using duckdb::feishu::EncodeWriteValue;
using duckdb::feishu::FEISHU_MAX_TIMESTAMP_MILLIS;
using duckdb::feishu::FeishuFieldTypeId;
using duckdb::feishu::PercentEncodeUrlComponent;
using duckdb::feishu::WriteBatch;
using duckdb::feishu::WriteBatchAccumulator;
using duckdb::feishu::WriteScalar;
using duckdb::feishu::WriteTargetKind;

namespace {

std::string Encode(const WriteScalar &value, WriteTargetKind target) {
	std::string out;
	REQUIRE(EncodeWriteValue(value, target, out));
	return out;
}

class CommaDecimalPoint : public std::numpunct<char> {
protected:
	char do_decimal_point() const override {
		return ',';
	}
};

class GlobalLocaleGuard {
public:
	GlobalLocaleGuard() : original(std::locale()) {
	}
	~GlobalLocaleGuard() {
		std::locale::global(original);
	}

private:
	std::locale original;
};

} // namespace

TEST_CASE("write encoding: section 8.3 mapping table", "[write_encoding]") {
	// Text-like fields accept VARCHAR, including the empty string.
	CHECK(Encode(WriteScalar::Text("hello"), WriteTargetKind::TEXT) == "\"hello\"");
	CHECK(Encode(WriteScalar::Text(""), WriteTargetKind::TEXT) == "\"\"");
	// Numbers keep their exact double representation.
	CHECK(Encode(WriteScalar::Number(42.5), WriteTargetKind::NUMBER) == "42.5");
	CHECK(Encode(WriteScalar::Number(1000), WriteTargetKind::NUMBER) == "1000");
	CHECK(Encode(WriteScalar::Number(-0.125), WriteTargetKind::NUMBER) == "-0.125");
	// Integers are emitted exactly, but only if conversion to DOUBLE is lossless.
	CHECK(Encode(WriteScalar::Integer("9007199254740992"), WriteTargetKind::NUMBER) == "9007199254740992");
	CHECK(Encode(WriteScalar::Integer("1267650600228229401496703205376"), WriteTargetKind::NUMBER) ==
	      "1267650600228229401496703205376"); // 2^100, an exactly representable HUGEINT.
	// Checkbox becomes a JSON boolean, never a number.
	CHECK(Encode(WriteScalar::Boolean(true), WriteTargetKind::CHECKBOX) == "true");
	CHECK(Encode(WriteScalar::Boolean(false), WriteTargetKind::CHECKBOX) == "false");
	// Date fields take UTC microseconds and emit milliseconds.
	CHECK(Encode(WriteScalar::TimestampMicros(1700000000123456LL), WriteTargetKind::DATE_TIME) == "1700000000123");
	CHECK(Encode(WriteScalar::TimestampMicros(0), WriteTargetKind::DATE_TIME) == "0");
	// Multi-select becomes an array of strings.
	CHECK(Encode(WriteScalar::List({"a", "b"}, {false, false}), WriteTargetKind::MULTI_SELECT) == "[\"a\",\"b\"]");
}

TEST_CASE("write encoding: SQL NULL omits the field", "[write_encoding]") {
	std::string out = "stale";
	const WriteTargetKind targets[] = {
	    WriteTargetKind::TEXT,         WriteTargetKind::NUMBER, WriteTargetKind::CHECKBOX, WriteTargetKind::DATE_TIME,
	    WriteTargetKind::MULTI_SELECT, WriteTargetKind::LINK,   WriteTargetKind::USER};
	for (auto target : targets) {
		CHECK_FALSE(EncodeWriteValue(WriteScalar::Null(), target, out));
		CHECK(out.empty());
		out = "stale";
	}
	CHECK(EncodeFieldsEntry("name", WriteScalar::Null(), WriteTargetKind::TEXT).empty());
}

TEST_CASE("write encoding: integers must convert to DOUBLE losslessly", "[write_encoding]") {
	std::string out;
	CHECK(Encode(WriteScalar::Integer("-42"), WriteTargetKind::NUMBER) == "-42");
	CHECK_THROWS_AS(EncodeWriteValue(WriteScalar::Integer("9007199254740993"), WriteTargetKind::NUMBER, out),
	                InvalidInputException);
	CHECK_THROWS_AS(
	    EncodeWriteValue(WriteScalar::Integer("170141183460469231731687303715884105727"), WriteTargetKind::NUMBER, out),
	    InvalidInputException); // DuckDB HUGEINT max is finite as DOUBLE, but not exact.
	CHECK_THROWS_AS(EncodeWriteValue(WriteScalar::Integer("01"), WriteTargetKind::NUMBER, out), InvalidInputException);
	CHECK_THROWS_AS(EncodeWriteValue(WriteScalar::Integer("1.0"), WriteTargetKind::NUMBER, out), InvalidInputException);
}

TEST_CASE("write encoding: JSON numbers ignore the process locale", "[write_encoding]") {
	GlobalLocaleGuard guard;
	std::locale::global(std::locale(std::locale::classic(), new CommaDecimalPoint));
	CHECK(Encode(WriteScalar::Number(42.5), WriteTargetKind::NUMBER) == "42.5");
}

TEST_CASE("write encoding: empty string is text-only", "[write_encoding]") {
	CHECK(Encode(WriteScalar::Text(""), WriteTargetKind::TEXT) == "\"\"");
	std::string out;
	CHECK_THROWS_AS(EncodeWriteValue(WriteScalar::Text(""), WriteTargetKind::NUMBER, out), InvalidInputException);
	CHECK_THROWS_AS(EncodeWriteValue(WriteScalar::Text(""), WriteTargetKind::CHECKBOX, out), InvalidInputException);
	CHECK_THROWS_AS(EncodeWriteValue(WriteScalar::Text(""), WriteTargetKind::DATE_TIME, out), InvalidInputException);
}

TEST_CASE("write encoding: list rules", "[write_encoding]") {
	// An empty list clears the multi-select field; a NULL element is rejected.
	CHECK(Encode(WriteScalar::List({}, {}), WriteTargetKind::MULTI_SELECT) == "[]");
	// MULTI_SELECT still permits empty-string elements (section 8.4 asymmetry).
	CHECK(Encode(WriteScalar::List({""}, {false}), WriteTargetKind::MULTI_SELECT) == "[\"\"]");
	std::string out;
	CHECK_THROWS_AS(EncodeWriteValue(WriteScalar::List({"x"}, {true}), WriteTargetKind::MULTI_SELECT, out),
	                InvalidInputException);
	CHECK_THROWS_AS(EncodeWriteValue(WriteScalar::List({"a", "b"}, {false}), WriteTargetKind::MULTI_SELECT, out),
	                InvalidInputException);
}

TEST_CASE("write encoding: LINK and USER wire shapes", "[write_encoding]") {
	CHECK(Encode(WriteScalar::List({"recXXX"}, {false}), WriteTargetKind::LINK) == "[\"recXXX\"]");
	CHECK(Encode(WriteScalar::List({"recA", "recB"}, {false, false}), WriteTargetKind::LINK) == "[\"recA\",\"recB\"]");
	CHECK(Encode(WriteScalar::List({}, {}), WriteTargetKind::LINK) == "[]");
	CHECK(Encode(WriteScalar::List({"ou_abc"}, {false}), WriteTargetKind::USER) == "[{\"id\":\"ou_abc\"}]");
	CHECK(Encode(WriteScalar::List({"ou_a", "ou_b"}, {false, false}), WriteTargetKind::USER) ==
	      "[{\"id\":\"ou_a\"},{\"id\":\"ou_b\"}]");
	CHECK(Encode(WriteScalar::List({}, {}), WriteTargetKind::USER) == "[]");

	std::string out;
	CHECK_THROWS_AS(EncodeWriteValue(WriteScalar::List({"x"}, {true}), WriteTargetKind::LINK, out),
	                InvalidInputException);
	CHECK_THROWS_AS(EncodeWriteValue(WriteScalar::List({""}, {false}), WriteTargetKind::LINK, out),
	                InvalidInputException);
	CHECK_THROWS_AS(EncodeWriteValue(WriteScalar::List({"x"}, {true}), WriteTargetKind::USER, out),
	                InvalidInputException);
	CHECK_THROWS_AS(EncodeWriteValue(WriteScalar::List({""}, {false}), WriteTargetKind::USER, out),
	                InvalidInputException);
	CHECK_THROWS_AS(EncodeWriteValue(WriteScalar::Text("recXXX"), WriteTargetKind::LINK, out), InvalidInputException);
	CHECK_THROWS_AS(EncodeWriteValue(WriteScalar::Text("ou_abc"), WriteTargetKind::USER, out), InvalidInputException);
}

TEST_CASE("write encoding: batch_create URL always carries user_id_type", "[write_encoding]") {
	CHECK(BuildRecordsBatchCreateUrl("https://open.feishu.cn/open-apis/bitable/v1", "basApp", "tblTable", "open_id") ==
	      "https://open.feishu.cn/open-apis/bitable/v1/apps/basApp/tables/tblTable/"
	      "records/batch_create?user_id_type=open_id");
	CHECK(BuildRecordsBatchCreateUrl("https://open.feishu.cn/open-apis/bitable/v1", "basApp", "tblTable", "union_id") ==
	      "https://open.feishu.cn/open-apis/bitable/v1/apps/basApp/tables/tblTable/"
	      "records/batch_create?user_id_type=union_id");
	CHECK(BuildRecordsBatchCreateUrl("https://open.feishu.cn/open-apis/bitable/v1", "basApp", "tblTable", "user_id") ==
	      "https://open.feishu.cn/open-apis/bitable/v1/apps/basApp/tables/tblTable/"
	      "records/batch_create?user_id_type=user_id");
	// Path segments are percent-encoded; the three legal values need no escaping.
	CHECK(PercentEncodeUrlComponent("open_id") == "open_id");
	CHECK(
	    BuildRecordsBatchCreateUrl("https://open.larksuite.com/open-apis/bitable/v1", "bas/App", "tbl Table", "open_id")
	        .find("apps/bas%2FApp/tables/tbl%20Table/records/batch_create?user_id_type=open_id") != std::string::npos);
}

TEST_CASE("write encoding: NaN and infinities are rejected", "[write_encoding]") {
	std::string out;
	CHECK_THROWS_AS(
	    EncodeWriteValue(WriteScalar::Number(std::numeric_limits<double>::quiet_NaN()), WriteTargetKind::NUMBER, out),
	    InvalidInputException);
	CHECK_THROWS_AS(
	    EncodeWriteValue(WriteScalar::Number(std::numeric_limits<double>::infinity()), WriteTargetKind::NUMBER, out),
	    InvalidInputException);
	CHECK_THROWS_AS(
	    EncodeWriteValue(WriteScalar::Number(-std::numeric_limits<double>::infinity()), WriteTargetKind::NUMBER, out),
	    InvalidInputException);
}

TEST_CASE("write encoding: timestamp bounds", "[write_encoding]") {
	// The last millisecond Feishu can represent is accepted.
	CHECK(Encode(WriteScalar::TimestampMicros(FEISHU_MAX_TIMESTAMP_MILLIS * 1000), WriteTargetKind::DATE_TIME) ==
	      std::to_string(FEISHU_MAX_TIMESTAMP_MILLIS));
	CHECK(Encode(WriteScalar::TimestampMicros(FEISHU_MAX_TIMESTAMP_MILLIS * 1000 + 999), WriteTargetKind::DATE_TIME) ==
	      std::to_string(FEISHU_MAX_TIMESTAMP_MILLIS));
	std::string out;
	// One millisecond later, and any pre-epoch value, is rejected.
	CHECK_THROWS_AS(EncodeWriteValue(WriteScalar::TimestampMicros((FEISHU_MAX_TIMESTAMP_MILLIS + 1) * 1000),
	                                 WriteTargetKind::DATE_TIME, out),
	                InvalidInputException);
	CHECK_THROWS_AS(EncodeWriteValue(WriteScalar::TimestampMicros(-1), WriteTargetKind::DATE_TIME, out),
	                InvalidInputException);
}

TEST_CASE("write encoding: mismatched scalar kinds are rejected", "[write_encoding]") {
	std::string out;
	CHECK_THROWS_AS(EncodeWriteValue(WriteScalar::Number(1), WriteTargetKind::TEXT, out), InvalidInputException);
	CHECK_THROWS_AS(EncodeWriteValue(WriteScalar::Text("1"), WriteTargetKind::NUMBER, out), InvalidInputException);
	CHECK_THROWS_AS(EncodeWriteValue(WriteScalar::Boolean(true), WriteTargetKind::NUMBER, out), InvalidInputException);
	CHECK_THROWS_AS(EncodeWriteValue(WriteScalar::Number(1), WriteTargetKind::DATE_TIME, out), InvalidInputException);
	CHECK_THROWS_AS(EncodeWriteValue(WriteScalar::Text("a"), WriteTargetKind::MULTI_SELECT, out),
	                InvalidInputException);
	// Read-only / unsupported / unknown targets never reach the encoder.
	CHECK_THROWS_AS(EncodeWriteValue(WriteScalar::Text("a"), WriteTargetKind::READ_ONLY, out), InvalidInputException);
	CHECK_THROWS_AS(EncodeWriteValue(WriteScalar::Text("a"), WriteTargetKind::UNSUPPORTED, out), InvalidInputException);
	CHECK_THROWS_AS(EncodeWriteValue(WriteScalar::Text("a"), WriteTargetKind::UNKNOWN, out), InvalidInputException);
	// Invalid target metadata is rejected before the SQL NULL omission rule.
	CHECK_THROWS_AS(EncodeWriteValue(WriteScalar::Null(), WriteTargetKind::READ_ONLY, out), InvalidInputException);
	CHECK_THROWS_AS(EncodeWriteValue(WriteScalar::Null(), WriteTargetKind::UNSUPPORTED, out), InvalidInputException);
	CHECK_THROWS_WITH(EncodeWriteValue(WriteScalar::Null(), WriteTargetKind::UNKNOWN, out),
	                  "feishu_bitable: refusing to encode an unknown Feishu field type");
}

TEST_CASE("write encoding: every published field type id is classified", "[write_encoding]") {
	using Id = FeishuFieldTypeId;
	const std::pair<int32_t, WriteTargetKind> expected[] = {
	    {static_cast<int32_t>(Id::TEXT), WriteTargetKind::TEXT},
	    {static_cast<int32_t>(Id::NUMBER), WriteTargetKind::NUMBER},
	    {static_cast<int32_t>(Id::SINGLE_SELECT), WriteTargetKind::TEXT},
	    {static_cast<int32_t>(Id::MULTI_SELECT), WriteTargetKind::MULTI_SELECT},
	    {static_cast<int32_t>(Id::DATE_TIME), WriteTargetKind::DATE_TIME},
	    {static_cast<int32_t>(Id::CHECKBOX), WriteTargetKind::CHECKBOX},
	    {static_cast<int32_t>(Id::USER), WriteTargetKind::USER},
	    {static_cast<int32_t>(Id::PHONE), WriteTargetKind::TEXT},
	    {static_cast<int32_t>(Id::URL), WriteTargetKind::URL},
	    {static_cast<int32_t>(Id::ATTACHMENT), WriteTargetKind::UNSUPPORTED},
	    {static_cast<int32_t>(Id::LINK), WriteTargetKind::LINK},
	    {static_cast<int32_t>(Id::FORMULA), WriteTargetKind::READ_ONLY},
	    {static_cast<int32_t>(Id::DUPLEX_LINK), WriteTargetKind::LINK},
	    {static_cast<int32_t>(Id::LOCATION), WriteTargetKind::UNSUPPORTED},
	    {static_cast<int32_t>(Id::GROUP_CHAT), WriteTargetKind::UNSUPPORTED},
	    {static_cast<int32_t>(Id::CREATED_TIME), WriteTargetKind::READ_ONLY},
	    {static_cast<int32_t>(Id::MODIFIED_TIME), WriteTargetKind::READ_ONLY},
	    {static_cast<int32_t>(Id::CREATED_USER), WriteTargetKind::READ_ONLY},
	    {static_cast<int32_t>(Id::MODIFIED_USER), WriteTargetKind::READ_ONLY},
	    {static_cast<int32_t>(Id::AUTO_NUMBER), WriteTargetKind::READ_ONLY},
	    {static_cast<int32_t>(Id::BARCODE), WriteTargetKind::TEXT},
	    {static_cast<int32_t>(Id::PROGRESS), WriteTargetKind::NUMBER},
	    {static_cast<int32_t>(Id::CURRENCY), WriteTargetKind::NUMBER},
	    {static_cast<int32_t>(Id::RATING), WriteTargetKind::NUMBER},
	    {static_cast<int32_t>(Id::EMAIL), WriteTargetKind::TEXT},
	};
	for (auto &entry : expected) {
		CHECK(ClassifyWriteTarget(entry.first) == entry.second);
	}
	// Ids outside the published enumeration are refused, not degraded.
	CHECK(ClassifyWriteTarget(424242) == WriteTargetKind::UNKNOWN);
	CHECK(ClassifyWriteTarget(-1) == WriteTargetKind::UNKNOWN);
}

TEST_CASE("write encoding: JSON string escaping", "[write_encoding]") {
	CHECK(EncodeJsonString("plain") == "\"plain\"");
	CHECK(EncodeJsonString("a\"b") == "\"a\\\"b\"");
	CHECK(EncodeJsonString("a\\b") == "\"a\\\\b\"");
	CHECK(EncodeJsonString("line\nbreak") == "\"line\\nbreak\"");
	CHECK(EncodeJsonString("back\bform\freturn\rtab\t") == "\"back\\bform\\freturn\\rtab\\t\"");
	CHECK(EncodeJsonString(std::string("bell\x07")) == "\"bell\\u0007\"");
	CHECK(EncodeJsonString(std::string("nul\0byte", 8)) == "\"nul\\u0000byte\"");
	CHECK(EncodeFieldsEntry("field name", WriteScalar::Text("v"), WriteTargetKind::TEXT) == "\"field name\":\"v\"");
	CHECK(Encode(WriteScalar::Text("https://example.invalid"), WriteTargetKind::URL) ==
	      "{\"link\":\"https://example.invalid\",\"text\":\"https://example.invalid\"}");
}

TEST_CASE("write batching: exact boundaries and tail flush", "[write_encoding][write_batch]") {
	WriteBatchAccumulator accumulator(3);
	WriteBatch batch;

	CHECK_FALSE(accumulator.Flush(batch));
	CHECK_FALSE(accumulator.AddRow("r0", batch));
	CHECK_FALSE(accumulator.AddRow("r1", batch));
	CHECK(accumulator.PendingRows() == 2);
	CHECK(accumulator.AddRow("r2", batch));
	CHECK(batch.first_row == 0);
	CHECK(batch.rows == std::vector<std::string>({"r0", "r1", "r2"}));
	CHECK(accumulator.PendingRows() == 0);

	CHECK_FALSE(accumulator.AddRow("r3", batch));
	CHECK_FALSE(accumulator.AddRow("r4", batch));
	CHECK(accumulator.Flush(batch));
	CHECK(batch.first_row == 3);
	CHECK(batch.rows == std::vector<std::string>({"r3", "r4"}));
	CHECK(accumulator.PendingRows() == 0);
	CHECK(accumulator.RowsSeen() == 5);
	CHECK_FALSE(accumulator.Flush(batch));
}

TEST_CASE("write batching: every full batch is emitted independently", "[write_encoding][write_batch]") {
	WriteBatchAccumulator accumulator(2);
	WriteBatch batch;
	for (idx_t i = 0; i < 4; i++) {
		CHECK(accumulator.AddRow("row" + std::to_string(i), batch) == (i % 2 == 1));
		if (i % 2 == 1) {
			CHECK(batch.first_row == i - 1);
			CHECK(batch.rows.size() == 2);
		}
	}
	CHECK_FALSE(accumulator.Flush(batch));
}

TEST_CASE("write batching: zero size is rejected", "[write_encoding][write_batch]") {
	CHECK_THROWS_AS(WriteBatchAccumulator(0), InvalidInputException);
}
