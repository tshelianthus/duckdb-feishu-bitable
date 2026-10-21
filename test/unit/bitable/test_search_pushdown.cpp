#include "catch.hpp"

#include "bitable/field_type.hpp"
#include "bitable/search_pushdown.hpp"
#include "bitable/url_parser.hpp"

#include <string>
#include <vector>

using duckdb::feishu::BuildSearchBody;
using duckdb::feishu::BuildSearchUrl;
using duckdb::feishu::ExecuteSearchFetch;
using duckdb::feishu::FEISHU_BITABLE_API_ROOT;
using duckdb::feishu::FeishuFieldTypeId;
using duckdb::feishu::HttpResponse;
using duckdb::feishu::IsSearchPushableFieldType;
using duckdb::feishu::IsSearchPushableOperator;
using duckdb::feishu::IsSortEquivalentFieldType;
using duckdb::feishu::PlanSearchPushdown;
using duckdb::feishu::SEARCH_MAX_CONDITIONS;
using duckdb::feishu::SEARCH_MAX_TIMESTAMP_MILLIS;
using duckdb::feishu::SearchAtomicPredicate;
using duckdb::feishu::SearchConjunction;
using duckdb::feishu::SearchOperator;
using duckdb::feishu::SearchPushdownInput;
using duckdb::feishu::SearchPushdownMode;
using duckdb::feishu::SearchScalarKind;
using duckdb::feishu::SearchSortSpec;

namespace {

SearchAtomicPredicate TextEq(const std::string &field, const std::string &value) {
	SearchAtomicPredicate pred;
	pred.field_name = field;
	pred.type_id = static_cast<int32_t>(FeishuFieldTypeId::TEXT);
	pred.op = SearchOperator::IS;
	pred.value_kind = SearchScalarKind::TEXT;
	pred.text_value = value;
	return pred;
}

SearchAtomicPredicate NumberCmp(const std::string &field, SearchOperator op, double value) {
	SearchAtomicPredicate pred;
	pred.field_name = field;
	pred.type_id = static_cast<int32_t>(FeishuFieldTypeId::NUMBER);
	pred.op = op;
	pred.value_kind = SearchScalarKind::NUMBER;
	pred.number_value = value;
	return pred;
}

SearchAtomicPredicate DateCmp(const std::string &field, int32_t type_id, SearchOperator op, int64_t micros) {
	SearchAtomicPredicate pred;
	pred.field_name = field;
	pred.type_id = type_id;
	pred.op = op;
	pred.value_kind = SearchScalarKind::TIMESTAMP_MICROS;
	pred.timestamp_micros = micros;
	return pred;
}

} // namespace

TEST_CASE("Search pushable field types match the H-1 first batch", "[search_pushdown]") {
	REQUIRE(IsSearchPushableFieldType(static_cast<int32_t>(FeishuFieldTypeId::TEXT)));
	REQUIRE(IsSearchPushableFieldType(static_cast<int32_t>(FeishuFieldTypeId::NUMBER)));
	REQUIRE(IsSearchPushableFieldType(static_cast<int32_t>(FeishuFieldTypeId::AUTO_NUMBER)));
	REQUIRE(IsSearchPushableFieldType(static_cast<int32_t>(FeishuFieldTypeId::DATE_TIME)));
	REQUIRE(IsSearchPushableFieldType(static_cast<int32_t>(FeishuFieldTypeId::CREATED_TIME)));
	REQUIRE(IsSearchPushableFieldType(static_cast<int32_t>(FeishuFieldTypeId::MODIFIED_TIME)));
	REQUIRE_FALSE(IsSearchPushableFieldType(static_cast<int32_t>(FeishuFieldTypeId::MULTI_SELECT)));
}

