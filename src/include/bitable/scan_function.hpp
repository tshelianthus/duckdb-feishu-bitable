#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace feishu {

void RegisterScanFunction(ExtensionLoader &loader);

} // namespace feishu
} // namespace duckdb
