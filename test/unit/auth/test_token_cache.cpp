#include "catch.hpp"

#include "auth/token_cache.hpp"

#include "duckdb/common/exception.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

using duckdb::idx_t;
using duckdb::IOException;
using duckdb::timestamp_t;
using duckdb::feishu::BuildTokenCacheKey;
using duckdb::feishu::CachedToken;
using duckdb::feishu::TokenCache;

namespace {

int64_t NowMicros() {
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
	    .count();
}

CachedToken FutureToken(const std::string &value, int64_t seconds = 3600) {
	return CachedToken {value, timestamp_t(NowMicros() + seconds * 1000000)};
}

void UpdateMaximum(std::atomic<idx_t> &maximum, idx_t candidate) {
	auto observed = maximum.load();
	while (observed < candidate && !maximum.compare_exchange_weak(observed, candidate)) {
	}
}

} // namespace

TEST_CASE("Credential cache keys include the secret without retaining either credential", "[token_cache][security]") {
	auto first = BuildTokenCacheKey("cli_same_app", "first-secret-value");
	auto rotated = BuildTokenCacheKey("cli_same_app", "second-secret-value");
	auto other_app = BuildTokenCacheKey("cli_other_app", "first-secret-value");

	REQUIRE(first.size() == 64);
	REQUIRE(first != rotated);
	REQUIRE(first != other_app);
	REQUIRE(first.find("cli_same_app") == std::string::npos);
	REQUIRE(first.find("first-secret-value") == std::string::npos);
	REQUIRE(first == BuildTokenCacheKey("cli_same_app", "first-secret-value"));
}

TEST_CASE("A rotated secret for the same app id cannot reuse the previous token", "[token_cache][security]") {
	TokenCache cache(4);
	std::atomic<idx_t> refresh_calls {0};
	auto first_key = BuildTokenCacheKey("cli_same_app", "first-secret-value");
	auto rotated_key = BuildTokenCacheKey("cli_same_app", "second-secret-value");

	auto first = cache.GetOrRefresh(first_key, [&]() {
		refresh_calls.fetch_add(1);
		return FutureToken("first-token");
	});
	auto rotated = cache.GetOrRefresh(rotated_key, [&]() {
		refresh_calls.fetch_add(1);
		return FutureToken("rotated-token");
	});

	REQUIRE(first == "first-token");
	REQUIRE(rotated == "rotated-token");
	REQUIRE(refresh_calls.load() == 2);
}

TEST_CASE("A token inside the refresh window is treated as stale", "[token_cache]") {
	TokenCache cache(4);
	const timestamp_t now(1000000000);
	cache.Put("credential", CachedToken {"token", timestamp_t(now.value + 301000000)});
	CachedToken output;
	REQUIRE(cache.TryGetValid("credential", output, now, 300));
	REQUIRE(output.access_token == "token");

	cache.Put("credential", CachedToken {"token", timestamp_t(now.value + 300000000)});
	REQUIRE_FALSE(cache.TryGetValid("credential", output, now, 300));
}

TEST_CASE("The token cache evicts the least recently used entry at its capacity", "[token_cache]") {
	TokenCache cache(2);
	cache.Put("one", FutureToken("token-one"));
	cache.Put("two", FutureToken("token-two"));
	CachedToken output;
	REQUIRE(cache.TryGet("one", output));
	cache.Put("three", FutureToken("token-three"));

	REQUIRE(cache.Size() == 2);
	REQUIRE(cache.TryGet("one", output));
	REQUIRE_FALSE(cache.TryGet("two", output));
	REQUIRE(cache.TryGet("three", output));
}

TEST_CASE("Concurrent callers for one credential share exactly one refresh", "[token_cache][concurrency]") {
	TokenCache cache(8);
	std::atomic<idx_t> refresh_calls {0};
	std::promise<void> start_promise;
	auto start = start_promise.get_future().share();
	std::vector<std::thread> threads;
	std::vector<std::string> results(16);

	for (idx_t index = 0; index < results.size(); index++) {
		threads.emplace_back([&, index]() {
			start.wait();
			results[index] = cache.GetOrRefresh("credential", [&]() {
				refresh_calls.fetch_add(1);
				std::this_thread::sleep_for(std::chrono::milliseconds(40));
				return FutureToken("shared-token");
			});
		});
	}
	start_promise.set_value();
	for (auto &thread : threads) {
		thread.join();
	}

	REQUIRE(refresh_calls.load() == 1);
	for (auto &result : results) {
		REQUIRE(result == "shared-token");
	}
	REQUIRE(cache.InFlightRefreshCount() == 0);
}

TEST_CASE("Different credentials can refresh concurrently while refresh state stays bounded",
          "[token_cache][concurrency]") {
	TokenCache cache(2);
	std::atomic<idx_t> active {0};
	std::atomic<idx_t> maximum {0};
	std::promise<void> start_promise;
	auto start = start_promise.get_future().share();
	std::vector<std::thread> threads;

	for (idx_t index = 0; index < 8; index++) {
		threads.emplace_back([&, index]() {
			start.wait();
			cache.GetOrRefresh("credential-" + std::to_string(index), [&]() {
				auto current = active.fetch_add(1) + 1;
				UpdateMaximum(maximum, current);
				std::this_thread::sleep_for(std::chrono::milliseconds(30));
				active.fetch_sub(1);
				return FutureToken("token-" + std::to_string(index));
			});
		});
	}
	start_promise.set_value();
	for (auto &thread : threads) {
		thread.join();
	}

	REQUIRE(maximum.load() >= 2);
	REQUIRE(maximum.load() <= cache.Capacity());
	REQUIRE(cache.Size() <= cache.Capacity());
	REQUIRE(cache.InFlightRefreshCount() == 0);
}

TEST_CASE("Refresh failures preserve their exception type and release refresh state", "[token_cache][concurrency]") {
	TokenCache cache(2);
	REQUIRE_THROWS_AS(
	    cache.GetOrRefresh("credential",
	                       []() -> CachedToken { throw IOException("feishu_bitable: authentication unavailable"); }),
	    IOException);
	REQUIRE(cache.Size() == 0);
	REQUIRE(cache.InFlightRefreshCount() == 0);

	REQUIRE(cache.GetOrRefresh("credential", []() { return FutureToken("recovered-token"); }) == "recovered-token");
}

TEST_CASE("Empty and already expired refresh results are rejected", "[token_cache]") {
	TokenCache cache(2);
	REQUIRE_THROWS_AS(cache.GetOrRefresh("credential", []() { return FutureToken(""); }), IOException);
	REQUIRE_THROWS_AS(cache.GetOrRefresh("credential",
	                                     []() {
		                                     return CachedToken {"expired", timestamp_t(NowMicros() - 1)};
	                                     }),
	                  IOException);
	REQUIRE(cache.InFlightRefreshCount() == 0);
}
