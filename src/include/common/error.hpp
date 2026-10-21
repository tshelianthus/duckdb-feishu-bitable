#pragma once

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {
namespace feishu {

//! Structured representation of a Feishu OpenAPI business error.
struct FeishuApiError {
	idx_t code = 0;
	string msg;
};

//! Throws a generic feishu_bitable InvalidInputException.
[[noreturn]] void ThrowFeishuError(const string &message);

//! Maps permanent HTTP 4xx request failures to InvalidInputException and
//! transient/server failures to IOException. The response body is intentionally
//! ignored so credentials can never be echoed.
[[noreturn]] void ThrowHttpError(idx_t status_code, const string &body);

//! Throws a structured Feishu business error as InvalidInputException. The
//! numeric code is exposed verbatim; the untrusted remote message is passed
//! through SanitizeErrorMessage first. Callers that hold a bare credential
//! (an app_secret has no recognizable prefix) must run the remote message
//! through RedactSecretValue before building the FeishuApiError.
[[noreturn]] void ThrowFeishuApiError(const FeishuApiError &err);

//! Parses a Feishu API JSON response body into a FeishuApiError when `code != 0`.
//! Returns `true` when the body represents a Feishu business error (code != 0).
bool TryParseFeishuError(const string &body, FeishuApiError &out);

//! Masks a sensitive token (app_secret / tenant_access_token) for safe logging.
//! Keeps the first 4 characters and masks the rest, e.g. "cli_************".
string MaskToken(const string &token);

//! Sanitizes error messages by masking credentials (app_secret, access tokens, app_ids).
string SanitizeErrorMessage(const string &msg);

//! Masks every literal occurrence of `secret` inside `msg`. Feishu app_secrets
//! carry no prefix, so SanitizeErrorMessage cannot recognize them; call this
//! whenever an untrusted remote message is produced by a request that carried
//! the credential. Short secrets are fully masked as well, so a malformed
//! credential cannot be echoed merely because it is unusually short.
string RedactSecretValue(const string &msg, const string &secret);

} // namespace feishu
} // namespace duckdb
