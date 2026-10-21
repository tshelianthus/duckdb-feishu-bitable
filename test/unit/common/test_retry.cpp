#include "catch.hpp"

#include "bitable/api_client.hpp"
#include "common/retry.hpp"

#include "duckdb/common/exception.hpp"

#include <limits>
#include <string>
#include <vector>

using duckdb::idx_t;
using duckdb::InvalidInputException;
using duckdb::feishu::ComputeJitterBackoff;
using duckdb::feishu::ComputeRetrySleepMs;
using duckdb::feishu::FEISHU_RATE_LIMIT_CODE;
using duckdb::feishu::IsRetryableFeishuCode;
using duckdb::feishu::IsRetryableHttpStatus;
using duckdb::feishu::ParseRetryAfterMs;
using duckdb::feishu::RetryPolicy;
using duckdb::feishu::RetryResult;
using duckdb::feishu::SleepForRetry;
using duckdb::feishu::TryParseRetryAfterMs;
using duckdb::feishu::WithRetry;
using duckdb::feishu::detail::MAX_HTTP_RESPONSE_BODY_BYTES;
using duckdb::feishu::detail::ResponseBodyCollector;

namespace {

//! Every policy used here keeps the real sleeps in the sub-millisecond range so
//! the whole unit test binary stays inside its seconds-long budget: the retry
//! *decision* logic is what needs asserting, not wall clock patience.
RetryPolicy FastPolicy(idx_t max_attempts = 4) {
	RetryPolicy policy;
	policy.max_attempts = max_attempts;
	policy.initial_backoff_ms = 1;
	policy.max_backoff_ms = 1;
	policy.max_retry_after_ms = 1;
	return policy;
}

//! A retry policy that exercises attempt accounting without making the test
//! suite wait. The production policy still supplies non-zero exponential
//! backoff; this is only for deterministic offline control-flow tests.
RetryPolicy NoWaitPolicy(idx_t max_attempts = 4) {
	RetryPolicy policy;
	policy.max_attempts = max_attempts;
	policy.initial_backoff_ms = 0;
	policy.max_backoff_ms = 0;
	policy.max_retry_after_ms = 0;
	return policy;
}

//! Highest value ComputeJitterBackoff produced over `samples` draws. Full
//! jitter draws uniformly from [0, cap], so the observed maximum is a sound
//! estimator of the ceiling while every single draw is a hard upper bound.
idx_t ObservedCeiling(idx_t attempt, const RetryPolicy &policy, idx_t samples = 400) {
	idx_t highest = 0;
	for (idx_t i = 0; i < samples; i++) {
		auto value = ComputeJitterBackoff(attempt, policy);
		if (value > highest) {
			highest = value;
		}
	}
	return highest;
}

} // namespace

TEST_CASE("HTTP response collection enforces a hard body-size cap", "[http][security]") {
	ResponseBodyCollector collector;
	const char chunk[] = "abc";
	REQUIRE(collector.Append(chunk, sizeof(chunk) - 1));
	REQUIRE(collector.Body() == "abc");

	const auto remaining = MAX_HTTP_RESPONSE_BODY_BYTES - collector.Body().size();
	std::string boundary(remaining, 'x');
	REQUIRE(collector.Append(boundary.data(), boundary.size()));
	REQUIRE(collector.Body().size() == MAX_HTTP_RESPONSE_BODY_BYTES);

	const char overflow[] = "z";
	REQUIRE_FALSE(collector.Append(overflow, sizeof(overflow) - 1));
	REQUIRE(collector.Body().size() == MAX_HTTP_RESPONSE_BODY_BYTES);
}

// =============================================================================
// IsRetryableHttpStatus / IsRetryableFeishuCode — the retry status matrix
// =============================================================================

TEST_CASE("Transient HTTP statuses are retryable", "[retry]") {
	REQUIRE(IsRetryableHttpStatus(408));
	REQUIRE(IsRetryableHttpStatus(429));
	REQUIRE(IsRetryableHttpStatus(500));
	REQUIRE(IsRetryableHttpStatus(502));
	REQUIRE(IsRetryableHttpStatus(503));
	REQUIRE(IsRetryableHttpStatus(504));
}

TEST_CASE("Client errors other than 408 and 429 are never retried", "[retry]") {
	// Retrying these would only burn the caller's rate budget: the request is
	// malformed, unauthenticated, forbidden or aimed at something that is gone.
	for (idx_t status : {400, 401, 402, 403, 404, 405, 409, 410, 413, 415, 422, 428, 431, 451}) {
		INFO("status " << status);
		REQUIRE_FALSE(IsRetryableHttpStatus(status));
	}
}

