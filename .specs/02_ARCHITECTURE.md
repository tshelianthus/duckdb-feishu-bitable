# Architecture Specification: duckdb-feishu-bitable

## 1. Tech Stack
- **Language**: **C++17** (与 DuckDB 核心及官方扩展模板对齐)。
- **Build System**: **CMake** (>= 3.5)，由 `extension-ci-tools` 驱动 `make` 命令。
- **Package Management**: **vcpkg**（管理 OpenSSL 及网络/JSON 依赖）。
- **Extension Skeleton**: 基于官方 `duckdb/extension-template`，提供静态库和动态加载库目标。

## 2. Core Subsystems

```
+-------------------------------------------------------------------------+
|                           DuckDB SQL Engine                             |
|  1. Table Function: read_feishu_bitable(...) / feishu_bitable_scan(...) |
|  2. Replacement Scan: SELECT * FROM 'https://...feishu.cn/base/...'     |
+-------------------------------------------------------------------------+
                                    |
                                    v
+-------------------------------------------------------------------------+
|                      URL Parser Subsystem (C++17)                       |
|  - Parse Browser URL (feishu.cn / larksuite.com)                        |
|  - Extract app_token, table_id, view_id                                 |
|  - Default First Table Fallback (Auto-fetch tables[0].table_id)         |
+-------------------------------------------------------------------------+
                                    |
                                    v
+-------------------------------------------------------------------------+
|                       DuckDB Table Function Layer                       |
|   - Bind: Metadata fetch & Schema inference                             |
|   - GlobalState: Page cursor & buffer management                        |
|   - Scan: Batch convert records into DataChunk (Vectorized)             |
+-------------------------------------------------------------------------+
          |                                       |
          v                                       v
+------------------------+             +----------------------+
|    Secrets / Auth      |             |     Type Mapping     |
| - TYPE feishu Secret   |             | - Feishu -> DuckDB   |
| - TokenProvider (HTTP) |             | - Typed / JSON Mode  |
| - TokenCache (Mutex)   |             | - ValidityMask (Null)|
+------------------------+             +----------------------+
          |
          v
+-------------------------------------------------------------------------+
|                          Network & Resilience                           |
| - ApiClient (HTTPS GET/POST)                                            |
| - Retryer (Exponential Backoff with Jitter for 429 / 5xx)               |
+-------------------------------------------------------------------------+
                                    |
                                    v
+-------------------------------------------------------------------------+
|                         Feishu Open Platform                            |
|   https://open.feishu.cn/open-apis/bitable/v1/...                       |
+-------------------------------------------------------------------------+
```

### 2.1 Secret & Token Subsystem
- 注册 `TYPE feishu` 密钥，接收 `app_id` 与 `app_secret`。
- `TokenCache` 单例或上下文缓存管理，采用读写锁保护，7200s 有效期，提前 300s 触发刷新。
- Token cache 的 key 必须区分凭证身份，不能只使用 `app_id`；至少应覆盖命名 Secret 身份或 `app_id` 与 `app_secret` 的不可逆指纹，避免密钥轮换或多个 Secret 并存时复用错误 token。
- 全局 token cache 和并发刷新锁必须有容量上限与淘汰策略；缓存只保存在进程内，不得写入查询结果、错误消息或日志。
- 同一凭证的并发刷新必须合并，避免多个查询同时击穿 tenant token 接口；刷新函数抛出的异常必须保持 DuckDB 异常类型，不得泄漏凭证。

### 2.2 Table Function Execution Architecture
- **Bind 阶段**：在发起记录扫描前完成参数、Secret、目标 Base/Table 和字段元数据校验；通过 `feishu_bitable_fields` API 预先探查目标表的字段列表与类型，动态设置 DuckDB 的输出列结构与类型。`LIMIT 0` 也不能绕过必要的 Bind 校验。
- 输出 schema 必须以字段元数据为权威，不得根据首条记录或首页记录推断字段集合。记录页之间字段缺省是正常情况，不能因此改变 schema 或丢列。
- **Scan 阶段**：向量化填充 `DataChunk`（每次至多 `STANDARD_VECTOR_SIZE` 批次），避免按行生成开销。
- 缺失字段和显式 `null` 必须写入 SQL NULL；每个向量批次开始时应清理或正确覆盖 validity，避免前一批次的值残留。
- 记录 API 的 `page_token` 是不透明游标，只能原样透传和 URL 编码，不能按数字 offset 推算或修改。
- 当响应 `has_more = true` 时，返回的 `page_token` 必须非空且不能与请求游标相同；违反时立即抛出错误，禁止出现重复拉取或无限循环。
- 记录扫描额外使用有界的近期游标检查与固定最大页数保险；不保留与总页数线性增长的完整游标集合，超过页数上限时以错误终止。
- **Pushdown 支持**：
  - **Projection Pushdown**：根据 SQL 实际查询的列，在向飞书发起 records API 时仅请求对应的 `field_names`，避免拉取未使用的字段。
  - **Limit 提前终止**：DuckDB v1.5.5 的 `TableFunction` 未提供 limit 下推接口（仅有 `projection_pushdown` / `filter_pushdown` / `sampling_pushdown`，`TableFunctionInitInput` 也不携带 limit）。本扩展改由惰性分页达成等效效果：每次 `scan` 调用只请求一页，`LIMIT N` 满足后执行器停止拉取，后续分页请求不再发出。

