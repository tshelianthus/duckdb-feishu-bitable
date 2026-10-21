#include "catch.hpp"

#include "bitable/write_state_machine.hpp"

#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using duckdb::idx_t;
using duckdb::InvalidInputException;
using duckdb::IOException;
using duckdb::feishu::RunBatchWithRetries;
using duckdb::feishu::WriteAttemptOutcome;
using duckdb::feishu::WriteAttemptResult;
using duckdb::feishu::WriteBatchLabel;
using duckdb::feishu::WritePreflightFailureMessage;
using duckdb::feishu::WriteProgress;
using duckdb::feishu::WriteRowFailureMessage;
using duckdb::feishu::WriteStateBatch;
using duckdb::feishu::WriteStateMachineOptions;

namespace {

WriteAttemptOutcome Outcome(WriteAttemptResult result, const char *detail = "") {
	WriteAttemptOutcome outcome;
	outcome.result = result;
	outcome.detail = detail;
	return outcome;
}

//! Records every call so tests can prove the transport is never used again
//! after a definite failure or an ambiguous result.
struct RecordingAttempts {
	std::vector<std::pair<idx_t, idx_t>> calls;

	duckdb::feishu::WriteAttemptFn Fn(std::vector<WriteAttemptResult> results) {
		return [this, results](const WriteStateBatch &batch, idx_t attempt) {
			calls.emplace_back(attempt, batch.first_row);
			auto index = static_cast<size_t>(attempt - 1);
			auto result = index < results.size() ? results[index] : WriteAttemptResult::UNKNOWN_RESULT;
			return Outcome(result, "detail");
		};
	}
};

std::string MessageOf(const std::function<void()> &call) {
	try {
		call();
	} catch (const std::exception &ex) {
		return std::string(ex.what());
	}
	return std::string();
}

} // namespace

TEST_CASE("write state machine: local row failure reports confirmed progress", "[write_state_machine]") {
	WriteProgress progress;
	progress.confirmed_batches = 1;
	progress.confirmed_rows = 100;
	auto message = WriteRowFailureMessage(101, progress, "NULL list elements cannot be written");
	CHECK(message.find("row #101") != std::string::npos);
	CHECK(message.find("1 confirmed batches / 100 rows") != std::string::npos);
	CHECK(message.find("verify the target table before re-running") != std::string::npos);
	CHECK(message.find("NULL list elements cannot be written") != std::string::npos);
}

TEST_CASE("write state machine: preflight failure names the unsubmitted batch", "[write_state_machine]") {
	WriteProgress progress;
	progress.confirmed_batches = 1;
	progress.confirmed_rows = 100;
	WriteStateBatch batch;
	batch.first_row = 100;
	batch.row_count = 25;
	auto message = WritePreflightFailureMessage(batch, progress, "access token could not be obtained");
	CHECK(message.find("batch #2 (rows 101-125)") != std::string::npos);
	CHECK(message.find("could not be submitted after 1 confirmed batches / 100 rows") != std::string::npos);
	CHECK(message.find("access token could not be obtained") != std::string::npos);
}

TEST_CASE("write state machine: a confirmed batch advances progress", "[write_state_machine]") {
	RecordingAttempts recorder;
	WriteStateBatch batch;
	batch.first_row = 0;
	batch.row_count = 3;
	WriteProgress progress;
	progress.confirmed_batches = 2;
	progress.confirmed_rows = 7;

	auto next = RunBatchWithRetries(batch, progress, recorder.Fn({WriteAttemptResult::CONFIRMED}),
	                                [](idx_t, idx_t) { throw std::runtime_error("must not retry"); });

	CHECK(next.confirmed_batches == 3);
	CHECK(next.confirmed_rows == 10);
	// A confirmed attempt must not be repeated.
	CHECK(recorder.calls.size() == 1);
}

TEST_CASE("write state machine: an ambiguous result stops immediately", "[write_state_machine]") {
	RecordingAttempts recorder;
	WriteStateBatch batch;
	batch.first_row = 1000;
	batch.row_count = 500;

	bool threw = false;
	try {
		RunBatchWithRetries(batch, WriteProgress(), recorder.Fn({WriteAttemptResult::UNKNOWN_RESULT}),
		                    [](idx_t, idx_t) {});
	} catch (const IOException &) {
		threw = true;
	}
	CHECK(threw);
	// The whole point of the state machine: exactly one attempt, never a replay.
	CHECK(recorder.calls.size() == 1);
}

