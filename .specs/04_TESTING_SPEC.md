# Testing & Verification Specification: duckdb-feishu-bitable

测试分为三层，对标 `gsheets` 扩展的分层方式：

| 层级 | 位置 | 执行命令 | 覆盖对象 |
| :--- | :--- | :--- | :--- |
| 单元测试 (Catch2) | `test/unit/` | `make test_unit` | 不依赖 DuckDB 运行时的纯逻辑：URL 解析、参数优先级、飞书响应体解析、写值编码、写批次累积、写侧重试分类与 HTTP 响应体上限。以 `test/unit/stubs/duckdb_stubs.cpp` 打桩异常类型，独立 CMake 工程，秒级构建 |
| SQLLogicTest（离线） | `test/sql/*.test` | `make test_debug` / `make test_release` | 注册面、Bind 契约、报错文案、替换扫描拦截与让渡、Secret 双模式（模式 A/模式 B）互斥与脱敏，以及显式 COPY 的选项/目标/凭据前置校验；不发起任何网络请求 |
| SQLLogicTest（在线） | `test/sql/integration_*.test` | 受保护环境执行 `.github/workflows/SQLTests.yml` 的 `make test_sql_online_debug`；本地可执行 `make test_debug` / `make test_release` | 端到端真实 API 往返；模式 A 使用 `FEISHU_TEST_APP_ID`、`FEISHU_TEST_APP_SECRET`、`FEISHU_TEST_APP_TOKEN`、`FEISHU_TEST_URL`；缺少任一变量时整文件跳过，请求失败时测试失败 |

公共 PR CI 与社区 CI 不注入 Feishu 凭据。它们必须通过构建、扩展加载、离线 SQLLogicTest、Catch2 单元测试和代码质量检查；在线文件因缺少 `require-env` 变量而跳过属于预期通过状态。真实 API 测试只允许在私有或受保护的手动/内部 job 中运行，凭据以 CI Secret 映射为进程环境变量，不得写入日志、命令行参数或仓库文件。

本地在线门禁将凭据保存在未纳入版本控制的根目录 `.env.local`：`make test_debug` 与 `make test_release` 的内部 runner 都会在该文件存在时以 Bash 导出其变量；文件不存在时，两者都保持上游的直接执行行为。因此 Debug 与 Release 对在线文件的执行/跳过条件完全一致。受保护 CI 通过 `.github/workflows/SQLTests.yml` 的可选 `workflow_call` secrets 供给同名环境变量；凭据检查 job 与实际测试 job 均绑定 `integration-tests` environment，因此 environment-scoped secrets 不会被误判为缺失；缺失时显式 warning 并跳过在线 job，存在时不吞掉 HTTP/连接错误。

自动化在线门控变量为：`FEISHU_TEST_APP_ID`、`FEISHU_TEST_APP_SECRET`、`FEISHU_TEST_APP_TOKEN`、`FEISHU_TEST_URL`。`FEISHU_TEST_TABLE_ID`、`FEISHU_TEST_EMPTY_TABLE_ID` 与 `FEISHU_TEST_PAGINATION_TABLE_ID` 仅用于人工探查，不是当前 `integration_*.test` 的自动化门禁变量。

读侧在线文件：`integration_first_table.test`（首表回退）、`integration_pushdown.test`（投影 / LIMIT / SCHEMA_MODE）、`integration_search_pushdown.test`（H-2 谓词 search 与 GET list 双路径等价、VIEW_ID / 不可下推谓词回退 list、H-3 sole `record_id =` 单条 GET）。受保护 `make test_sql_online_debug` 按该顺序执行这三项（写侧 `integration_copy_writeback.test` 仍走独立 `make test_sql_write_debug`）。

集成测试使用模式 A（`APP_ID` + `APP_SECRET`）；自建应用需开通 `bitable:app:readonly` / `bitable:app` / `base:table:read` 任一权限。模式 B 仅保留给人工探查，不作为可复现的 CI 凭据。

