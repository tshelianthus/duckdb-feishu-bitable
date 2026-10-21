#include "bitable/write_value_encoding.hpp"

#include "bitable/field_type.hpp"

#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <utility>

namespace duckdb {
namespace feishu {

namespace {

//! Decimal digits needed to round-trip an IEEE-754 double through JSON.
constexpr int DOUBLE_JSON_PRECISION = 17;

string FormatNumber(double value) {
	std::ostringstream stream;
	// JSON always uses `.` as its decimal separator, independently of the
	// process-global locale inherited by this extension.
	stream.imbue(std::locale::classic());
	stream << std::setprecision(DOUBLE_JSON_PRECISION) << std::defaultfloat << value;
	return stream.str();
}

bool IsCanonicalInteger(const string &value) {
	if (value.empty()) {
		return false;
	}
	idx_t offset = value[0] == '-' ? 1 : 0;
	if (offset == value.size()) {
		return false;
	}
	if (value[offset] == '0') {
		return offset + 1 == value.size() && offset == 0;
	}
	for (idx_t i = offset; i < value.size(); i++) {
		if (value[i] < '0' || value[i] > '9') {
			return false;
		}
	}
	return true;
}

//! Section 8.3 permits integer values only when conversion to DOUBLE is
//! lossless. Round-tripping through fixed decimal notation checks that rule
//! without depending on DuckDB's HUGEINT implementation.
bool IsLosslesslyRepresentableAsDouble(const string &value) {
	if (!IsCanonicalInteger(value)) {
		return false;
	}
	std::istringstream input(value);
	input.imbue(std::locale::classic());
	double converted;
	input >> converted;
	if (!input || !input.eof() || !std::isfinite(converted)) {
		return false;
	}
	std::ostringstream round_trip;
	round_trip.imbue(std::locale::classic());
	round_trip << std::fixed << std::setprecision(0) << converted;
	return round_trip.str() == value;
}

} // namespace

WriteTargetKind ClassifyWriteTarget(int32_t type_id) {
	switch (type_id) {
	case static_cast<int32_t>(FeishuFieldTypeId::TEXT):
	case static_cast<int32_t>(FeishuFieldTypeId::PHONE):
	case static_cast<int32_t>(FeishuFieldTypeId::BARCODE):
	case static_cast<int32_t>(FeishuFieldTypeId::SINGLE_SELECT):
	case static_cast<int32_t>(FeishuFieldTypeId::EMAIL):
		return WriteTargetKind::TEXT;
	case static_cast<int32_t>(FeishuFieldTypeId::URL):
		return WriteTargetKind::URL;
	case static_cast<int32_t>(FeishuFieldTypeId::NUMBER):
	case static_cast<int32_t>(FeishuFieldTypeId::PROGRESS):
	case static_cast<int32_t>(FeishuFieldTypeId::CURRENCY):
	case static_cast<int32_t>(FeishuFieldTypeId::RATING):
		return WriteTargetKind::NUMBER;
	case static_cast<int32_t>(FeishuFieldTypeId::CHECKBOX):
		return WriteTargetKind::CHECKBOX;
	case static_cast<int32_t>(FeishuFieldTypeId::DATE_TIME):
		return WriteTargetKind::DATE_TIME;
	case static_cast<int32_t>(FeishuFieldTypeId::MULTI_SELECT):
		return WriteTargetKind::MULTI_SELECT;
	case static_cast<int32_t>(FeishuFieldTypeId::LINK):
	case static_cast<int32_t>(FeishuFieldTypeId::DUPLEX_LINK):
		return WriteTargetKind::LINK;
	case static_cast<int32_t>(FeishuFieldTypeId::USER):
		return WriteTargetKind::USER;
	case static_cast<int32_t>(FeishuFieldTypeId::FORMULA):
	case static_cast<int32_t>(FeishuFieldTypeId::CREATED_TIME):
	case static_cast<int32_t>(FeishuFieldTypeId::MODIFIED_TIME):
	case static_cast<int32_t>(FeishuFieldTypeId::CREATED_USER):
	case static_cast<int32_t>(FeishuFieldTypeId::MODIFIED_USER):
	case static_cast<int32_t>(FeishuFieldTypeId::AUTO_NUMBER):
		return WriteTargetKind::READ_ONLY;
	case static_cast<int32_t>(FeishuFieldTypeId::ATTACHMENT):
	case static_cast<int32_t>(FeishuFieldTypeId::LOCATION):
	case static_cast<int32_t>(FeishuFieldTypeId::GROUP_CHAT):
		return WriteTargetKind::UNSUPPORTED;
	default:
		return WriteTargetKind::UNKNOWN;
	}
}

WriteScalar WriteScalar::Null() {
	return WriteScalar();
}

WriteScalar WriteScalar::Text(string value) {
	WriteScalar result;
	result.kind = WriteScalarKind::TEXT;
	result.text = std::move(value);
	return result;
}

WriteScalar WriteScalar::Number(double value) {
	WriteScalar result;
	result.kind = WriteScalarKind::NUMBER;
	result.number = value;
	return result;
}

WriteScalar WriteScalar::Integer(string value) {
	WriteScalar result;
	result.kind = WriteScalarKind::INTEGER;
	result.text = std::move(value);
	return result;
}

WriteScalar WriteScalar::Boolean(bool value) {
	WriteScalar result;
	result.kind = WriteScalarKind::BOOLEAN;
	result.boolean = value;
	return result;
}

WriteScalar WriteScalar::TimestampMicros(int64_t value) {
	WriteScalar result;
	result.kind = WriteScalarKind::TIMESTAMP_MICROS;
	result.micros = value;
	return result;
}

WriteScalar WriteScalar::List(std::vector<string> values, std::vector<bool> has_null) {
	WriteScalar result;
	result.kind = WriteScalarKind::STRING_LIST;
	result.list = std::move(values);
	result.list_has_null = std::move(has_null);
	return result;
}

WriteBatchAccumulator::WriteBatchAccumulator(idx_t batch_size_p) : batch_size(batch_size_p) {
	if (batch_size == 0) {
		throw InvalidInputException("feishu_bitable: write batch size must be greater than zero");
	}
	pending_rows.reserve(batch_size);
}

bool WriteBatchAccumulator::AddRow(string row, WriteBatch &ready) {
	pending_rows.push_back(std::move(row));
	rows_seen++;
	if (pending_rows.size() < batch_size) {
		return false;
	}
	return Flush(ready);
}

bool WriteBatchAccumulator::Flush(WriteBatch &ready) {
	if (pending_rows.empty()) {
		return false;
	}
	ready.first_row = rows_seen - pending_rows.size();
	ready.rows = std::move(pending_rows);
	pending_rows.clear();
	pending_rows.reserve(batch_size);
	return true;
}

idx_t WriteBatchAccumulator::PendingRows() const {
	return pending_rows.size();
}

idx_t WriteBatchAccumulator::RowsSeen() const {
	return rows_seen;
}

string EncodeJsonString(const string &value) {
	static const char *HEX = "0123456789abcdef";
	string result;
	result.reserve(value.size() + 2);
	result.push_back('"');
	for (idx_t i = 0; i < value.size(); i++) {
		auto ch = static_cast<unsigned char>(value[i]);
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
	result.push_back('"');
	return result;
}

string PercentEncodeUrlComponent(const string &value) {
	static const char *HEX = "0123456789ABCDEF";
	string result;
	result.reserve(value.size());
	for (idx_t i = 0; i < value.size(); i++) {
		auto byte = static_cast<unsigned char>(value[i]);
		if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
		    byte == '-' || byte == '_' || byte == '.' || byte == '~') {
			result.push_back(static_cast<char>(byte));
		} else {
			result.push_back('%');
			result.push_back(HEX[(byte >> 4) & 0x0F]);
			result.push_back(HEX[byte & 0x0F]);
		}
	}
	return result;
}

string BuildRecordsBatchCreateUrl(const string &api_root, const string &app_token, const string &table_id,
                                  const string &user_id_type) {
	return api_root + "/apps/" + PercentEncodeUrlComponent(app_token) + "/tables/" +
	       PercentEncodeUrlComponent(table_id) +
	       "/records/batch_create?user_id_type=" + PercentEncodeUrlComponent(user_id_type);
}

bool EncodeWriteValue(const WriteScalar &value, WriteTargetKind target, string &out) {
	out.clear();
	// Section 8.5 is a bind-time contract. Keep the scalar encoder defensive so
	// even SQL NULL cannot accidentally bypass that classification.
	switch (target) {
	case WriteTargetKind::READ_ONLY:
		throw InvalidInputException("feishu_bitable: refusing to encode a read-only Feishu field");
	case WriteTargetKind::UNSUPPORTED:
		throw InvalidInputException("feishu_bitable: refusing to encode a Feishu field type v0.2 does not support");
	case WriteTargetKind::UNKNOWN:
		throw InvalidInputException("feishu_bitable: refusing to encode an unknown Feishu field type");
	default:
		break;
	}
	if (value.kind == WriteScalarKind::NULL_VALUE) {
		// Section 8.4: SQL NULL omits the field from the request entirely.
		return false;
	}
	switch (target) {
	case WriteTargetKind::TEXT:
		if (value.kind != WriteScalarKind::TEXT) {
			throw InvalidInputException("feishu_bitable: non-text value cannot be written to a Feishu text field");
		}
		// The empty string is a legitimate text value (section 8.4).
		out = EncodeJsonString(value.text);
		return true;
	case WriteTargetKind::URL:
		if (value.kind != WriteScalarKind::TEXT) {
			throw InvalidInputException("feishu_bitable: non-text value cannot be written to a Feishu URL field");
		}
		// The records API represents a URL as a typed object. The SQL contract
		// intentionally exposes only its display/link string, so both members
		// use the same VARCHAR value on write.
		out = "{\"link\":" + EncodeJsonString(value.text) + ",\"text\":" + EncodeJsonString(value.text) + "}";
		return true;
	case WriteTargetKind::NUMBER:
		if (value.kind == WriteScalarKind::INTEGER) {
			if (!IsLosslesslyRepresentableAsDouble(value.text)) {
				throw InvalidInputException(
				    "feishu_bitable: integer cannot be losslessly converted to a Feishu number field");
			}
			out = value.text;
			return true;
		}
		if (value.kind != WriteScalarKind::NUMBER) {
			throw InvalidInputException("feishu_bitable: non-numeric value cannot be written to a Feishu number field");
		}
		if (!std::isfinite(value.number)) {
			throw InvalidInputException("feishu_bitable: NaN and infinity cannot be written to a Feishu number field");
		}
		out = FormatNumber(value.number);
		return true;
	case WriteTargetKind::CHECKBOX:
		if (value.kind != WriteScalarKind::BOOLEAN) {
			throw InvalidInputException(
			    "feishu_bitable: non-boolean value cannot be written to a Feishu checkbox field");
		}
		out = value.boolean ? string("true") : string("false");
		return true;
	case WriteTargetKind::DATE_TIME: {
		if (value.kind != WriteScalarKind::TIMESTAMP_MICROS) {
			throw InvalidInputException("feishu_bitable: non-timestamp value cannot be written to a Feishu date field");
		}
		// Section 8.3: TIMESTAMP is interpreted as UTC; TIMESTAMPTZ was already
		// normalized to UTC upstream. Anything before the epoch is rejected
		// explicitly: truncating division alone would fold -1us onto millis = 0.
		if (value.micros < 0) {
			throw InvalidInputException(
			    "feishu_bitable: timestamp is outside the range supported by Feishu date fields");
		}
		auto millis = value.micros / 1000;
		if (millis > FEISHU_MAX_TIMESTAMP_MILLIS) {
			throw InvalidInputException(
			    "feishu_bitable: timestamp is outside the range supported by Feishu date fields");
		}
		out = std::to_string(millis);
		return true;
	}
	case WriteTargetKind::MULTI_SELECT: {
		if (value.kind != WriteScalarKind::STRING_LIST) {
			throw InvalidInputException(
			    "feishu_bitable: non-list value cannot be written to a Feishu multi-select field");
		}
		if (value.list_has_null.size() != value.list.size()) {
			throw InvalidInputException("feishu_bitable: malformed list value during writeback encoding");
		}
		string encoded = "[";
		for (idx_t i = 0; i < value.list.size(); i++) {
			if (value.list_has_null[i]) {
				// Section 8.4: NULL elements are rejected, never dropped.
				throw InvalidInputException("feishu_bitable: NULL list elements cannot be written to a Feishu "
				                            "multi-select field");
			}
			if (i > 0) {
				encoded.push_back(',');
			}
			encoded += EncodeJsonString(value.list[i]);
		}
		encoded.push_back(']');
		out = std::move(encoded);
		return true;
	}
	case WriteTargetKind::LINK: {
		if (value.kind != WriteScalarKind::STRING_LIST) {
			throw InvalidInputException("feishu_bitable: non-list value cannot be written to a Feishu link field");
		}
		if (value.list_has_null.size() != value.list.size()) {
			throw InvalidInputException("feishu_bitable: malformed list value during writeback encoding");
		}
		string encoded = "[";
		for (idx_t i = 0; i < value.list.size(); i++) {
			if (value.list_has_null[i]) {
				throw InvalidInputException(
				    "feishu_bitable: NULL list elements cannot be written to a Feishu link field");
			}
			if (value.list[i].empty()) {
				// Section 8.4: empty-string elements are rejected for LINK / USER.
				throw InvalidInputException(
				    "feishu_bitable: empty-string list elements cannot be written to a Feishu link field");
			}
			if (i > 0) {
				encoded.push_back(',');
			}
			encoded += EncodeJsonString(value.list[i]);
		}
		encoded.push_back(']');
		out = std::move(encoded);
		return true;
	}
	case WriteTargetKind::USER: {
		if (value.kind != WriteScalarKind::STRING_LIST) {
			throw InvalidInputException("feishu_bitable: non-list value cannot be written to a Feishu user field");
		}
		if (value.list_has_null.size() != value.list.size()) {
			throw InvalidInputException("feishu_bitable: malformed list value during writeback encoding");
		}
		// Always [{"id":"..."},...], never a plain string array (I-3 / section 8.4).
		string encoded = "[";
		for (idx_t i = 0; i < value.list.size(); i++) {
			if (value.list_has_null[i]) {
				throw InvalidInputException(
				    "feishu_bitable: NULL list elements cannot be written to a Feishu user field");
			}
			if (value.list[i].empty()) {
				throw InvalidInputException(
				    "feishu_bitable: empty-string list elements cannot be written to a Feishu user field");
			}
			if (i > 0) {
				encoded.push_back(',');
			}
			encoded += "{\"id\":" + EncodeJsonString(value.list[i]) + "}";
		}
		encoded.push_back(']');
		out = std::move(encoded);
		return true;
	}
	default:
		throw InvalidInputException("feishu_bitable: invalid write target kind");
	}
}

string EncodeFieldsEntry(const string &field_name, const WriteScalar &value, WriteTargetKind target) {
	string encoded;
	if (!EncodeWriteValue(value, target, encoded)) {
		return string();
	}
	return EncodeJsonString(field_name) + ":" + encoded;
}

} // namespace feishu
} // namespace duckdb
