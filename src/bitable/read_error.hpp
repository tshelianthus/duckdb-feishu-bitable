#pragma once

#include "bitable/api_response.hpp"
#include "common/error.hpp"

#include <cstdlib>
#include <memory>

namespace duckdb {
namespace feishu {

// Error bodies may be empty or HTML. Parsing them must never hide the HTTP
// status. The same best-effort parse is used only for diagnostics/redaction.
using ReadErrorDocument = std::unique_ptr<duckdb_yyjson::yyjson_doc, decltype(&duckdb_yyjson::yyjson_doc_free)>;

inline ReadErrorDocument TryReadErrorDocument(const string &body) {
	return ReadErrorDocument(duckdb_yyjson::yyjson_read(body.data(), body.size(), 0), duckdb_yyjson::yyjson_doc_free);
}

inline void EnsureReadHttpSuccess(uint64_t status, const string &body) {
	if (status < 200 || status >= 300) {
		auto json = TryReadErrorDocument(body);
		EnsureApiSuccess(status, duckdb_yyjson::yyjson_doc_get_root(json.get()));
	}
}

// Read requests send only the access token. Strike out its literal value
// after JSON decoding: delegated tokens need not have a recognizable prefix,
// and a server can JSON-escape an echoed token. Never alter successful data.
inline string RedactReadErrorResponse(const string &body, const string &access_token) {
	using namespace duckdb_yyjson; // NOLINT
	if (access_token.empty()) {
		return body;
	}
	auto json = TryReadErrorDocument(body);
	auto root = yyjson_doc_get_root(json.get());
	auto code = yyjson_obj_get(root, "code");
	int64_t code_number;
	if (!yyjson_is_int(code) || !JsonInt64(code, code_number) || code_number == 0) {
		return body;
	}
	auto msg = yyjson_obj_get(root, "msg");
	if (!yyjson_is_str(msg)) {
		return body;
	}
	auto safe_message = RedactSecretValue(JsonString(msg), access_token);
	yyjson_set_strn(msg, safe_message.data(), safe_message.size());
	size_t length;
	std::unique_ptr<char, decltype(&std::free)> serialized(yyjson_write(json.get(), 0, &length), std::free);
	if (!serialized) {
		throw IOException("feishu_bitable: failed to sanitize a read API error");
	}
	return string(serialized.get(), length);
}

} // namespace feishu
} // namespace duckdb
