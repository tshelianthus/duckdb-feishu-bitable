// Minimal stubs for the DuckDB types used by the unit tests.
// These avoid linking the full DuckDB library for fast unit test builds, and
// are the reason the units under test only ever throw exceptions built from a
// plain string (never from a printf-style format).
//
// Linux CI (libstdc++) emits inline helpers such as CreateRelationAlias from
// duckdb.hpp transitive includes; macOS clang often DCE's them. Provide the
// few symbols those helpers reference so the standalone unit binary links.

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/enums/relation_type.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception_format_value.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/timestamp.hpp"

#include "bitable/api_client.hpp"

#include <chrono>
#include <utility>
#include <vector>

namespace duckdb {

Exception::Exception(ExceptionType exception_type, const string &message) : std::runtime_error(message) {
}

Exception::Exception(const unordered_map<string, string> &extra_info, ExceptionType exception_type,
                     const string &message)
    : std::runtime_error(message) {
}

string Exception::ExceptionTypeToString(ExceptionType type) {
	return "UNKNOWN";
}

string Exception::ConstructMessageRecursive(const string &msg, std::vector<ExceptionFormatValue> &values) {
	(void)values;
	return msg;
}

IOException::IOException(const string &msg) : Exception(ExceptionType::IO, msg) {
}

IOException::IOException(const unordered_map<string, string> &extra_info, const string &msg)
    : Exception(extra_info, ExceptionType::IO, msg) {
}

InvalidInputException::InvalidInputException(const string &msg) : Exception(ExceptionType::INVALID_INPUT, msg) {
}

InvalidInputException::InvalidInputException(const unordered_map<string, string> &extra_info, const string &msg)
    : Exception(extra_info, ExceptionType::INVALID_INPUT, msg) {
}

ExceptionFormatValue::ExceptionFormatValue(string str_val_p)
    : type(ExceptionFormatValueType::FORMAT_VALUE_TYPE_STRING), dbl_val(0), int_val(0, 0),
      str_val(std::move(str_val_p)) {
}

template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const string &value) {
	return ExceptionFormatValue(string(value));
}

string StringUtil::GenerateRandomName(idx_t length) {
	return string(static_cast<size_t>(length), 'x');
}

template <>
const char *EnumUtil::ToChars<RelationType>(RelationType) {
	return "RELATION";
}

timestamp_t Timestamp::GetCurrentTimestamp() {
	auto now = std::chrono::system_clock::now();
	auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
	return timestamp_t(micros);
}

namespace feishu {

ApiClient::ApiClient(ClientContext &context_p) : context(context_p) {
}

HttpResponse ApiClient::PostJson(const string &, const string &, const HeaderMap &) {
	throw IOException("ApiClient network access is disabled in standalone unit tests");
}

} // namespace feishu

} // namespace duckdb
