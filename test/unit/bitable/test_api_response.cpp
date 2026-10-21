#include "catch.hpp"

#include "bitable/api_response.hpp"
#include "../../../src/bitable/read_error.hpp"

#include "duckdb/common/exception.hpp"

#include <unordered_set>

using duckdb::InvalidInputException;
using duckdb::IOException;
using duckdb::feishu::AdvanceMetadataPaginationCursor;
using duckdb::feishu::EnsureApiSuccess;
using duckdb::feishu::IsValidPaginationCursor;
using duckdb::feishu::JsonDocument;
using duckdb::feishu::ParseFieldsMetadataPage;
using duckdb::feishu::ParseFirstTableId;
using duckdb::feishu::ParseTablesMetadataPage;

namespace {

// Recorded shape of GET /open-apis/bitable/v1/apps/{app_token}/tables.
const char *const TWO_TABLES = R"({
  "code": 0,
  "msg": "success",
  "data": {
    "has_more": false,
    "page_token": "",
    "total": 2,
    "items": [
      {"table_id": "tblFirst01", "revision": 7, "name": "Sales"},
      {"table_id": "tblSecond2", "revision": 3, "name": "Costs"}
    ]
  }
})";

} // namespace

// =============================================================================
// ParseFirstTableId — the default first table fallback
// =============================================================================

TEST_CASE("ParseFirstTableId returns the first table of the base", "[api_response]") {
	REQUIRE(ParseFirstTableId(200, TWO_TABLES) == "tblFirst01");
}

TEST_CASE("ParseFirstTableId reports an empty base", "[api_response]") {
	auto empty_items = R"({"code": 0, "data": {"has_more": false, "items": []}})";
	auto null_items = R"({"code": 0, "data": {"has_more": false, "items": null}})";
	auto no_items = R"({"code": 0, "data": {"has_more": false}})";

	REQUIRE_THROWS_WITH(ParseFirstTableId(200, empty_items), "feishu_bitable: base contains no tables");
	REQUIRE_THROWS_WITH(ParseFirstTableId(200, null_items), "feishu_bitable: base contains no tables");
	REQUIRE_THROWS_WITH(ParseFirstTableId(200, no_items), "feishu_bitable: base contains no tables");
	REQUIRE_THROWS_AS(ParseFirstTableId(200, empty_items), InvalidInputException);
}

TEST_CASE("ParseFirstTableId rejects a first table without an id", "[api_response]") {
	auto missing_id = R"({"code": 0, "data": {"items": [{"name": "Sales"}], "has_more": false}})";

	REQUIRE_THROWS_WITH(ParseFirstTableId(200, missing_id), "feishu_bitable: base contains no tables");
}

TEST_CASE("First table fallback rejects malformed pagination instead of reporting an empty base",
          "[api_response][pagination]") {
	REQUIRE_THROWS_AS(ParseFirstTableId(200, R"({"code":0,"data":{"items":[]}})"), IOException);
	REQUIRE_THROWS_AS(ParseFirstTableId(200, R"({"code":0,"data":{"items":[],"has_more":true}})"), IOException);
	REQUIRE_THROWS_AS(ParseFirstTableId(200, R"({"code":0,"data":{"items":[{"table_id":"tblXXXX"}],"has_more":true}})"),
	                  IOException);
	REQUIRE_THROWS_AS(ParseFirstTableId(200, R"({"code":0,"data":{"items":[],"has_more":false,"page_token":5}})"),
	                  IOException);
}

TEST_CASE("JsonInt64 rejects numeric strings with trailing characters", "[api_response][metadata]") {
	JsonDocument malformed(R"("1junk")");
	int64_t value = 0;
	REQUIRE_FALSE(duckdb::feishu::JsonInt64(malformed.Root(), value));
	JsonDocument valid(R"("123")");
	REQUIRE(duckdb::feishu::JsonInt64(valid.Root(), value));
	REQUIRE(value == 123);
}