被单元测试覆盖的纯逻辑不再向 SQL 层暴露辅助标量函数：`test/unit/` 直接编译 `src/bitable/url_parser.cpp` 与 `src/bitable/api_response.cpp`，因此这两个 TU 必须保持自包含（只依赖 std 与 DuckDB 异常类型，异常消息使用字符串拼接而非 `printf` 风格格式化）。

## 1. 站立门禁 (Standing Gate: Load Smoke)
在 Phase 0 之后，任何代码提交均必须通过此门禁：
1. `make debug` 成功产出 `build/debug/extension/feishu_bitable/feishu_bitable.duckdb_extension`。
2. `make test_debug` 成功运行 `test/sql/feishu_bitable.test`。
3. 扩展能正常被 `LOAD feishu_bitable;` 载入，不会发生段错误、符号丢失或崩溃。

## 2. 算子测试矩阵
每个函数必须覆盖以下用例：
1. **Happy Path（主路径）**：合法参数、正常响应流式填充。
2. **Boundary Cases（边界值）**：空表格（0 行）、空字段（0 列）、极大字段内容、翻页临界点（单页正好满或只有 1 行）。
3. **Invalid Inputs（非法入参）**：
   - 非法 `app_token`、非法 `table_id`、不存在的 `SECRET`。
   - 负数或超大 `PAGE_SIZE`。
   - 不支持的 `SCHEMA_MODE`（如 `'unsupported_mode'` 应报错）。
4. **NULL Handling（空值处理）**：
   - 字段在部分行存在、部分行缺失（稀疏表格）。
   - 缺失字段被准确标记为 NULL，无脏数据残留。
5. **Pushdown Verification（下推优化验证）**：
   - 投影下推：只查询特定列时，抓包/Mock 验证仅向 API 传递对应字段。
   - Limit 下推：`LIMIT 5` 时在拉取完第一页后立即停止后续请求。

## 3. 对标体验功能测试规约 (Benchmarked Features)

针对对标 Google Sheets 扩展落地的零摩擦交互特性，按上表分层编写对应的独立测试集：

### 3.1 浏览器 URL 解析测试 (`test/unit/bitable/test_url_parser.cpp`)
- **标准 URL 解析**：
  - 飞书标准格式：`https://sample.feishu.cn/base/bascn123456?table=tbl789012&view=vew345678`，验证正确提取 `app_token=bascn123456`、`table_id=tbl789012`、`view_id=vew345678`。
  - Lark 国际域名：`https://sample.larksuite.com/base/bascn123456?table=tbl789012`，验证提取成功。
  - 路径附加格式：包含 `/wiki/` 挂载路径、端口、Hash 片段（`#...`）、百分号编码及附加 Query 参数的 URL，验证参数提取健壮性。
  - 大小写与首尾空白：scheme、host、query key 全部大小写不敏感，首尾空白自动裁剪。
- **参数优先级覆盖**（`ResolveBitableTarget`）：
  - URL 中包含 `table=tblOld`，但调用显式指定 `table_id := 'tblNew'`，验证以显式参数为准；未被覆盖的 `view` 仍沿用 URL 值。
- **异常输入验证**：
  - 非飞书/Lark 域名（含 `feishu.cn.example.com` 这类后缀伪装）、协议缺失、Path 中缺少 `app_token`，抛出可读的 `InvalidInputException`，且消息以调用函数名为前缀。
- **拦截谓词**（`IsBitableUrl`）：命中 Bitable URL 返回 true，其余（普通表名、文件名、非法域名、空串）一律 false。

对应的 SQL 层断言收敛到 `test/sql/url_parser.test`：只验证 Bind 阶段可观测的行为——非法 URL 在凭据解析与网络请求之前报错，合法 URL 顺利进入凭据解析。

