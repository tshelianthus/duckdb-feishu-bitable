#pragma once

#include "auth/token_cache.hpp"
#include "bitable/api_client.hpp"
#include "bitable/url_parser.hpp"

namespace duckdb {
namespace feishu {

//! Parses a successful tenant-token response using an injected timestamp so
//! token lifetime and malformed-response behavior can be tested offline.
CachedToken ParseTenantAccessTokenResponse(const HttpResponse &response, const string &app_secret, timestamp_t now);

class TokenProvider {
public:
	TokenProvider(ClientContext &context, TokenCache &cache);
	//! The API root selects both the authentication endpoint and cache namespace.
	//! Existing callers default to the Feishu region.
	string GetTenantAccessToken(const string &app_id, const string &app_secret,
	                            const string &api_root = FEISHU_BITABLE_API_ROOT);

private:
	TokenCache &cache;
	ApiClient client;
};

} // namespace feishu
} // namespace duckdb