TEST_CASE("ParseFirstTableId surfaces Feishu business errors", "[api_response]") {
	auto denied = R"({"code": 91403, "msg": "Forbidden", "data": {}})";

	REQUIRE_THROWS_WITH(ParseFirstTableId(200, denied), "feishu_bitable: Feishu API error [code=91403]: Forbidden");
	REQUIRE_THROWS_AS(ParseFirstTableId(200, denied), InvalidInputException);
}

TEST_CASE("ParseFirstTableId includes business messages without leaking token-shaped text",
          "[api_response][security]") {
	auto denied = R"({"code": 91403, "msg": "Forbidden for cli_sensitive and t-sensitive-token", "data": {}})";

	try {
		ParseFirstTableId(200, denied);
		FAIL("ParseFirstTableId must throw");
	} catch (const InvalidInputException &error) {
		auto message = std::string(error.what());
		REQUIRE(message.find("Forbidden") != std::string::npos);
		REQUIRE(message.find("cli_sensitive") == std::string::npos);
		REQUIRE(message.find("t-sensitive-token") == std::string::npos);
	}
}

TEST_CASE("ParseFirstTableId maps HTTP failures onto the right exception type", "[api_response]") {
	REQUIRE_THROWS_AS(ParseFirstTableId(403, R"({"code": 0, "data": {}})"), InvalidInputException);
	REQUIRE_THROWS_WITH(ParseFirstTableId(403, R"({"code": 0, "data": {}})"),
	                    "feishu_bitable: Feishu API rejected the request (HTTP 403)");
	// Transient server failures are IO errors, not user input errors.
	REQUIRE_THROWS_AS(ParseFirstTableId(503, R"({"code": 0, "data": {}})"), IOException);
	REQUIRE_THROWS_WITH(ParseFirstTableId(503, R"({"code": 0, "data": {}})"),
	                    "feishu_bitable: Feishu API request failed (HTTP 503)");
}

TEST_CASE("ParseFirstTableId rejects malformed payloads without crashing", "[api_response]") {
	REQUIRE_THROWS_AS(ParseFirstTableId(200, "not json at all"), IOException);
	REQUIRE_THROWS_AS(ParseFirstTableId(200, "[]"), IOException);
	REQUIRE_THROWS_AS(ParseFirstTableId(200, R"({"data": {"items": []}})"), IOException);
	REQUIRE_THROWS_AS(ParseFirstTableId(200, R"({"code": 0, "data": {"items": 42}})"), IOException);
	REQUIRE_THROWS_AS(ParseFirstTableId(200, R"({"code": 0})"), IOException);
}

TEST_CASE("HTTP failures take precedence over malformed response bodies", "[api_response][review_regression]") {
	for (const auto &body : {"", "<html>access denied</html>", "not JSON"}) {
		for (auto status : {400, 401, 403, 404}) {
			CHECK_THROWS_AS(ParseFirstTableId(status, body), InvalidInputException);
			CHECK_THROWS_WITH(ParseFirstTableId(status, body), Catch::Contains("HTTP " + std::to_string(status)));
		}
		CHECK_THROWS_AS(ParseFirstTableId(503, body), IOException);
		CHECK_THROWS_WITH(ParseFirstTableId(503, body), Catch::Contains("HTTP 503"));
	}
}

TEST_CASE("JsonInt64 rejects a rounded INT64 overflow", "[api_response][security]") {
	REQUIRE_THROWS_AS(ParseFirstTableId(200, R"({"code":9.223372036854776e18,"data":{"items":[]}})"), IOException);
}

// =============================================================================
// EnsureApiSuccess
// =============================================================================

TEST_CASE("EnsureApiSuccess accepts any 2xx response carrying code 0", "[api_response]") {
	JsonDocument json(R"({"code": 0, "data": {}})");

	REQUIRE_NOTHROW(EnsureApiSuccess(200, json.Root()));
	REQUIRE_NOTHROW(EnsureApiSuccess(299, json.Root()));
}

