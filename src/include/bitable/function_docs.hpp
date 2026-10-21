#pragma once

#include "duckdb.hpp"
#include "duckdb/parser/parsed_data/create_function_info.hpp"

namespace duckdb {
namespace feishu {

//! Build catalog FunctionDescription for community-page / duckdb_functions() metadata.
//!
//! Scalar / aggregate: set real parameter_names (e.g. {"name"}) so duckdb_functions()
//! does not fall back to col0/col1.
//!
//! Table functions with named_parameters: leave parameter_names empty. DuckDB 1.5.x
//! GetParameterNames either (a) uses the fallback path — positionals as colN, named
//! params by their real names — or (b) uses a flat parameter_names list aligned to
//! GetParameterTypes, which also walks the unordered named_parameters map. Snapshotting
//! that map at registration time is not stable across TableFunction copies, so filling
//! parameter_names either mis-pairs names/types or turns named params into colN.
//! Keep description / examples / categories; fix positional names when DuckDB grows a
//! hybrid path.
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
