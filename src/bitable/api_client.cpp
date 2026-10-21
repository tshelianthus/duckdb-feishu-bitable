#include "bitable/api_client.hpp"
#include "read_error.hpp"

#include "common/error.hpp"
#include "common/retry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "feishu_httplib.hpp"

namespace duckdb {
namespace feishu {

namespace {

static string Lowercase(const string &value) {
	return StringUtil::Lower(value);
}

static bool IsRetryableTransportError(duckdb_httplib_openssl::Error error) {
	using Error = duckdb_httplib_openssl::Error;
	switch (error) {
	case Error::Connection:
	case Error::Read:
	case Error::Write:
	case Error::SSLConnection:
	case Error::ConnectionTimeout:
	case Error::ProxyConnection:
	case Error::SSLPeerCouldBeClosed_:
		return true;
	default:
		return false;
	}
}

static HttpTransportError ClassifyTransportError(duckdb_httplib_openssl::Error error) {
	using Error = duckdb_httplib_openssl::Error;
	switch (error) {
	case Error::Connection:
		return HttpTransportError::CONNECTION;
	case Error::Read:
		return HttpTransportError::READ;
	case Error::Write:
		return HttpTransportError::WRITE;
	case Error::SSLConnection:
		return HttpTransportError::TLS_CONNECTION;
	case Error::ConnectionTimeout:
		return HttpTransportError::CONNECTION_TIMEOUT;
	case Error::ProxyConnection:
		return HttpTransportError::PROXY_CONNECTION;
	case Error::SSLPeerCouldBeClosed_:
		return HttpTransportError::TLS_PEER_CLOSED;
	default:
		return HttpTransportError::OTHER;
	}
}

static void SplitUrl(const string &url, string &origin, string &path) {
	auto scheme_end = url.find("://");
	if (scheme_end == string::npos || StringUtil::Lower(url.substr(0, scheme_end)) != "https") {
		throw InvalidInputException("feishu_bitable: only HTTPS Feishu API URLs are supported");
	}
	auto path_start = url.find('/', scheme_end + 3);
	if (path_start == string::npos) {
		origin = url;
		path = "/";
	} else {
		origin = url.substr(0, path_start);
		path = url.substr(path_start);
	}
}

} // namespace

ApiClient::ApiClient(ClientContext &context_p) : context(context_p) {
	(void)context;
}

string HttpResponse::GetHeader(const string &name) const {
	auto entry = headers.find(Lowercase(name));
	return entry == headers.end() ? string() : entry->second;
}

bool HttpResponse::IsRetryableStatus() const {
	return IsRetryableHttpStatus(status_code);
}

bool HttpResponse::IsRateLimited() const {
	if (status_code == 429) {
		return true;
	}
	FeishuApiError err;
	if (TryParseFeishuError(body, err) && IsRetryableFeishuCode(err.code)) {
		return true;
	}
	return false;
}

bool HttpResponse::IsRetryable() const {
	return IsRetryableStatus() || IsRateLimited();
}

HttpResponse ApiClient::PostJson(const string &url, const string &body, const HeaderMap &headers) {
	return PerformRequest("POST", url, body, headers);
}

HttpResponse ApiClient::PostJsonNoRetry(const string &url, const string &body, const HeaderMap &headers) {
	return PerformRequest("POST", url, body, headers, false);
}

HttpResponse ApiClient::Get(const string &url, const HeaderMap &headers) {
	auto response = PerformRequest("GET", url, string(), headers);
	for (const auto &header : headers) {
		if (Lowercase(header.first) == "authorization" && header.second.size() >= 7 &&
		    Lowercase(header.second.substr(0, 7)) == "bearer ") {
			response.body = RedactReadErrorResponse(response.body, header.second.substr(7));
		}
	}
	return response;
}

HttpResponse ApiClient::PerformRequest(const string &method, const string &url, const string &body,
                                       const HeaderMap &headers, bool retry_transient) {
	string origin;
	string path;
	SplitUrl(url, origin, path);
	duckdb_httplib_openssl::Client client(origin);
	client.enable_server_certificate_verification(true);
	client.enable_server_hostname_verification(true);
	client.set_connection_timeout(10, 0);
	client.set_read_timeout(30, 0);
	client.set_write_timeout(30, 0);
	client.set_follow_location(false);
	client.set_keep_alive(false);
	client.set_decompress(false);

	duckdb_httplib_openssl::Headers request_headers;
	request_headers.emplace("Accept", "application/json");
	request_headers.emplace("Accept-Encoding", "identity");
	for (auto &header : headers) {
		request_headers.emplace(header.first, header.second);
	}

	RetryPolicy retry_policy;
	HttpResponse result;
	string transport_error;
	for (idx_t attempt = 1; attempt <= retry_policy.max_attempts; attempt++) {
		detail::ResponseBodyCollector response_body;
		bool response_body_too_large = false;
		auto receive_response_chunk = [&](const char *data, size_t data_length) {
			if (!response_body.Append(data, data_length)) {
				response_body_too_large = true;
				return false;
			}
			return true;
		};
		duckdb_httplib_openssl::Result response =
		    method == "POST" ? client.Post(path, request_headers, body, "application/json", receive_response_chunk)
		                     : client.Get(path, request_headers, receive_response_chunk);
		if (response_body_too_large) {
			throw IOException("feishu_bitable: HTTP response body exceeds the maximum allowed size");
		}
		if (!response) {
			transport_error = duckdb_httplib_openssl::to_string(response.error());
			if (!retry_transient) {
				result.transport_error = ClassifyTransportError(response.error());
				result.transport_error_message = transport_error;
				return result;
			}
			if (IsRetryableTransportError(response.error()) && attempt < retry_policy.max_attempts) {
				SleepForRetry(retry_policy, attempt);
				continue;
			}
			throw IOException("feishu_bitable: HTTPS request failed: " + transport_error);
		}

		result.status_code = static_cast<idx_t>(response->status);
		result.body = response_body.Body();
		result.headers.clear();
		for (auto &header : response->headers) {
			result.headers[Lowercase(header.first)] = header.second;
		}
		if (!retry_transient) {
			return result;
		}
		if (!result.IsRetryable() || attempt == retry_policy.max_attempts) {
			return result;
		}
		idx_t retry_after_ms = 0;
		auto has_retry_after = TryParseRetryAfterMs(response->get_header_value("Retry-After"), retry_after_ms);
		if (!has_retry_after || retry_after_ms > 0) {
			SleepForRetry(retry_policy, attempt, retry_after_ms);
		}
	}
	throw IOException("feishu_bitable: HTTPS request failed after retries");
}

} // namespace feishu
} // namespace duckdb
