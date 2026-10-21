#include "catch.hpp"

#include "bitable/write_retry.hpp"
#include "bitable/copy_function.hpp"
#include "bitable/write_state_machine.hpp"
#include "common/retry.hpp"

using duckdb::feishu::ClassifyWriteRetry;
using duckdb::feishu::FEISHU_RATE_LIMIT_CODE;
using duckdb::feishu::WriteRetryResult;
using duckdb::feishu::WriteTransportError;

TEST_CASE("Write response classification preserves signed business rejections", "[write_retry]") {
	using namespace duckdb::feishu;
	for (const auto &code : {"-1", "-9223372036854775808", "1254005"}) {
		auto outcome = ClassifyWriteResponse(200, std::string("{\"code\":") + code + ",\"msg\":\"rejected\"}", 1);
		CHECK(outcome.result == WriteAttemptResult::FAIL);
		CHECK(outcome.detail.find(std::string("code ") + code + ": rejected") != std::string::npos);
	}
}

TEST_CASE("Write response envelopes cannot override transport uncertainty", "[write_retry]") {
	using namespace duckdb::feishu;
	const std::string success = R"({"code":0,"data":{"records":[{"record_id":"recXXXX"}]}})";
	CHECK(ClassifyWriteResponse(200, success, 1).result == WriteAttemptResult::CONFIRMED);
	CHECK(ClassifyWriteResponse(200, success, 2).result == WriteAttemptResult::UNKNOWN_RESULT);
	CHECK(ClassifyWriteResponse(200, success, 1, WriteTransportError::READ_TIMEOUT).result ==
	      WriteAttemptResult::UNKNOWN_RESULT);
	for (const auto &body : {R"({"code":99991400})", R"({"code":-1})"}) {
		CHECK(ClassifyWriteResponse(503, body, 1).result == WriteAttemptResult::UNKNOWN_RESULT);
		CHECK(ClassifyWriteResponse(200, body, 1, WriteTransportError::CONNECTION_INTERRUPTED).result ==
		      WriteAttemptResult::UNKNOWN_RESULT);
	}
	CHECK(ClassifyWriteResponse(200, R"({"code":99991400})", 1).result == WriteAttemptResult::RETRY_SAFE);
	CHECK(ClassifyWriteResponse(429, "not JSON", 1).result == WriteAttemptResult::RETRY_SAFE);
	for (int status : {400, 401, 403, 404}) {
		CHECK(ClassifyWriteResponse(status, "not JSON", 1).result == WriteAttemptResult::FAIL);
	}
	for (const auto &body : {"not JSON", R"({"code":-1.0})", R"({"code":"-1"})", R"({"code":9223372036854775808})",
	                         R"({"code":0,"data":{"records":[null]}})"}) {
		CHECK(ClassifyWriteResponse(200, body, 1).result == WriteAttemptResult::UNKNOWN_RESULT);
	}
}

TEST_CASE("Write confirmation requires distinct valid records, not only a matching count", "[write_retry]") {
	using duckdb::feishu::ResponseConfirmsWriteBatch;
	CHECK(ResponseConfirmsWriteBatch(R"({"code":0,"data":{"records":[{"record_id":"recXXXX"}]}})", 1));
	CHECK(ResponseConfirmsWriteBatch(
	    R"({"code":0,"data":{"records":[{"record_id":"recXXXX"},{"record_id":"recYYYY"}]}})", 2));
	for (const auto &body :
	     {R"({"code":0,"data":{"records":[null]}})", R"({"code":0,"data":{"records":[1]}})",
	      R"({"code":0,"data":{"records":[{}]}})", R"({"code":0,"data":{"records":[{"record_id":null}]}})",
	      R"({"code":0,"data":{"records":[{"record_id":1}]}})", R"({"code":0,"data":{"records":[{"record_id":""}]}})",
	      R"({"code":"0","data":{"records":[{"record_id":"recXXXX"}]}})",
	      R"({"code":0.0,"data":{"records":[{"record_id":"recXXXX"}]}})",
	      R"({"code":1254005,"data":{"records":[{"record_id":"recXXXX"}]}})", R"({"code":0,"data":{"records":[]}})",
	      R"({"code":0,"data":{}})", "not JSON"}) {
		INFO(body);
		CHECK_FALSE(ResponseConfirmsWriteBatch(body, 1));
	}
	CHECK_FALSE(ResponseConfirmsWriteBatch(
	    R"({"code":0,"data":{"records":[{"record_id":"recXXXX"},{"record_id":"recXXXX"}]}})", 2));
	CHECK_FALSE(ResponseConfirmsWriteBatch(R"({"code":0,"data":{"records":[]}})", 0));
}

TEST_CASE("Malformed success records terminate write attempts without advancing progress", "[write_retry]") {
	using namespace duckdb::feishu;
	WriteStateBatch batch;
	batch.first_row = 500;
	batch.row_count = 1;
	WriteProgress progress;
	progress.confirmed_batches = 1;
	progress.confirmed_rows = 500;
	duckdb::idx_t calls = 0;
	auto attempt = [&](const WriteStateBatch &, duckdb::idx_t) {
		calls++;
		WriteAttemptOutcome outcome;
		if (ResponseConfirmsWriteBatch(R"({"code":0,"data":{"records":[null]}})", 1)) {
			outcome.result = WriteAttemptResult::CONFIRMED;
		} else {
			REQUIRE(ClassifyWriteRetry(200) == WriteRetryResult::UNKNOWN_RESULT);
			outcome.result = WriteAttemptResult::UNKNOWN_RESULT;
		}
		return outcome;
	};
	CHECK_THROWS_AS(RunBatchWithRetries(batch, progress, attempt, {}), duckdb::IOException);
	CHECK(calls == 1);
	CHECK(progress.confirmed_batches == 1);
	CHECK(progress.confirmed_rows == 500);
}