TEST_CASE("Numeric comparisons are pushable; text comparisons are not", "[search_pushdown]") {
	auto text = static_cast<int32_t>(FeishuFieldTypeId::TEXT);
	auto number = static_cast<int32_t>(FeishuFieldTypeId::NUMBER);
	REQUIRE(IsSearchPushableOperator(text, SearchOperator::IS));
	REQUIRE_FALSE(IsSearchPushableOperator(text, SearchOperator::IS_GREATER));
	REQUIRE(IsSearchPushableOperator(number, SearchOperator::IS_GREATER));
}

TEST_CASE("Date fields accept is/isGreater/isLess only", "[search_pushdown]") {
	auto date = static_cast<int32_t>(FeishuFieldTypeId::DATE_TIME);
	REQUIRE(IsSearchPushableOperator(date, SearchOperator::IS));
	REQUIRE(IsSearchPushableOperator(date, SearchOperator::IS_GREATER));
	REQUIRE(IsSearchPushableOperator(date, SearchOperator::IS_LESS));
	REQUIRE_FALSE(IsSearchPushableOperator(date, SearchOperator::IS_NOT));
	REQUIRE_FALSE(IsSearchPushableOperator(date, SearchOperator::IS_GREATER_EQUAL));
	REQUIRE_FALSE(IsSearchPushableOperator(date, SearchOperator::IS_LESS_EQUAL));
}

TEST_CASE("VIEW_ID refuses search pushdown", "[search_pushdown]") {
	SearchPushdownInput input;
	input.view_id_set = true;
	input.predicates.push_back(TextEq("Name", "Ada"));
	auto plan = PlanSearchPushdown(input);
	REQUIRE_FALSE(plan.enabled());
	REQUIRE(plan.reject_reason == "view_id_set");
}

TEST_CASE("Flat AND of pushable predicates plans SEARCH", "[search_pushdown]") {
	SearchPushdownInput input;
	input.conjunction = SearchConjunction::AND;
	input.predicates.push_back(TextEq("Name", "Ada"));
	input.predicates.push_back(NumberCmp("Amount", SearchOperator::IS_GREATER, 10));
	auto plan = PlanSearchPushdown(input);
	REQUIRE(plan.enabled());
	REQUIRE(plan.mode == SearchPushdownMode::SEARCH);
	REQUIRE(plan.conditions.size() == 2);
	REQUIRE(plan.conditions[1].op == SearchOperator::IS_GREATER);
	REQUIRE(plan.conditions[1].values[0] == "10");
}

TEST_CASE("Flat OR of pushable predicates plans SEARCH", "[search_pushdown]") {
	SearchPushdownInput input;
	input.conjunction = SearchConjunction::OR;
	input.predicates.push_back(TextEq("Status", "ready"));
	input.predicates.push_back(TextEq("Status", "done"));
	auto plan = PlanSearchPushdown(input);
	REQUIRE(plan.enabled());
	REQUIRE(plan.conjunction == SearchConjunction::OR);
}

TEST_CASE("51st condition refuses search pushdown", "[search_pushdown]") {
	SearchPushdownInput input;
	for (idx_t i = 0; i < SEARCH_MAX_CONDITIONS + 1; i++) {
		input.predicates.push_back(TextEq("Name", "x"));
	}
	auto plan = PlanSearchPushdown(input);
	REQUIRE_FALSE(plan.enabled());
	REQUIRE(plan.reject_reason == "too_many_conditions");
}

TEST_CASE("Date ExactDate equality encodes official two-element value", "[search_pushdown]") {
	SearchPushdownInput input;
	// 2020-01-01T00:00:00.000Z
	input.predicates.push_back(
	    DateCmp("When", static_cast<int32_t>(FeishuFieldTypeId::DATE_TIME), SearchOperator::IS, 1577836800000000LL));
	auto plan = PlanSearchPushdown(input);
	REQUIRE(plan.enabled());
	REQUIRE(plan.conditions.size() == 1);
	REQUIRE(plan.conditions[0].op == SearchOperator::IS);
	REQUIRE(plan.conditions[0].values.size() == 2);
	REQUIRE(plan.conditions[0].values[0] == "ExactDate");
	REQUIRE(plan.conditions[0].values[1] == "1577836800000");
	auto body = BuildSearchBody(plan, {"When"}, false);
	REQUIRE(body.find("\"operator\":\"is\"") != std::string::npos);
	REQUIRE(body.find("\"value\":[\"ExactDate\",\"1577836800000\"]") != std::string::npos);
}

