#pragma once

#include "bitable/api_client.hpp"
#include "duckdb/common/string.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace duckdb {
namespace feishu {

constexpr idx_t SEARCH_MAX_CONDITIONS = 50;
constexpr idx_t SEARCH_MAX_SORT_KEYS = 100;

//! Same defensive upper bound as write-side date encoding (9999-12-31T23:59:59.999Z).
constexpr int64_t SEARCH_MAX_TIMESTAMP_MILLIS = 253402300799999LL;

enum class SearchConjunction : uint8_t { AND = 0, OR = 1 };

enum class SearchOperator : uint8_t {
	IS = 0,
	IS_NOT = 1,
	IS_GREATER = 2,
	IS_GREATER_EQUAL = 3,
	IS_LESS = 4,
	IS_LESS_EQUAL = 5
};

enum class SearchScalarKind : uint8_t { TEXT = 0, NUMBER = 1, BOOLEAN = 2, TIMESTAMP_MICROS = 3 };

enum class SearchPushdownMode : uint8_t { NONE = 0, SEARCH = 1 };

struct SearchAtomicPredicate {
	string field_name;
	int32_t type_id = 0;
	SearchOperator op = SearchOperator::IS;
	SearchScalarKind value_kind = SearchScalarKind::TEXT;
	string text_value;
	double number_value = 0;
	bool bool_value = false;
	//! UTC microseconds since the epoch (DuckDB TIMESTAMP internal unit).
	int64_t timestamp_micros = 0;
};

struct SearchSortSpec {
	string field_name;
	int32_t type_id = 0;
	bool descending = false;
};

struct SearchCondition {
	string field_name;
	SearchOperator op = SearchOperator::IS;
	std::vector<string> values;
};

struct SearchSortKey {
	string field_name;
	bool descending = false;
};

struct SearchPushdownInput {
	bool view_id_set = false;
	SearchConjunction conjunction = SearchConjunction::AND;
	std::vector<SearchAtomicPredicate> predicates;
	std::vector<SearchSortSpec> sorts;
};

struct SearchPushdownPlan {
	SearchPushdownMode mode = SearchPushdownMode::NONE;
	SearchConjunction conjunction = SearchConjunction::AND;
	std::vector<SearchCondition> conditions;
	std::vector<SearchSortKey> sorts;
	string reject_reason;

	bool enabled() const {
		return mode == SearchPushdownMode::SEARCH;
	}

	bool Equals(const SearchPushdownPlan &other) const {
		if (mode != other.mode || conjunction != other.conjunction || conditions.size() != other.conditions.size() ||
		    sorts.size() != other.sorts.size()) {
			return false;
		}
		for (idx_t i = 0; i < conditions.size(); i++) {
			if (conditions[i].field_name != other.conditions[i].field_name ||
			    conditions[i].op != other.conditions[i].op || conditions[i].values != other.conditions[i].values) {
				return false;
			}
		}
		for (idx_t i = 0; i < sorts.size(); i++) {
			if (sorts[i].field_name != other.sorts[i].field_name || sorts[i].descending != other.sorts[i].descending) {
				return false;
			}
		}
		return true;
	}
};

bool IsSearchPushableFieldType(int32_t type_id);
//! I-1 live dual-path: only these field types sort like DuckDB's default ORDER BY.
bool IsSortEquivalentFieldType(int32_t type_id);
bool IsSearchPushableOperator(int32_t type_id, SearchOperator op);
bool TryEncodeSearchFilterValue(int32_t type_id, SearchScalarKind value_kind, const string &text_value,
                                double number_value, bool bool_value, int64_t timestamp_micros,
                                std::vector<string> &out_values);
SearchPushdownPlan PlanSearchPushdown(const SearchPushdownInput &input);
string BuildSearchUrl(const string &api_root, const string &app_token, const string &table_id, idx_t page_size,
                      const string &page_token);
string BuildSearchBody(const SearchPushdownPlan &plan, const std::vector<string> &field_names, bool automatic_fields);
const char *SearchOperatorWireName(SearchOperator op);
using SearchFetchPost = std::function<HttpResponse(const string &url, const string &body)>;
HttpResponse ExecuteSearchFetch(const string &url, const string &body, const SearchFetchPost &post);

} // namespace feishu
} // namespace duckdb
