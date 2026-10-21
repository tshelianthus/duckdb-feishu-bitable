#include "auth/token_cache.hpp"

#include "duckdb/common/exception.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <chrono>
#include <limits>

namespace duckdb {
namespace feishu {

namespace {

timestamp_t CurrentTimestamp() {
	auto now = std::chrono::system_clock::now();
	auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
	return timestamp_t(micros);
}

int64_t RefreshWindowMicros(idx_t refresh_window_seconds) {
	constexpr idx_t MAX_REFRESH_WINDOW_SECONDS = idx_t(86400) * 30;
	auto seconds = std::min<idx_t>(refresh_window_seconds, MAX_REFRESH_WINDOW_SECONDS);
	return static_cast<int64_t>(seconds) * 1000000;
}

} // namespace

string BuildTokenCacheKey(const string &app_id, const string &app_secret) {
	// Length-prefix the app id so arbitrary byte strings cannot create an
	// ambiguous concatenation, then hash the complete credential identity.
	auto material = std::to_string(app_id.size()) + ":" + app_id + app_secret;
	unsigned char digest[SHA256_DIGEST_LENGTH];
	SHA256(reinterpret_cast<const unsigned char *>(material.data()), material.size(), digest);
	static const char *HEX = "0123456789abcdef";
	string result;
	result.reserve(SHA256_DIGEST_LENGTH * 2);
	for (auto byte : digest) {
		result.push_back(HEX[(byte >> 4) & 0x0f]);
		result.push_back(HEX[byte & 0x0f]);
	}
	return result;
}

TokenCache::TokenCache(idx_t capacity_p) : capacity(std::max<idx_t>(1, capacity_p)) {
}

uint64_t TokenCache::NextAccessSequence() const {
	if (access_sequence == std::numeric_limits<uint64_t>::max()) {
		uint64_t sequence = 0;
		for (auto &entry : tokens) {
			entry.second.last_access = ++sequence;
		}
		access_sequence = sequence;
	}
	return ++access_sequence;
}

bool TokenCache::TryGet(const string &cache_key, CachedToken &out) const {
	lock_guard<mutex> guard(lock);
	auto it = tokens.find(cache_key);
	if (it == tokens.end()) {
		return false;
	}
	it->second.last_access = NextAccessSequence();
	out = it->second.token;
	return true;
}

bool TokenCache::TryGetValidLocked(const string &cache_key, CachedToken &out, timestamp_t now,
                                   idx_t refresh_window_seconds) const {
	auto it = tokens.find(cache_key);
	if (it == tokens.end() || it->second.token.access_token.empty()) {
		return false;
	}
	const auto refresh_micros = RefreshWindowMicros(refresh_window_seconds);
	if (now.value > std::numeric_limits<int64_t>::max() - refresh_micros ||
	    it->second.token.expires_at.value <= now.value + refresh_micros) {
		return false;
	}
	it->second.last_access = NextAccessSequence();
	out = it->second.token;
	return true;
}

bool TokenCache::TryGetValid(const string &cache_key, CachedToken &out, timestamp_t now,
                             idx_t refresh_window_seconds) const {
	lock_guard<mutex> guard(lock);
	return TryGetValidLocked(cache_key, out, now, refresh_window_seconds);
}

void TokenCache::EvictIfNeededLocked() {
	while (tokens.size() > capacity) {
		auto victim = tokens.end();
		for (auto it = tokens.begin(); it != tokens.end(); ++it) {
			if (victim == tokens.end() || it->second.last_access < victim->second.last_access) {
				victim = it;
			}
		}
		if (victim == tokens.end()) {
			return;
		}
		tokens.erase(victim);
	}
}

void TokenCache::PutLocked(const string &cache_key, CachedToken token) {
	auto &entry = tokens[cache_key];
	entry.token = std::move(token);
	entry.last_access = NextAccessSequence();
	EvictIfNeededLocked();
}

void TokenCache::Put(const string &cache_key, CachedToken token) {
	lock_guard<mutex> guard(lock);
	PutLocked(cache_key, std::move(token));
}

void TokenCache::Invalidate(const string &cache_key) {
	lock_guard<mutex> guard(lock);
	tokens.erase(cache_key);
}

string TokenCache::GetOrRefresh(const string &cache_key, const std::function<CachedToken()> &refresh_fn,
                                idx_t refresh_window_seconds) {
	if (cache_key.empty()) {
		throw InvalidInputException("feishu_bitable: token cache key must not be empty");
	}
	if (!refresh_fn) {
		throw InvalidInputException("feishu_bitable: token refresh function must not be empty");
	}

	std::shared_ptr<RefreshState> state;
	{
		unique_lock<mutex> guard(lock);
		while (true) {
			CachedToken cached;
			if (TryGetValidLocked(cache_key, cached, CurrentTimestamp(), refresh_window_seconds)) {
				return cached.access_token;
			}

			auto active = refreshes.find(cache_key);
			if (active != refreshes.end()) {
				state = active->second;
				state->ready.wait(guard, [&]() { return state->done; });
				if (state->error) {
					std::rethrow_exception(state->error);
				}
				return state->access_token;
			}

			// Active refresh state is also bounded. When all slots are busy,
			// wait for one to finish rather than allocating an unbounded map of
			// per-credential locks.
			if (refreshes.size() >= capacity) {
				refresh_capacity_changed.wait(guard);
				continue;
			}

			state = std::make_shared<RefreshState>();
			refreshes.emplace(cache_key, state);
			break;
		}
	}

	CachedToken refreshed;
	string access_token;
	std::exception_ptr refresh_error;
	try {
		refreshed = refresh_fn();
		auto now = CurrentTimestamp();
		if (refreshed.access_token.empty() || refreshed.expires_at.value <= now.value) {
			throw IOException("feishu_bitable: authentication returned an invalid token lifetime");
		}
		access_token = refreshed.access_token;
	} catch (...) {
		refresh_error = std::current_exception();
	}

	{
		lock_guard<mutex> guard(lock);
		if (!refresh_error) {
			PutLocked(cache_key, std::move(refreshed));
		}
		state->access_token = access_token;
		state->error = refresh_error;
		state->done = true;
		auto active = refreshes.find(cache_key);
		if (active != refreshes.end() && active->second == state) {
			refreshes.erase(active);
		}
	}
	state->ready.notify_all();
	refresh_capacity_changed.notify_all();

	if (refresh_error) {
		std::rethrow_exception(refresh_error);
	}
	return access_token;
}

idx_t TokenCache::Size() const {
	lock_guard<mutex> guard(lock);
	return tokens.size();
}

idx_t TokenCache::InFlightRefreshCount() const {
	lock_guard<mutex> guard(lock);
	return refreshes.size();
}

idx_t TokenCache::Capacity() const {
	return capacity;
}

TokenCache &GetGlobalTokenCache() {
	static TokenCache cache;
	return cache;
}

} // namespace feishu
} // namespace duckdb