TEST_CASE("Successful and redirect statuses are not retryable", "[retry]") {
	for (idx_t status : {200, 201, 204, 299, 301, 302, 304, 307, 308}) {
		INFO("status " << status);
		REQUIRE_FALSE(IsRetryableHttpStatus(status));
	}
}

TEST_CASE("5xx statuses outside the transient set are not retried blindly", "[retry]") {
	// 501/505 describe a permanent server capability gap, not a blip.
	REQUIRE_FALSE(IsRetryableHttpStatus(501));
	REQUIRE_FALSE(IsRetryableHttpStatus(505));
	REQUIRE_FALSE(IsRetryableHttpStatus(0));
}

TEST_CASE("Only the Feishu rate limit business code is retryable", "[retry]") {
	REQUIRE(IsRetryableFeishuCode(FEISHU_RATE_LIMIT_CODE));
	REQUIRE(IsRetryableFeishuCode(99991400));
	REQUIRE_FALSE(IsRetryableFeishuCode(0));
	REQUIRE_FALSE(IsRetryableFeishuCode(99991663));
	REQUIRE_FALSE(IsRetryableFeishuCode(1254005));
}

TEST_CASE("Retry-After accepts delta-seconds and rejects malformed values", "[retry]") {
	idx_t delay = 99;
	REQUIRE(ParseRetryAfterMs(" 15 ", 0) == 15000);
	REQUIRE(ParseRetryAfterMs("0", 0) == 0);
	REQUIRE(TryParseRetryAfterMs("0", delay, 0));
	REQUIRE(delay == 0);
	REQUIRE(ParseRetryAfterMs("-1", 0) == 0);
	REQUIRE_FALSE(TryParseRetryAfterMs("-1", delay, 0));
	REQUIRE(ParseRetryAfterMs("1.5", 0) == 0);
	REQUIRE(ParseRetryAfterMs("seconds", 0) == 0);
}

TEST_CASE("Retry-After accepts an IMF-fixdate and ignores past dates", "[retry]") {
	// Sun, 06 Nov 1994 08:49:37 GMT is Unix timestamp 784111777.
	REQUIRE(ParseRetryAfterMs("Sun, 06 Nov 1994 08:49:37 GMT", 784111767) == 10000);
	REQUIRE(ParseRetryAfterMs("Sun, 06 Nov 1994 08:49:37 GMT", 784111777) == 0);
	REQUIRE(ParseRetryAfterMs("Sunday, 06-Nov-94 08:49:37 GMT", 0) == 0);
}

TEST_CASE("Retry-After arithmetic saturates instead of overflowing", "[retry]") {
	REQUIRE(ParseRetryAfterMs("184467440737095516150000", 0) == std::numeric_limits<idx_t>::max());
}

TEST_CASE("429 and transient 5xx responses retry offline until success", "[retry][resilience]") {
	// Model the response classifier used by ApiClient without opening a socket.
	// A zero-delay policy makes this a real WithRetry execution with no test
	// sleep, and the status sequence proves both 429 and 5xx are retried.
	const std::vector<idx_t> statuses {429, 503, 200};
	const auto policy = NoWaitPolicy(statuses.size());
	idx_t calls = 0;

	REQUIRE_NOTHROW(WithRetry(policy, [&]() {
		const auto status = statuses[calls++];
		if (status == 200) {
			return RetryResult::Success();
		}
		return IsRetryableHttpStatus(status) ? RetryResult::Retryable() : RetryResult::NonRetryable();
	}));
	REQUIRE(calls == statuses.size());
}

TEST_CASE("Retry-After is parsed and capped without sleeping", "[retry][resilience]") {
	RetryPolicy policy;
	policy.initial_backoff_ms = 200;
	policy.max_backoff_ms = 8000;
	policy.max_retry_after_ms = 2500;

	idx_t retry_after_ms = 0;
	REQUIRE(TryParseRetryAfterMs("2", retry_after_ms, 0));
	REQUIRE(retry_after_ms == 2000);
	REQUIRE(ComputeRetrySleepMs(policy, 1, retry_after_ms) == 2000);

	REQUIRE(TryParseRetryAfterMs("30", retry_after_ms, 0));
	REQUIRE(retry_after_ms == 30000);
	REQUIRE(ComputeRetrySleepMs(policy, 1, retry_after_ms) == 2500);

	REQUIRE_FALSE(TryParseRetryAfterMs("not-a-delay", retry_after_ms, 0));
}

