#include "bitable/tables_function.hpp"

#include "auth/token_cache.hpp"
#include "auth/token_provider.hpp"
#include "bitable/api_client.hpp"
#include "bitable/api_response.hpp"
#include "bitable/function_docs.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "secrets/feishu_secret.hpp"

#include <unordered_set>

namespace duckdb {
namespace feishu {

namespace {

constexpr idx_t FEISHU_METADATA_PAGE_SIZE = 100;
constexpr const char *FEISHU_BITABLE_API_ROOT = "https://open.feishu.cn/open-apis/bitable/v1/apps/";

struct TablesBindData : public TableFunctionData {
	TablesBindData(string app_token_p, string secret_name_p)
	    : app_token(std::move(app_token_p)), secret_name(std::move(secret_name_p)) {
	}

	string app_token;
	string secret_name;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<TablesBindData>(app_token, secret_name);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<TablesBindData>();
		return app_token == other.app_token && secret_name == other.secret_name;
	}

	bool SupportStatementCache() const override {
		return false;
	}
};

struct TablesGlobalState : public GlobalTableFunctionState {
	explicit TablesGlobalState(string access_token_p) : access_token(std::move(access_token_p)) {
	}

	string access_token;
	string page_token;
	bool has_more = true;
	vector<TableMetadata> buffered_rows;
	std::unordered_set<string> seen_page_tokens;
	idx_t buffered_offset = 0;
	idx_t consecutive_empty_pages = 0;
};

static string RequiredStringArgument(const Value &value, const char *argument_name) {
	if (value.IsNull()) {
		throw InvalidInputException("feishu_bitable_tables: %s must not be NULL", argument_name);
	}
	auto result = value.GetValue<string>();
	if (result.empty()) {
		throw InvalidInputException("feishu_bitable_tables: %s must not be empty", argument_name);
	}
	return result;
}

static string OptionalNamedString(const named_parameter_map_t &parameters, const string &name) {
	auto entry = parameters.find(name);
	if (entry == parameters.end()) {
		return string();
	}
	if (entry->second.IsNull()) {
		throw InvalidInputException("feishu_bitable_tables: %s must not be NULL", name);
	}
	auto result = entry->second.GetValue<string>();
	if (result.empty()) {
		throw InvalidInputException("feishu_bitable_tables: %s must not be empty", name);
	}
	return result;
}

static string BuildTablesUrl(const TablesBindData &bind_data, const string &page_token) {
	string result = string(FEISHU_BITABLE_API_ROOT) + StringUtil::URLEncode(bind_data.app_token) +
	                "/tables?page_size=" + std::to_string(FEISHU_METADATA_PAGE_SIZE);
	if (!page_token.empty()) {
		result += "&page_token=" + StringUtil::URLEncode(page_token);
	}
	return result;
}

static void FetchNextPage(ClientContext &context, const TablesBindData &bind_data, TablesGlobalState &state) {
	ApiClient client(context);
	HeaderMap headers;
	headers["Authorization"] = "Bearer " + state.access_token;
	auto response = client.Get(BuildTablesUrl(bind_data, state.page_token), headers);
	auto page = ParseTablesMetadataPage(response.status_code, response.body);
	AdvanceMetadataPaginationCursor("feishu_bitable_tables", page.has_more, page.page_token, state.page_token,
	                                state.seen_page_tokens);
	state.has_more = page.has_more;
	state.buffered_rows = std::move(page.rows);
	state.buffered_offset = 0;
}

static unique_ptr<FunctionData> TablesBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
	auto app_token = RequiredStringArgument(input.inputs[0], "app_token");
	auto secret_name = OptionalNamedString(input.named_parameters, "secret");
	// Validate locally even when LIMIT 0 removes initialization and scanning.
	// Token exchange remains lazy; static schema discovery needs no network.
	GetFeishuCredentials(context, secret_name);

	names = {"table_id", "name", "revision"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT};
	return make_uniq<TablesBindData>(std::move(app_token), std::move(secret_name));
}

static unique_ptr<GlobalTableFunctionState> TablesInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<TablesBindData>();
	auto credentials = GetFeishuCredentials(context, bind_data.secret_name);
	string access_token;
	if (!credentials.user_access_token.empty()) {
		access_token = credentials.user_access_token;
	} else {
		TokenProvider token_provider(context, GetGlobalTokenCache());
		access_token = token_provider.GetTenantAccessToken(credentials.app_id, credentials.app_secret);
	}
	if (access_token.empty()) {
		throw IOException("feishu_bitable_tables: authentication returned an empty access token");
	}
	return make_uniq<TablesGlobalState>(std::move(access_token));
}

static void TablesScan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<TablesBindData>();
	auto &state = input.global_state->Cast<TablesGlobalState>();

	for (auto &column : output.data) {
		FlatVector::Validity(column).SetAllValid(STANDARD_VECTOR_SIZE);
	}
	auto table_ids = FlatVector::GetData<string_t>(output.data[0]);
	auto names = FlatVector::GetData<string_t>(output.data[1]);
	auto revisions = FlatVector::GetData<int64_t>(output.data[2]);
	idx_t output_count = 0;

	while (output_count < STANDARD_VECTOR_SIZE) {
		if (state.buffered_offset >= state.buffered_rows.size()) {
			if (!state.has_more) {
				break;
			}
			FetchNextPage(context, bind_data, state);
			if (state.buffered_rows.empty()) {
				state.consecutive_empty_pages++;
				if (state.consecutive_empty_pages > 32) {
					throw IOException(
					    "feishu_bitable_tables: exceeded maximum consecutive empty pages during pagination");
				}
				continue;
			}
			state.consecutive_empty_pages = 0;
		}

		auto available = state.buffered_rows.size() - state.buffered_offset;
		auto emit_count = MinValue<idx_t>(STANDARD_VECTOR_SIZE - output_count, available);
		for (idx_t index = 0; index < emit_count; index++) {
			auto &row = state.buffered_rows[state.buffered_offset + index];
			auto output_index = output_count + index;
			if (row.table_id) {
				table_ids[output_index] = StringVector::AddString(output.data[0], *row.table_id);
			} else {
				FlatVector::Validity(output.data[0]).SetInvalid(output_index);
			}
			if (row.name) {
				names[output_index] = StringVector::AddString(output.data[1], *row.name);
			} else {
				FlatVector::Validity(output.data[1]).SetInvalid(output_index);
			}
			if (row.revision) {
				revisions[output_index] = *row.revision;
			} else {
				FlatVector::Validity(output.data[2]).SetInvalid(output_index);
			}
		}
		state.buffered_offset += emit_count;
		output_count += emit_count;
	}

	output.SetCardinality(output_count);
}

} // namespace

void RegisterTablesFunction(ExtensionLoader &loader) {
	TableFunction function("feishu_bitable_tables", {LogicalType::VARCHAR}, TablesScan, TablesBind, TablesInitGlobal);
	function.named_parameters["secret"] = LogicalType::VARCHAR;
	CreateTableFunctionInfo info(std::move(function));
	info.descriptions.push_back(
	    MakeFunctionDescription("List tables in a Feishu/Lark Bitable app (table_id, name, revision).",
	                            {"SELECT * FROM feishu_bitable_tables('<app_token>');",
	                             "SELECT * FROM feishu_bitable_tables('<app_token>', secret => 'my_feishu');"},
	                            {LogicalType::VARCHAR}, {}, {"feishu", "bitable"}));
	loader.RegisterFunction(std::move(info));
}

} // namespace feishu
} // namespace duckdb
