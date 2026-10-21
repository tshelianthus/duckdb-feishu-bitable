# Agent Execution Guidelines: duckdb-feishu-bitable

You are an expert systems engineer and database extension specialist working on `duckdb-feishu-bitable` (a DuckDB Community Extension for Feishu/Lark Bitable).

## 0. Language & Build Stack
- **Language**: **C++17**. The extension is built on the official C++ `duckdb/extension-template` (CMake + `extension-ci-tools` makefiles).
- **Dependencies**: OpenSSL (HTTPS/TLS) and HTTP/JSON dependencies declared via `vcpkg.json`.
- Entry point: `DUCKDB_CPP_EXTENSION_ENTRY(feishu_bitable, loader)` in `src/feishu_bitable_extension.cpp`.

## 1. Core Principles & SSD Rules
- **Strict Spec Adherence**: Always read `todolist.md` and `.specs/` before writing code. Never invent SQL function names, parameters, or return types not defined in specifications.
- **Loadable Every Commit**: Every commit that touches build, entry, or registration code MUST keep the extension load path working, even if SQL functions return `NotImplemented`.
  **Automatic verification (authoritative)**: `make debug && make test_debug` must pass (covers `test/sql/feishu_bitable.test`), and `make test_unit` must pass when `src/bitable/url_parser.cpp` or `src/bitable/api_response.cpp` change.
  Prefer an explicit `ThrowFeishuError("... is not implemented yet")` over a broken or crashing loadable binary.
- **Vectorized First**: All table function scans must emit data via DuckDB `DataChunk` vectors in batches (`STANDARD_VECTOR_SIZE`), never per-row scalar iterations.
- **Zero Panic / Zero Crash**: Never `std::abort`, never UB, never uncaught exceptions. Validate inputs and raise DuckDB query errors (`InvalidInputException`, `IOException`).
- **Secret Isolation**: Use DuckDB Secret Manager (`TYPE feishu`) for `app_id` and `app_secret`. Never log or leak credentials in error messages or traces.
- **Credential Placeholders Only**: 纳入版本控制的任何文件中禁止出现真实的 `app_id`、`app_secret`、`app_token`、`table_id`、访问令牌或租户域名；示例一律使用尖括号或 `XXXX` 占位符，并在提交前运行 `scripts/check_no_secrets.sh`。
- **TDD Workflow**: For every function implemented, write the corresponding `.test` (SQLLogicTest) file first or simultaneously.
- **Catalog Discoverability**: Register via `Create*FunctionInfo` + `FunctionDescription` (`description`, `examples`, optional `categories`). Scalars must set real `parameter_names` (no `colN`). Table functions with `named_parameters` leave `parameter_names` empty on DuckDB 1.5.x so named args stay visible — see `.specs/03_API_CONTRACT.md` §7 and `src/include/bitable/function_docs.hpp`. Gate with `duckdb_functions()` in SQLLogicTest. Agents cannot read README over a SQL connection.
- **Test Tiers (benchmarked against `gsheets`)**: pure logic goes to Catch2 unit tests in `test/unit/` (units compiled there must stay self-contained: std + DuckDB exception types only, and exception messages built by string concatenation, never `printf`-style formatting); SQL-observable contracts go to offline `test/sql/*.test`; live API round trips go to `test/sql/integration_*.test` behind `require-env`. Do not expose helper scalar functions purely to make internals testable.

## 2. Project Naming Rules
- GitHub Repo: `duckdb-feishu-bitable`
- DuckDB Extension Name: `feishu_bitable` (EXT_NAME in Makefile, TARGET_NAME in CMakeLists.txt)
- SQL Load command: `LOAD feishu_bitable;` (Never use hyphens in SQL extension names!)
- Binary Artifact: `feishu_bitable.duckdb_extension`

## 3. Essential Commands
- Init submodules: `git submodule update --init --recursive`
- Build Debug: `make debug`
- Build Release: `make release`
- Run SQL Tests: `make test_debug`
- Run Unit Tests: `make test_unit` (standalone Catch2 build under `test/unit`, no full DuckDB link)
- Test with CLI (optional): `duckdb -unsigned` -> `LOAD './build/debug/extension/feishu_bitable/feishu_bitable.duckdb_extension';`
- Load smoke in CI/local: `make test_debug` (runs `test/sql/feishu_bitable.test`)

## 4. CI Trigger Policy
- 普通 `git push origin dev` 不启动主 CI；主 workflow 只保留 `pull_request` 与
  `workflow_dispatch` 触发方式。
- 需要对当前 `dev` 明确启动检查时，使用：
  ```sh
  gh workflow run MainDistributionPipeline.yml --ref dev
  gh run watch
  ```
- 不要使用 `git push -o ci=1` 作为 GitHub Actions 条件；GitHub Actions 没有标准的
  push-option 条件变量。启动检查以 `gh workflow run` 为准。
