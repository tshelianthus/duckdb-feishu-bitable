#include "catch.hpp"

#include "bitable/url_parser.hpp"

#include "duckdb/common/exception.hpp"

using duckdb::InvalidInputException;
using duckdb::feishu::HasHttpScheme;
using duckdb::feishu::IsBitableUrl;
using duckdb::feishu::ParseBitableUrl;
using duckdb::feishu::ResolveBitableTarget;

// =============================================================================
// ParseBitableUrl
// =============================================================================

TEST_CASE("ParseBitableUrl extracts app_token, table and view", "[url_parser]") {
	auto components =
	    ParseBitableUrl("https://sample.feishu.cn/base/bascn123456?table=tbl789012&view=vew345678", "read_bitable");

	REQUIRE(components.app_token == "bascn123456");
	REQUIRE(components.table_id == "tbl789012");
	REQUIRE(components.view_id == "vew345678");
	REQUIRE(components.api_root == "https://open.feishu.cn/open-apis/bitable/v1");
	REQUIRE_FALSE(components.is_wiki);
}

TEST_CASE("ParseBitableUrl accepts the Lark international domain", "[url_parser]") {
	auto components = ParseBitableUrl("https://sample.larksuite.com/base/bascn123456?table=tbl789012", "read_bitable");

	REQUIRE(components.app_token == "bascn123456");
	REQUIRE(components.table_id == "tbl789012");
	REQUIRE(components.view_id.empty());
	REQUIRE(components.api_root == "https://open.larksuite.com/open-apis/bitable/v1");
	REQUIRE_FALSE(components.is_wiki);
}

TEST_CASE("ParseBitableUrl handles wiki paths, ports, fragments and extra parameters", "[url_parser]") {
	auto components = ParseBitableUrl(
	    "https://sample.feishu.cn:443/wiki/wikcn987654?other=z&table=tbl%2D1&view=vew1#anchor", "read_bitable");

	REQUIRE(components.app_token == "wikcn987654");
	REQUIRE(components.table_id == "tbl-1");
	REQUIRE(components.view_id == "vew1");
	REQUIRE(components.api_root == "https://open.feishu.cn/open-apis/bitable/v1");
	REQUIRE(components.is_wiki);
}

TEST_CASE("ParseBitableUrl is case insensitive and trims blanks", "[url_parser]") {
	auto components = ParseBitableUrl("  HTTPS://SAMPLE.FEISHU.CN/base/bascn1?TABLE=tbl1  ", "read_bitable");

	REQUIRE(components.app_token == "bascn1");
	REQUIRE(components.table_id == "tbl1");
}

TEST_CASE("ParseBitableUrl ignores userinfo in the authority", "[url_parser]") {
	auto components = ParseBitableUrl("https://user@sample.feishu.cn/base/bascn1", "read_bitable");

	REQUIRE(components.app_token == "bascn1");
}

TEST_CASE("ParseBitableUrl leaves table_id empty for a base URL", "[url_parser]") {
	// An unresolved table_id is what triggers the default first table fallback.
	auto components = ParseBitableUrl("https://sample.feishu.cn/base/bascn123456", "read_bitable");

	REQUIRE(components.app_token == "bascn123456");
	REQUIRE(components.table_id.empty());
	REQUIRE(components.view_id.empty());
}

TEST_CASE("ParseBitableUrl rejects a URL without a scheme", "[url_parser]") {
	REQUIRE_THROWS_WITH(ParseBitableUrl("sample.feishu.cn/base/bascn1", "read_bitable"),
	                    Catch::Contains("must start with http:// or https://"));
	REQUIRE_THROWS_AS(ParseBitableUrl("sample.feishu.cn/base/bascn1", "read_bitable"), InvalidInputException);
}

TEST_CASE("ParseBitableUrl rejects foreign domains", "[url_parser]") {
	REQUIRE_THROWS_WITH(ParseBitableUrl("https://example.com/base/bascn1", "read_bitable"),
	                    Catch::Contains("is not hosted on a Feishu or Lark domain"));
	// A lookalike host must not match by suffix accident.
	REQUIRE_THROWS_AS(ParseBitableUrl("https://feishu.cn.example.com/base/bascn1", "read_bitable"),
	                  InvalidInputException);
}