TEST_CASE("Date isGreater and isLess encode ExactDate body", "[search_pushdown]") {
	SearchPushdownInput input;
	input.predicates.push_back(DateCmp("When", static_cast<int32_t>(FeishuFieldTypeId::DATE_TIME),
	                                   SearchOperator::IS_GREATER, 1577836800000000LL));
	input.predicates.push_back(DateCmp("Created", static_cast<int32_t>(FeishuFieldTypeId::CREATED_TIME),
	                                   SearchOperator::IS_LESS, 1609459200000000LL));
	auto plan = PlanSearchPushdown(input);
	REQUIRE(plan.enabled());
	REQUIRE(plan.conditions[0].op == SearchOperator::IS_GREATER);
	REQUIRE(plan.conditions[0].values[0] == "ExactDate");
	REQUIRE(plan.conditions[0].values[1] == "1577836800000");
	REQUIRE(plan.conditions[1].op == SearchOperator::IS_LESS);
	REQUIRE(plan.conditions[1].values[1] == "1609459200000");
	auto body = BuildSearchBody(plan, {"When", "Created"}, false);
	REQUIRE(body.find("\"operator\":\"isGreater\"") != std::string::npos);
	REQUIRE(body.find("\"operator\":\"isLess\"") != std::string::npos);
}

TEST_CASE("Date isNot / isGreaterEqual / isLessEqual refuse pushdown", "[search_pushdown]") {
	for (auto op : {SearchOperator::IS_NOT, SearchOperator::IS_GREATER_EQUAL, SearchOperator::IS_LESS_EQUAL}) {
		SearchPushdownInput input;
		input.predicates.push_back(
		    DateCmp("When", static_cast<int32_t>(FeishuFieldTypeId::DATE_TIME), op, 1577836800000000LL));
		auto plan = PlanSearchPushdown(input);
		REQUIRE_FALSE(plan.enabled());
		REQUIRE(plan.reject_reason == "unpushable_operator");
	}
}

TEST_CASE("Relative or text date values refuse pushdown", "[search_pushdown]") {
	SearchPushdownInput input;
	SearchAtomicPredicate pred;
	pred.field_name = "When";
	pred.type_id = static_cast<int32_t>(FeishuFieldTypeId::DATE_TIME);
	pred.op = SearchOperator::IS;
	pred.value_kind = SearchScalarKind::TEXT;
	pred.text_value = "Today";
	input.predicates.push_back(pred);
	auto plan = PlanSearchPushdown(input);
	REQUIRE_FALSE(plan.enabled());
	REQUIRE(plan.reject_reason == "unencodable_value");
}

TEST_CASE("Negative or out-of-range timestamps refuse date pushdown", "[search_pushdown]") {
	{
		SearchPushdownInput input;
		input.predicates.push_back(
		    DateCmp("When", static_cast<int32_t>(FeishuFieldTypeId::DATE_TIME), SearchOperator::IS, -1000));
		auto plan = PlanSearchPushdown(input);
		REQUIRE_FALSE(plan.enabled());
		REQUIRE(plan.reject_reason == "unencodable_value");
	}
	{
		SearchPushdownInput input;
		// Micros for millis just past SEARCH_MAX_TIMESTAMP_MILLIS.
		input.predicates.push_back(DateCmp("When", static_cast<int32_t>(FeishuFieldTypeId::DATE_TIME),
		                                   SearchOperator::IS, (SEARCH_MAX_TIMESTAMP_MILLIS + 1) * 1000));
		auto plan = PlanSearchPushdown(input);
		REQUIRE_FALSE(plan.enabled());
		REQUIRE(plan.reject_reason == "unencodable_value");
	}
}

