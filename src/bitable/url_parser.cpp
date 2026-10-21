#include "bitable/url_parser.hpp"

#include "duckdb/common/exception.hpp"

#include <cctype>
#include <vector>

namespace duckdb {
namespace feishu {

namespace {

//! Registered domains that host Feishu (China) and Lark (international) Bitables.
//! A host matches when it is the domain itself or any tenant subdomain of it.
const char *const BITABLE_DOMAINS[] = {"feishu.cn", "larksuite.com"};

//! Path segments that introduce the token identifying a Base.
const char *const BITABLE_PATH_KEYS[] = {"base", "wiki"};

enum class UrlParseStatus : uint8_t { SUCCESS, NOT_HTTP, FOREIGN_DOMAIN, NO_APP_TOKEN };

// This file stays free of DuckDB helpers beyond the exception types so that it
// can be compiled into the standalone unit test binary (see test/unit).

string ToLower(const string &value) {
	string result = value;
	for (size_t i = 0; i < result.size(); i++) {
		result[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(result[i])));
	}
	return result;
}

bool StartsWith(const string &value, const string &prefix) {
	return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(const string &value, const string &suffix) {
	return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<string> Split(const string &value, char delimiter) {
	std::vector<string> result;
	size_t begin = 0;
	while (true) {
		auto end = value.find(delimiter, begin);
		if (end == string::npos) {
			result.push_back(value.substr(begin));
			return result;
		}
		result.push_back(value.substr(begin, end - begin));
		begin = end + 1;
	}
}

bool HexValue(char character, uint8_t &result) {
	if (character >= '0' && character <= '9') {
		result = static_cast<uint8_t>(character - '0');
		return true;
	}
	if (character >= 'a' && character <= 'f') {
		result = static_cast<uint8_t>(character - 'a' + 10);
		return true;
	}
	if (character >= 'A' && character <= 'F') {
		result = static_cast<uint8_t>(character - 'A' + 10);
		return true;
	}
	return false;
}

//! Decodes percent escapes. Invalid escapes are kept verbatim so that a
//! malformed URL never loses characters. `decode_plus` applies the
//! application/x-www-form-urlencoded rule and is only used for query values.
string PercentDecode(const string &value, bool decode_plus) {
	string result;
	result.reserve(value.size());
	for (size_t i = 0; i < value.size(); i++) {
		auto character = value[i];
		uint8_t high, low;
		if (character == '%' && i + 2 < value.size() && HexValue(value[i + 1], high) && HexValue(value[i + 2], low)) {
			result.push_back(static_cast<char>((high << 4) | low));
			i += 2;
		} else if (decode_plus && character == '+') {
			result.push_back(' ');
		} else {
			result.push_back(character);
		}
	}
	return result;
}

string TrimWhitespace(const string &value) {
	size_t begin = 0;
	size_t end = value.size();
	while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) {
		begin++;
	}
	while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
		end--;
	}
	return value.substr(begin, end - begin);
}

bool HostIsBitableDomain(const string &host) {
	for (auto &domain : BITABLE_DOMAINS) {
		string suffix(domain);
		if (host == suffix || EndsWith(host, "." + suffix)) {
			return true;
		}
	}
	return false;
}

bool IsBitablePathKey(const string &segment) {
	for (auto &key : BITABLE_PATH_KEYS) {
		if (ToLower(segment) == key) {
			return true;
		}
	}
	return false;
}

//! Extracts `table` / `view` from a query string. Unknown parameters are ignored.
void ParseQueryString(const string &query, BitableUrlComponents &result) {
	for (auto &parameter : Split(query, '&')) {
		if (parameter.empty()) {
			continue;
		}
		auto separator = parameter.find('=');
		if (separator == string::npos) {
			continue;
		}
		auto key = ToLower(PercentDecode(parameter.substr(0, separator), true));
		auto value = PercentDecode(parameter.substr(separator + 1), true);
		if (value.empty()) {
			continue;
		}
		if (key == "table") {
			result.table_id = std::move(value);
		} else if (key == "view") {
			result.view_id = std::move(value);
		}
	}
}

UrlParseStatus TryParseBitableUrl(const string &input, BitableUrlComponents &result) {
	auto url = TrimWhitespace(input);
	auto lower_url = ToLower(url);
	size_t authority_begin;
	if (StartsWith(lower_url, "https://")) {
		authority_begin = 8;
	} else if (StartsWith(lower_url, "http://")) {
		authority_begin = 7;
	} else {
		return UrlParseStatus::NOT_HTTP;
	}

	// The authority runs until the first path, query or fragment delimiter.
	auto authority_end = url.find_first_of("/?#", authority_begin);
	auto authority =
	    url.substr(authority_begin, authority_end == string::npos ? string::npos : authority_end - authority_begin);
	auto userinfo_separator = authority.rfind('@');
	if (userinfo_separator != string::npos) {
		authority = authority.substr(userinfo_separator + 1);
	}
	auto port_separator = authority.rfind(':');
	if (port_separator != string::npos) {
		authority = authority.substr(0, port_separator);
	}
	auto host = ToLower(authority);
	if (!HostIsBitableDomain(host)) {
		return UrlParseStatus::FOREIGN_DOMAIN;
	}
	result.api_root =
	    host == "larksuite.com" || EndsWith(host, ".larksuite.com") ? LARK_BITABLE_API_ROOT : FEISHU_BITABLE_API_ROOT;

	auto remainder = authority_end == string::npos ? string() : url.substr(authority_end);
	// A fragment is client-side only and never carries Bitable parameters.
	auto fragment_begin = remainder.find('#');
	if (fragment_begin != string::npos) {
		remainder = remainder.substr(0, fragment_begin);
	}
	string path = remainder;
	string query;
	auto query_begin = remainder.find('?');
	if (query_begin != string::npos) {
		path = remainder.substr(0, query_begin);
		query = remainder.substr(query_begin + 1);
	}

	auto segments = Split(path, '/');
	for (size_t i = 0; i + 1 < segments.size(); i++) {
		if (!IsBitablePathKey(segments[i])) {
			continue;
		}
		auto app_token = PercentDecode(segments[i + 1], false);
		if (app_token.empty()) {
			continue;
		}
		result.app_token = std::move(app_token);
		result.is_wiki = ToLower(segments[i]) == "wiki";
		ParseQueryString(query, result);
		return UrlParseStatus::SUCCESS;
	}
	return UrlParseStatus::NO_APP_TOKEN;
}

} // namespace

bool HasHttpScheme(const string &input) {
	auto lower_input = ToLower(TrimWhitespace(input));
	return StartsWith(lower_input, "https://") || StartsWith(lower_input, "http://");
}

bool IsBitableUrl(const string &input) {
	BitableUrlComponents components;
	return TryParseBitableUrl(input, components) == UrlParseStatus::SUCCESS;
}

BitableUrlComponents ParseBitableUrl(const string &url, const string &function_name) {
	BitableUrlComponents result;
	switch (TryParseBitableUrl(url, result)) {
	case UrlParseStatus::SUCCESS:
		return result;
	case UrlParseStatus::NOT_HTTP:
		throw InvalidInputException(function_name +
		                            ": input must start with http:// or https:// to be parsed as a Bitable URL");
	case UrlParseStatus::FOREIGN_DOMAIN:
		// Browser URLs can contain credentials in any component. Keep the
		// diagnostic actionable without copying untrusted input into errors.
		throw InvalidInputException(function_name +
		                            ": input is not hosted on a Feishu or Lark domain (expected *.feishu.cn or "
		                            "*.larksuite.com)");
	default:
		throw InvalidInputException(function_name +
		                            ": cannot extract an app_token from input (expected a /base/<app_token> path)");
	}
}

BitableUrlComponents ResolveBitableTarget(const string &source, const string &explicit_table,
                                          const string &explicit_view, const string &function_name) {
	BitableUrlComponents result;
	if (HasHttpScheme(source)) {
		result = ParseBitableUrl(source, function_name);
	} else {
		result.app_token = source;
	}
	if (!explicit_table.empty()) {
		result.table_id = explicit_table;
	}
	if (!explicit_view.empty()) {
		result.view_id = explicit_view;
	}
	return result;
}

} // namespace feishu
} // namespace duckdb
