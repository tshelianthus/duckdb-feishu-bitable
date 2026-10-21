#pragma once

#include <cstddef>
#include <string>

namespace duckdb {
class ExtensionLoader;
namespace feishu {

//! Registers the `COPY ... TO '<target>' (FORMAT feishu_bitable)` writer.
//! The writeback contract is frozen in .specs/03_API_CONTRACT.md section 8.
void RegisterCopyFunction(ExtensionLoader &loader);

//! Validates the batch_create success envelope before advancing COPY progress.
//! A matching array length alone cannot confirm malformed record entries.
bool ResponseConfirmsWriteBatch(const std::string &body, size_t expected_rows);

} // namespace feishu
} // namespace duckdb
