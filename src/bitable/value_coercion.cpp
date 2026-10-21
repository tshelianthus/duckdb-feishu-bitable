#include "bitable/value_coercion.hpp"

#include "bitable/api_response.hpp"
#include "duckdb/common/exception.hpp"

#include <cmath>
#include <cstdlib>
#include <limits>

namespace duckdb {
namespace feishu {

using namespace duckdb_yyjson; // NOLINT

namespace {

//! Lowercases ASCII in place; the accepted boolean spellings are all ASCII, and
//! StringUtil is off limits in a self-contained translation unit.
string AsciiLower(const string &input) {
	string result = input;
	for (size_t i = 0; i < result.size(); i++) {
		auto character = static_cast<unsigned char>(result[i]);
		if (character >= 'A' && character <= 'Z') {
			result[i] = static_cast<char>(character - 'A' + 'a');
		}
	}
	return result;
}

//! Timestamp fields carry epoch milliseconds, so they must be exact integers.
//! JsonInt64 deliberately accepts a broader set of values for API metadata;
//! accepting its fractional/string-prefix forms here would silently change the
//! represented instant.
bool CoerceToIntegralMilliseconds(yyjson_val *value, int64_t &result) {
	if (!value || yyjson_is_null(value)) {
		return false;
	}
	if (yyjson_is_sint(value)) {
		result = yyjson_get_sint(value);
		return true;
	}
	if (yyjson_is_uint(value)) {
		auto number = yyjson_get_uint(value);
		if (number > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
			return false;
		}
		result = static_cast<int64_t>(number);
		return true;
	}
	if (yyjson_is_real(value)) {
		auto number = yyjson_get_real(value);
		if (!std::isfinite(number) || std::trunc(number) != number ||
		    number < static_cast<double>(std::numeric_limits<int64_t>::min()) ||
		    number >= static_cast<double>(std::numeric_limits<int64_t>::max())) {
			return false;
		}
		result = static_cast<int64_t>(number);
		return true;
	}
	if (!yyjson_is_str(value)) {
		return false;
	}
	try {
		auto text = JsonString(value);
		size_t consumed = 0;
		auto number = std::stoll(text, &consumed);
		if (consumed != text.size()) {
			return false;
		}
		result = number;
		return true;
	} catch (const std::exception &) {
		return false;
	}
}

} // namespace

string SerializeJsonValue(yyjson_val *value) {
	if (!value) {
		return string();
	}
	size_t length = 0;
	auto serialized = yyjson_val_write(value, 0, &length);
	if (!serialized) {
		throw IOException("feishu_bitable: failed to serialize a Feishu field value");
	}
	string result(serialized, length);
	std::free(serialized);
	return result;
}

string CoerceToString(yyjson_val *value) {
	if (!value || yyjson_is_null(value)) {
		return string();
	}
	if (yyjson_is_str(value)) {
		return JsonString(value);
	}
	if (yyjson_is_obj(value)) {
		const char *preferred_keys[] = {"text", "name", "link", "value"};
		for (size_t i = 0; i < sizeof(preferred_keys) / sizeof(preferred_keys[0]); i++) {
			auto child = yyjson_obj_get(value, preferred_keys[i]);
			if (child && yyjson_is_str(child)) {
				return JsonString(child);
			}
		}
		return SerializeJsonValue(value);
	}
	if (yyjson_is_arr(value)) {
		// Text-like fields arrive as rich text segments: [{"type":"text","text":"a"}, ...].
		string result;
		yyjson_arr_iter iterator;
		yyjson_arr_iter_init(value, &iterator);
		yyjson_val *child;
		while ((child = yyjson_arr_iter_next(&iterator))) {
			result += CoerceToString(child);
		}
		return result;
	}
	return SerializeJsonValue(value);
}

bool CoerceToDouble(yyjson_val *value, double &result) {
	if (!value || yyjson_is_null(value)) {
		return false;
	}
	if (yyjson_is_num(value)) {
		auto number = yyjson_get_num(value);
		if (!std::isfinite(number)) {
			return false;
		}
		result = number;
		return true;
	}
	if (yyjson_is_str(value)) {
		try {
			size_t consumed = 0;
			auto text = JsonString(value);
			auto number = std::stod(text, &consumed);
			if (consumed != text.size() || !std::isfinite(number)) {
				return false;
			}
			result = number;
			return true;
		} catch (const std::exception &) {
			return false;
		}
	}
	return false;
}

bool CoerceToBoolean(yyjson_val *value, bool &result) {
	if (!value || yyjson_is_null(value)) {
		return false;
	}
	if (yyjson_is_bool(value)) {
		result = yyjson_get_bool(value);
		return true;
	}
	if (yyjson_is_num(value)) {
		result = yyjson_get_num(value) != 0;
		return true;
	}
	if (yyjson_is_str(value)) {
		auto text = AsciiLower(JsonString(value));
		if (text == "true" || text == "1") {
			result = true;
			return true;
		}
		if (text == "false" || text == "0") {
			result = false;
			return true;
		}
	}
	return false;
}

bool CoerceToTimestampMicros(yyjson_val *value, int64_t &result) {
	int64_t milliseconds;
	if (!CoerceToIntegralMilliseconds(value, milliseconds)) {
		return false;
	}
	if (milliseconds > std::numeric_limits<int64_t>::max() / 1000 ||
	    milliseconds < std::numeric_limits<int64_t>::min() / 1000) {
		return false;
	}
	result = milliseconds * 1000;
	return true;
}

bool CoerceToStringList(yyjson_val *value, std::vector<CoercedListEntry> &result) {
	result.clear();
	if (!value || yyjson_is_null(value)) {
		return false;
	}
	if (!yyjson_is_arr(value)) {
		// A single select style scalar still forms a one element list.
		CoercedListEntry entry;
		entry.value = CoerceToString(value);
		result.push_back(std::move(entry));
		return true;
	}
	result.reserve(yyjson_arr_size(value));
	yyjson_arr_iter iterator;
	yyjson_arr_iter_init(value, &iterator);
	yyjson_val *child;
	while ((child = yyjson_arr_iter_next(&iterator))) {
		CoercedListEntry entry;
		if (!child || yyjson_is_null(child)) {
			entry.is_null = true;
		} else {
			entry.value = CoerceToString(child);
		}
		result.push_back(std::move(entry));
	}
	return true;
}

} // namespace feishu
} // namespace duckdb