TEST_CASE("Write retries are safe before the request is sent", "[write_retry]") {
	for (auto error : {WriteTransportError::CONNECTION_FAILURE, WriteTransportError::DNS_FAILURE,
	                   WriteTransportError::TLS_HANDSHAKE_FAILURE}) {
		REQUIRE(ClassifyWriteRetry(0, std::nullopt, error) == WriteRetryResult::RETRY_SAFE);
		REQUIRE(ClassifyWriteRetry(200, 0, error) == WriteRetryResult::UNKNOWN_RESULT);
		REQUIRE(ClassifyWriteRetry(0, FEISHU_RATE_LIMIT_CODE, error) == WriteRetryResult::UNKNOWN_RESULT);
	}
}

TEST_CASE("Write rate limiting is a confirmed rejection", "[write_retry]") {
	REQUIRE(ClassifyWriteRetry(429) == WriteRetryResult::RETRY_SAFE);
	REQUIRE(ClassifyWriteRetry(429, 0) == WriteRetryResult::RETRY_SAFE);
	REQUIRE(ClassifyWriteRetry(200, FEISHU_RATE_LIMIT_CODE) == WriteRetryResult::RETRY_SAFE);
	REQUIRE(ClassifyWriteRetry(400, FEISHU_RATE_LIMIT_CODE) == WriteRetryResult::RETRY_SAFE);
	REQUIRE(ClassifyWriteRetry(200, 99991400) == WriteRetryResult::RETRY_SAFE);
}

TEST_CASE("Write read timeout must never be safe to retry", "[write_retry]") {
	REQUIRE(ClassifyWriteRetry(0, std::nullopt, WriteTransportError::READ_TIMEOUT) != WriteRetryResult::RETRY_SAFE);
	REQUIRE(ClassifyWriteRetry(0, std::nullopt, WriteTransportError::READ_TIMEOUT) == WriteRetryResult::UNKNOWN_RESULT);
}

TEST_CASE("Uncertain write transport and responses override status and business codes", "[write_retry]") {
	for (auto error : {WriteTransportError::READ_TIMEOUT, WriteTransportError::CONNECTION_INTERRUPTED,
	                   WriteTransportError::RESPONSE_PARSE_FAILURE, WriteTransportError::INVALID_RESPONSE,
	                   WriteTransportError::UNKNOWN}) {
		for (int status : {0, 200, 400, 429, 500}) {
			INFO("HTTP status " << status << ", error " << static_cast<int>(error));
			REQUIRE(ClassifyWriteRetry(status, std::nullopt, error) == WriteRetryResult::UNKNOWN_RESULT);
			REQUIRE(ClassifyWriteRetry(status, FEISHU_RATE_LIMIT_CODE, error) == WriteRetryResult::UNKNOWN_RESULT);
		}
	}
	REQUIRE(ClassifyWriteRetry(200, 0, WriteTransportError::INVALID_RESPONSE) == WriteRetryResult::UNKNOWN_RESULT);
	REQUIRE(ClassifyWriteRetry(0, std::nullopt, static_cast<WriteTransportError>(99)) ==
	        WriteRetryResult::UNKNOWN_RESULT);
}

TEST_CASE("Every write 5xx is unknown even with an apparent rejection code", "[write_retry]") {
	for (int status = 500; status <= 599; ++status) {
		INFO("HTTP status " << status);
		REQUIRE(ClassifyWriteRetry(status) == WriteRetryResult::UNKNOWN_RESULT);
		REQUIRE(ClassifyWriteRetry(status, FEISHU_RATE_LIMIT_CODE) == WriteRetryResult::UNKNOWN_RESULT);
		REQUIRE(ClassifyWriteRetry(status, 1254005) == WriteRetryResult::UNKNOWN_RESULT);
	}
}

TEST_CASE("Ordinary write 4xx and explicit business errors fail", "[write_retry]") {
	for (int status = 400; status <= 499; ++status) {
		if (status != 429) {
			INFO("HTTP status " << status);
			REQUIRE(ClassifyWriteRetry(status) == WriteRetryResult::FAIL);
		}
	}
	for (int64_t code : {-1, 1254005, 99991663, 99991672}) {
		INFO("business code " << code);
		REQUIRE(ClassifyWriteRetry(200, code) == WriteRetryResult::FAIL);
		REQUIRE(ClassifyWriteRetry(400, code) == WriteRetryResult::FAIL);
	}
}

TEST_CASE("Absent or unvalidated write results do not authorize retries", "[write_retry]") {
	for (int status : {-1, 0, 100, 199, 200, 201, 204, 299, 301, 307, 399, 600}) {
		INFO("HTTP status " << status);
		REQUIRE(ClassifyWriteRetry(status) == WriteRetryResult::UNKNOWN_RESULT);
		REQUIRE(ClassifyWriteRetry(status, 0) == WriteRetryResult::UNKNOWN_RESULT);
	}
	REQUIRE(ClassifyWriteRetry(0, FEISHU_RATE_LIMIT_CODE) == WriteRetryResult::UNKNOWN_RESULT);
}
