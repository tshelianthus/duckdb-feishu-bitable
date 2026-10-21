#pragma once

#include "duckdb.hpp"
#include "duckdb/common/unordered_map.hpp"

#include <cstdint>

namespace duckdb {
namespace feishu {

using HeaderMap = unordered_map<string, string>;

//! Transport outcome for a request made without the read-side automatic
//! retry policy. Write endpoints use this to distinguish a request that was
//! never sent from a result that may already have reached Feishu.
enum class HttpTransportError : uint8_t {
	NONE,
	CONNECTION,
	READ,
	WRITE,
	TLS_CONNECTION,
	CONNECTION_TIMEOUT,
	PROXY_CONNECTION,
	TLS_PEER_CLOSED,
	OTHER
};

namespace detail {

//! Feishu record pages contain at most 500 rows. A 16 MiB response cap leaves
//! ample room for legitimate pages while preventing an untrusted peer from
//! growing a query's response buffer without bound.
constexpr idx_t MAX_HTTP_RESPONSE_BODY_BYTES = idx_t(16) * 1024 * 1024;

//! Incrementally collects an HTTP response body without exceeding the fixed
//! cap above. Kept header-only so the boundary behavior is unit-testable
//! without opening a socket or linking the DuckDB runtime.
class ResponseBodyCollector {
public:
	bool Append(const char *data, idx_t data_length) {
		if (data_length > MAX_HTTP_RESPONSE_BODY_BYTES - body.size()) {
			return false;
		}
		body.append(data, data_length);
		return true;
	}

	const string &Body() const {
		return body;
	}

private:
	string body;
};

} // namespace detail

struct HttpResponse {
	idx_t status_code = 0;
	string body;
	HeaderMap headers;
	HttpTransportError transport_error = HttpTransportError::NONE;
	string transport_error_message;

	string GetHeader(const string &name) const;
	bool IsRetryableStatus() const;
	bool IsRateLimited() const;
	bool IsRetryable() const;
};

//! HTTPS client scoped to a DuckDB query context. Credentials are accepted
//! only as request headers/body and are never retained or included in errors.
class ApiClient {
public:
	explicit ApiClient(ClientContext &context);

	HttpResponse PostJson(const string &url, const string &body, const HeaderMap &headers = {});
	//! Sends exactly one request and returns transport failures in the response
	//! instead of applying the read-side retry policy. This is required for
	//! non-idempotent write endpoints.
	HttpResponse PostJsonNoRetry(const string &url, const string &body, const HeaderMap &headers = {});
	HttpResponse Get(const string &url, const HeaderMap &headers = {});

private:
	HttpResponse PerformRequest(const string &method, const string &url, const string &body, const HeaderMap &headers,
	                            bool retry_transient = true);
	ClientContext &context;
};

} // namespace feishu
} // namespace duckdb
