#pragma once

#include "duckdb/common/string.hpp"

namespace duckdb {
namespace feishu {

inline constexpr const char *FEISHU_BITABLE_API_ROOT = "https://open.feishu.cn/open-apis/bitable/v1";
inline constexpr const char *LARK_BITABLE_API_ROOT = "https://open.larksuite.com/open-apis/bitable/v1";

//! Components extracted from a Feishu/Lark Bitable browser URL.
//! `table_id` and `view_id` are empty when the URL carries no `table` / `view`
//! query parameter.
struct BitableUrlComponents {
	string app_token;
	string table_id;
	string view_id;
	//! Bare app tokens retain the domestic API default. Browser hosts never
	//! become request destinations themselves: only these canonical roots do.
	string api_root = FEISHU_BITABLE_API_ROOT;
	//! For Wiki URLs, app_token initially holds a node token, resolved at bind.
	bool is_wiki = false;
};

//! Returns true when the input starts with an http:// or https:// scheme.
bool HasHttpScheme(const string &input);

//! Returns true when the input is a Feishu/Lark Bitable browser URL.
//! Used by the replacement scan callback to decide whether to intercept a
//! string table name; never throws.
bool IsBitableUrl(const string &input);

//! Parses a Feishu/Lark Bitable browser URL into its components.
//! `function_name` is only used to prefix error messages.
//! Throws InvalidInputException when the input is not a Bitable URL.
BitableUrlComponents ParseBitableUrl(const string &url, const string &function_name);

//! Resolves the scan target from a URL-or-app_token source plus the explicitly
//! provided overrides. Explicit values always win over URL query parameters.
//! `table_id` stays empty when neither the caller nor the URL names a table,
//! which is what triggers the default first table fallback during bind.
BitableUrlComponents ResolveBitableTarget(const string &source, const string &explicit_table,
                                          const string &explicit_view, const string &function_name);

} // namespace feishu
} // namespace duckdb