TEST_CASE("EnsureApiSuccess requires a numeric code", "[api_response]") {
	JsonDocument json(R"({"code": "zero"})");

	REQUIRE_THROWS_AS(EnsureApiSuccess(200, json.Root()), IOException);
}

TEST_CASE("Business codes cannot be coerced into success", "[api_response][review_regression]") {
	for (const auto &body :
	     {R"({"code":0.5})", R"({"code":-0.5})", R"({"code":"0invalid"})", R"({"code":"0"})", R"({"code":false})"}) {
		JsonDocument json(body);
		CHECK_THROWS_AS(EnsureApiSuccess(200, json.Root()), IOException);
	}
}

TEST_CASE("JsonDocument rejects invalid JSON", "[api_response]") {
	REQUIRE_THROWS_AS(JsonDocument("{"), IOException);
}

TEST_CASE("Read API errors redact credential-shaped tokens and preserve business codes",
          "[api_response][security][review_regression]") {
	const std::string credential = "cli_app_id_XXXX";
	const std::string body = R"({"code":42,"msg":"rejected cli_app_id_XXXX","data":{}})";
	JsonDocument json(body);
	auto check_error = [&](const auto &invoke) {
		try {
			invoke();
			FAIL("a nonzero business code must fail");
		} catch (const InvalidInputException &error) {
			const std::string message = error.what();
			CHECK(message.find("42") != std::string::npos);
			CHECK(message.find(credential) == std::string::npos);
			CHECK(message.find("rejected") != std::string::npos);
		}
	};
	check_error([&]() { EnsureApiSuccess(200, json.Root()); });
	check_error([&]() { ParseFirstTableId(200, body); });
	check_error([&]() { ParseTablesMetadataPage(200, body); });
	check_error([&]() { ParseFieldsMetadataPage(200, body); });
}

// =============================================================================
// Metadata table/field pages — Phase 2.3 boundary behavior
// =============================================================================

TEST_CASE("Tables metadata parser preserves rows and nullable values", "[api_response][metadata]") {
	auto page = ParseTablesMetadataPage(
	    200,
	    R"({"code":0,"data":{"items":[{"table_id":"tbl1","name":"Sales","revision":7},{"name":null}],"has_more":false}})");

	REQUIRE_FALSE(page.has_more);
	REQUIRE(page.page_token.empty());
	REQUIRE(page.rows.size() == 2);
	REQUIRE(page.rows[0].table_id == "tbl1");
	REQUIRE(page.rows[0].name == "Sales");
	REQUIRE(page.rows[0].revision == 7);
	REQUIRE_FALSE(page.rows[1].table_id.has_value());
	REQUIRE_FALSE(page.rows[1].name.has_value());
	REQUIRE_FALSE(page.rows[1].revision.has_value());
}

TEST_CASE("Empty base shapes produce an empty tables result", "[api_response][metadata]") {
	for (auto body :
	     {R"({"code":0,"data":{"items":[],"has_more":false}})", R"({"code":0,"data":{"items":null,"has_more":false}})",
	      R"({"code":0,"data":{"has_more":false}})"}) {
		auto page = ParseTablesMetadataPage(200, body);
		REQUIRE(page.rows.empty());
		REQUIRE_FALSE(page.has_more);
	}
}

TEST_CASE("Fields metadata parser accepts an empty field collection", "[api_response][metadata]") {
	auto empty = ParseFieldsMetadataPage(200, R"({"code":0,"data":{"items":[],"has_more":false}})");
	auto missing = ParseFieldsMetadataPage(200, R"({"code":0,"data":{"has_more":false}})");

	REQUIRE(empty.rows.empty());
	REQUIRE(missing.rows.empty());
}