TEST_CASE("Unpushable field type keeps the list path", "[search_pushdown]") {
	SearchPushdownInput input;
	SearchAtomicPredicate pred;
	pred.field_name = "Tags";
	pred.type_id = static_cast<int32_t>(FeishuFieldTypeId::MULTI_SELECT);
	pred.op = SearchOperator::IS;
	pred.value_kind = SearchScalarKind::TEXT;
	pred.text_value = "a";
	input.predicates.push_back(pred);
	auto plan = PlanSearchPushdown(input);
	REQUIRE_FALSE(plan.enabled());
}

TEST_CASE("Sort pushdown accepts only I-1 equivalent field types", "[search_pushdown]") {
	REQUIRE(IsSortEquivalentFieldType(static_cast<int32_t>(FeishuFieldTypeId::TEXT)));
	REQUIRE(IsSortEquivalentFieldType(static_cast<int32_t>(FeishuFieldTypeId::NUMBER)));
	REQUIRE(IsSortEquivalentFieldType(static_cast<int32_t>(FeishuFieldTypeId::DATE_TIME)));
	REQUIRE_FALSE(IsSortEquivalentFieldType(static_cast<int32_t>(FeishuFieldTypeId::SINGLE_SELECT)));
	REQUIRE_FALSE(IsSortEquivalentFieldType(static_cast<int32_t>(FeishuFieldTypeId::MULTI_SELECT)));
	REQUIRE_FALSE(IsSortEquivalentFieldType(static_cast<int32_t>(FeishuFieldTypeId::FORMULA)));
	REQUIRE_FALSE(IsSortEquivalentFieldType(static_cast<int32_t>(FeishuFieldTypeId::CHECKBOX)));
	REQUIRE_FALSE(IsSortEquivalentFieldType(static_cast<int32_t>(FeishuFieldTypeId::PHONE)));

	SearchPushdownInput text_only;
	SearchSortSpec text_sort;
	text_sort.field_name = "Name";
	text_sort.type_id = static_cast<int32_t>(FeishuFieldTypeId::TEXT);
	text_only.sorts.push_back(text_sort);
	auto text_plan = PlanSearchPushdown(text_only);
	REQUIRE(text_plan.enabled());
	REQUIRE(text_plan.conditions.empty());
	REQUIRE(text_plan.sorts.size() == 1);
	REQUIRE_FALSE(text_plan.sorts[0].descending);

	SearchPushdownInput rejected;
	SearchSortSpec select_sort;
	select_sort.field_name = "Status";
	select_sort.type_id = static_cast<int32_t>(FeishuFieldTypeId::SINGLE_SELECT);
	rejected.sorts.push_back(select_sort);
	auto rejected_plan = PlanSearchPushdown(rejected);
	REQUIRE_FALSE(rejected_plan.enabled());
	REQUIRE(rejected_plan.reject_reason == "unpushable_sort");
}

TEST_CASE("Sort keys are included when fields are pushable", "[search_pushdown]") {
	SearchPushdownInput input;
	input.predicates.push_back(TextEq("Name", "Ada"));
	SearchSortSpec sort;
	sort.field_name = "Amount";
	sort.type_id = static_cast<int32_t>(FeishuFieldTypeId::NUMBER);
	sort.descending = true;
	input.sorts.push_back(sort);
	auto plan = PlanSearchPushdown(input);
	REQUIRE(plan.enabled());
	REQUIRE(plan.sorts.size() == 1);
	REQUIRE(plan.sorts[0].descending);
}

