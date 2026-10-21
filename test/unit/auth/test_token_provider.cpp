#include "catch.hpp"

#include "auth/token_provider.hpp"

#include "duckdb/common/exception.hpp"

#include <string>

using duckdb::InvalidInputException;
using duckdb::IOException;
using duckdb::timestamp_t;
using duckdb::feishu::HttpResponse;
using duckdb::feishu::ParseTenantAccessTokenResponse;

TEST_CASE("Tenant token responses produce a token with an absolute expiry", "[token_provider]") {
	HttpResponse response;
	response.status_code = 200;
	response.body = "{\"code\":0,\"tenant_access_token\":\"t-valid-token\",\"expire\":7200}";
	auto token = ParseTenantAccessTokenResponse(response, "app-secret", timestamp_t(1000000));
	REQUIRE(token.access_token == "t-valid-token");
	REQUIRE(token.expires_at.value == 7201000000);
}

TEST_CASE("Tenant token parsing rejects malformed payloads and invalid lifetimes", "[token_provider]") {
	HttpResponse response;
	response.body = "not-json";
	REQUIRE_THROWS_AS(ParseTenantAccessTokenResponse(response, "app-secret", timestamp_t(0)), IOException);

	response.body = "{\"code\":0,\"tenant_access_token\":\"t-token\",\"expire\":0}";
	REQUIRE_THROWS_AS(ParseTenantAccessTokenResponse(response, "app-secret", timestamp_t(0)), IOException);

	response.body = "{\"code\":0,\"tenant_access_token\":\"t-token\",\"expire\":2592001}";
	REQUIRE_THROWS_AS(ParseTenantAccessTokenResponse(response, "app-secret", timestamp_t(0)), IOException);

	response.body = "{\"code\":0,\"expire\":7200}";
	REQUIRE_THROWS_AS(ParseTenantAccessTokenResponse(response, "app-secret", timestamp_t(0)), IOException);
}

TEST_CASE("Tenant token business errors preserve type and redact the app secret", "[token_provider][security]") {
	const std::string secret = "bare-app-secret-value";
	HttpResponse response;
	response.body = "{\"code\":99991663,\"msg\":\"credential " + secret + " is invalid\"}";
	try {
		ParseTenantAccessTokenResponse(response, secret, timestamp_t(0));
		FAIL("business errors must throw");
	} catch (const InvalidInputException &error) {
		auto message = std::string(error.what());
		REQUIRE(message.find("99991663") != std::string::npos);
		REQUIRE(message.find(secret) == std::string::npos);
	}
}

TEST_CASE("Tenant token metadata failures never echo a returned token", "[token_provider][security]") {
	const std::string token = "t-should-never-appear-in-error";
	HttpResponse response;
	response.body = "{\"code\":0,\"tenant_access_token\":\"" + token + "\",\"expire\":-1}";
	try {
		ParseTenantAccessTokenResponse(response, "app-secret", timestamp_t(0));
		FAIL("an invalid expiry must throw");
	} catch (const IOException &error) {
		REQUIRE(std::string(error.what()).find(token) == std::string::npos);
	}
}
