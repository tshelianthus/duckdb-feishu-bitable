#include "common/error.hpp"
#include "common/retry.hpp"

#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <limits>
#include <locale>
#include <random>
#include <sstream>
#include <thread>

namespace duckdb {
namespace feishu {

namespace {

//! Thread-safe RNG for jitter. Per-thread to avoid contention.
std::mt19937 &Rng() {
	thread_local std::mt19937 rng([]() {
		std::random_device rd;
		auto seed = static_cast<uint64_t>(rd()) ^
		            static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
		return static_cast<uint32_t>(seed ^ (seed >> 32));
	}());
	return rng;
}

idx_t SecondsToMilliseconds(uint64_t seconds) {
	constexpr auto MAX_VALUE = std::numeric_limits<idx_t>::max();
	if (seconds > static_cast<uint64_t>(MAX_VALUE / 1000)) {
		return MAX_VALUE;
	}
	return static_cast<idx_t>(seconds * 1000);
}

time_t TimegmPortable(std::tm *value) {
#ifdef _WIN32
	return _mkgmtime(value);
#else
	return timegm(value);
#endif
}

} // namespace

//! Computes the sleep duration using "full jitter":
//!   sleep = random(0, min(max_backoff, initial * 2^(attempt - 1)))
idx_t ComputeJitterBackoff(idx_t attempt, const RetryPolicy &policy) {
	idx_t base = policy.initial_backoff_ms;
	for (idx_t i = 1; i < attempt && base < policy.max_backoff_ms; i++) {
		if (base > policy.max_backoff_ms / 2) {
			base = policy.max_backoff_ms;
			break;
		}
		base *= 2;
	}
	idx_t capped = base < policy.max_backoff_ms ? base : policy.max_backoff_ms;
	std::uniform_int_distribution<idx_t> dist(0, capped);
	return dist(Rng());
}

idx_t ComputeRetrySleepMs(const RetryPolicy &policy, idx_t attempt, idx_t retry_after_ms) {
	if (retry_after_ms == 0) {
		return ComputeJitterBackoff(attempt, policy);
	}
	if (policy.max_retry_after_ms > 0 && retry_after_ms > policy.max_retry_after_ms) {
		return policy.max_retry_after_ms;
	}
	return retry_after_ms;
}

bool IsRetryableHttpStatus(idx_t status_code) {
	return status_code == 408 || status_code == 429 || status_code == 500 || status_code == 502 || status_code == 503 ||
	       status_code == 504;
}

bool IsRetryableFeishuCode(idx_t code) {
	return code == FEISHU_RATE_LIMIT_CODE;
}

bool TryParseRetryAfterMs(const string &input, idx_t &result_ms, int64_t now_epoch_seconds) {
	result_ms = 0;
	auto begin = input.begin();
	auto end = input.end();
	while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) {
		++begin;
	}
	while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
		--end;
	}
	if (begin == end) {
		return false;
	}

	bool digits_only = true;
	uint64_t seconds = 0;
	for (auto it = begin; it != end; ++it) {
		if (!std::isdigit(static_cast<unsigned char>(*it))) {
			digits_only = false;
			break;
		}
		auto digit = static_cast<uint64_t>(*it - '0');
		if (seconds > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
			seconds = std::numeric_limits<uint64_t>::max();
		} else {
			seconds = seconds * 10 + digit;
		}
	}
	if (digits_only) {
		result_ms = SecondsToMilliseconds(seconds);
		return true;
	}

	std::tm parsed = {};
	std::istringstream stream(string(begin, end));
	stream.imbue(std::locale::classic());
	stream >> std::get_time(&parsed, "%a, %d %b %Y %H:%M:%S GMT");
	if (stream.fail()) {
		return false;
	}
	stream >> std::ws;
	if (!stream.eof()) {
		return false;
	}
	auto retry_at = TimegmPortable(&parsed);
	if (retry_at < 0) {
		return false;
	}
	if (now_epoch_seconds < 0) {
		now_epoch_seconds =
		    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
		        .count();
	}
	if (static_cast<int64_t>(retry_at) <= now_epoch_seconds) {
		return true;
	}
	result_ms = SecondsToMilliseconds(static_cast<uint64_t>(static_cast<int64_t>(retry_at) - now_epoch_seconds));
	return true;
}

idx_t ParseRetryAfterMs(const string &input, int64_t now_epoch_seconds) {
	idx_t result_ms = 0;
	TryParseRetryAfterMs(input, result_ms, now_epoch_seconds);
	return result_ms;
}

void SleepForRetry(const RetryPolicy &policy, idx_t attempt, idx_t retry_after_ms) {
	std::this_thread::sleep_for(std::chrono::milliseconds(ComputeRetrySleepMs(policy, attempt, retry_after_ms)));
}

void WithRetry(const RetryPolicy &policy, const std::function<RetryResult()> &fn) {
	if (policy.max_attempts == 0) {
		ThrowFeishuError("retry policy must allow at least one attempt");
	}
	for (idx_t attempt = 1; attempt <= policy.max_attempts; attempt++) {
		RetryResult result = fn();

		switch (result.type) {
		case RetryResultType::SUCCESS:
			return;

		case RetryResultType::NON_RETRYABLE:
			// The operation should have thrown a specific error; if it
			// returned NON_RETRYABLE without throwing, surface a generic error.
			ThrowFeishuError("operation failed with a non-retryable error");

		case RetryResultType::RETRYABLE: {
			if (attempt == policy.max_attempts) {
				break;
			}

			SleepForRetry(policy, attempt, result.retry_after_ms);
			break;
		}
		}
	}

	throw IOException("feishu_bitable: exhausted retry attempts (" + std::to_string(policy.max_attempts) + ")");
}

} // namespace feishu
} // namespace duckdb