TEST_CASE("Search URL targets records/search with pagination query params", "[search_pushdown]") {
	auto url = BuildSearchUrl(FEISHU_BITABLE_API_ROOT, "bascnXXX", "tblYYY", 500, "tok en/1");
	REQUIRE(url.find("/records/search?page_size=500") != std::string::npos);
	REQUIRE(url.find("page_token=tok%20en%2F1") != std::string::npos);
}

TEST_CASE("Search body contains expected filter, sort, and field_names", "[search_pushdown]") {
	SearchPushdownInput input;
	input.predicates.push_back(TextEq("Name", "Ada\"X"));
	input.predicates.push_back(NumberCmp("Amount", SearchOperator::IS_LESS_EQUAL, 3.5));
	SearchSortSpec sort;
	sort.field_name = "Name";
	sort.type_id = static_cast<int32_t>(FeishuFieldTypeId::TEXT);
	input.sorts.push_back(sort);
	auto plan = PlanSearchPushdown(input);
	auto body = BuildSearchBody(plan, {"Name", "Amount"}, true);
	REQUIRE(body.find("\"field_names\":[\"Name\",\"Amount\"]") != std::string::npos);
	REQUIRE(body.find("\"automatic_fields\":true") != std::string::npos);
	REQUIRE(body.find("\"operator\":\"is\"") != std::string::npos);
	REQUIRE(body.find("\"value\":[\"Ada\\\"X\"]") != std::string::npos);
	REQUIRE(body.find("\"operator\":\"isLessEqual\"") != std::string::npos);
	REQUIRE(body.find("\"sort\":[{\"field_name\":\"Name\",\"desc\":false}]") != std::string::npos);
	REQUIRE(body.find("view_id") == std::string::npos);
}

TEST_CASE("Zero-column min-field still appears in search body", "[search_pushdown]") {
	SearchPushdownInput input;
	input.predicates.push_back(TextEq("Name", "Ada"));
	auto plan = PlanSearchPushdown(input);
	auto body = BuildSearchBody(plan, {"Name"}, false);
	REQUIRE(body.find("\"field_names\":[\"Name\"]") != std::string::npos);
	REQUIRE(body.find("automatic_fields") == std::string::npos);
}

TEST_CASE("Checkbox encodes true/false strings", "[search_pushdown]") {
	SearchPushdownInput input;
	SearchAtomicPredicate pred;
	pred.field_name = "Done";
	pred.type_id = static_cast<int32_t>(FeishuFieldTypeId::CHECKBOX);
	pred.op = SearchOperator::IS;
	pred.value_kind = SearchScalarKind::BOOLEAN;
	pred.bool_value = true;
	input.predicates.push_back(pred);
	auto plan = PlanSearchPushdown(input);
	REQUIRE(plan.enabled());
	REQUIRE(plan.conditions[0].values[0] == "true");
}

TEST_CASE("Empty plan keeps list path", "[search_pushdown]") {
	SearchPushdownInput input;
	auto plan = PlanSearchPushdown(input);
	REQUIRE_FALSE(plan.enabled());
	REQUIRE(plan.reject_reason == "empty");
}

TEST_CASE("ExecuteSearchFetch issues exactly one POST", "[search_pushdown]") {
	idx_t post_count = 0;
	HttpResponse response;
	response.status_code = 200;
	response.body = R"({"code":0,"data":{"items":[],"has_more":false}})";
	SearchPushdownInput input;
	input.predicates.push_back(TextEq("Name", "Ada"));
	auto plan = PlanSearchPushdown(input);
	auto url = BuildSearchUrl(FEISHU_BITABLE_API_ROOT, "bascnXXX", "tblYYY", 100, "");
	auto body = BuildSearchBody(plan, {"Name"}, false);
	auto out = ExecuteSearchFetch(url, body, [&](const std::string &, const std::string &) {
		post_count++;
		return response;
	});
	REQUIRE(post_count == 1);
	REQUIRE(out.status_code == 200);
}
