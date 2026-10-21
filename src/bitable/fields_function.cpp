#include "bitable/fields_function.hpp"

#include "auth/token_cache.hpp"
#include "auth/token_provider.hpp"
#include "bitable/api_client.hpp"
#include "bitable/api_response.hpp"
#include "bitable/field_type.hpp"
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

struct FieldsBindData : public TableFunctionData {
	FieldsBindData(string app_token_p, string table_id_p, string secret_name_p, string view_id_p)
	    : app_token(std::move(app_token_p)), table_id(std::move(table_id_p)), secret_name(std::move(secret_name_p)),
	      view_id(std::move(view_id_p)) {
	}

	string app_token;
	string table_id;
	string secret_name;
	string view_id;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<FieldsBindData>(app_token, table_id, secret_name, view_id);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<FieldsBindData>();
		return app_token == other.app_token && table_id == other.table_id && secret_name == other.secret_name &&
		       view_id == other.view_id;
	}

	bool SupportStatementCache() const override {
		return false;
	}
};

struct FieldsGlobalState : public GlobalTableFunctionState {
	explicit FieldsGlobalState(string access_token_p) : access_token(std::move(access_token_p)) {
	}

	string access_token;
	string page_token;
	bool has_more = true;
	vector<FieldMetadata> buffered_rows;
	std::unordered_set<string> seen_page_tokens;
	idx_t buffered_offset = 0;
	idx_t consecutive_empty_pages = 0;
};

static string RequiredStringArgument(const Value &value, const char *argument_name) {
	if (value.IsNull()) {
		throw InvalidInputException("feishu_bitable_fields: %s must not be NULL", argument_name);
	}
	auto result = value.GetValue<string>();
	if (result.empty()) {
		throw InvalidInputException("feishu_bitable_fields: %s must not be empty", argument_name);
	}
	return result;
}

static string OptionalNamedString(const named_parameter_map_t &parameters, const string &name) {
	auto entry = parameters.find(name);
	if (entry == parameters.end()) {
		return string();
	}
	if (entry->second.IsNull()) {
		throw InvalidInputException("feishu_bitable_fields: %s must not be NULL", name);
	}
	auto result = entry->second.GetValue<string>();
	if (result.empty()) {
		throw InvalidInputException("feishu_bitable_fields: %s must not be empty", name);
	}
	return result;
}

static string BuildFieldsUrl(const FieldsBindData &bind_data, const string &page_token) {
	string result = string(FEISHU_BITABLE_API_ROOT) + StringUtil::URLEncode(bind_data.app_token) + "/tables/" +
	                StringUtil::URLEncode(bind_data.table_id) +
	                "/fields?page_size=" + std::to_string(FEISHU_METADATA_PAGE_SIZE);
	if (!bind_data.view_id.empty()) {
		result += "&view_id=" + StringUtil::URLEncode(bind_data.view_id);
	}
	if (!page_token.empty()) {
		result += "&page_token=" + StringUtil::URLEncode(page_token);
	}
	return result;
}

static void FetchNextPage(ClientContext &context, const FieldsBindData &bind_data, FieldsGlobalState &state) {
	ApiClient client(context);
	HeaderMap headers;
	headers["Authorization"] = "Bearer " + state.access_token;
	auto response = client.Get(BuildFieldsUrl(bind_data, state.page_token), headers);
	auto page = ParseFieldsMetadataPage(response.status_code, response.body);
	for (auto &field : page.rows) {
		if (!field.type_name && field.type_id) {
			field.type_name = FeishuFieldTypeName(*field.type_id);
		}
	}
	AdvanceMetadataPaginationCursor("feishu_bitable_fields", page.has_more, page.page_token, state.page_token,
	                                state.seen_page_tokens);
	state.has_more = page.has_more;
	state.buffered_rows = std::move(page.rows);
	state.buffered_offset = 0;
}

static unique_ptr<FunctionData> FieldsBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
	auto app_token = RequiredStringArgument(input.inputs[0], "app_token");
	auto table_id = RequiredStringArgument(input.inputs[1], "table_id");
	auto secret_name = OptionalNamedString(input.named_parameters, "secret");
	auto view_id = OptionalNamedString(input.named_parameters, "view_id");
	// Validate locally even when LIMIT 0 removes initialization and scanning.
	// Token exchange remains lazy; static schema discovery needs no network.
	GetFeishuCredentials(context, secret_name);

	names = {"field_id", "field_name", "type_id", "type_name", "is_primary", "description", "property_json"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::VARCHAR,
	                LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::JSON()};
	return make_uniq<FieldsBindData>(std::move(app_token), std::move(table_id), std::move(secret_name),
	                                 std::move(view_id));
}

