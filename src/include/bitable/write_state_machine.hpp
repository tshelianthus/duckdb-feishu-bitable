#pragma once

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string.hpp"

#include <cstdint>
#include <functional>

namespace duckdb {
namespace feishu {

//! One append-only batch as the state machine sees it. Kept separate from the
//! encoding TU so this translation unit stays self-contained and unit testable.
struct WriteStateBatch {
	//! Zero-based index of the batch's first row in the COPY input.
	idx_t first_row = 0;
	idx_t row_count = 0;
};

//! Outcome of a single submission attempt.
enum class WriteAttemptResult : uint8_t {
	//! The remote confirmed this exact batch.
	CONFIRMED,
	//! The remote definitely did not commit; resending cannot duplicate data.
	RETRY_SAFE,
	//! A definite rejection; resending cannot succeed.
	FAIL,
	//! The request may or may not have landed. Never replay it.
	UNKNOWN_RESULT,
};

struct WriteAttemptOutcome {
	WriteAttemptResult result = WriteAttemptResult::UNKNOWN_RESULT;
	//! Redacted diagnostic detail. Transport code must not put credentials here.
	string detail;
	//! Server-provided Retry-After hint for RETRY_SAFE outcomes.
	idx_t retry_after_ms = 0;
};

//! Injected transport. `attempt` is 1-based.
using WriteAttemptFn = std::function<WriteAttemptOutcome(const WriteStateBatch &batch, idx_t attempt)>;
//! Injected back-off hook, so tests never sleep.
using WriteRetryHook = std::function<void(idx_t attempt, idx_t retry_after_ms)>;

//! Confirmed write progress. Only a CONFIRMED attempt ever advances it
//! (.specs/03_API_CONTRACT.md section 8.9, .specs/02_ARCHITECTURE.md 2.6).
struct WriteProgress {
	idx_t confirmed_batches = 0;
	idx_t confirmed_rows = 0;
};

struct WriteStateMachineOptions {
	idx_t max_attempts = 3;
};

//! Runs one batch through its retry loop.
//! - CONFIRMED advances the returned progress.
//! - RETRY_SAFE retries the same batch up to `max_attempts` (calling
//!   `on_retry` between attempts), then throws IOException: no remote write
//!   was confirmed, so the work can be re-run safely.
//! - FAIL throws InvalidInputException immediately.
//! - UNKNOWN_RESULT throws IOException immediately.
//! The transport is never called again after FAIL or UNKNOWN_RESULT, which is
//! what makes "no automatic replay after an ambiguous result" testable.
WriteProgress RunBatchWithRetries(const WriteStateBatch &batch, const WriteProgress &progress,
                                  const WriteAttemptFn &attempt_fn, const WriteRetryHook &on_retry,
                                  const WriteStateMachineOptions &options = WriteStateMachineOptions());

//! Reports a local row encoding failure after earlier batches may already
//! have committed. `row_number` is 1-based; `detail` must be redacted first.
string WriteRowFailureMessage(idx_t row_number, const WriteProgress &progress, const string &detail);

//! A failure before batch_create was attempted still needs the progress from
//! earlier confirmed batches, but cannot imply that this batch was submitted.
string WritePreflightFailureMessage(const WriteStateBatch &batch, const WriteProgress &progress, const string &detail);

//! Human-readable batch label used by every diagnostic, e.g.
//! "batch #3 (rows 1001-1500 is 1-based)". Exposed for tests.
string WriteBatchLabel(const WriteStateBatch &batch, idx_t batch_number);

} // namespace feishu
} // namespace duckdb
