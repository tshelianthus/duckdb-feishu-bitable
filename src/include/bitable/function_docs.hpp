#pragma once

#include "duckdb.hpp"
#include "duckdb/parser/parsed_data/create_function_info.hpp"

namespace duckdb {
namespace feishu {

//! Build catalog FunctionDescription for community-page / duckdb_functions() metadata.
//!
//! DuckDB 2.0 duckdb_functions() prefers FunctionSignature parameter names when that
//! vector is the same length as the parameter types. FunctionDescription::parameter_names
//! is only the fallback used when the counts differ; missing slots become colN.
//!
//! Scalars: name the signature parameter itself (SetName). A description entry of
//! "name" does not override a signature parameter that is still called col0.
//!
//! Table functions: positional arguments stay colN (AddParameter(LogicalType)).
//! Optional named arguments are TypedKwargs options (WithTypedKwargs). A keyword-only
//! parameter without a default is required, so omitting it fails overload resolution
//! before bind. duckdb_functions() lists those options after the positional names.
//! Leave description parameter_names empty so the signature names are reported.
inline FunctionDescription MakeFunctionDescription(string description, vector<string> examples,
                                                   vector<LogicalType> parameter_types = {},
                                                   vector<string> parameter_names = {},
                                                   vector<string> categories = {}) {
	FunctionDescription doc;
	doc.description = std::move(description);
	doc.examples = std::move(examples);
	doc.parameter_types = std::move(parameter_types);
	doc.parameter_names = std::move(parameter_names);
	doc.categories = std::move(categories);
	return doc;
}

} // namespace feishu
} // namespace duckdb