### 3.2 默认首表自动回退测试
- **响应解析层**（`test/unit/bitable/test_api_response.cpp`，以录制的 `GET /apps/{app_token}/tables` 报文断言）：
  - 多表报文返回 `tables[0].table_id`。
  - `items` 为空数组 / `null` / 缺失，以及首表缺少 `table_id` 时，抛出 `InvalidInputException("feishu_bitable: base contains no tables")`。
  - 业务错误码 `code != 0`、HTTP 4xx（`InvalidInputException`）与 5xx（`IOException`）、非法 JSON 均按契约归一化，绝不崩溃。
- **Bind 契约层**（`test/sql/fallback_first_table.test`）：
  - URL 未指定 table 与纯 `app_token` 两种入口均不再要求 `table_id`，直接进入凭据解析。
- **端到端**（`test/sql/integration_first_table.test`，需模式 A 的 `FEISHU_TEST_APP_ID` / `FEISHU_TEST_APP_SECRET` / `FEISHU_TEST_APP_TOKEN` / `FEISHU_TEST_URL`）：
  - 纯 token 与 Base URL 两条回退路径命中同一张首表，行数一致。

### 3.3 Replacement Scan 语法测试 (`test/sql/replacement_scan.test`)
- **透明重写扫描**：
  - 测试 `SELECT * FROM 'https://sample.feishu.cn/base/bascn123?table=tbl456';`，验证其结果与显式调用 `read_feishu_bitable` 完全等价。
- **Replacement Scan 结合首表回退**：
  - 测试 `SELECT * FROM 'https://sample.feishu.cn/base/bascn123';`，验证直接执行首表扫描。
- **非目标表名让渡（Fallthrough）**：
  - 测试普通表名、`*.parquet` 文件名与非 Feishu/Lark 域名 URL，验证替换扫描回调返回 `nullptr`，不拦截 DuckDB 原生扫描或其它文件格式。
  - 拦截谓词本身的取值矩阵在 `test/unit/bitable/test_url_parser.cpp` 中断言。

### 3.4 双轨兼容性与别名测试 (`test/sql/alias_and_compatibility.test`)
- 验证 `read_bitable(...)` 别名与 `read_feishu_bitable(...)` 输出完全一致。
- 验证原有 `feishu_bitable_scan(app_token, table_id)` 保持原有行为，零兼容性破坏。

## 4. 错误与韧性验证
- **429 限流退避**：模拟连续 429 响应，验证指数退避重试生效。
- **凭据脱敏验证**：测试故意触发鉴权失败时，输出的报错信息中不包含 Secret 明文。
- **ASan 内存检测**：运行完整测试集无内存泄漏或越界读写。

## 5. Feishu 回归防线

以下用例属于必须长期保留的回归契约，不依赖真实凭证即可通过录制响应或纯逻辑单元测试验证：

### 5.1 Token Cache
- 相同 `app_id` 但不同 `app_secret` 不得命中同一个缓存条目。
- 同一凭证的并发获取只允许一次刷新请求，其余调用复用刷新结果。
- token 剩余有效期小于刷新窗口时必须刷新；无效或非正的 `expire` 必须失败。
- 连续创建大量不同凭证时 cache 和刷新锁的内存规模必须受上限约束。
- 任意鉴权失败响应不得在异常文本中出现完整 `app_secret`、token 或 Authorization header。

### 5.2 Pagination
- `has_more = true` 且 `page_token` 缺失时必须失败。
- 返回的 `page_token` 与请求游标相同时必须失败，不能重复请求同一页。
- `has_more = false` 时允许缺失或为空的 `page_token`，并正确结束扫描。
- `/tables`、`/fields`、`/records` 三类接口都必须覆盖以上游标矩阵。
- `LIMIT N` 满足后不得请求后续记录页；`COUNT(*)` 或零列投影不得退化为全字段网络读取。