TEST_CASE("Fields metadata parser preserves field properties as JSON", "[api_response][metadata]") {
	auto page = ParseFieldsMetadataPage(
	    200,
	    R"({"code":0,"data":{"items":[{"field_id":"fld1","field_name":"Status","type":3,"is_primary":false,"description":"state","property":{"options":[{"name":"Open"}]}}],"has_more":false}})");

	REQUIRE(page.rows.size() == 1);
	REQUIRE(page.rows[0].field_id == "fld1");
	REQUIRE(page.rows[0].field_name == "Status");
	REQUIRE(page.rows[0].type_id == 3);
	REQUIRE(page.rows[0].is_primary == false);
	REQUIRE(page.rows[0].description == "state");
	REQUIRE(page.rows[0].property_json == R"({"options":[{"name":"Open"}]})");
}

TEST_CASE("Fields metadata parser accepts structured descriptions", "[api_response][metadata]") {
	auto body =
	    R"({"code":0,"data":{"items":[{"field_id":"fld1","field_name":"Name","type":1,"description":{"text":"Customer name","disable_sync":true}}],"has_more":false}})";
	auto page = ParseFieldsMetadataPage(200, body);
	REQUIRE(page.rows.size() == 1);
	REQUIRE(page.rows[0].description.value() == "Customer name");
}

TEST_CASE("Metadata parsers reject invalid JSON and malformed item collections", "[api_response][metadata]") {
	REQUIRE_THROWS_AS(ParseTablesMetadataPage(200, "not-json"), IOException);
	REQUIRE_THROWS_AS(ParseFieldsMetadataPage(200, "{"), IOException);
	REQUIRE_THROWS_AS(ParseTablesMetadataPage(200, R"({"code":0,"data":{"items":{},"has_more":false}})"), IOException);
	REQUIRE_THROWS_AS(ParseFieldsMetadataPage(200, R"({"code":0,"data":{"items":[1],"has_more":false}})"), IOException);
	REQUIRE_THROWS_AS(
	    ParseFieldsMetadataPage(200, R"({"code":0,"data":{"items":[{"type":"1junk"}],"has_more":false}})"),
	    IOException);
	REQUIRE_THROWS_AS(
	    ParseTablesMetadataPage(200, R"({"code":0,"data":{"items":[{"revision":"7"}],"has_more":false}})"),
	    IOException);
	REQUIRE_THROWS_AS(ParseFieldsMetadataPage(200, R"({"code":0,"data":{"items":[{"type":1.5}],"has_more":false}})"),
	                  IOException);
}

TEST_CASE("Every metadata page requires an explicit boolean has_more", "[api_response][metadata][pagination]") {
	REQUIRE_THROWS_WITH(ParseTablesMetadataPage(200, R"({"code":0,"data":{"items":[]}})"),
	                    "feishu_bitable_tables: malformed response: missing BOOLEAN 'has_more'");
	REQUIRE_THROWS_WITH(ParseFieldsMetadataPage(200, R"({"code":0,"data":{"items":[],"has_more":null}})"),
	                    "feishu_bitable_fields: malformed response: missing BOOLEAN 'has_more'");
	REQUIRE_THROWS_AS(ParseTablesMetadataPage(200, R"({"code":0,"data":{"has_more":"false"}})"), IOException);
}

