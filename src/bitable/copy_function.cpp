#include "bitable/copy_function.hpp"

#include "auth/token_provider.hpp"
#include "bitable/api_client.hpp"
#include "bitable/api_response.hpp"
#include "bitable/field_type.hpp"
#include "bitable/url_parser.hpp"
#include "bitable/write_value_encoding.hpp"
#include "common/error.hpp"
#include "common/retry.hpp"
#include "bitable/write_retry.hpp"
#include "bitable/write_state_machine.hpp"
#include "secrets/feishu_secret.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/copy_info.hpp"
#include "yyjson.hpp"

#include <algorithm>
#include <mutex>
#include <unordered_set>

namespace duckdb {
namespace feishu {

using namespace duckdb_yyjson; // NOLINT

namespace {

//! Format name; `(FORMAT feishu_bitable)` is the only way to select this writer.
constexpr const char *COPY_FORMAT = "feishu_bitable";
//! Prefix used by the shared metadata helpers for their own diagnostics.
constexpr const char *COPY_FUNCTION_NAME = "feishu_bitable";

//! Batch bounds frozen by .specs/03_API_CONTRACT.md section 8.2 (500 comes from
//! the batch_create endpoint contract, not from a defensive guess).
constexpr idx_t COPY_BATCH_SIZE_DEFAULT = 500;
constexpr idx_t COPY_BATCH_SIZE_MIN = 1;
constexpr idx_t COPY_BATCH_SIZE_MAX = 500;
//! Termination guard for the field-metadata pagination loop.
constexpr idx_t COPY_MAX_FIELD_PAGES = 1000000;

//! Mirrors the accepted source types of .specs/03_API_CONTRACT.md section 8.3.
bool SourceTypeIsWritable(const LogicalType &type, WriteTargetKind kind) {
	switch (kind) {
	case WriteTargetKind::TEXT:
		return type.id() == LogicalTypeId::VARCHAR;
	case WriteTargetKind::URL:
		return type.id() == LogicalTypeId::VARCHAR;
	case WriteTargetKind::NUMBER:
		switch (type.id()) {
		case LogicalTypeId::TINYINT:
		case LogicalTypeId::SMALLINT:
		case LogicalTypeId::INTEGER:
		case LogicalTypeId::BIGINT:
		case LogicalTypeId::UTINYINT:
		case LogicalTypeId::USMALLINT:
		case LogicalTypeId::UINTEGER:
		case LogicalTypeId::UBIGINT:
		case LogicalTypeId::HUGEINT:
		case LogicalTypeId::DOUBLE:
		case LogicalTypeId::FLOAT:
			return true;
		default:
			return false;
		}
	case WriteTargetKind::CHECKBOX:
		return type.id() == LogicalTypeId::BOOLEAN;
	case WriteTargetKind::DATE_TIME:
		return type.id() == LogicalTypeId::TIMESTAMP || type.id() == LogicalTypeId::TIMESTAMP_TZ;
	case WriteTargetKind::MULTI_SELECT:
	case WriteTargetKind::LINK:
	case WriteTargetKind::USER:
		return type.id() == LogicalTypeId::LIST && ListType::GetChildType(type).id() == LogicalTypeId::VARCHAR;
	default:
		return false;
	}
}

struct CopyBindData : public FunctionData {
	//! Raw COPY target as written by the user.
	string file_path;
	string app_token;
	string table_id;
	string api_root = FEISHU_BITABLE_API_ROOT;
	bool is_wiki = false;
	//! Name of the `TYPE feishu` secret; empty means "DuckDB default secret".
	string secret_option;
	//! Records per batch_create request.
	idx_t batch_size = COPY_BATCH_SIZE_DEFAULT;
	//! Query parameter for USER field ids; always sent on batch_create.
	string user_id_type = "open_id";
	FeishuCredentials credentials;
	//! Upstream column names and types, already stripped of partition columns
	//! by DuckDB (see Binder::BindCopyTo).
	vector<string> column_names;
	vector<LogicalType> column_types;
	//! Feishu type id of the target field each column maps to, positionally
	//! aligned with column_names.
	vector<int32_t> column_target_type_ids;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<CopyBindData>();
		result->file_path = file_path;
		result->app_token = app_token;
		result->table_id = table_id;
		result->api_root = api_root;
		result->is_wiki = is_wiki;
		result->secret_option = secret_option;
		result->batch_size = batch_size;
		result->user_id_type = user_id_type;
		result->credentials = credentials;
		result->column_names = column_names;
		result->column_types = column_types;
		result->column_target_type_ids = column_target_type_ids;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<CopyBindData>();
		return file_path == other.file_path && app_token == other.app_token && table_id == other.table_id &&
		       api_root == other.api_root && is_wiki == other.is_wiki && secret_option == other.secret_option &&
		       batch_size == other.batch_size && user_id_type == other.user_id_type &&
		       credentials.app_id == other.credentials.app_id &&
		       credentials.app_secret == other.credentials.app_secret &&
		       credentials.user_access_token == other.credentials.user_access_token &&
		       column_names == other.column_names && column_types == other.column_types &&
		       column_target_type_ids == other.column_target_type_ids;
	}
};

struct CopyGlobalState : public GlobalFunctionData {
	explicit CopyGlobalState(idx_t batch_size) : batches(batch_size) {
	}

