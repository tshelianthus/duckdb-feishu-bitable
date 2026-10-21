#include "bitable/write_state_machine.hpp"

namespace duckdb {
namespace feishu {

namespace {

string PositiveNumber(idx_t value) {
	return std::to_string(value);
}

} // namespace

string WriteBatchLabel(const WriteStateBatch &batch, idx_t batch_number) {
	// Row ranges are reported 1-based: a batch holding the first row starts at 1.
	auto first = batch.first_row + 1;
	auto last = batch.first_row + batch.row_count;
	return "batch #" + PositiveNumber(batch_number) + " (rows " + PositiveNumber(first) + "-" + PositiveNumber(last) +
	       ")";
}

string WriteRowFailureMessage(idx_t row_number, const WriteProgress &progress, const string &detail) {
	return "feishu_bitable: row #" + PositiveNumber(row_number) + " could not be encoded after " +
	       PositiveNumber(progress.confirmed_batches) + " confirmed batches / " +
	       PositiveNumber(progress.confirmed_rows) + " rows; verify the target table before re-running: " + detail;
}

string WritePreflightFailureMessage(const WriteStateBatch &batch, const WriteProgress &progress, const string &detail) {
	return "feishu_bitable: " + WriteBatchLabel(batch, progress.confirmed_batches + 1) +
	       " could not be submitted after " + PositiveNumber(progress.confirmed_batches) + " confirmed batches / " +
	       PositiveNumber(progress.confirmed_rows) + " rows: " + detail;
}

WriteProgress RunBatchWithRetries(const WriteStateBatch &batch, const WriteProgress &progress,
                                  const WriteAttemptFn &attempt_fn, const WriteRetryHook &on_retry,
                                  const WriteStateMachineOptions &options) {
	if (!attempt_fn) {
		throw InvalidInputException("feishu_bitable: write state machine requires an attempt callback");
	}
	if (options.max_attempts == 0) {
		throw InvalidInputException("feishu_bitable: write state machine requires at least one attempt");
	}
	auto batch_number = progress.confirmed_batches + 1;
	auto label = WriteBatchLabel(batch, batch_number);
	auto confirmed = " after " + PositiveNumber(progress.confirmed_batches) + " confirmed batches / " +
	                 PositiveNumber(progress.confirmed_rows) + " rows";

	for (idx_t attempt = 1; attempt <= options.max_attempts; attempt++) {
		auto outcome = attempt_fn(batch, attempt);
		switch (outcome.result) {
		case WriteAttemptResult::CONFIRMED: {
			auto next = progress;
			next.confirmed_batches++;
			next.confirmed_rows += batch.row_count;
			return next;
		}
		case WriteAttemptResult::RETRY_SAFE:
			if (attempt == options.max_attempts) {
				throw IOException("feishu_bitable: " + label + " was rejected safely after exhausting retry attempts" +
				                  confirmed + "; no remote write was confirmed");
			}
			if (on_retry) {
				on_retry(attempt, outcome.retry_after_ms);
			}
			continue;
		case WriteAttemptResult::FAIL:
			throw InvalidInputException("feishu_bitable: " + label + " failed" + confirmed + ": " + outcome.detail);
		case WriteAttemptResult::UNKNOWN_RESULT:
			throw IOException("feishu_bitable: " + label + " has an unknown result" + confirmed +
			                  "; verify the target table before re-running: " + outcome.detail);
		}
	}
	throw IOException("feishu_bitable: write state machine terminated unexpectedly");
}

} // namespace feishu
} // namespace duckdb