TEST_CASE("ParseBitableUrl rejects paths without an app_token", "[url_parser]") {
	REQUIRE_THROWS_WITH(ParseBitableUrl("https://sample.feishu.cn/docs/bascn1", "read_bitable"),
	                    Catch::Contains("cannot extract an app_token"));
	REQUIRE_THROWS_WITH(ParseBitableUrl("https://sample.feishu.cn/base/", "read_bitable"),
	                    Catch::Contains("cannot extract an app_token"));
}

TEST_CASE("ParseBitableUrl prefixes errors with the calling function", "[url_parser]") {
	REQUIRE_THROWS_WITH(ParseBitableUrl("https://example.com/base/bascn1", "read_feishu_bitable"),
	                    Catch::StartsWith("read_feishu_bitable:"));
}

TEST_CASE("URL diagnostics never echo input credentials", "[url_parser][security][review_regression]") {
	for (const auto &url :
	     {"sample.feishu.cn/base/<private_base_token>?app_secret=<app_secret>",
	      "https://<user>:<password>@example.com/base/<private_base_token>?token=<access_token>",
	      "https://sample.feishu.cn/docs/<private_base_token>?token=<access_token>#<private_fragment>"}) {
		try {
			ParseBitableUrl(url, "read_bitable");
			FAIL("Malformed URL must be rejected");
		} catch (const InvalidInputException &error) {
			auto message = std::string(error.what());
			CHECK(message.find("read_bitable:") == 0);
			for (const auto &private_value : {"<private_base_token>", "<app_secret>", "<access_token>", "<user>",
			                                  "<password>", "<private_fragment>"}) {
				CHECK(message.find(private_value) == std::string::npos);
			}
		}
	}
}

// =============================================================================
// IsBitableUrl / HasHttpScheme
// =============================================================================

TEST_CASE("IsBitableUrl only accepts Feishu and Lark Bitable URLs", "[url_parser]") {
	REQUIRE(IsBitableUrl("https://sample.feishu.cn/base/bascn1"));
	REQUIRE(IsBitableUrl("https://sample.larksuite.com/base/bascn1?table=tbl1"));
	REQUIRE(IsBitableUrl("https://sample.feishu.cn/wiki/wikcn1"));
	// The replacement scan must hand everything else back to DuckDB.
	REQUIRE_FALSE(IsBitableUrl("https://example.com/base/bascn1"));
	REQUIRE_FALSE(IsBitableUrl("https://example.com/data.csv"));
	REQUIRE_FALSE(IsBitableUrl("https://feishu.cn.example.com/base/bascn1"));
	REQUIRE_FALSE(IsBitableUrl("sample.feishu.cn/base/bascn1"));
	REQUIRE_FALSE(IsBitableUrl("https://sample.feishu.cn/docs/bascn1"));
	REQUIRE_FALSE(IsBitableUrl("https://sample.feishu.cn/base/"));
	REQUIRE_FALSE(IsBitableUrl("no_such_file.parquet"));
	REQUIRE_FALSE(IsBitableUrl(""));
}

TEST_CASE("HasHttpScheme distinguishes URLs from bare app tokens", "[url_parser]") {
	REQUIRE(HasHttpScheme("https://sample.feishu.cn/base/bascn1"));
	REQUIRE(HasHttpScheme("http://sample.feishu.cn/base/bascn1"));
	REQUIRE(HasHttpScheme("  HTTPS://sample.feishu.cn/base/bascn1"));
	REQUIRE_FALSE(HasHttpScheme("bascn123456"));
}

// =============================================================================
// ResolveBitableTarget (the override precedence applied during bind)
// =============================================================================

TEST_CASE("ResolveBitableTarget passes a bare app_token through", "[url_parser]") {
	auto target = ResolveBitableTarget("bascn123456", "", "", "read_bitable");

	REQUIRE(target.app_token == "bascn123456");
	REQUIRE(target.table_id.empty());
	REQUIRE(target.view_id.empty());
	REQUIRE(target.api_root == "https://open.feishu.cn/open-apis/bitable/v1");
	REQUIRE_FALSE(target.is_wiki);
}