TEST_CASE("Metadata pagination rejects missing and repeated cursors", "[api_response][metadata][pagination]") {
	std::string current;
	std::unordered_set<std::string> seen;

	REQUIRE_THROWS_WITH(AdvanceMetadataPaginationCursor("feishu_bitable_tables", true, "", current, seen),
	                    "feishu_bitable_tables: has_more is true but page_token is missing");
	REQUIRE_NOTHROW(AdvanceMetadataPaginationCursor("feishu_bitable_tables", true, "cursor-a", current, seen));
	REQUIRE(current == "cursor-a");
	REQUIRE_NOTHROW(AdvanceMetadataPaginationCursor("feishu_bitable_tables", true, "cursor-b", current, seen));
	REQUIRE(current == "cursor-b");
	REQUIRE_THROWS_WITH(AdvanceMetadataPaginationCursor("feishu_bitable_tables", true, "cursor-a", current, seen),
	                    "feishu_bitable_tables: repeated page_token returned by Feishu API");

	current = "field-cursor";
	seen = {"field-cursor"};
	REQUIRE_THROWS_WITH(AdvanceMetadataPaginationCursor("feishu_bitable_fields", true, "field-cursor", current, seen),
	                    "feishu_bitable_fields: repeated page_token returned by Feishu API");
}

TEST_CASE("Metadata pagination rejects a cursor repeated after many pages", "[api_response][metadata][pagination]") {
	std::string current;
	std::unordered_set<std::string> seen;
	for (int index = 0; index < 70; index++) {
		AdvanceMetadataPaginationCursor("feishu_bitable", true, "cursor-" + std::to_string(index), current, seen);
	}
	REQUIRE_THROWS_WITH(AdvanceMetadataPaginationCursor("feishu_bitable", true, "cursor-0", current, seen),
	                    "feishu_bitable: repeated page_token returned by Feishu API");
}

TEST_CASE("All three paginated endpoints obey the cursor validity matrix", "[api_response][pagination]") {
	// The records scan has its own response loop, while tables and fields use
	// AdvanceMetadataPaginationCursor. Exercise the protocol matrix for all
	// endpoint classes so a future change cannot silently relax one of them.
	for (const char *endpoint : {"tables", "fields", "records"}) {
		INFO("endpoint " << endpoint);
		REQUIRE(IsValidPaginationCursor(false, "", ""));
		REQUIRE(IsValidPaginationCursor(false, "", "cursor-a"));
		REQUIRE_FALSE(IsValidPaginationCursor(true, "", "cursor-a"));
		REQUIRE_FALSE(IsValidPaginationCursor(true, "cursor-a", "cursor-a"));
		REQUIRE(IsValidPaginationCursor(true, "cursor-b", "cursor-a"));

		std::string current;
		std::unordered_set<std::string> seen;
		REQUIRE_THROWS(AdvanceMetadataPaginationCursor(endpoint, true, "", current, seen));
		REQUIRE_NOTHROW(AdvanceMetadataPaginationCursor(endpoint, true, "cursor-a", current, seen));
		REQUIRE_THROWS(AdvanceMetadataPaginationCursor(endpoint, true, "cursor-a", current, seen));
		REQUIRE_NOTHROW(AdvanceMetadataPaginationCursor(endpoint, false, "", current, seen));
		REQUIRE(current.empty());
	}
}

TEST_CASE("Metadata parsers preserve a non-terminal cursor for the next request", "[api_response][pagination]") {
	auto tables =
	    ParseTablesMetadataPage(200, R"({"code":0,"data":{"items":[],"has_more":true,"page_token":"tbl-next"}})");
	auto fields =
	    ParseFieldsMetadataPage(200, R"({"code":0,"data":{"items":[],"has_more":true,"page_token":"fld-next"}})");

	REQUIRE(tables.has_more);
	REQUIRE(tables.page_token == "tbl-next");
	REQUIRE(fields.has_more);
	REQUIRE(fields.page_token == "fld-next");
}

TEST_CASE("Terminal metadata pages may omit page_token", "[api_response][metadata][pagination]") {
	std::string current = "cursor-a";
	std::unordered_set<std::string> seen = {"cursor-a"};

	REQUIRE_NOTHROW(AdvanceMetadataPaginationCursor("feishu_bitable_fields", false, "", current, seen));
	REQUIRE(current.empty());
}

