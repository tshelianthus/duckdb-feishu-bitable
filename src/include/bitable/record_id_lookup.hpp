#pragma once

#include "bitable/api_client.hpp"
#include "duckdb/common/string.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace duckdb {
namespace feishu {

//! How the scan should fetch Feishu records for the current query shape.
enum class RecordFetchMode : uint8_t {
	//! Paginated GET .../records (existing path).
	LIST_PAGES = 0,
	//! Single GET .../records/{record_id} (H-3).
	SINGLE_RECORD = 1
};

struct RecordFetchPlan {
	RecordFetchMode mode = RecordFetchMode::LIST_PAGES;
	string record_id;
};

//! DuckDB-independent description of a predicate, used by unit tests and by
//! the complex-filter adapter that classifies Expressions before planning.
enum class RecordIdPredicateKind : uint8_t {
	//! `record_id = '<non-empty>'`
	EQUALITY = 0,
	//! Any other predicate on `record_id` (inequality, IS NULL, IN, ...).
	NON_EQUALITY = 1,
	//! Predicate on any other column (or an unrecognised shape).
	OTHER = 2
};

struct RecordIdPredicate {
	RecordIdPredicateKind kind = RecordIdPredicateKind::OTHER;
	string equality_value;
};

//! Returns SINGLE_RECORD only when the sole predicate is a non-empty
//! `record_id` equality. Compound or non-equality shapes keep LIST_PAGES.
RecordFetchPlan PlanRecordFetch(const std::vector<RecordIdPredicate> &predicates);

//! Builds `GET /apps/{app_token}/tables/{table_id}/records/{record_id}`.
//! Optional `automatic_fields` mirrors the list-scan system-column flag.
//! Feishu's single-record endpoint does not accept `page_size` / `page_token` /
//! `view_id` / `field_names`; projection stays local.
string BuildSingleRecordUrl(const string &api_root, const string &app_token, const string &table_id,
                            const string &record_id, bool automatic_fields);

//! True when Feishu reports that the requested record does not exist. Callers
//! should emit zero rows (SQL filter semantics) instead of raising an error.
bool IsSingleRecordNotFound(uint64_t status_code, const string &response_body);

//! Extracts `data.record` from a successful single-record response. Throws on
//! malformed success payloads. Empty string when the record object is absent.
string ExtractSingleRecordJson(uint64_t status_code, const string &response_body);

//! Transport used by ExecuteRecordFetch so unit tests can count GETs without
//! opening sockets or linking the DuckDB runtime.
using RecordFetchGet = std::function<HttpResponse(const string &url)>;

//! Performs exactly one GET for the given plan URL. Single-record and list
//! first-page fetches share this entry point so tests can assert call counts.
HttpResponse ExecuteRecordFetch(const RecordFetchPlan &plan, const string &url, const RecordFetchGet &get);

} // namespace feishu
} // namespace duckdb
