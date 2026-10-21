#include "catch.hpp"

#include "bitable/record_id_lookup.hpp"
#include "bitable/url_parser.hpp"

#include "duckdb/common/exception.hpp"

#include <string>
#include <vector>

using duckdb::IOException;
using duckdb::feishu::BuildSingleRecordUrl;
using duckdb::feishu::ExecuteRecordFetch;
using duckdb::feishu::ExtractSingleRecordJson;
using duckdb::feishu::FEISHU_BITABLE_API_ROOT;
using duckdb::feishu::HttpResponse;
using duckdb::feishu::IsSingleRecordNotFound;
using duckdb::feishu::PlanRecordFetch;
using duckdb::feishu::RecordFetchMode;
using duckdb::feishu::RecordFetchPlan;
using duckdb::feishu::RecordIdPredicate;
using duckdb::feishu::RecordIdPredicateKind;

namespace {

struct CountingGetClient {
	idx_t get_count = 0;
	std::vector<std::string> urls;
	HttpResponse response;

	HttpResponse Get(const std::string &url) {
		get_count++;
		urls.push_back(url);
		return response;
	}
};

} // namespace

TEST_CASE("Sole record_id equality plans a single-record fetch", "[record_id_lookup]") {
	std::vector<RecordIdPredicate> predicates = {{RecordIdPredicateKind::EQUALITY, "recXXXXXXXX"}};
	auto plan = PlanRecordFetch(predicates);
	REQUIRE(plan.mode == RecordFetchMode::SINGLE_RECORD);
	REQUIRE(plan.record_id == "recXXXXXXXX");
}

TEST_CASE("Empty equality value keeps the list path", "[record_id_lookup]") {
	std::vector<RecordIdPredicate> predicates = {{RecordIdPredicateKind::EQUALITY, ""}};
	auto plan = PlanRecordFetch(predicates);
	REQUIRE(plan.mode == RecordFetchMode::LIST_PAGES);
	REQUIRE(plan.record_id.empty());
}

TEST_CASE("Non-equality record_id predicates keep the list path", "[record_id_lookup]") {
	std::vector<RecordIdPredicate> predicates = {{RecordIdPredicateKind::NON_EQUALITY, ""}};
	auto plan = PlanRecordFetch(predicates);
	REQUIRE(plan.mode == RecordFetchMode::LIST_PAGES);
}

TEST_CASE("Compound predicates keep the list path", "[record_id_lookup]") {
	std::vector<RecordIdPredicate> predicates = {{RecordIdPredicateKind::EQUALITY, "recXXXXXXXX"},
	                                             {RecordIdPredicateKind::OTHER, ""}};
	auto plan = PlanRecordFetch(predicates);
	REQUIRE(plan.mode == RecordFetchMode::LIST_PAGES);

	predicates = {{RecordIdPredicateKind::OTHER, ""}, {RecordIdPredicateKind::EQUALITY, "recXXXXXXXX"}};
	plan = PlanRecordFetch(predicates);
	REQUIRE(plan.mode == RecordFetchMode::LIST_PAGES);
}

TEST_CASE("Other-column-only predicates keep the list path", "[record_id_lookup]") {
	std::vector<RecordIdPredicate> predicates = {{RecordIdPredicateKind::OTHER, ""}};
	auto plan = PlanRecordFetch(predicates);
	REQUIRE(plan.mode == RecordFetchMode::LIST_PAGES);
}

TEST_CASE("No predicates keep the list path", "[record_id_lookup]") {
	auto plan = PlanRecordFetch({});
	REQUIRE(plan.mode == RecordFetchMode::LIST_PAGES);
}

TEST_CASE("Single-record URL targets records/{id} without pagination params", "[record_id_lookup]") {
	auto url = BuildSingleRecordUrl(FEISHU_BITABLE_API_ROOT, "bascnXXX", "tblYYY", "rec Z/1", true);
	REQUIRE(url.find("/apps/bascnXXX/tables/tblYYY/records/rec%20Z%2F1") != std::string::npos);
	REQUIRE(url.find("automatic_fields=true") != std::string::npos);
	REQUIRE(url.find("page_size") == std::string::npos);
	REQUIRE(url.find("page_token") == std::string::npos);
	REQUIRE(url.find("view_id") == std::string::npos);
	REQUIRE(url.find("field_names") == std::string::npos);
}

TEST_CASE("WHERE record_id equality issues exactly one ApiClient GET", "[record_id_lookup]") {
	CountingGetClient client;
	client.response.status_code = 200;
	client.response.body = R"({"code":0,"data":{"record":{"record_id":"recXXXXXXXX","fields":{"Name":"Ada"}}}})";

	RecordFetchPlan plan;
	plan.mode = RecordFetchMode::SINGLE_RECORD;
	plan.record_id = "recXXXXXXXX";
	auto url = BuildSingleRecordUrl(FEISHU_BITABLE_API_ROOT, "bascnXXX", "tblYYY", plan.record_id, true);

	auto response =
	    ExecuteRecordFetch(plan, url, [&](const std::string &request_url) { return client.Get(request_url); });

	REQUIRE(client.get_count == 1);
	REQUIRE(client.urls.size() == 1);
	REQUIRE(client.urls[0] == url);
	REQUIRE(client.urls[0].find("/records/recXXXXXXXX") != std::string::npos);
	REQUIRE(client.urls[0].find("page_token") == std::string::npos);
	REQUIRE(response.status_code == 200);

	auto record_json = ExtractSingleRecordJson(response.status_code, response.body);
	REQUIRE(record_json.find("\"record_id\":\"recXXXXXXXX\"") != std::string::npos);
	REQUIRE(record_json.find("Ada") != std::string::npos);
}

TEST_CASE("List-mode fetch also performs a single GET per page request", "[record_id_lookup]") {
	CountingGetClient client;
	client.response.status_code = 200;
	client.response.body = R"({"code":0,"data":{"items":[],"has_more":false}})";

	RecordFetchPlan plan;
	plan.mode = RecordFetchMode::LIST_PAGES;
	auto list_url = std::string(FEISHU_BITABLE_API_ROOT) + "/apps/bascnXXX/tables/tblYYY/records?page_size=500";
	auto response =
	    ExecuteRecordFetch(plan, list_url, [&](const std::string &request_url) { return client.Get(request_url); });

	REQUIRE(client.get_count == 1);
	REQUIRE(client.urls[0] == list_url);
	REQUIRE(response.body.find("has_more") != std::string::npos);
}

TEST_CASE("Missing single records are reported as not-found", "[record_id_lookup]") {
	REQUIRE(IsSingleRecordNotFound(404, "{}"));
	REQUIRE(IsSingleRecordNotFound(200, R"({"code":1254043,"msg":"RecordIdNotFound"})"));
	REQUIRE_FALSE(IsSingleRecordNotFound(200, R"({"code":0,"data":{"record":{}}})"));
	REQUIRE_FALSE(IsSingleRecordNotFound(500, R"({"code":1255001,"msg":"internal"})"));
}

TEST_CASE("ExtractSingleRecordJson rejects malformed success payloads", "[record_id_lookup]") {
	REQUIRE_THROWS_AS(ExtractSingleRecordJson(200, R"({"code":0,"data":"x"})"), IOException);
	REQUIRE_THROWS_AS(ExtractSingleRecordJson(200, R"({"code":0,"data":{"record":[]}})"), IOException);
}