### 2.3 Network & Resilience
- 内置带抖动的指数退避机制（针对 HTTP 429 Too Many Requests 及服务瞬态故障）。
- 仅对明确的瞬态状态码、飞书限流业务码和可恢复网络错误重试；认证失败、资源不存在、参数错误等请求级错误不得盲目重试。
- 优先遵循响应中的 `Retry-After`，同时设置最大等待上限，避免恶意或异常响应让查询无限阻塞。
- HTTP 状态码、飞书业务 `code`、`msg` 和网络错误应归一化为可行动的 DuckDB 错误；错误中不得回显完整 `app_secret`、tenant token 或 Authorization header。
- 绝不在错误消息、堆栈或日志中泄漏 App Secret 和 Token。

### 2.4 URL Parser Subsystem & Replacement Scan Registration
- **URL Parser 模块职责 (`src/bitable/url_parser.cpp`, `src/include/bitable/url_parser.hpp`)**：
  - **协议与域名识别**：精准匹配 `feishu.cn` 与 `larksuite.com` 域名（含租户自定义子域名与 Wiki 空间关联路径），提取 `/base/<app_token>`。
  - **参数提取**：自 URL Path 中抽取 `app_token`；自 Query String 中抽取 `table`（对应 `table_id`）与 `view`（对应 `view_id`）。
  - **首表自动回退 (Default First Table Fallback)**：当 URL 或调用参数中未指定 `table` / `table_id` 时，驱动元数据探查（`feishu_bitable_tables`）获取首张数据表（`tables[0].table_id`）作为扫描目标；若 Base 下无可用表则抛出明确的 `InvalidInputException`。
  - **参数覆盖优先级**：显式命名参数（如 `table := 'tbl...'`, `view_id := 'vew...'`）严格优先于 URL Query 中解析的参数。
- **Replacement Scan 在扩展入口的注册机制 (`src/feishu_bitable_extension.cpp`)**：
  - 在扩展入口 `DUCKDB_CPP_EXTENSION_ENTRY` / `FeishuBitableExtension::Load` 阶段，向 DuckDB 的 `DBConfig::GetConfig(context).replacement_scans` 注册全局替换扫描回调函数。
  - **拦截规则**：当 SQL `FROM '...'` 遇到字符串表名时，判断字符串是否以 `http://` 或 `https://` 开头且包含飞书/Lark 多维表格特征特征路径（如 `/base/`）。
  - **等价重写**：命中特征时，构造 `TableFunctionRef` 透明重写为 `read_feishu_bitable('<url>')` 表函数调用；未命中则返回 `nullptr`，交还 DuckDB 默认扫描处理。
- Replacement Scan 必须使用窄匹配：只拦截确认属于 Feishu/Lark Bitable 的 URL，普通 HTTPS URL、文件路径和其它云服务 URL 必须让渡给 DuckDB 或其它扩展。
- URL 中的 `table`/`view` 与显式 SQL 参数必须有固定优先级，显式参数覆盖 URL 参数；解析失败应在 Bind 阶段报出调用函数名和脱敏后的输入诊断。

### 2.5 Feishu API Boundary Rules
- `/tables`、`/fields` 和 `/records` 三类分页接口统一使用 `has_more` + `page_token` 语义，并复用同一套游标合法性校验。
- `page_size` 必须在客户端限制到服务端契约范围内；不能因为某一接口接受更大的值，就将该值套用于所有接口。
- 空 Base、空字段集合、空记录集和记录字段缺省是不同状态，分别按 API 契约处理，不能用“首行为空”推断 schema 或权限失败。

### 2.6 Writeback Architecture (Future Phase)
- 写入 MVP 采用 DuckDB `COPY` writer：上游查询结果以 `DataChunk` 批量消费，转换为飞书 records batch-create 请求；不通过逐行 SQL 标量函数实现回写。
- 写入目标、认证和数据写入必须显式提供，禁止因为查询某个远程表而隐式回写；目标表不得依赖“首表回退”。
- 第一阶段只做 append-only。每个批次成功后才推进本地进度；网络失败、超时或响应不确定时不得自动重复提交，直到确定幂等策略或提供显式重试语义。
- 写入 API、字段映射、批量上限、跨批次部分完成与错误恢复必须独立于读取 API 建立契约；复用 Secret Manager、TokenCache、Retryer，但不能复用只读错误语义掩盖写入结果。
- 写入凭据应支持独立的最小写权限 Secret 配置；公共 CI 不保存或注入写入凭据，所有真实回写测试使用专用隔离 Base。
- 写算子固定使用 `REGULAR_COPY_TO_FILE` 单写者模式：批次顺序、进度推进与“结果未知”状态必须由扩展自身可控，不得依赖并发 writer 语义或文件式清理保障。
- DuckDB 的 `write_empty_file` 默认 true，会让 0 行查询仍然执行 global init；因此“0 行 ⇒ 零远端请求”必须由扩展自己在 sink/finalize 内保证，不能依赖 DuckDB 的默认行为。
- 复杂字段优先保留 JSON 结构；只有契约明确的标量类型才做 Typed Mode 转换。未知字段类型必须降级为 JSON，不能静默丢弃。
