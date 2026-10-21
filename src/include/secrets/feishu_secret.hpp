#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace feishu {

struct FeishuCredentials {
	string app_id;
	string app_secret;
	string user_access_token;
};

void RegisterFeishuSecretType(ExtensionLoader &loader);
FeishuCredentials GetFeishuCredentials(ClientContext &context, const string &secret_name = "");

} // namespace feishu
} // namespace duckdb