### 5.3 Schema 与稀疏记录
- schema 只能由字段元数据决定，不能由首条记录或首页记录决定。
- 后续页出现首条记录没有的字段时，不得静默扩展、重排或丢弃已绑定 schema；未知字段按 JSON 或明确的忽略策略处理。
- 同一字段在不同记录中缺失、为 `null` 或为空数组时，分别验证 SQL NULL、SQL NULL 和空 LIST 的语义。
- 每个 `DataChunk` 批次都要验证 validity mask，防止上一批次的非 NULL 值残留到当前批次。

### 5.4 Bind、URL 与 Replacement Scan
- 缺失参数、非法 schema mode、无 Secret、非法 URL 和空 Base 必须在 Bind 阶段失败，即使查询带有 `LIMIT 0`。
- 显式 `table_id`/`view_id` 必须覆盖 URL 中的同名参数；未覆盖的 URL 参数继续生效。
- Replacement Scan 只拦截合法 Feishu/Lark Bitable URL；普通 HTTPS URL、文件路径和伪造域名必须返回 `nullptr`。
- Base URL 未指定 table 时只允许走明确的首表回退逻辑；空 Base 必须返回可识别的 InvalidInputException。

## 6. 回写测试分层（范围 B 当前状态）

回写继续沿用三层测试，并且在线层必须使用专用写入 Base，禁止复用读取回归 Base：

- 单元测试：已覆盖字段类型分类、NULL/空值、JSON 转义、URL wire object、数字与时间边界、批次累积器和写侧三态重试分类；不访问网络。尚缺 E-3.4 要求的全部精确切分组合、`UNKNOWN_RESULT` 后不再提交的独立状态机断言，以及 E-3.5 的完整响应/异常矩阵。
- 离线 SQLLogicTest：`test/sql/copy_writeback.test` 已覆盖 COPY 注册、未知选项、`batch_size` 上下界、`file_extension`、缺 `table_id`、空上游仍执行 Bind、非法 URL 和无可用 Secret；均在发起网络请求前失败。选项类型错误、命名 Secret 不存在、缺省 FORMAT 让渡（写本地 CSV 而非调用本扩展）与 format 名近似值由 catalog 解析等断言均已补齐，共 13 条。
- 私有在线测试：`test/sql/integration_copy_writeback.test` 已建立并由**独立于读侧**的四项凭据变量加一项非敏感 marker 变量门控——`FEISHU_WRITE_TEST_APP_TOKEN`、`FEISHU_WRITE_TEST_TABLE_ID`、`FEISHU_WRITE_TEST_APP_ID`、`FEISHU_WRITE_TEST_APP_SECRET`、`FEISHU_WRITE_TEST_PREFIX`（缺任一项则整文件跳过，公共 CI 即如此）。I-5 人员字段读回段另需 `FEISHU_WRITE_TEST_OPEN_ID`（写入应用/机器人的 `open_id` 夹具；缺则该段 `require-env` 跳过，不跳过文件前半）。写入身份与读侧一致，使用应用凭据（`app_id` + `app_secret`）换取 `tenant_access_token`，不引入会过期的委托用户令牌。专用写入表至少含：`name`（文本）、`amount`（数字）、`status`（单选，含 `ready`）、`formula_column`（公式）、`person`（人员）、`link`（单向关联，指向同表）。专用写入 Base 的创建与受保护 environment `write-tests` 的 Secret 配置属 E-5.2（👤），在完成前**不得宣称真实 API 写回已验收**。测试覆盖专用 Base 的批量追加、逐列写后读回、零行不变、整批失败不产生副作用、跨批次部分完成，以及 I-5 的合法 `open_id` / 同表 `record_id` 读回；结果未知的传输恢复仍须按 E-5.5 的人工场景验证。写入 Secret 只注入受保护 job，`scripts/cleanup_write_test_records.sh` 在 workflow 前后按 marker 前缀清理测试记录。

写入公共 CI 的通过条件仍然是无凭据下完成构建和离线测试；不能通过给公共 CI 添加写权限 Secret 来“消除”在线测试跳过。

本次范围不包含 dev→main merge、tag、GitHub Release 或社区收录 PR；测试规约的完成状态不得以发布动作代替。
