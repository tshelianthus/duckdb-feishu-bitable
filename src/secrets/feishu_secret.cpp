#include "secrets/feishu_secret.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {
namespace feishu {

namespace {

static constexpr const char *FEISHU_SECRET_TYPE = "feishu";
static constexpr const char *FEISHU_CONFIG_PROVIDER = "config";
static constexpr const char *APP_ID_KEY = "app_id";
static constexpr const char *APP_SECRET_KEY = "app_secret";
static constexpr const char *USER_ACCESS_TOKEN_KEY = "user_access_token";

string RequireNonEmptyOption(const CreateSecretInput &input, const string &key) {
	auto entry = input.options.find(key);
	if (entry == input.options.end() || entry->second.IsNull()) {
		throw InvalidInputException("Feishu secret requires non-empty %s", StringUtil::Upper(key));
	}
	auto value = entry->second.GetValue<string>();
	if (value.empty()) {
		throw InvalidInputException("Feishu secret requires non-empty %s", StringUtil::Upper(key));
	}
	return value;
}

unique_ptr<BaseSecret> CreateFeishuSecret(ClientContext &context, CreateSecretInput &input) {
	(void)context;

	auto secret = make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);

	// USER_ACCESS_TOKEN is an alternative to APP_ID + APP_SECRET for environments
	// where only a delegated user token is available (e.g. lark-cli built-in app).
	auto user_token_entry = input.options.find(USER_ACCESS_TOKEN_KEY);
	bool has_user_token = user_token_entry != input.options.end() && !user_token_entry->second.IsNull();
	if (has_user_token) {
		auto user_token = user_token_entry->second.GetValue<string>();
		if (user_token.empty()) {
			throw InvalidInputException("Feishu secret requires non-empty USER_ACCESS_TOKEN");
		}
		secret->secret_map[USER_ACCESS_TOKEN_KEY] = Value(std::move(user_token));
		secret->redact_keys.insert(USER_ACCESS_TOKEN_KEY);
	} else {
		auto app_id = RequireNonEmptyOption(input, APP_ID_KEY);
		auto app_secret = RequireNonEmptyOption(input, APP_SECRET_KEY);
		secret->secret_map[APP_ID_KEY] = Value(std::move(app_id));
		secret->secret_map[APP_SECRET_KEY] = Value(std::move(app_secret));
		secret->redact_keys.insert(APP_SECRET_KEY);
	}

	return std::move(secret);
}

const KeyValueSecret &RequireFeishuKeyValueSecret(const SecretEntry &entry, const string &description) {
	if (!entry.secret || !StringUtil::CIEquals(entry.secret->GetType(), FEISHU_SECRET_TYPE)) {
		throw InvalidInputException("Secret %s is not a feishu secret", description);
	}
	auto secret = dynamic_cast<const KeyValueSecret *>(entry.secret.get());
	if (!secret) {
		throw InvalidInputException("Feishu secret %s has an unsupported storage format", description);
	}
	return *secret;
}

string RequireCredentialValue(const KeyValueSecret &secret, const string &key, const string &description) {
	Value value;
	if (!secret.TryGetValue(key, value) || value.IsNull()) {
		throw InvalidInputException("Feishu secret %s is missing required credentials", description);
	}
	string result;
	try {
		result = value.GetValue<string>();
	} catch (const Exception &) {
		throw InvalidInputException("Feishu secret %s contains invalid credentials", description);
	}
	if (result.empty()) {
		throw InvalidInputException("Feishu secret %s contains empty required credentials", description);
	}
	return result;
}

} // namespace

void RegisterFeishuSecretType(ExtensionLoader &loader) {
	SecretType secret_type;
	secret_type.name = FEISHU_SECRET_TYPE;
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = FEISHU_CONFIG_PROVIDER;
	loader.RegisterSecretType(std::move(secret_type));

	CreateSecretFunction config_provider;
	config_provider.secret_type = FEISHU_SECRET_TYPE;
	config_provider.provider = FEISHU_CONFIG_PROVIDER;
	config_provider.function = CreateFeishuSecret;
	config_provider.named_parameters[APP_ID_KEY] = LogicalType::VARCHAR;
	config_provider.named_parameters[APP_SECRET_KEY] = LogicalType::VARCHAR;
	config_provider.named_parameters[USER_ACCESS_TOKEN_KEY] = LogicalType::VARCHAR;
	loader.RegisterFunction(std::move(config_provider));
}

FeishuCredentials GetFeishuCredentials(ClientContext &context, const string &secret_name) {
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);

	unique_ptr<SecretEntry> owned_entry;
	try {
		if (!secret_name.empty()) {
			owned_entry = secret_manager.GetSecretByName(transaction, secret_name);
			if (!owned_entry) {
				throw InvalidInputException("Feishu secret with the requested name does not exist");
			}
		} else {
			auto match = secret_manager.LookupSecret(transaction, "", FEISHU_SECRET_TYPE);
			if (!match.HasMatch()) {
				throw InvalidInputException(
				    "No feishu secret is available; create one with APP_ID and APP_SECRET, or USER_ACCESS_TOKEN");
			}
			owned_entry = std::move(match.secret_entry);
		}
	} catch (const InvalidInputException &) {
		throw;
	} catch (const Exception &) {
		throw InvalidInputException("Unable to resolve the requested feishu secret");
	}

	const auto description = secret_name.empty() ? string("selected by type") : string("with the requested name");
	auto &secret = RequireFeishuKeyValueSecret(*owned_entry, description);
	FeishuCredentials result;

	Value user_token_value;
	if (secret.TryGetValue(USER_ACCESS_TOKEN_KEY, user_token_value) && !user_token_value.IsNull()) {
		result.user_access_token = user_token_value.GetValue<string>();
		if (result.user_access_token.empty()) {
			throw InvalidInputException("Feishu secret %s contains empty USER_ACCESS_TOKEN", description);
		}
	} else {
		result.app_id = RequireCredentialValue(secret, APP_ID_KEY, description);
		result.app_secret = RequireCredentialValue(secret, APP_SECRET_KEY, description);
	}
	return result;
}

} // namespace feishu
} // namespace duckdb
