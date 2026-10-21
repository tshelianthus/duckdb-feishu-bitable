#include "catch.hpp"

#include "common/error.hpp"

#include <string>

using duckdb::InvalidInputException;
using duckdb::IOException;
using duckdb::feishu::FeishuApiError;
using duckdb::feishu::RedactSecretValue;
using duckdb::feishu::SanitizeErrorMessage;
using duckdb::feishu::ThrowFeishuApiError;
using duckdb::feishu::ThrowHttpError;
using duckdb::feishu::TryParseFeishuError;

TEST_CASE("HTTP request errors preserve the documented DuckDB exception types", "[error]") {
	REQUIRE_THROWS_AS(ThrowHttpError(400, "ignored secret body"), InvalidInputException);
	REQUIRE_THROWS_AS(ThrowHttpError(401, "ignored secret body"), InvalidInputException);
	REQUIRE_THROWS_AS(ThrowHttpError(404, "ignored secret body"), InvalidInputException);
	REQUIRE_THROWS_AS(ThrowHttpError(408, "ignored secret body"), IOException);
	REQUIRE_THROWS_AS(ThrowHttpError(429, "ignored secret body"), IOException);
	REQUIRE_THROWS_AS(ThrowHttpError(500, "ignored secret body"), IOException);
	REQUIRE_THROWS_AS(ThrowHttpError(503, "ignored secret body"), IOException);
}

TEST_CASE("HTTP error bodies are never copied into exceptions", "[error][security]") {
	const std::string secret = "unprefixed-app-secret-value";
	try {
		ThrowHttpError(401, secret);
		FAIL("ThrowHttpError must throw");
	} catch (const InvalidInputException &error) {
		REQUIRE(std::string(error.what()).find(secret) == std::string::npos);
	}
}

TEST_CASE("Structured API messages mask app ids, tokens, and Authorization headers", "[error][security]") {
	const std::string message = "app cli_sensitiveid; token t-sensitive-token; user u-sensitive-token; Authorization: "
	                            "Bearer opaque-token-value; done";
	auto sanitized = SanitizeErrorMessage(message);
	REQUIRE(sanitized.find("cli_sensitiveid") == std::string::npos);
	REQUIRE(sanitized.find("t-sensitive-token") == std::string::npos);
	REQUIRE(sanitized.find("u-sensitive-token") == std::string::npos);
	REQUIRE(sanitized.find("opaque-token-value") == std::string::npos);
	REQUIRE(sanitized.find("Authorization") != std::string::npos);
}

TEST_CASE("Bare app secrets are redacted literally before surfacing remote messages", "[error][security]") {
	const std::string secret = "bare-secret-without-prefix";
	auto redacted = RedactSecretValue("remote echoed " + secret + " twice " + secret, secret);
	REQUIRE(redacted.find(secret) == std::string::npos);
	REQUIRE(redacted.find("bare") != std::string::npos);
}

TEST_CASE("Short app secrets are redacted literally", "[error][security]") {
	REQUIRE(RedactSecretValue("remote echoed xyz", "xyz") == "remote echoed ***");
}

TEST_CASE("Feishu business errors parse structurally and remain InvalidInputException", "[error]") {
	FeishuApiError parsed;
	REQUIRE(TryParseFeishuError("{\"code\":99991663,\"msg\":\"invalid app cli_sensitive\"}", parsed));
	REQUIRE(parsed.code == 99991663);
	REQUIRE_THROWS_AS(ThrowFeishuApiError(parsed), InvalidInputException);
	try {
		ThrowFeishuApiError(parsed);
		FAIL("ThrowFeishuApiError must throw");
	} catch (const InvalidInputException &error) {
		auto message = std::string(error.what());
		REQUIRE(message.find("99991663") != std::string::npos);
		REQUIRE(message.find("cli_sensitive") == std::string::npos);
	}
}

TEST_CASE("Successful, malformed, and zero-code bodies are not business errors", "[error]") {
	FeishuApiError parsed;
	REQUIRE_FALSE(TryParseFeishuError("{\"code\":0,\"msg\":\"ok\"}", parsed));
	REQUIRE_FALSE(TryParseFeishuError("not-json", parsed));
	REQUIRE_FALSE(TryParseFeishuError("{\"msg\":\"missing code\"}", parsed));
}
