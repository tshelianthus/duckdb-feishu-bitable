#define DUCKDB_EXTENSION_MAIN

#include "feishu_bitable_extension.hpp"

#include "bitable/copy_function.hpp"
#include "bitable/fields_function.hpp"
#include "bitable/function_docs.hpp"
#include "bitable/scan_function.hpp"
#include "bitable/tables_function.hpp"
#include "bitable/url_parser.hpp"
#include "common/error.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "secrets/feishu_secret.hpp"

namespace duckdb {

namespace {

//! Load-smoke scalar kept for standing-gate coverage. Not part of the public API.
void FeishuBitableSmokeScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "feishu_bitable ready: " + name.GetString());
	});
}

void RegisterSmokeScalar(ExtensionLoader &loader) {
	ScalarFunction smoke("feishu_bitable", {LogicalType::VARCHAR}, LogicalType::VARCHAR, FeishuBitableSmokeScalarFun);
	CreateScalarFunctionInfo info(std::move(smoke));
	info.descriptions.push_back(feishu::MakeFunctionDescription(
	    "Internal load-smoke helper for extension tests; not part of the public API.",
	    {"SELECT feishu_bitable('world');"}, {LogicalType::VARCHAR}, {"name"}, {"internal"}));
	loader.RegisterFunction(std::move(info));
}

//! Rewrites `FROM 'https://...feishu.cn/base/...'` into a read_feishu_bitable
//! call; every other table name is left to DuckDB.
unique_ptr<TableRef> BitableReplacementScan(ClientContext &context, ReplacementScanInput &input,
                                            optional_ptr<ReplacementScanData> data) {
	auto table_name = ReplacementScan::GetFullPath(input);
	if (!feishu::IsBitableUrl(table_name)) {
		return nullptr;
	}
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ConstantExpression>(Value(table_name)));
	auto table_function = make_uniq<TableFunctionRef>();
	table_function->function = make_uniq<FunctionExpression>("read_feishu_bitable", std::move(children));
	return std::move(table_function);
}

void LoadInternal(ExtensionLoader &loader) {
	RegisterSmokeScalar(loader);
	feishu::RegisterFeishuSecretType(loader);
	feishu::RegisterTablesFunction(loader);
	feishu::RegisterFieldsFunction(loader);
	feishu::RegisterScanFunction(loader);
	feishu::RegisterCopyFunction(loader);

	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.replacement_scans.emplace_back(BitableReplacementScan);
}

} // namespace

void FeishuBitableExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string FeishuBitableExtension::Name() {
	return "feishu_bitable";
}

std::string FeishuBitableExtension::Version() const {
#ifdef EXT_VERSION_FEISHU_BITABLE
	return EXT_VERSION_FEISHU_BITABLE;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(feishu_bitable, loader) {
	duckdb::LoadInternal(loader);
}
}