static unique_ptr<GlobalTableFunctionState> FieldsInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<FieldsBindData>();
	auto credentials = GetFeishuCredentials(context, bind_data.secret_name);
	string access_token;
	if (!credentials.user_access_token.empty()) {
		access_token = credentials.user_access_token;
	} else {
		TokenProvider token_provider(context, GetGlobalTokenCache());
		access_token = token_provider.GetTenantAccessToken(credentials.app_id, credentials.app_secret);
	}
	if (access_token.empty()) {
		throw IOException("feishu_bitable_fields: authentication returned an empty access token");
	}
	return make_uniq<FieldsGlobalState>(std::move(access_token));
}

static void FieldsScan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<FieldsBindData>();
	auto &state = input.global_state->Cast<FieldsGlobalState>();

	for (auto &column : output.data) {
		FlatVector::Validity(column).SetAllValid(STANDARD_VECTOR_SIZE);
	}
	auto field_ids = FlatVector::GetData<string_t>(output.data[0]);
	auto field_names = FlatVector::GetData<string_t>(output.data[1]);
	auto type_ids = FlatVector::GetData<int32_t>(output.data[2]);
	auto type_names = FlatVector::GetData<string_t>(output.data[3]);
	auto is_primary = FlatVector::GetData<bool>(output.data[4]);
	auto descriptions = FlatVector::GetData<string_t>(output.data[5]);
	auto properties = FlatVector::GetData<string_t>(output.data[6]);
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
					    "feishu_bitable_fields: exceeded maximum consecutive empty pages during pagination");
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
			if (row.field_id) {
				field_ids[output_index] = StringVector::AddString(output.data[0], *row.field_id);
			} else {
				FlatVector::Validity(output.data[0]).SetInvalid(output_index);
			}
			if (row.field_name) {
				field_names[output_index] = StringVector::AddString(output.data[1], *row.field_name);
			} else {
				FlatVector::Validity(output.data[1]).SetInvalid(output_index);
			}
			if (row.type_id) {
				type_ids[output_index] = *row.type_id;
			} else {
				FlatVector::Validity(output.data[2]).SetInvalid(output_index);
			}
			if (row.type_name) {
				type_names[output_index] = StringVector::AddString(output.data[3], *row.type_name);
			} else {
				FlatVector::Validity(output.data[3]).SetInvalid(output_index);
			}
			if (row.is_primary) {
				is_primary[output_index] = *row.is_primary;
			} else {
				FlatVector::Validity(output.data[4]).SetInvalid(output_index);
			}
			if (row.description) {
				descriptions[output_index] = StringVector::AddString(output.data[5], *row.description);
			} else {
				FlatVector::Validity(output.data[5]).SetInvalid(output_index);
			}
			if (row.property_json) {
				properties[output_index] = StringVector::AddString(output.data[6], *row.property_json);
			} else {
				FlatVector::Validity(output.data[6]).SetInvalid(output_index);
			}
		}
		state.buffered_offset += emit_count;
		output_count += emit_count;
	}

	output.SetCardinality(output_count);
}

} // namespace

void RegisterFieldsFunction(ExtensionLoader &loader) {
	TableFunction function("feishu_bitable_fields", {LogicalType::VARCHAR, LogicalType::VARCHAR}, FieldsScan,
	                       FieldsBind, FieldsInitGlobal);
	function.named_parameters["secret"] = LogicalType::VARCHAR;
	function.named_parameters["view_id"] = LogicalType::VARCHAR;
	CreateTableFunctionInfo info(std::move(function));
	info.descriptions.push_back(MakeFunctionDescription(
	    "List field metadata for a Feishu/Lark Bitable table (field_id, type, primary flag, property_json).",
	    {"SELECT * FROM feishu_bitable_fields('<app_token>', '<table_id>');",
	     "SELECT * FROM feishu_bitable_fields('<app_token>', '<table_id>', view_id => '<view_id>');"},
	    {LogicalType::VARCHAR, LogicalType::VARCHAR}, {}, {"feishu", "bitable"}));
	loader.RegisterFunction(std::move(info));
}

} // namespace feishu
} // namespace duckdb