TEST_CASE("write state machine: a definite failure stops immediately", "[write_state_machine]") {
	RecordingAttempts recorder;
	WriteStateBatch batch;
	batch.row_count = 1;

	bool invalid_input = false;
	try {
		RunBatchWithRetries(batch, WriteProgress(), recorder.Fn({WriteAttemptResult::FAIL}), [](idx_t, idx_t) {});
	} catch (const InvalidInputException &) {
		invalid_input = true;
	}
	CHECK(invalid_input);
	CHECK(recorder.calls.size() == 1);
}

TEST_CASE("write state machine: retry-safe attempts are bounded", "[write_state_machine]") {
	RecordingAttempts recorder;
	WriteStateBatch batch;
	batch.row_count = 2;
	WriteStateMachineOptions options;
	options.max_attempts = 3;
	idx_t retries = 0;

	bool threw = false;
	try {
		RunBatchWithRetries(
		    batch, WriteProgress(),
		    recorder.Fn(
		        {WriteAttemptResult::RETRY_SAFE, WriteAttemptResult::RETRY_SAFE, WriteAttemptResult::RETRY_SAFE}),
		    [&retries](idx_t, idx_t) { retries++; }, options);
	} catch (const IOException &) {
		threw = true;
	}
	CHECK(threw);
	// Three attempts, two back-offs in between, then a safe-to-rerun error.
	CHECK(recorder.calls.size() == 3);
	CHECK(retries == 2);
}

TEST_CASE("write state machine: retry-safe then confirmed is allowed", "[write_state_machine]") {
	RecordingAttempts recorder;
	WriteStateBatch batch;
	batch.row_count = 4;
	WriteStateMachineOptions options;
	options.max_attempts = 5;

	auto next = RunBatchWithRetries(
	    batch, WriteProgress(),
	    recorder.Fn({WriteAttemptResult::RETRY_SAFE, WriteAttemptResult::RETRY_SAFE, WriteAttemptResult::CONFIRMED}),
	    [](idx_t, idx_t) {}, options);
	CHECK(next.confirmed_batches == 1);
	CHECK(next.confirmed_rows == 4);
	CHECK(recorder.calls.size() == 3);
}

TEST_CASE("write state machine: diagnostics carry the batch number and row range", "[write_state_machine]") {
	RecordingAttempts recorder;
	WriteStateBatch batch;
	batch.first_row = 1500;
	batch.row_count = 250;
	WriteProgress progress;
	progress.confirmed_batches = 3;
	progress.confirmed_rows = 1500;

	auto unknown = MessageOf([&]() {
		RunBatchWithRetries(batch, progress, recorder.Fn({WriteAttemptResult::UNKNOWN_RESULT}), [](idx_t, idx_t) {});
	});
	CHECK(unknown.find("batch #4") != std::string::npos);
	CHECK(unknown.find("rows 1501-1750") != std::string::npos);
	CHECK(unknown.find("3 confirmed batches / 1500 rows") != std::string::npos);
	CHECK(unknown.find("verify the target table") != std::string::npos);

	auto failure = MessageOf(
	    [&]() { RunBatchWithRetries(batch, progress, recorder.Fn({WriteAttemptResult::FAIL}), [](idx_t, idx_t) {}); });
	CHECK(failure.find("batch #4") != std::string::npos);
	CHECK(failure.find("rows 1501-1750") != std::string::npos);
}

TEST_CASE("write state machine: labels are 1-based and readable", "[write_state_machine]") {
	WriteStateBatch batch;
	batch.first_row = 0;
	batch.row_count = 1;
	CHECK(WriteBatchLabel(batch, 1) == "batch #1 (rows 1-1)");
	batch.first_row = 500;
	batch.row_count = 500;
	CHECK(WriteBatchLabel(batch, 2) == "batch #2 (rows 501-1000)");
}
