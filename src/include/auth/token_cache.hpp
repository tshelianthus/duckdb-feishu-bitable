#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/unordered_map.hpp"

#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>

namespace duckdb {
namespace feishu {

struct CachedToken {
	string access_token;
	timestamp_t expires_at;
};

//! Builds an opaque, irreversible cache key from the complete credential
//! identity. Neither the app id nor the app secret appears in the result.
string BuildTokenCacheKey(const string &app_id, const string &app_secret);

//! Thread-safe in-memory tenant_access_token cache. Tokens never hit disk.
class TokenCache {
public:
	static constexpr idx_t DEFAULT_CAPACITY = 128;

	explicit TokenCache(idx_t capacity = DEFAULT_CAPACITY);
	TokenCache(const TokenCache &) = delete;
	TokenCache &operator=(const TokenCache &) = delete;

	bool TryGet(const string &cache_key, CachedToken &out) const;
	bool TryGetValid(const string &cache_key, CachedToken &out, timestamp_t now,
	                 idx_t refresh_window_seconds = 300) const;
	void Put(const string &cache_key, CachedToken token);
	void Invalidate(const string &cache_key);

	//! Atomically return a valid token or invoke refresh_fn and cache its result.
	//! The refresh is serialized to prevent concurrent requests from stampeding
	//! the Feishu authentication endpoint.
	string GetOrRefresh(const string &cache_key, const std::function<CachedToken()> &refresh_fn,
	                    idx_t refresh_window_seconds = 300);

	idx_t Size() const;
	idx_t InFlightRefreshCount() const;
	idx_t Capacity() const;

private:
	struct CacheEntry {
		CachedToken token;
		uint64_t last_access = 0;
	};

	struct RefreshState {
		std::condition_variable ready;
		bool done = false;
		string access_token;
		std::exception_ptr error;
	};

	bool TryGetValidLocked(const string &cache_key, CachedToken &out, timestamp_t now,
	                       idx_t refresh_window_seconds) const;
	void PutLocked(const string &cache_key, CachedToken token);
	void EvictIfNeededLocked();
	uint64_t NextAccessSequence() const;

	mutable mutex lock;
	mutable unordered_map<string, CacheEntry> tokens;
	unordered_map<string, std::shared_ptr<RefreshState>> refreshes;
	std::condition_variable refresh_capacity_changed;
	idx_t capacity;
	mutable uint64_t access_sequence = 0;
};

//! Process-local in-memory cache shared by all table functions.
TokenCache &GetGlobalTokenCache();

} // namespace feishu
} // namespace duckdb
