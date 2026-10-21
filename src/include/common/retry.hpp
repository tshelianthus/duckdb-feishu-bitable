#pragma once

#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"

#include <functional>
#include <string>

namespace duckdb {
namespace feishu {

//! Retry configuration for exponential backoff with jitter.
struct RetryPolicy {
	idx_t max_attempts = 5;
	idx_t initial_backoff_ms = 200;
	idx_t max_backoff_ms = 8000;
	//! If non-zero, caps the sleep derived from a server's Retry-After header.
	idx_t max_retry_after_ms = 60000;
};

//! Outcome of a single retryable operation.
enum class RetryResultType : uint8_t {
	SUCCESS,      // Operation succeeded; stop retrying.
	RETRYABLE,    // Transient failure; retry with exponential backoff.
	NON_RETRYABLE // Permanent failure; stop and surface the error.
};

//! Result returned by the operation passed to WithRetry.
struct RetryResult {
	RetryResultType type = RetryResultType::RETRYABLE;
	//! Optional server-specified retry delay (e.g. from Retry-After header).
	//! 0 means "use exponential backoff".
	idx_t retry_after_ms = 0;

	static RetryResult Success() {
		return RetryResult {RetryResultType::SUCCESS, 0};
	}
	static RetryResult Retryable(idx_t retry_after = 0) {
		return RetryResult {RetryResultType::RETRYABLE, retry_after};
	}
	static RetryResult NonRetryable() {
		return RetryResult {RetryResultType::NON_RETRYABLE, 0};
	}
};

//! Feishu rate-limiting business error code (HTTP 200 or 429 with {"code": 99991400}).
constexpr idx_t FEISHU_RATE_LIMIT_CODE = 99991400;

//! Returns whether an HTTP status is safe to retry.
bool IsRetryableHttpStatus(idx_t status_code);

//! Returns whether a Feishu business error code is retryable (rate limit).
bool IsRetryableFeishuCode(idx_t code);

//! Parses Retry-After as either delta-seconds or an IMF-fixdate. Invalid and
//! past values return 0. `now_epoch_seconds` is injectable for deterministic
//! tests; pass a negative value to use the current system time.
idx_t ParseRetryAfterMs(const string &value, int64_t now_epoch_seconds = -1);

//! Distinguishes a valid zero-delay Retry-After from a missing or malformed
//! header. The parsed delay is written to `result_ms` on success.
bool TryParseRetryAfterMs(const string &value, idx_t &result_ms, int64_t now_epoch_seconds = -1);

//! Returns the "full jitter" backoff for a one-based attempt number:
//!   random(0, min(max_backoff_ms, initial_backoff_ms * 2^(attempt - 1)))
//! Exposed so the growth curve and its ceiling can be asserted without sleeping.
idx_t ComputeJitterBackoff(idx_t attempt, const RetryPolicy &policy);

//! Returns the number of milliseconds SleepForRetry would sleep. A non-zero
//! `retry_after_ms` (server-provided Retry-After) wins over the computed
//! backoff and is capped by `policy.max_retry_after_ms`.
idx_t ComputeRetrySleepMs(const RetryPolicy &policy, idx_t attempt, idx_t retry_after_ms);

//! Sleeps according to the retry policy. A non-zero server delay is honored,
//! subject to max_retry_after_ms; otherwise exponential backoff with jitter is
//! used for the one-based attempt number.
void SleepForRetry(const RetryPolicy &policy, idx_t attempt, idx_t retry_after_ms = 0);

//! Executes `fn` with exponential backoff + full jitter on retryable failures.
//! Retries HTTP 429, 5xx, and Feishu business code 99991400 (rate limit).
//! Honors a server-provided Retry-After header when `retry_after_ms > 0`.
//! Non-retryable failures and max-attempts exhaustion throw via the caller's
//! own exception (fn should throw for NON_RETRYABLE, or WithRetry throws
//! a generic error when retries are exhausted).
void WithRetry(const RetryPolicy &policy, const std::function<RetryResult()> &fn);

} // namespace feishu
} // namespace duckdb
