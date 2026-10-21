#pragma once

#include "bitable/write_state_machine.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace duckdb {
namespace feishu {

enum class WriteRetryResult { RETRY_SAFE, UNKNOWN_RESULT, FAIL };

// Connection/DNS/TLS failures mean failure BEFORE the request was sent. A
// generic socket error after sending must be CONNECTION_INTERRUPTED or UNKNOWN.
enum class WriteTransportError {
	NONE,
	CONNECTION_FAILURE,
	DNS_FAILURE,
	TLS_HANDSHAKE_FAILURE,
	READ_TIMEOUT,
	CONNECTION_INTERRUPTED,
	RESPONSE_PARSE_FAILURE,
	INVALID_RESPONSE,
	UNKNOWN
};

// Classify failed write attempts only; the caller must validate successful
// responses separately. HTTP 0 means no status, nullopt means no parsed business
// code, and code 0 alone is not proof of a valid success payload. INVALID_RESPONSE
// includes a 2xx response whose structure/record count fails validation.
// Transport uncertainty and 5xx take precedence over apparent rate-limit codes.
// This function neither retries nor advances confirmed write progress.
WriteRetryResult ClassifyWriteRetry(int http_status, std::optional<int64_t> feishu_code = std::nullopt,
                                    WriteTransportError transport_error = WriteTransportError::NONE) noexcept;

// Classifies the actual response envelope, including signed business codes.
// The caller must redact detail before exposing it and supply Retry-After from
// the HTTP headers. No response body is included in malformed-response errors.
WriteAttemptOutcome ClassifyWriteResponse(int http_status, const std::string &body, size_t expected_rows,
                                          WriteTransportError transport_error = WriteTransportError::NONE);

} // namespace feishu
} // namespace duckdb
