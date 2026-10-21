#include "common/error.hpp"

#include "yyjson.hpp"

#include <cctype>
#include <limits>
#include <string>

namespace duckdb {
namespace feishu {

void ThrowFeishuError(const string &message) {
	throw InvalidInputException("feishu_bitable: " + message);
}

void ThrowHttpError(idx_t status_code, const string &body) {
	(void)body;
	// Response bodies are deliberately omitted: authentication services may
	// echo request details and no remote payload is safe to expose verbatim.
	auto message =
	    "feishu_bitable: HTTP request failed with status " + std::to_string(static_cast<uint64_t>(status_code));
	if (status_code >= 400 && status_code < 500 && status_code != 408 && status_code != 429) {
		throw InvalidInputException(message);
	}
	throw IOException(message);
}

string MaskToken(const string &token) {
	if (token.empty()) {
		return "";
	}
	if (token.size() <= 4) {
		return string(token.size(), '*');
	}
	return token.substr(0, 4) + string(token.size() - 4, '*');
}

string SanitizeErrorMessage(const string &msg) {
	if (msg.empty()) {
		return msg;
	}
	string sanitized;
	sanitized.reserve(msg.size());
	for (idx_t i = 0; i < msg.size();) {
		// Authorization headers can carry tokens without a recognizable Feishu
		// prefix. Mask the complete value, case-insensitively, through the next
		// line break or common structured-message delimiter.
		constexpr const char *AUTHORIZATION = "authorization";
		constexpr idx_t AUTHORIZATION_LENGTH = 13;
		bool authorization_match = i + AUTHORIZATION_LENGTH <= msg.size();
		for (idx_t offset = 0; authorization_match && offset < AUTHORIZATION_LENGTH; offset++) {
			auto actual = static_cast<unsigned char>(msg[i + offset]);
			auto expected = static_cast<unsigned char>(AUTHORIZATION[offset]);
			authorization_match = std::tolower(actual) == std::tolower(expected);
		}
		if (authorization_match) {
			auto value_start = i + AUTHORIZATION_LENGTH;
			while (value_start < msg.size() &&
			       (std::isspace(static_cast<unsigned char>(msg[value_start])) || msg[value_start] == ':' ||
			        msg[value_start] == '=' || msg[value_start] == '\"' || msg[value_start] == '\'')) {
				value_start++;
			}
			auto value_end = value_start;
			while (value_end < msg.size() && msg[value_end] != '\r' && msg[value_end] != '\n' &&
			       msg[value_end] != ',' && msg[value_end] != ';' && msg[value_end] != '}' && msg[value_end] != '\"' &&
			       msg[value_end] != '\'') {
				value_end++;
			}
			sanitized.append(msg, i, value_start - i);
			sanitized += "********";
			i = value_end;
			continue;
		}
		// Check for "cli_" prefix (app_id token)
		if (i + 4 <= msg.size() && msg.substr(i, 4) == "cli_") {
			idx_t end = i + 4;
			while (end < msg.size() && (std::isalnum(static_cast<unsigned char>(msg[end])) || msg[end] == '_')) {
				end++;
			}
			auto token = msg.substr(i, end - i);
			sanitized += MaskToken(token);
			i = end;
			continue;
		}
		// Check for "t-" prefix (tenant_access_token)
		if (i + 2 <= msg.size() && msg.substr(i, 2) == "t-") {
			idx_t end = i + 2;
			while (end < msg.size() &&
			       (std::isalnum(static_cast<unsigned char>(msg[end])) || msg[end] == '-' || msg[end] == '_')) {
				end++;
			}
			if (end - i >= 8) { // Only mask if it looks like a token
				auto token = msg.substr(i, end - i);
				sanitized += MaskToken(token);
				i = end;
				continue;
			}
		}
		// Delegated user access tokens use the analogous "u-" prefix. Keep a
		// minimum length so ordinary prose such as "u-turn" is not masked.
		if (i + 2 <= msg.size() && msg.substr(i, 2) == "u-") {
			idx_t end = i + 2;
			while (end < msg.size() &&
			       (std::isalnum(static_cast<unsigned char>(msg[end])) || msg[end] == '-' || msg[end] == '_')) {
				end++;
			}
			if (end - i >= 8) {
				auto token = msg.substr(i, end - i);
				sanitized += MaskToken(token);
				i = end;
				continue;
			}
		}
		sanitized.push_back(msg[i]);
		i++;
	}
	return sanitized;
}

string RedactSecretValue(const string &msg, const string &secret) {
	// Prefix based sanitizing cannot help with an app_secret: Feishu issues it
	// as a bare 32 character random string with no recognizable shape. The only
	// reliable defense is to strike out every literal occurrence of the value we
	// are about to send, which is what this does before the remote message is
	// ever attached to an exception.
	if (msg.empty() || secret.empty()) {
		return msg;
	}
	string masked = secret.size() < 4 ? string(secret.size(), '*') : MaskToken(secret);
	string result;
	result.reserve(msg.size());
	size_t position = 0;
	while (position < msg.size()) {
		auto hit = msg.find(secret, position);
		if (hit == string::npos) {
			result.append(msg, position, msg.size() - position);
			break;
		}
		result.append(msg, position, hit - position);
		result += masked;
		position = hit + secret.size();
	}
	return result;
}

void ThrowFeishuApiError(const FeishuApiError &err) {
	const string prefix =
	    "feishu_bitable: Feishu API error [code=" + std::to_string(static_cast<uint64_t>(err.code)) + "]";
	if (err.msg.empty() || err.msg == "(no message provided by Feishu API)") {
		throw InvalidInputException(prefix);
	}
	throw InvalidInputException(prefix + ": " + SanitizeErrorMessage(err.msg));
}

bool TryParseFeishuError(const string &body, FeishuApiError &out) {
	if (body.empty()) {
		return false;
	}
	using namespace duckdb_yyjson; // NOLINT
	yyjson_doc *doc = yyjson_read(body.data(), body.size(), 0);
	if (!doc) {
		return false;
	}
	yyjson_val *root = yyjson_doc_get_root(doc);
	if (!root || !yyjson_is_obj(root)) {
		yyjson_doc_free(doc);
		return false;
	}
	yyjson_val *code_val = yyjson_obj_get(root, "code");
	if (!code_val) {
		yyjson_doc_free(doc);
		return false;
	}
	int64_t code = 0;
	if (yyjson_is_sint(code_val)) {
		code = yyjson_get_sint(code_val);
	} else if (yyjson_is_uint(code_val)) {
		auto unsigned_code = yyjson_get_uint(code_val);
		if (unsigned_code <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
			code = static_cast<int64_t>(unsigned_code);
		} else {
			yyjson_doc_free(doc);
			return false;
		}
	} else {
		yyjson_doc_free(doc);
		return false;
	}
	if (code <= 0) {
		yyjson_doc_free(doc);
		return false;
	}
	out.code = static_cast<idx_t>(code);
	yyjson_val *msg_val = yyjson_obj_get(root, "msg");
	if (msg_val && yyjson_is_str(msg_val)) {
		out.msg = string(yyjson_get_str(msg_val), yyjson_get_len(msg_val));
	} else {
		out.msg = "(no message provided by Feishu API)";
	}
	yyjson_doc_free(doc);
	return true;
}

} // namespace feishu
} // namespace duckdb
