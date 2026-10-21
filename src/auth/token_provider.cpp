#include "auth/token_provider.hpp"

#include "common/error.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "yyjson.hpp"

namespace duckdb {
namespace feishu {

namespace {

static const char *const FEISHU_TOKEN_URL = "https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal";
static const char *const LARK_TOKEN_URL = "https://open.larksuite.com/open-apis/auth/v3/tenant_access_token/internal";

static string JsonEscape(const string &input) {
	static const char *HEX = "0123456789abcdef";
	string result;
	result.reserve(input.size() + 8);
	for (idx_t i = 0; i < input.size(); i++) {
		auto ch = static_cast<unsigned char>(input[i]);
		switch (ch) {
		case '"':
			result += "\\\"";
			break;
		case '\\':
			result += "\\\\";
			break;
		case '\b':
			result += "\\b";
			break;
		case '\f':
			result += "\\f";
			break;
		case '\n':
			result += "\\n";
			break;
		case '\r':
			result += "\\r";
			break;
		case '\t':
			result += "\\t";
			break;
		default:
			if (ch < 0x20) {
				result += "\\u00";
				result.push_back(HEX[(ch >> 4) & 0x0F]);
				result.push_back(HEX[ch & 0x0F]);
			} else {
				result.push_back(static_cast<char>(ch));
			}
		}
	}
	return result;
}

struct JsonGuard {
	explicit JsonGuard(duckdb_yyjson::yyjson_doc *document_p) : document(document_p) {
	}
	~JsonGuard() {
		duckdb_yyjson::yyjson_doc_free(document);
	}
	duckdb_yyjson::yyjson_doc *document;
};

static bool JsonInt64(duckdb_yyjson::yyjson_val *value, int64_t &result) {
	using namespace duckdb_yyjson; // NOLINT
	if (yyjson_is_sint(value)) {
		result = yyjson_get_sint(value);
		return true;
	}
	if (yyjson_is_uint(value)) {
		auto number = yyjson_get_uint(value);
		if (number <= static_cast<uint64_t>(NumericLimits<int64_t>::Maximum())) {
			result = static_cast<int64_t>(number);
			return true;
		}
	}
	return false;
}

} // namespace

CachedToken ParseTenantAccessTokenResponse(const HttpResponse &response, const string &app_secret, timestamp_t now) {
	using namespace duckdb_yyjson; // NOLINT
	auto document = yyjson_read(response.body.data(), response.body.size(), 0);
	if (!document) {
		throw IOException("feishu_bitable: authentication endpoint returned invalid JSON");
	}
	JsonGuard guard(document);
	auto root = yyjson_doc_get_root(document);
	if (!root || !yyjson_is_obj(root)) {
		throw IOException("feishu_bitable: authentication endpoint returned an invalid object");
	}
	auto code_value = yyjson_obj_get(root, "code");
	int64_t code;
	if (!JsonInt64(code_value, code)) {
		throw IOException("feishu_bitable: authentication response is missing API code");
	}
	if (code != 0) {
		FeishuApiError error;
		error.code = code > 0 ? static_cast<idx_t>(code) : 0;
		auto msg_val = yyjson_obj_get(root, "msg");
		if (msg_val && yyjson_is_str(msg_val)) {
			// The token endpoint is the only request that carries the raw
			// app_secret, so strike the credential out of the untrusted remote
			// message before it can reach an exception.
			error.msg = RedactSecretValue(string(yyjson_get_str(msg_val), yyjson_get_len(msg_val)), app_secret);
		}
		ThrowFeishuApiError(error);
	}
	auto token_value = yyjson_obj_get(root, "tenant_access_token");
	auto expire_value = yyjson_obj_get(root, "expire");
	int64_t expire_seconds;
	if (!token_value || !yyjson_is_str(token_value) || yyjson_get_len(token_value) == 0 ||
	    !JsonInt64(expire_value, expire_seconds)) {
		throw IOException("feishu_bitable: authentication response is missing token metadata");
	}
	if (expire_seconds <= 0 || expire_seconds > int64_t(86400) * 30 ||
	    expire_seconds > (NumericLimits<int64_t>::Maximum() - std::max<int64_t>(0, now.value)) / 1000000) {
		throw IOException("feishu_bitable: authentication response has an invalid token lifetime");
	}
	CachedToken result;
	result.access_token.assign(yyjson_get_str(token_value), yyjson_get_len(token_value));
	result.expires_at = timestamp_t(now.value + expire_seconds * int64_t(1000000));
	return result;
}

TokenProvider::TokenProvider(ClientContext &context, TokenCache &cache_p) : cache(cache_p), client(context) {
}

string TokenProvider::GetTenantAccessToken(const string &app_id, const string &app_secret, const string &api_root) {
	if (app_id.empty() || app_secret.empty()) {
		throw InvalidInputException("feishu_bitable: APP_ID and APP_SECRET must not be empty");
	}
	if (api_root != FEISHU_BITABLE_API_ROOT && api_root != LARK_BITABLE_API_ROOT) {
		throw InvalidInputException("feishu_bitable: unsupported API root for tenant authentication");
	}
	auto token_url = api_root == LARK_BITABLE_API_ROOT ? LARK_TOKEN_URL : FEISHU_TOKEN_URL;
	// Identical credentials in different regions must never share a token or
	// a refresh lock. The credential portion remains an irreversible hash.
	auto cache_key = api_root + ":" + BuildTokenCacheKey(app_id, app_secret);
	return cache.GetOrRefresh(cache_key, [&]() {
		auto body = "{\"app_id\":\"" + JsonEscape(app_id) + "\",\"app_secret\":\"" + JsonEscape(app_secret) + "\"}";
		auto response = client.PostJson(token_url, body);
		if (response.status_code < 200 || response.status_code >= 300) {
			FeishuApiError api_err;
			if (TryParseFeishuError(response.body, api_err)) {
				api_err.msg = RedactSecretValue(api_err.msg, app_secret);
				ThrowFeishuApiError(api_err);
			}
			ThrowHttpError(response.status_code, string());
		}
		return ParseTenantAccessTokenResponse(response, app_secret, Timestamp::GetCurrentTimestamp());
	});
}

} // namespace feishu
} // namespace duckdb