TEST_CASE("ResolveBitableTarget lets explicit arguments win over the URL query", "[url_parser]") {
	auto target = ResolveBitableTarget("https://sample.feishu.cn/base/bascn1?table=tblOld&view=vewOld", "tblNew",
	                                   "vewNew", "read_bitable");

	REQUIRE(target.table_id == "tblNew");
	REQUIRE(target.view_id == "vewNew");
}

TEST_CASE("ResolveBitableTarget keeps URL values that are not overridden", "[url_parser]") {
	auto target = ResolveBitableTarget("https://sample.feishu.cn/base/bascn1?table=tblOld&view=vewOld", "tblNew", "",
	                                   "read_bitable");

	REQUIRE(target.table_id == "tblNew");
	REQUIRE(target.view_id == "vewOld");
}

TEST_CASE("ResolveBitableTarget applies explicit arguments to a bare app_token", "[url_parser]") {
	auto target = ResolveBitableTarget("bascn123456", "tbl789012", "vew345678", "read_bitable");

	REQUIRE(target.app_token == "bascn123456");
	REQUIRE(target.table_id == "tbl789012");
	REQUIRE(target.view_id == "vew345678");
	REQUIRE(target.api_root == "https://open.feishu.cn/open-apis/bitable/v1");
	REQUIRE_FALSE(target.is_wiki);
}

TEST_CASE("URL regions use canonical HTTPS API roots regardless of browser authority details", "[url_parser]") {
	for (const auto &url : {"http://larksuite.com/base/bascn1", "  HTTPS://TENANT.LARKSUITE.COM:443/base/bascn1  ",
	                        "https://feishu.cn@tenant.larksuite.com:8443/base/bascn1"}) {
		auto target = ParseBitableUrl(url, "read_bitable");
		REQUIRE(target.api_root == "https://open.larksuite.com/open-apis/bitable/v1");
		REQUIRE_FALSE(target.is_wiki);
	}
	for (const auto &url : {"http://feishu.cn/base/bascn1", "https://larksuite.com@tenant.feishu.cn/base/bascn1"}) {
		auto target = ParseBitableUrl(url, "read_bitable");
		REQUIRE(target.api_root == "https://open.feishu.cn/open-apis/bitable/v1");
		REQUIRE_FALSE(target.is_wiki);
	}
	REQUIRE_FALSE(IsBitableUrl("https://larksuite.com.example.com/base/bascn1"));
	REQUIRE_FALSE(IsBitableUrl("https://evillarksuite.com/base/bascn1"));
}

TEST_CASE("Wiki identity and region survive explicit table and view overrides", "[url_parser]") {
	auto target = ResolveBitableTarget("https://tenant.larksuite.com/WIKI/wik%2D1?table=tblOld&view=vewOld#anchor",
	                                   "tblNew", "vewNew", "read_feishu_bitable");
	REQUIRE(target.app_token == "wik-1");
	REQUIRE(target.table_id == "tblNew");
	REQUIRE(target.view_id == "vewNew");
	REQUIRE(target.api_root == "https://open.larksuite.com/open-apis/bitable/v1");
	REQUIRE(target.is_wiki);

	auto fallback = ResolveBitableTarget("https://tenant.larksuite.com/wiki/wik1", "", "", "read_bitable");
	REQUIRE(fallback.app_token == "wik1");
	REQUIRE(fallback.table_id.empty());
	REQUIRE(fallback.view_id.empty());
	REQUIRE(fallback.api_root == target.api_root);
	REQUIRE(fallback.is_wiki);

	// A token prefix or query/fragment text is not a Wiki path.
	REQUIRE_FALSE(ResolveBitableTarget("wik1", "", "", "read_bitable").is_wiki);
	REQUIRE_FALSE(ParseBitableUrl("https://tenant.larksuite.com/base/bascn1?other=/wiki/wik1", "read_bitable").is_wiki);
}