TEST_CASE("Metadata access failures are actionable InvalidInput errors", "[api_response][metadata]") {
	REQUIRE_THROWS_WITH(ParseTablesMetadataPage(403, "not-json"),
	                    "feishu_bitable_tables: permission denied for base (HTTP 403)");
	REQUIRE_THROWS_WITH(ParseFieldsMetadataPage(404, "not-json"),
	                    "feishu_bitable_fields: table does not exist or is not accessible (HTTP 404)");
	REQUIRE_THROWS_WITH(ParseFieldsMetadataPage(403, "not-json"),
	                    "feishu_bitable_fields: permission denied for table (HTTP 403)");
	REQUIRE_THROWS_AS(ParseTablesMetadataPage(403, "not-json"), InvalidInputException);
	REQUIRE_THROWS_AS(ParseTablesMetadataPage(404, "not-json"), InvalidInputException);
	REQUIRE_THROWS_AS(ParseFieldsMetadataPage(404, "not-json"), InvalidInputException);
	REQUIRE_THROWS_AS(ParseFieldsMetadataPage(503, "not-json"), IOException);
}

TEST_CASE("Metadata business errors are InvalidInput errors", "[api_response][metadata]") {
	auto denied = R"({"code":1254302,"msg":"forbidden","data":{}})";

	REQUIRE_THROWS_AS(ParseTablesMetadataPage(200, denied), InvalidInputException);
	REQUIRE_THROWS_WITH(
	    ParseFieldsMetadataPage(200, denied),
	    "feishu_bitable_fields: Feishu API rejected the table metadata request (code 1254302): forbidden");
}

TEST_CASE("HTTP errors retain business diagnostics across read endpoints", "[api_response][read_contract]") {
	const std::string body = R"({"code":99991672,"msg":"permission denied for u-XXXX-token"})";
	for (auto status : {400, 403, 422, 429, 503}) {
		auto check = [&](const auto &invoke) {
			try {
				invoke();
				FAIL("HTTP failure must throw");
			} catch (const duckdb::Exception &error) {
				const std::string message = error.what();
				CHECK(message.find("HTTP " + std::to_string(status)) != std::string::npos);
				CHECK(message.find("99991672") != std::string::npos);
				CHECK(message.find("permission denied") != std::string::npos);
				CHECK(message.find("u-XXXX-token") == std::string::npos);
			}
		};
		JsonDocument json(body);
		check([&]() { EnsureApiSuccess(status, json.Root()); });
		check([&]() { ParseFirstTableId(status, body); });
		check([&]() { ParseTablesMetadataPage(status, body); });
		check([&]() { ParseFieldsMetadataPage(status, body); });
	}
}

TEST_CASE("Signed business codes are preserved in read diagnostics", "[api_response][read_contract]") {
	JsonDocument json(R"({"code":-1,"msg":"internal error"})");
	CHECK_THROWS_WITH(EnsureApiSuccess(200, json.Root()), Catch::Contains("code=-1"));
}

TEST_CASE("Read errors redact literal opaque tokens after JSON decoding", "[api_response][security][read_contract]") {
	const std::string token = "<opaque_XXXX>";
	const std::string body = R"({"code":42,"msg":"denied <opaque_\u0058XXX>"})";
	auto safe = duckdb::feishu::RedactReadErrorResponse(body, token);
	for (auto status : {200, 403}) {
		CHECK_THROWS_WITH(ParseFirstTableId(status, safe), Catch::Contains("denied"));
		try {
			ParseFirstTableId(status, safe);
			FAIL("business failure must throw");
		} catch (const InvalidInputException &error) {
			CHECK(std::string(error.what()).find(token) == std::string::npos);
		}
	}
	const std::string success = R"({"code":0,"msg":"<opaque_XXXX>","data":{"value":"<opaque_XXXX>"}})";
	CHECK(duckdb::feishu::RedactReadErrorResponse(success, token) == success);
	CHECK(duckdb::feishu::RedactReadErrorResponse("not JSON", token) == "not JSON");
}