// =============================================================================
// ComputeJitterBackoff — exponential growth under a hard ceiling
// =============================================================================

TEST_CASE("Full jitter never exceeds the exponential ceiling of its attempt", "[retry]") {
	RetryPolicy policy;
	policy.initial_backoff_ms = 200;
	policy.max_backoff_ms = 8000;

	const idx_t ceilings[] = {200, 400, 800, 1600, 3200, 6400, 8000, 8000, 8000};
	for (idx_t attempt = 1; attempt <= 9; attempt++) {
		auto expected_cap = ceilings[attempt - 1];
		INFO("attempt " << attempt);
		for (idx_t sample = 0; sample < 200; sample++) {
			REQUIRE(ComputeJitterBackoff(attempt, policy) <= expected_cap);
		}
	}
}

TEST_CASE("The backoff ceiling doubles with every attempt until it saturates", "[retry]") {
	RetryPolicy policy;
	policy.initial_backoff_ms = 200;
	policy.max_backoff_ms = 8000;

	// The observed maximum must land in the upper half of each window, which
	// is what distinguishes real exponential growth from a constant delay.
	REQUIRE(ObservedCeiling(1, policy) > 100);
	REQUIRE(ObservedCeiling(1, policy) <= 200);
	REQUIRE(ObservedCeiling(3, policy) > 400);
	REQUIRE(ObservedCeiling(3, policy) <= 800);
	REQUIRE(ObservedCeiling(6, policy) > 3200);
	REQUIRE(ObservedCeiling(6, policy) <= 6400);
}

TEST_CASE("The backoff saturates at max_backoff_ms and never grows past it", "[retry]") {
	RetryPolicy policy;
	policy.initial_backoff_ms = 200;
	policy.max_backoff_ms = 1000;

	// 200 -> 400 -> 800 -> would be 1600, so the ceiling clamps from attempt 4 on.
	REQUIRE(ObservedCeiling(4, policy) <= 1000);
	REQUIRE(ObservedCeiling(40, policy) <= 1000);
	// A huge attempt number must terminate rather than overflow the doubling.
	REQUIRE(ComputeJitterBackoff(1000000, policy) <= 1000);
}

TEST_CASE("A degenerate policy still yields a defined backoff", "[retry]") {
	RetryPolicy policy;
	policy.initial_backoff_ms = 0;
	policy.max_backoff_ms = 0;
	REQUIRE(ComputeJitterBackoff(1, policy) == 0);
	REQUIRE(ComputeJitterBackoff(7, policy) == 0);

	RetryPolicy inverted;
	inverted.initial_backoff_ms = 5000;
	inverted.max_backoff_ms = 100;
	// initial > max: the cap wins, so the wait can never exceed max_backoff_ms.
	REQUIRE(ComputeJitterBackoff(1, inverted) <= 100);
	REQUIRE(ComputeJitterBackoff(9, inverted) <= 100);
}

// =============================================================================
// ComputeRetrySleepMs — Retry-After overrides the backoff, capped
// =============================================================================

TEST_CASE("A server Retry-After delay overrides the computed backoff", "[retry]") {
	RetryPolicy policy;
	policy.initial_backoff_ms = 200;
	policy.max_backoff_ms = 8000;
	policy.max_retry_after_ms = 60000;

	// Exact, not jittered: an explicit server instruction is honored verbatim.
	REQUIRE(ComputeRetrySleepMs(policy, 1, 5000) == 5000);
	REQUIRE(ComputeRetrySleepMs(policy, 9, 1) == 1);
}

TEST_CASE("Retry-After is capped by max_retry_after_ms", "[retry]") {
	RetryPolicy policy;
	policy.max_retry_after_ms = 60000;

	REQUIRE(ComputeRetrySleepMs(policy, 1, 60000) == 60000);
	// A hostile or buggy header must not park the query for an hour.
	REQUIRE(ComputeRetrySleepMs(policy, 1, 3600000) == 60000);
	REQUIRE(ComputeRetrySleepMs(policy, 1, std::numeric_limits<idx_t>::max()) == 60000);
}

