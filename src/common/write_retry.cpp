#include "bitable/write_retry.hpp"

#include "bitable/api_response.hpp"
#include "bitable/copy_function.hpp"
#include "yyjson.hpp"

#include <limits>
#include <unordered_set>

namespace duckdb {
namespace feishu {

bool ResponseConfirmsWriteBatch(const std::string &body, size_t expected_rows) {
	using namespace duckdb_yyjson; // NOLINT
	try {
		JsonDocument json(body);
		auto root = json.Root();
		auto code = yyjson_is_obj(root) ? yyjson_obj_get(root, "code") : nullptr;
		auto data = yyjson_is_obj(root) ? yyjson_obj_get(root, "data") : nullptr;
		auto records = yyjson_is_obj(data) ? yyjson_obj_get(data, "records") : nullptr;
		if (!yyjson_is_int(code) || yyjson_get_sint(code) != 0 || !yyjson_is_arr(records) || expected_rows == 0 ||
		    yyjson_arr_size(records) != expected_rows) {
			return false;
		}
		std::unordered_set<std::string> record_ids;
		for (size_t i = 0; i < expected_rows; i++) {
			auto record = yyjson_arr_get(records, i);
			auto id = yyjson_is_obj(record) ? yyjson_obj_get(record, "record_id") : nullptr;
			if (!yyjson_is_str(id) || yyjson_get_len(id) == 0 ||
			    !record_ids.emplace(yyjson_get_str(id), yyjson_get_len(id)).second) {
				return false;
			}
		}
		return true;
	} catch (const Exception &) {
		return false;
	}
}

WriteAttemptOutcome ClassifyWriteResponse(int http_status, const std::string &body, size_t expected_rows,
                                          WriteTransportError transport_error) {
	using namespace duckdb_yyjson; // NOLINT
	std::optional<int64_t> business_code;
	std::string message;
	try {
		JsonDocument json(body);
		auto root = json.Root();
		auto code = yyjson_is_obj(root) ? yyjson_obj_get(root, "code") : nullptr;
		// The shared read-side error parser only accepts positive codes. Writes
		// must preserve every nonzero signed code as a definite rejection.
		if (yyjson_is_sint(code)) {
			business_code = yyjson_get_sint(code);
		} else if (yyjson_is_uint(code) &&
		           yyjson_get_uint(code) <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
			business_code = static_cast<int64_t>(yyjson_get_uint(code));
		}
		auto msg = yyjson_is_obj(root) ? yyjson_obj_get(root, "msg") : nullptr;
		if (yyjson_is_str(msg)) {
			message.assign(yyjson_get_str(msg), yyjson_get_len(msg));
		}
	} catch (const Exception &) {
		// An unreadable body cannot prove success; the status/transport still
		// determines whether there was a definite rejection (e.g. HTTP 429).
	}
	WriteAttemptOutcome outcome;
	if (transport_error == WriteTransportError::NONE && http_status >= 200 && http_status < 300 && business_code &&
	    *business_code == 0 && ResponseConfirmsWriteBatch(body, expected_rows)) {
		outcome.result = WriteAttemptResult::CONFIRMED;
		return outcome;
	}
	switch (ClassifyWriteRetry(http_status, business_code, transport_error)) {
	case WriteRetryResult::RETRY_SAFE:
		outcome.result = WriteAttemptResult::RETRY_SAFE;
		break;
	case WriteRetryResult::FAIL:
		outcome.result = WriteAttemptResult::FAIL;
		outcome.detail = business_code && *business_code != 0
		                     ? "Feishu API returned code " + std::to_string(*business_code) + ": " + message
		                     : "Feishu API rejected the request (HTTP " + std::to_string(http_status) + ")";
		break;
	case WriteRetryResult::UNKNOWN_RESULT:
		outcome.result = WriteAttemptResult::UNKNOWN_RESULT;
		outcome.detail = "the response body could not be validated";
		break;
	}
	return outcome;
}

WriteRetryResult ClassifyWriteRetry(int http_status, std::optional<int64_t> feishu_code,
                                    WriteTransportError transport_error) noexcept {
	// .specs/06_WRITEBACK_RESEARCH.md section 1.4: batch_create has no
	// documented usable idempotency key or server-side deduplication guarantee.
	// Replaying after a timeout, disconnect, invalid response or 5xx could create
	// duplicates if the original request committed. Do not reuse read-side
	// WithRetry classification: UNKNOWN_RESULT must stop automatic replay, even
	// when a 5xx body happens to contain a rate-limit code.
	switch (transport_error) {
	case WriteTransportError::CONNECTION_FAILURE:
	case WriteTransportError::DNS_FAILURE:
	case WriteTransportError::TLS_HANDSHAKE_FAILURE:
		// A received status/code contradicts the claim that nothing was sent.
		return http_status == 0 && !feishu_code ? WriteRetryResult::RETRY_SAFE : WriteRetryResult::UNKNOWN_RESULT;
	case WriteTransportError::NONE:
		break;
	default:
		return WriteRetryResult::UNKNOWN_RESULT;
	}
	if (http_status >= 500 && http_status <= 599) {
		return WriteRetryResult::UNKNOWN_RESULT;
	}
	if (http_status < 200 || http_status >= 600) {
		return WriteRetryResult::UNKNOWN_RESULT;
	}
	// FEISHU_RATE_LIMIT_CODE from common/retry.hpp. The classifier itself needs
	// no DuckDB runtime types or read-side retry implementation.
	constexpr int64_t RATE_LIMIT_CODE = 99991400;
	if (http_status == 429 || (feishu_code && *feishu_code == RATE_LIMIT_CODE)) {
		return WriteRetryResult::RETRY_SAFE;
	}
	if (http_status >= 400 || (feishu_code && *feishu_code != 0)) {
		return WriteRetryResult::FAIL;
	}
	// No confirmed rejection or valid success: never infer permission to replay.
	return WriteRetryResult::UNKNOWN_RESULT;
}

} // namespace feishu
} // namespace duckdb