	WriteBatchAccumulator batches;
	std::mutex mutex;
	idx_t confirmed_batches = 0;
	idx_t confirmed_rows = 0;
};

void FeishuCopyOptions(ClientContext &, CopyOptionsInput &input) {
	input.options["table_id"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::WRITE_ONLY);
	input.options["secret"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::WRITE_ONLY);
	input.options["batch_size"] = CopyOption(LogicalType::BIGINT, CopyOptionMode::WRITE_ONLY);
	input.options["user_id_type"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::WRITE_ONLY);
}

//! DuckDB deduplicates COPY output names after copy_to_select returns. Check
//! the original select-list names here so two source columns cannot silently
//! become `name` and `name_1` and then map to different remote fields.
vector<unique_ptr<Expression>> CopyToSelect(CopyToSelectInput &input) {
	std::unordered_set<string> seen_columns;
	for (auto &expression : input.select_list) {
		auto name = expression->GetName();
		auto normalized_name = StringUtil::Lower(name);
		if (!seen_columns.insert(normalized_name).second) {
			throw InvalidInputException("feishu_bitable: duplicate source column \"" + name + "\" cannot be written");
		}
	}
	return {};
}

//! Reads a VARCHAR option from the already type-validated COPY option map.
string CopyOptionString(const CopyFunctionBindInput &input, const string &name) {
	auto entry = input.info.options.find(name);
	if (entry == input.info.options.end() || entry->second.empty()) {
		return string();
	}
	return entry->second[0].ToString();
}

idx_t CopyBatchSize(const CopyFunctionBindInput &input) {
	auto entry = input.info.options.find("batch_size");
	if (entry == input.info.options.end() || entry->second.empty()) {
		return COPY_BATCH_SIZE_DEFAULT;
	}
	auto value = entry->second[0].GetValue<int64_t>();
	if (value < static_cast<int64_t>(COPY_BATCH_SIZE_MIN) || value > static_cast<int64_t>(COPY_BATCH_SIZE_MAX)) {
		throw InvalidInputException("feishu_bitable: option \"batch_size\" must be between 1 and 500");
	}
	return static_cast<idx_t>(value);
}

//! Section 8.2: only the three exact lowercase values; empty / case variants fail at Bind.
string CopyUserIdType(const CopyFunctionBindInput &input) {
	auto entry = input.info.options.find("user_id_type");
	if (entry == input.info.options.end() || entry->second.empty()) {
		return string("open_id");
	}
	auto value = entry->second[0].ToString();
	if (value == "open_id" || value == "union_id" || value == "user_id") {
		return value;
	}
	throw InvalidInputException("feishu_bitable: option \"user_id_type\" must be one of open_id, union_id, user_id");
}

HeaderMap AuthorizationHeaders(const string &access_token) {
	HeaderMap headers;
	headers["Authorization"] = "Bearer " + access_token;
	return headers;
}

string AccessToken(ClientContext &context, const CopyBindData &bind_data) {
	if (!bind_data.credentials.user_access_token.empty()) {
		return bind_data.credentials.user_access_token;
	}
	TokenProvider provider(context, GetGlobalTokenCache());
	return provider.GetTenantAccessToken(bind_data.credentials.app_id, bind_data.credentials.app_secret,
	                                     bind_data.api_root);
}

string RedactWriteMessage(const string &message, const CopyBindData &bind_data, const string &access_token) {
	auto result = SanitizeErrorMessage(message);
	result = RedactSecretValue(result, bind_data.credentials.app_secret);
	return RedactSecretValue(result, access_token);
}

string FieldsUrl(const CopyBindData &bind_data, const string &page_token) {
	string url = bind_data.api_root + "/apps/" + PercentEncodeUrlComponent(bind_data.app_token) + "/tables/" +
	             PercentEncodeUrlComponent(bind_data.table_id) + "/fields?page_size=100";
	if (!page_token.empty()) {
		url += "&page_token=" + PercentEncodeUrlComponent(page_token);
	}
	return url;
}

//! A Wiki URL identifies a node, not a Base; resolve the node to its app_token
//! before any table-scoped request, exactly like the read path does.
string ResolveWikiAppToken(ClientContext &context, const CopyBindData &bind_data) {
	ApiClient api(context);
	auto access_token = AccessToken(context, bind_data);
	try {
		// api_root is canonical and comes only from the URL parser or the default.
		auto origin = bind_data.api_root.substr(0, bind_data.api_root.find("/open-apis/"));
		auto response = api.Get(
		    origin + "/open-apis/wiki/v2/spaces/get_node?token=" + PercentEncodeUrlComponent(bind_data.app_token),
		    AuthorizationHeaders(access_token));
		if (response.status_code < 200 || response.status_code >= 300) {
			EnsureApiSuccess(response.status_code, nullptr);
		}
		JsonDocument json(response.body);
		auto root = json.Root();
		auto code = yyjson_is_obj(root) ? yyjson_obj_get(root, "code") : nullptr;
		if (!code || !yyjson_is_int(code)) {
			throw IOException("feishu_bitable: malformed wiki node response: expected integer code");
		}
		EnsureApiSuccess(response.status_code, root);
		auto data = yyjson_obj_get(root, "data");
		auto node = yyjson_is_obj(data) ? yyjson_obj_get(data, "node") : nullptr;
		if (!yyjson_is_obj(node)) {
			throw IOException("feishu_bitable: malformed wiki node response: expected object data.node");
		}
		auto obj_type = JsonString(yyjson_obj_get(node, "obj_type"));
		if (obj_type.empty()) {
			throw IOException("feishu_bitable: malformed wiki node response: missing non-empty obj_type");
		}
		if (obj_type != "bitable") {
			throw InvalidInputException("feishu_bitable: wiki node does not reference a Bitable base");
		}
		auto obj_token = JsonString(yyjson_obj_get(node, "obj_token"));
		if (obj_token.empty()) {
			throw IOException("feishu_bitable: malformed wiki node response: missing non-empty obj_token");
		}
		return obj_token;
	} catch (const InvalidInputException &error) {
		throw InvalidInputException(RedactWriteMessage(error.what(), bind_data, access_token));
	} catch (const IOException &error) {
		throw IOException(RedactWriteMessage(error.what(), bind_data, access_token));
	}
}

vector<FieldMetadata> LoadTargetFields(ClientContext &context, const CopyBindData &bind_data) {
	ApiClient api(context);
	vector<FieldMetadata> fields;
	string page_token;
	bool has_more = true;
	idx_t page_count = 0;
	std::unordered_set<string> seen_page_tokens;
	while (has_more) {
		auto access_token = AccessToken(context, bind_data);
		auto response = api.Get(FieldsUrl(bind_data, page_token), AuthorizationHeaders(access_token));
		FieldsMetadataPage page;
		try {
			page = ParseFieldsMetadataPage(response.status_code, response.body);
		} catch (const InvalidInputException &error) {
			throw InvalidInputException(RedactWriteMessage(error.what(), bind_data, access_token));
		} catch (const IOException &error) {
			throw IOException(RedactWriteMessage(error.what(), bind_data, access_token));
		}
		for (auto &row : page.rows) {
			fields.push_back(std::move(row));
		}
		has_more = page.has_more;
		AdvanceMetadataPaginationCursor(COPY_FUNCTION_NAME, page.has_more, page.page_token, page_token,
		                                seen_page_tokens);
		if (has_more && ++page_count >= COPY_MAX_FIELD_PAGES) {
			throw IOException("feishu_bitable: fields pagination exceeded the maximum page limit");
		}
	}
	return fields;
}

WriteScalar WriteScalarFromValue(const Value &value, WriteTargetKind target, const string &column_name) {
	if (value.IsNull()) {
		return WriteScalar::Null();
	}
	try {
		switch (target) {
		case WriteTargetKind::TEXT:
			return WriteScalar::Text(StringValue::Get(value));
		case WriteTargetKind::URL:
			return WriteScalar::Text(StringValue::Get(value));
		case WriteTargetKind::NUMBER:
			if (value.type().IsIntegral()) {
				return WriteScalar::Integer(value.ToString());
			}
			return WriteScalar::Number(value.GetValue<double>());
		case WriteTargetKind::CHECKBOX:
			return WriteScalar::Boolean(value.GetValue<bool>());
		case WriteTargetKind::DATE_TIME:
			if (value.type().id() == LogicalTypeId::TIMESTAMP_TZ) {
				return WriteScalar::TimestampMicros(TimestampTZValue::Get(value).value);
			}
			return WriteScalar::TimestampMicros(TimestampValue::Get(value).value);
		case WriteTargetKind::MULTI_SELECT:
		case WriteTargetKind::LINK:
		case WriteTargetKind::USER: {
			const auto &children = ListValue::GetChildren(value);
			vector<string> values;
			vector<bool> has_null;
			values.reserve(children.size());
			has_null.reserve(children.size());
			for (const auto &child : children) {
				if (child.IsNull()) {
					values.emplace_back();
					has_null.push_back(true);
				} else {
					values.push_back(StringValue::Get(child));
					has_null.push_back(false);
				}
			}
			return WriteScalar::List(std::move(values), std::move(has_null));
		}
		default:
			throw InvalidInputException("feishu_bitable: column \"" + column_name +
			                            "\" has a non-writable Feishu target field");
		}
	} catch (const InvalidInputException &) {
		throw;
	} catch (const Exception &) {
		throw InvalidInputException("feishu_bitable: value in column \"" + column_name +
		                            "\" could not be converted for writeback");
	}
}

string EncodeRecord(const CopyBindData &bind_data, DataChunk &input, idx_t row) {
	string fields = "{";
	bool first = true;
	for (idx_t col = 0; col < bind_data.column_names.size(); col++) {
		auto value = input.GetValue(col, row);
		auto target = ClassifyWriteTarget(bind_data.column_target_type_ids[col]);
		auto entry = EncodeFieldsEntry(bind_data.column_names[col],
		                               WriteScalarFromValue(value, target, bind_data.column_names[col]), target);
		if (entry.empty()) {
			continue;
		}
		if (!first) {
			fields.push_back(',');
		}
		fields += entry;
		first = false;
	}
	fields += "}";
	return "{\"fields\":" + fields + "}";
}

string RecordsBatchCreateUrl(const CopyBindData &bind_data) {
	return BuildRecordsBatchCreateUrl(bind_data.api_root, bind_data.app_token, bind_data.table_id,
	                                  bind_data.user_id_type);
}

string BatchBody(const WriteBatch &batch) {
	string body = "{\"records\":[";
	for (idx_t i = 0; i < batch.rows.size(); i++) {
		if (i > 0) {
			body.push_back(',');
		}
		body += batch.rows[i];
	}
	body += "]}";
	return body;
}

WriteTransportError WriteTransportFromHttp(HttpTransportError error) {
	switch (error) {
	case HttpTransportError::CONNECTION:
	case HttpTransportError::PROXY_CONNECTION:
		return WriteTransportError::CONNECTION_FAILURE;
	case HttpTransportError::TLS_CONNECTION:
		return WriteTransportError::TLS_HANDSHAKE_FAILURE;
	case HttpTransportError::READ:
		return WriteTransportError::CONNECTION_INTERRUPTED;
	case HttpTransportError::CONNECTION_TIMEOUT:
		// httplib does not distinguish connect timeout from read timeout in its
		// public error enum, so the conservative write-side classification is
		// UNKNOWN_RESULT.
		return WriteTransportError::UNKNOWN;
	case HttpTransportError::WRITE:
	case HttpTransportError::TLS_PEER_CLOSED:
	case HttpTransportError::OTHER:
		return WriteTransportError::UNKNOWN;
	case HttpTransportError::NONE:
		return WriteTransportError::NONE;
	}
	return WriteTransportError::UNKNOWN;
}

void SubmitBatch(ClientContext &context, const CopyBindData &bind_data, CopyGlobalState &state,
                 const WriteBatch &batch) {
	ApiClient api(context);
	WriteStateBatch state_batch;
	state_batch.first_row = batch.first_row;
	state_batch.row_count = batch.rows.size();
	WriteProgress progress {state.confirmed_batches, state.confirmed_rows};
	string access_token;
	try {
		access_token = AccessToken(context, bind_data);
	} catch (const InvalidInputException &) {
		throw InvalidInputException(
		    WritePreflightFailureMessage(state_batch, progress, "access token could not be obtained"));
	} catch (const IOException &) {
		throw IOException(WritePreflightFailureMessage(state_batch, progress, "access token could not be obtained"));
	}

	// The transport only classifies and redacts; the state machine owns retry
	// bounds, progress advancement and the guarantee that an ambiguous outcome
	// is never replayed (.specs/06_WRITEBACK_RESEARCH.md section 1.4).
	auto attempt_fn = [&](const WriteStateBatch &, idx_t) -> WriteAttemptOutcome {
		WriteAttemptOutcome outcome;
		HttpResponse response;
		try {
			response = api.PostJsonNoRetry(RecordsBatchCreateUrl(bind_data), BatchBody(batch),
			                               AuthorizationHeaders(access_token));
		} catch (const IOException &) {
			// A client-side receive failure happens after the request may already
			// have been sent, and batch_create carries no idempotency key.
			outcome.result = WriteAttemptResult::UNKNOWN_RESULT;
			outcome.detail = "the HTTP response could not be collected";
			return outcome;
		}
		outcome = ClassifyWriteResponse(static_cast<int>(response.status_code), response.body, batch.rows.size(),
		                                WriteTransportFromHttp(response.transport_error));
		if (outcome.result == WriteAttemptResult::RETRY_SAFE) {
			TryParseRetryAfterMs(response.GetHeader("retry-after"), outcome.retry_after_ms);
		}
		if (outcome.result == WriteAttemptResult::UNKNOWN_RESULT &&
		    response.transport_error != HttpTransportError::NONE) {
			outcome.detail = "the HTTPS request failed: " + response.transport_error_message;
		}
		outcome.detail = RedactWriteMessage(outcome.detail, bind_data, access_token);
		return outcome;
	};
	auto on_retry = [](idx_t attempt, idx_t retry_after_ms) {
		RetryPolicy retry_policy;
		SleepForRetry(retry_policy, attempt, retry_after_ms);
	};
	RetryPolicy retry_policy;
	WriteStateMachineOptions state_options;
	state_options.max_attempts = retry_policy.max_attempts;

	auto next = RunBatchWithRetries(state_batch, progress, attempt_fn, on_retry, state_options);
	state.confirmed_batches = next.confirmed_batches;
	state.confirmed_rows = next.confirmed_rows;
}

//! Matches every upstream column against the target table by field name and
//! rejects what .specs/03_API_CONTRACT.md section 8.5 forbids. Errors never
//! carry credentials: they only name columns, fields and type ids.
void ValidateWriteColumns(const vector<string> &names, const vector<LogicalType> &types,
                          const vector<FieldMetadata> &fields, CopyBindData &result) {
	std::unordered_set<string> seen_columns;
	for (auto &name : names) {
		if (!seen_columns.insert(name).second) {
			throw InvalidInputException("feishu_bitable: duplicate source column \"" + name + "\" cannot be written");
		}
	}
	for (idx_t i = 0; i < names.size(); i++) {
		const FieldMetadata *match = nullptr;
		for (auto &field : fields) {
			if (field.field_name && *field.field_name == names[i]) {
				match = &field;
				break;
			}
		}
		if (!match) {
			throw InvalidInputException("feishu_bitable: no field named \"" + names[i] +
			                            "\" exists in the target table");
		}
		if (!match->type_id) {
			throw IOException("feishu_bitable: field \"" + names[i] + "\" is missing a type id");
		}
		auto type_id = *match->type_id;
		auto type_label = std::to_string(type_id);
		switch (ClassifyWriteTarget(type_id)) {
		case WriteTargetKind::READ_ONLY:
			throw InvalidInputException("feishu_bitable: column \"" + names[i] +
			                            "\" maps to read-only Feishu field \"" + names[i] + "\" (type " + type_label +
			                            ") and cannot be a COPY target");
		case WriteTargetKind::UNSUPPORTED:
			throw InvalidInputException("feishu_bitable: column \"" + names[i] + "\" maps to Feishu field \"" +
			                            names[i] + "\" (type " + type_label +
			                            ") which v0.2 writeback does not support");
		case WriteTargetKind::UNKNOWN:
			throw InvalidInputException("feishu_bitable: column \"" + names[i] +
			                            "\" maps to unknown Feishu field type " + type_label + "; refusing to write");
		default:
			break;
		}
		if (!SourceTypeIsWritable(types[i], ClassifyWriteTarget(type_id))) {
			throw InvalidInputException("feishu_bitable: column \"" + names[i] + "\" has DuckDB type " +
			                            types[i].ToString() + " which cannot be written to Feishu field type " +
			                            type_label);
		}
		result.column_target_type_ids.push_back(type_id);
	}
}

unique_ptr<FunctionData> CopyToBind(ClientContext &context, CopyFunctionBindInput &input, const vector<string> &names,
                                    const vector<LogicalType> &sql_types) {
	// .specs/03_API_CONTRACT.md section 8.7: DuckDB's Binder consumes most
	// generic write options before this callback runs, so they are invisible
	// here. `file_extension` is the one signal that survives, and setting it is
	// never meaningful for a remote Bitable target.
	if (!input.file_extension.empty()) {
		throw InvalidInputException(
		    "feishu_bitable: option \"file_extension\" is not supported for remote Bitable writeback");
	}
	auto result = make_uniq<CopyBindData>();
	result->file_path = input.info.file_path;
	result->secret_option = CopyOptionString(input, "secret");
	result->batch_size = CopyBatchSize(input);
	result->user_id_type = CopyUserIdType(input);

	// Reuse the read-side parser; a separate URL implementation would drift.
	auto target =
	    ResolveBitableTarget(result->file_path, CopyOptionString(input, "table_id"), string(), COPY_FUNCTION_NAME);
	result->app_token = target.app_token;
	result->table_id = target.table_id;
	result->api_root = target.api_root;
	result->is_wiki = target.is_wiki;

	// Writeback never guesses a target: the first-table fallback is read-only
	// (.specs/02_ARCHITECTURE.md section 2.6).
	if (result->table_id.empty()) {
		throw InvalidInputException("feishu_bitable: COPY writeback requires a table_id (add ?table=<table_id> to "
		                            "the URL or pass the table_id option)");
	}
	for (idx_t i = 0; i < sql_types.size(); i++) {
		// Reject source types outside the entire section 8.3 allow-list before
		// looking up credentials or fetching metadata, even for empty inputs.
		if (!SourceTypeIsWritable(sql_types[i], WriteTargetKind::TEXT) &&
		    !SourceTypeIsWritable(sql_types[i], WriteTargetKind::NUMBER) &&
		    !SourceTypeIsWritable(sql_types[i], WriteTargetKind::CHECKBOX) &&
		    !SourceTypeIsWritable(sql_types[i], WriteTargetKind::DATE_TIME) &&
		    !SourceTypeIsWritable(sql_types[i], WriteTargetKind::MULTI_SELECT) &&
		    !SourceTypeIsWritable(sql_types[i], WriteTargetKind::LINK) &&
		    !SourceTypeIsWritable(sql_types[i], WriteTargetKind::USER)) {
			throw InvalidInputException("feishu_bitable: column \"" + names[i] + "\" has DuckDB type " +
			                            sql_types[i].ToString() + " which cannot be written to Feishu");
		}
	}

	try {
		result->credentials = GetFeishuCredentials(context, result->secret_option);
	} catch (const Exception &) {
		// GetFeishuCredentials throws DuckDB-native messages whose text is not
		// stable across the extension boundary (it arrives JSON-encoded), and
		// the writeback contract requires the feishu_bitable prefix on every
		// diagnostic. Report the actionable cause instead of echoing it.
		if (result->secret_option.empty()) {
			throw InvalidInputException("feishu_bitable: no TYPE feishu secret is available; create one with APP_ID "
			                            "and APP_SECRET, or USER_ACCESS_TOKEN");
		}
		throw InvalidInputException(
		    "feishu_bitable: requested secret does not exist or is not a usable TYPE feishu secret");
	}

	if (result->is_wiki) {
		result->app_token = ResolveWikiAppToken(context, *result);
	}

	auto fields = LoadTargetFields(context, *result);
	result->column_names = names;
	result->column_types = sql_types;
	ValidateWriteColumns(names, sql_types, fields, *result);
	return std::move(result);
}

unique_ptr<GlobalFunctionData> CopyToInitializeGlobal(ClientContext &, FunctionData &bind_data, const string &) {
	// Deliberately performs no I/O. `write_empty_file` defaults to true, so a
	// zero-row COPY still reaches this callback; "0 rows means zero remote
	// write requests" is guaranteed here and in CopyToFinalize, not by DuckDB
	// (see .specs/02_ARCHITECTURE.md section 2.6).
	auto &copy_bind_data = bind_data.Cast<CopyBindData>();
	return make_uniq<CopyGlobalState>(copy_bind_data.batch_size);
}

unique_ptr<LocalFunctionData> CopyToInitializeLocal(ExecutionContext &, FunctionData &) {
	return make_uniq<LocalFunctionData>();
}

void CopyToSink(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &global_state,
                LocalFunctionData &, DataChunk &input) {
	input.Flatten();
	auto &copy_bind_data = bind_data.Cast<CopyBindData>();
	auto &copy_global_state = global_state.Cast<CopyGlobalState>();
	std::lock_guard<std::mutex> guard(copy_global_state.mutex);
	for (idx_t row = 0; row < input.size(); row++) {
		string encoded;
		try {
			encoded = EncodeRecord(copy_bind_data, input, row);
		} catch (const InvalidInputException &error) {
			WriteProgress progress {copy_global_state.confirmed_batches, copy_global_state.confirmed_rows};
			throw InvalidInputException(WriteRowFailureMessage(
			    copy_global_state.batches.RowsSeen() + 1, progress,
			    RedactWriteMessage(error.what(), copy_bind_data, copy_bind_data.credentials.user_access_token)));
		} catch (const Exception &) {
			WriteProgress progress {copy_global_state.confirmed_batches, copy_global_state.confirmed_rows};
			throw InvalidInputException(WriteRowFailureMessage(copy_global_state.batches.RowsSeen() + 1, progress,
			                                                   "the source value could not be converted"));
		}
		WriteBatch ready;
		if (copy_global_state.batches.AddRow(std::move(encoded), ready)) {
			SubmitBatch(context.client, copy_bind_data, copy_global_state, ready);
		}
	}
}

void CopyToCombine(ExecutionContext &, FunctionData &, GlobalFunctionData &, LocalFunctionData &) {
}

void CopyToFinalize(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &global_state) {
	auto &copy_bind_data = bind_data.Cast<CopyBindData>();
	auto &copy_global_state = global_state.Cast<CopyGlobalState>();
	std::lock_guard<std::mutex> guard(copy_global_state.mutex);
	WriteBatch ready;
	if (copy_global_state.batches.Flush(ready)) {
		SubmitBatch(context, copy_bind_data, copy_global_state, ready);
	}
}

CopyFunctionExecutionMode CopyToExecutionMode(bool, bool) {
	return CopyFunctionExecutionMode::REGULAR_COPY_TO_FILE;
}

} // namespace

void RegisterCopyFunction(ExtensionLoader &loader) {
	CopyFunction function(COPY_FORMAT);
	function.copy_to_select = CopyToSelect;
	function.copy_options = FeishuCopyOptions;
	function.copy_to_bind = CopyToBind;
	function.copy_to_initialize_global = CopyToInitializeGlobal;
	function.copy_to_initialize_local = CopyToInitializeLocal;
	function.copy_to_sink = CopyToSink;
	function.copy_to_combine = CopyToCombine;
	function.copy_to_finalize = CopyToFinalize;
	function.execution_mode = CopyToExecutionMode;
	loader.RegisterFunction(std::move(function));
}

} // namespace feishu
} // namespace duckdb