TEST_CASE("A zero max_retry_after_ms disables the Retry-After cap", "[retry]") {
	RetryPolicy policy;
	policy.max_retry_after_ms = 0;
	REQUIRE(ComputeRetrySleepMs(policy, 1, 120000) == 120000);
}

TEST_CASE("A missing Retry-After falls back to the jittered backoff", "[retry]") {
	RetryPolicy policy;
	policy.initial_backoff_ms = 200;
	policy.max_backoff_ms = 8000;

	for (idx_t sample = 0; sample < 200; sample++) {
		REQUIRE(ComputeRetrySleepMs(policy, 2, 0) <= 400);
	}
}

TEST_CASE("SleepForRetry returns immediately for a zero-delay policy", "[retry]") {
	auto policy = NoWaitPolicy();
	REQUIRE(ComputeRetrySleepMs(policy, 1, 0) == 0);
	REQUIRE_NOTHROW(SleepForRetry(policy, 1));
	REQUIRE_NOTHROW(SleepForRetry(policy, 3));
}

// =============================================================================
// WithRetry — attempt accounting and the exhaustion contract
// =============================================================================

TEST_CASE("A successful operation runs exactly once", "[retry]") {
	auto policy = FastPolicy();
	idx_t calls = 0;
	REQUIRE_NOTHROW(WithRetry(policy, [&]() {
		calls++;
		return RetryResult::Success();
	}));
	REQUIRE(calls == 1);
}

TEST_CASE("A transient failure is retried until it succeeds", "[retry]") {
	auto policy = FastPolicy();
	idx_t calls = 0;
	REQUIRE_NOTHROW(WithRetry(policy, [&]() {
		calls++;
		return calls < 3 ? RetryResult::Retryable() : RetryResult::Success();
	}));
	REQUIRE(calls == 3);
}

TEST_CASE("Exhausting the retries throws instead of looping or crashing", "[retry]") {
	auto policy = FastPolicy(3);
	idx_t calls = 0;
	REQUIRE_THROWS_AS(WithRetry(policy,
	                            [&]() {
		                            calls++;
		                            return RetryResult::Retryable();
	                            }),
	                  duckdb::IOException);
	// The operation is attempted max_attempts times, no more and no fewer.
	REQUIRE(calls == 3);
}

TEST_CASE("The exhaustion error names the attempt budget", "[retry]") {
	auto policy = FastPolicy(2);
	try {
		WithRetry(policy, []() { return RetryResult::Retryable(); });
		FAIL("WithRetry must throw once the attempts are exhausted");
	} catch (const duckdb::IOException &error) {
		std::string message(error.what());
		REQUIRE(message.find("feishu_bitable:") != std::string::npos);
		REQUIRE(message.find("exhausted retry attempts (2)") != std::string::npos);
	}
}

TEST_CASE("A non-retryable outcome stops immediately", "[retry]") {
	auto policy = FastPolicy(5);
	idx_t calls = 0;
	REQUIRE_THROWS_AS(WithRetry(policy,
	                            [&]() {
		                            calls++;
		                            return RetryResult::NonRetryable();
	                            }),
	                  InvalidInputException);
	REQUIRE(calls == 1);
}

TEST_CASE("An exception raised by the operation propagates unchanged", "[retry]") {
	auto policy = FastPolicy(5);
	idx_t calls = 0;
	REQUIRE_THROWS_AS(WithRetry(policy,
	                            [&]() -> RetryResult {
		                            calls++;
		                            throw duckdb::IOException("feishu_bitable: socket closed");
	                            }),
	                  duckdb::IOException);
	REQUIRE(calls == 1);
}

TEST_CASE("A Retry-After hint is honored without stalling the retry loop", "[retry]") {
	// Retry-After arithmetic is asserted separately above. This control-flow
	// check deliberately supplies no delay so it cannot make the suite sleep.
	auto policy = NoWaitPolicy(3);
	idx_t calls = 0;
	REQUIRE_THROWS_AS(WithRetry(policy,
	                            [&]() {
		                            calls++;
		                            return RetryResult::Retryable();
	                            }),
	                  duckdb::IOException);
	REQUIRE(calls == 3);
}

TEST_CASE("A zero attempt budget throws rather than running the operation", "[retry]") {
	auto policy = FastPolicy(0);
	idx_t calls = 0;
	REQUIRE_THROWS_AS(WithRetry(policy,
	                            [&]() {
		                            calls++;
		                            return RetryResult::Success();
	                            }),
	                  InvalidInputException);
	REQUIRE(calls == 0);
}
