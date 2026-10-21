# Phase 7 回写调研纪要（E-1）

> 调研日期：2026-09-10；范围仅限批次 E-1。本文不是 SQL API 契约，不能据此提前实现 E-2 以后的工作。
>
> 结论优先级：端点级官方文档 / DuckDB v1.5.5 固定源码 > 官方通用指南 > SDK 生成的端点元数据。动态渲染的飞书端点页无法由本地文本抓取器完整导出，链接仍保留为一手核验入口；凡端点页未能直接确认的能力均标为“需实测”。

## 1. Feishu Bitable records 批量写入

官方端点：

- [新增多条记录（`batch_create`）](https://open.feishu.cn/document/server-docs/docs/bitable-v1/app-table-record/batch_create)：`POST /open-apis/bitable/v1/apps/{app_token}/tables/{table_id}/records/batch_create`
- [更新多条记录（`batch_update`）](https://open.feishu.cn/document/server-docs/docs/bitable-v1/app-table-record/batch_update)：`POST /open-apis/bitable/v1/apps/{app_token}/tables/{table_id}/records/batch_update`
- [多维表格 API 接入指南](https://open.feishu.cn/document/server-docs/docs/bitable-v1/bitable-overview)：批量操作、权限与限制的通用说明。

### 1.1 凭据、权限与资源 ACL

| 项目 | 结论 | 证据 / 后续约束 |
| --- | --- | --- |
| 可用访问凭证 | 两端点均接受 `tenant_access_token` 或 `user_access_token`。 | 端点级权限表；与现有 `TYPE feishu` 两种凭据模式一致。 |
| OpenAPI 权限范围 | 需满足“查看、评论、编辑和管理多维表格”（常用权限标识为 `bitable:app`）。 | 端点级权限表。只拥有读权限不能写入。 |
| 目标资源权限 | 授权范围不是充分条件：应用/授权用户仍须是该 Bitable 的协作者并具备可编辑权限；高级权限或行列权限也可拒绝请求。 | 通用指南与端点错误说明把无权访问归因于高级权限。**E-2.6 实测（2026-09-30）**：具备可编辑权限的应用身份对专用写入 Base 可成功 `batch_create` / `batch_update`；只读 scope 应用写 → `99991672`（缺 scope）；**有写 scope 但非协作者的应用写 → `403 / 91403 Forbidden`**，同 token 对该 Base 的各类**读**请求却全部 `200 / code=0`（详见 §1.5）。应用支路拒绝码已记录（证据等级 A，`403 / 91403`，脚本 `scripts/e2_6_api_facts.sh` 可复现）；「scope 已生效」的自建 Base 正向对照与读端点成功是仓外一次性探测（证据等级 B），机制解释有条件成立（在接受等级 B 的前提下，写入需要资源级协作者权限）。「应用或用户都必须是协作者」保留自官方权限表；用户身份支路本轮未复测。 |
| 最小授权 | 公开资料未给出低于 `bitable:app` 的端点级 scope 替代项；`user_access_token` 可在资源 ACL 允许时写入。 | **E-2.6 实测**：主路径用应用身份已验证可写；只读 scope 应用换得到 tenant token 但写入被拒，拒绝码为 `99991672`（缺 `[bitable:app, base:record:create]`），说明 scope 校验先于资源 ACL（见 §1.5）。不得把“可读取”误称为“可写入”。 |

### 1.2 单批上限与串行化

| 端点 | 当前可查结论 | 实现前处置 |
| --- | --- | --- |
| `batch_create` | 官方文档：单次最多 **500**。**E-2.6 实测（2026-09-29）**：500 → HTTP 200 / `code=0` / `data.records=500`；**501 亦成功**（HTTP 200 / `code=0` / `data.records=501`）。 | v0.2 契约仍冻结上界 **500**（与官方文档对齐，不把实测 501 放宽进产品契约）；客户端切分不得超过 500。 |
| `batch_update` | **E-2.6 实测消解 G1（2026-09-29）**：500 / 501 / 1000 → HTTP 200 / `code=0` 且返回条数与提交一致；**1001 → HTTP 200 / `code=1254104` / `msg=RecordAddOnceExceedLimit`**。 | 实测硬上限为 **1000**（非 500）。v0.2 仍不实现该端点；v0.3 若启用须以 1000 为上界。 |
| 并发 | 通用指南建议同一时刻对同一篇多维表格只发起一次写 API 请求。 | 写侧状态机须以同一 Base 的单写者 / 串行请求为默认，不可假定可安全并发。 |

**E-2.7 冻结结论（2026-09-11）**：v0.2 写入 MVP 只实现 append-only 的 `batch_create`，其单批上限 **500** 取自官方端点文档；**2026-09-29 实测**显示服务端当前可接受 501，但产品契约 **不放宽**，以免依赖未文档化行为。下界 1；已写入 `.specs/03_API_CONTRACT.md` §8.2。

### 1.3 成功、失败与“部分成功”

两接口请求体的核心为 `records` 数组：create 项提供 `fields`；update 项必须另含 `record_id`。端点成功响应的 `data.records` 是成功创建/更新后的记录列表（每项含 `record_id` 和 `fields`）；顶层仍以 `code` / `msg` 表示 API 调用结果。

官方通用指南明确规定：限制内的多条记录操作只有**全部成功或全部失败**，不存在每条独立成功/失败的部分结果；若部分参数不合法，该次请求提交的全部内容都失败，不影响线上数据。因此：

- MVP 不应设计或宣称存在可由响应直接消费的“per-record partial success”结构；成功时可将 `data.records` 与提交批次核对，失败时以整个 batch 失败处理。
- HTTP 超时、连接断开或无法解析响应不是“全部失败”的证据；远端是否已提交仍是**结果未知**，不得自动重放。
- 返回条数、响应记录顺序以及 API 内部失败时是否始终原子回滚，虽与通用指南一致，但应在 E-5 的专用写入 Base 上做在线验证；这是**需实测**，不应从 HTTP 200 或 `code = 0` 以外推导。

**E-5.4 实测补强（2026-09-29）**：单批超大文本触发 `1254130 TooLargeCell` 时，整批失败且此前基线行数不变（0 confirmed batches）；成功路径下 `batch_create` 的 `data.records` 条数与提交条数一致（见 §1.5 的 500/501）。跨批次时第 1 批成功落库、第 2 批失败不回滚第 1 批（E-5.5）。
### 1.4 幂等键

在两个端点的公开参数、JSON 请求体及公开错误说明中，未找到 `idempotency_key`、`client_token`、请求去重 header 或等价的服务端幂等承诺。故结论是：**公开文档没有可用的幂等键能力；服务端是否接受未文档化 header/字段为“需实测”，不得依赖。**

这不等同于断言服务端绝不可能做内部去重；它只意味着扩展不能把超时后的再次 `batch_create` 当作安全重试。`batch_update` 即使业务上较接近幂等，仍受并发修改、表达式字段和结果未知影响，亦不能在没有明确契约时自动重放。该结论直接支持 `.specs/02_ARCHITECTURE.md` §2.6 和 `.specs/03_API_CONTRACT.md` §8 的“未知结果不自动重复提交”要求。

## 2. DuckDB v1.5.5 `CopyFunction`

本仓库的 `duckdb` 子模块固定在 `v1.5.5`（`d8cdaa33fda8df955cc76ef58a280f68f4cd43fa`）。以下结论针对该固定版本，不能直接套用于其他 DuckDB 版本。

### 2.1 注册点与生命周期

DuckDB C++ 扩展应在扩展 `Load` / `DUCKDB_CPP_EXTENSION_ENTRY` 加载路径中：构造 `CopyFunction("<format>")`，填充所需回调，再调用 `ExtensionLoader::RegisterFunction(CopyFunction)`。注册实现会建立 `CreateCopyFunctionInfo` 并写入 system catalog；格式名由 `COPY ... (FORMAT <format>)` 解析选择。

一手源码：

- [`CopyFunction` 定义（v1.5.5）](https://github.com/duckdb/duckdb/blob/v1.5.5/src/include/duckdb/function/copy_function.hpp)
- [`ExtensionLoader::RegisterFunction(CopyFunction)`（v1.5.5）](https://github.com/duckdb/duckdb/blob/v1.5.5/src/main/extension/extension_loader.cpp#L126-L131)
- [DuckDB `COPY` SQL 文档](https://duckdb.org/docs/stable/sql/statements/copy.html)

写侧最小生命周期是 `copy_to_bind`（校验目标/选项并取得输入列名和 LogicalType）、`copy_to_initialize_global`、`copy_to_initialize_local`、`copy_to_sink`（接收 `DataChunk`）、`copy_to_combine` 与 `copy_to_finalize`。`CopyFunction` 还提供 `copy_options`、`execution_mode`、`prepare_batch` / `flush_batch` / `desired_batch_size` 等可选钩子。

核心 Parquet 实现是 v1.5.5 的直接先例：它构造 `CopyFunction("parquet")`、配置 bind/global/local/sink/combine/finalize/execution mode，最后 `loader.RegisterFunction(function)`；见 [`parquet_extension.cpp` L887–910](https://github.com/duckdb/duckdb/blob/v1.5.5/extension/parquet/parquet_extension.cpp#L887-L910)。JSON 扩展也展示格式别名通过复制同一 `CopyFunction` 并修改 `name` / `extension` 后分别注册；见 [`json_extension.cpp` L64–72](https://github.com/duckdb/duckdb/blob/v1.5.5/extension/json/json_extension.cpp#L64-L72)。

### 2.2 对本项目的架构含义（非 E-2 契约）

- `copy_to_sink` 的输入是 `DataChunk`，因此符合 §2.6 的向量化批量消费要求；不能退化为每行 SQL 标量调用。
- 默认 `COPY TO` 物理算子是面向“文件路径”的机制：它会把 `CopyInfo` 的目标作为 `file_path` 传入 global init，并包含文件创建、临时文件与失败清理逻辑。把远端 Bitable 标识塞进该槽位的精确目标编码、FORMAT 名称和 options 仍是 **E-2 冻结事项**，本文不定义语法。
- 远端副作用不能获得本地文件的回滚/删除保障。应使用 `REGULAR_COPY_TO_FILE` 或等效单写者模式，令批次顺序、进度和结果未知状态可控；是否需要/能够使用 `BATCH_COPY_TO_FILE` 仍为 **需原型验证**。
- `CopyFunctionReturnType` 只有 `CHANGED_ROWS`、`CHANGED_ROWS_AND_FILE_LIST`、`WRITTEN_FILE_STATISTICS`。后两者显然带文件语义；本项目采用 `CHANGED_ROWS`（返回单列 `Count`），其语义与返回形状已由 `.specs/03_API_CONTRACT.md` §8.6 冻结，不再是待验证问题。

### 2.3 社区写侧先例核验

**结论（2026-09-11 修订）：存在已被社区收录、且可核验源码的 SaaS records API 写入先例。**

先例是 [`evidence-dev/duckdb_gsheets`](https://github.com/evidence-dev/duckdb_gsheets)（C++，MIT），
它本身就是“把非文件型 SaaS API 做成 `CopyFunction` sink”的收录案例：

- 收录信息：`duckdb/community-extensions` 的 `extensions/gsheets/description.yml`（v0.0.9，
  `repo.ref: a209d8a42c8e35cd571467dd7ae2eb65fcb96f75`），描述为 `Read and write Google Sheets using SQL`。
- 源码位置：`src/gsheets_copy.cpp`、`src/include/gsheets_copy.hpp`；其头文件自述改编自
  `duckdb/postgres_scanner` 的 `src/include/postgres_binary_copy.hpp`。
- 注册面：`GSheetCopyFunction` 注册了 `copy_to_bind` / `copy_to_initialize_global` /
  `copy_to_initialize_local` / `copy_to_sink`，**未**注册 `copy_to_combine` 与 `copy_to_finalize`。
- 对外形状：`COPY <table> TO '<browser URL>' (FORMAT gsheet, ...)`，且显式选项优先于 URL 参数。

由此，本项目回写设计应当**照社区既有做法办**（目标用浏览器 URL、格式名必须显式书写、
选项走 `(FORMAT x, opt 'v')`、显式选项优先于 URL 参数），而不是按“无先例、需自行原型”重新论证。

补充说明：gsheets 提供的是**对外契约形状**的先例。DuckDB v1.5.5 固定版本下的 `CopyFunction`
生命周期（§2.1）与 `PhysicalCopyToFile` 行为（§2.2）仍是本项目的实现依据；不要因为 gsheets
未注册 `combine` / `finalize` 就推断它们对本项目不必要。

另一个可借鉴但形态不同的例子是 [sshfs](https://duckdb.org/community_extensions/extensions/sshfs)：
它让 `COPY ... TO 'sshfs://…/data.parquet'` 写远程文件，方式是实现文件系统协议，使 DuckDB 既有
Parquet/CSV writer 写文件；**不是** records API 写入。它可借鉴“显式远端目标、Secret scope、远端 I/O”，
不能据此推导 records API 的失败、原子性或幂等语义。

本结论取代了此前“无已验证先例，需原型验证”的判断；上述仓库路径、文件名与版本号均可一次点击核验。

## 3. 对后续批次的输入（不改变任务状态）

1. E-2 必须先消解 `batch_update` 的 500 / 1,000 差异，冻结 create/update 各自的运行批次大小；在此之前任何实现都不得假定 1,000 可用。
2. E-3 的 `CopyFunction` 应走官方 loader 注册路径，批量处理 `DataChunk`，并明确选定单写者执行模式。
3. E-4 必须把超时、断连和不可解析响应表示为“结果未知”；没有文档化幂等键前禁止自动重放。
4. E-5 的在线测试应使用独立写入 Base，验证应用/用户 token 两种凭据的实际编辑 ACL、端点批量上限、响应记录核对和未知结果恢复策略；公共 CI 不注入写凭据。

## 4. PostgreSQL FDW 生态对标（2026-09-12，供批次 H 参考）

> 背景：用户要求持续对标 PostgreSQL 第三方 FDW 生态（Supabase Wrappers、Steampipe Postgres FDW、
> ogr_fdw、Multicorn2），为本项目挖掘可复用的设计与流程经验。以下三条是**已核验、直接影响本文档既有
> 结论**的发现；触发新增实现动作的发现见 `.specs/05_TASKS.md` 批次 H，本节不重复列出。

### 4.1 单表格类 SaaS FDW 优先只读、不做 modify 的社区先例

Supabase Wrappers 的 Airtable adapter（源：`supabase.com/docs/guides/database/extensions/wrappers/airtable`）
**只读、不实现 `insert`/`update`/`delete`**，尽管 Wrappers 框架本身的 `ForeignDataWrapper` trait
完整支持这些回调。这与 gsheets（§2.3）、本项目的 append-only 回写范围（`.specs/03_API_CONTRACT.md` §8）
构成同一方向的两个独立先例：**同类“浏览器表格类 SaaS”数据源，即使底层框架支持全量 CRUD，
社区实现仍普遍选择只读或最小化写入**。这进一步支持本项目"v0.2 只做 append-only batch_create、
不做 UPDATE/DELETE"是符合生态惯例的选择，而非能力不足的权宜之计。

### 4.2 批量写入无同类先例佐证，本项目状态机设计维持原判

对标的三个支持写操作的项目（Supabase Wrappers、ogr_fdw、Multicorn2）在 FDW 框架层面的写回调
（`insert`/`update`/`delete`）均是**单行级 DML**，不存在"提交一批记录、部分确认部分未知"这类
批次边界问题；因此无法直接印证或推翻 `.specs/03_API_CONTRACT.md` §8 与 E-4 系列已冻结的
"批次内整批失败不产生副作用、结果未知绝不重放"状态机设计。Steampipe 是否支持写操作，
其公开 README 与插件开发文档均未提及，按调研要求如实标注为**未找到可核验来源**，不作为结论采信。

### 4.3 凭据管理方式对比：现状已优于多数同类方案，无需变更

- Steampipe：`.spc` 明文连接配置文件；Multicorn2 / ogr_fdw：`CREATE SERVER`/`CREATE USER MAPPING`
  的选项本身即明文存储在系统目录。
- Supabase Wrappers：依赖 Postgres Vault 做加密存储，是四者中安全性最高的方案，但绑定 Postgres
  生态（依赖 `pgsodium`/Vault 扩展），无法直接移植到 DuckDB。
- 本项目：DuckDB `TYPE feishu` Secret + Secret Manager 脱敏、`TokenCache` 按凭据指纹隔离，
  在“不依赖 Postgres 专属加密设施”的前提下已达到与 Supabase Wrappers 同等级别的凭据保护，
  且优于 Steampipe / Multicorn2 / ogr_fdw 的裸配置方案。

**结论：凭据管理机制维持现状，本条不触发任何新任务**（对应候选 H-10，已评估为"无需变更"）。

### 4.4 存档：若未来做 UPDATE/DELETE，rowid 列的设计输入（v0.3+，非当前契约）

ogr_fdw 要求显式 `fid` 列并通过 `updateable` 选项声明可写性；Multicorn2 的 `update(oldvalues, newvalues)` /
`delete(oldvalues)` 回调依赖框架声明的 `rowid_column`。若本项目未来（v0.3+）评估支持 `UPDATE`/`DELETE`，
两者共同的设计输入是：**用飞书 `record_id` 作为 rowid 列**，因为它是 Bitable 记录的唯一稳定标识、
读侧已在 `feishu_bitable_scan` 的输出中可见（详见 `.specs/03_API_CONTRACT.md`），无需额外映射。
本节仅存档该设计输入供未来评估参考，**不改变 v0.2 append-only 的现有范围**，也不生成任何新任务。

本文仅新增调研材料；未修改实现或 `.specs/03_API_CONTRACT.md`。H-1 的完成状态与证据已同步记录在 `.specs/05_TASKS.md`，其余 TODO 勾选状态未改变。

## 5. H-1：`records/search` 谓词 / 排序下推可行性（2026-09-12）

> 范围：为 v0.3+ 的读侧下推预研提供输入，不改变当前 `feishu_bitable_scan` 的 GET
> `records` 路径、SQL 契约或任务勾选状态。本文的“下推”仅指把 DuckDB 已接收的 filter/sort
> 下放到飞书 `POST /open-apis/bitable/v1/apps/{app_token}/tables/{table_id}/records/search`，
> 绝不表示 DuckDB v1.5.5 存在 LIMIT 下推接口。
>
> 证据优先级：官方 [检索记录](https://open.feishu.cn/document/uAjLw4CM/ukTMukTMukTM/reference/bitable-v1/app-table-record/search)
> 与官方 [记录筛选参数填写指南](https://open.feishu.cn/document/uAjLw4CM/ukTMukTMukTM/reference/bitable-v1/app-table-record/record-filter-guide)。
> 两页均为动态渲染，无法由本地文本抓取器导出参数表；故以链接为一手核验入口，并用其关联的
> 生成端点元数据（字段、上限、注释与请求/响应形状）交叉核验。当前环境未提供
> `FEISHU_TEST_APP_ID` / `FEISHU_TEST_APP_SECRET` / `FEISHU_TEST_APP_TOKEN` /
> `FEISHU_TEST_URL`，本次**没有**对真实 Base 发送请求；下表中标作“需在线验证”的行为不得在
> H-2 中未经测试而宣称为契约。

### 5.1 端点能力表

| 能力 | `records/search` 已核验形态 | 对 DuckDB 下推的结论 |
| --- | --- | --- |
| 请求与投影 | `POST .../records/search`；`filter`、`sort`、`field_names`、`view_id`、`automatic_fields` 在 JSON body；`page_size`、`page_token` 在 query。`field_names` 最多 200 个。 | 与现有投影下推可组合；搜索路径必须继续为零列扫描请求一个最小字段，不能因省略 `field_names` 意外取回全部字段。 |
| 等值 / 不等值 | 条件为 `{field_name, operator, value[]}`；`is` / `isNot` 可表达字段等于 / 不等于。目标值是字符串数组而非 DuckDB 标量。 | **部分可下推**：仅元数据已知、可无歧义编码为飞书 value 的标量字段；日期字段不能使用这两个运算符。不能把 SQL `NULL` 改写成 `is` / `isNot`。 |
| 比较 | `isGreater`、`isGreaterEqual`、`isLess`、`isLessEqual`。 | **部分可下推**：可映射 `>`、`>=`、`<`、`<=`，但必须按字段类型的官方 value 编码；日期比较另走官方日期筛选写法，不能套用 `is` / `isNot` / `isGreaterEqual` / `isLessEqual` 的通用映射。H-2 必须逐类型在线对照本地 DuckDB 比较结果。 |
| 文本 / 集合匹配 | `contains`、`doesNotContain`、`like`；官方说明明确前两者不支持日期字段，且**不支持 `IN`**。 | `contains` / `doesNotContain` / `LIKE` 只能作为飞书语义专用的可选优化，不能直接承诺等同 SQL `LIKE`、`IN`、数组包含或任意 JSON/LIST 谓词；默认保留本地求值。 |
| 空值 | `isEmpty`、`isNotEmpty`，value 可为空数组。 | 可作为 `IS NULL` / `IS NOT NULL` 的**候选**映射，但空字符串、空数组、公式/计算字段的服务端“空”语义尚未在线比对；H-2 前不得默认下推。 |
| 布尔逻辑与最大复杂度 | 一个 `filter` 对象只有 `conjunction: "and" | "or"` 和最多 **50** 个扁平 `conditions`；condition 不是子 filter，故请求 schema 不提供递归嵌套。 | 可下推的最大已核验表达式是**同一种连接词的单层、最多 50 个原子条件**（全 AND 或全 OR）。混合 AND/OR、括号嵌套、NOT（除单原子可逆运算符）、超过 50 条以及不可编码原子条件均不可整体下推；保持 GET + 本地过滤。 |
| 排序 | `sort` 为最多 **100** 个 `{field_name, desc}` 对象；`desc` 缺省/false 为升序，true 为降序。 | 端点能力如此。与 DuckDB 的等价性以 §8 的在线双路径为准，不再沿用本节写作时的“待验证”状态。 |
| 视图交互 | body 可带 `view_id`，但 `filter` 或 `sort` 非空时，请求按整张数据表做条件查询，`view_id` 被忽略。 | 任何已下推谓词或排序的查询不得同时依赖当前 `VIEW_ID` 的视图筛选/排序语义；这是切换端点时的正确性门槛，而非性能细节。 |
| 分页游标 | 一页最多 **500** 条（默认 20）；响应为 `data.items`、`has_more`、`page_token`，`has_more = true` 时返回下一页游标，另有 `total`。 | **兼容**现有惰性 DataChunk 扫描：继续原样透传不透明游标，并复用“真 `has_more` 不得配空/重复游标”的保护。`total` 不应改变 SQL 行数语义，也不能替代扫描结果。 |
| 返回记录与复合字段 | 每个 item 含 `record_id`、`fields`，可按 `automatic_fields` 返回 `created_time`、`last_modified_time`、创建人、修改人；search 额外可有 `record_url`（list 对应 `shared_url`）。`fields` 是同一 records 数据结构的字段对象。 | **兼容**现有 Typed/JSON 映射：基础字段仍按元数据映射，User/Attachment/Link/DuplexLink/Location/GroupChat/Formula/CreatedUser/ModifiedUser 保持 JSON。现有系统列读取已接受 `last_modified_time`（并兼容 `updated_time`），因此 search 的审计时间字段可直接接入；`record_url` 不在当前输出 schema，忽略即可。复合字段的具体 wire 值仍须 H-2 在线同表双路径逐类比对。 |

### 5.2 SQL 映射边界与实施前置

1. H-2 的安全首批范围应收窄为：同层全 AND（可扩展到同层全 OR）、不超过 50 个、每个原子条件都由字段元数据和已验证 value 编码支持的谓词；排序仅限经同一 Base 双路径结果比对的字段与 ASC/DESC 组合。任何一项不满足时继续走既有 GET `records` + DuckDB 本地过滤/排序路径。
2. 不能把飞书的 `contains`、`like`、`isEmpty`、日期相对条件或多值条件推断成 SQL 的普遍等价物。尤其不可下推 `IN`、任意嵌套 AND/OR、复杂 JSON/LIST 比较、`NULLS FIRST/LAST`、collation，或需要稳定 tie-break 的 `ORDER BY`。
3. 由于 filter/sort 会使 `view_id` 被忽略，H-2 必须把“存在 `VIEW_ID`”列为拒绝 search 下推的条件，除非随后有设计变更并以“表级 search + 本地重现视图语义”证明等价；当前不作此假设。
4. 必须增加受保护的在线测试：同一独立 Base 用 GET 本地过滤/排序与 POST search 双路径比较全量行、字段 JSON、分页边界、`automatic_fields` 与复合字段；还须覆盖日期、空值、同值排序、每种下推运算符、50 条边界和第 51 条拒绝下推。没有这些证据时，search 只可作为未启用的候选路径。

### 5.3 H-1 最终结论：**部分可行**

`records/search` 具备结构化 filter、最多 50 个同层条件、最多 100 个排序键、500 行游标分页和与
GET records 相同的 `items[].fields` 主载荷，故足以承载受限的标量谓词与普通多键排序下推；现有
Typed/JSON 映射及系统列解析在响应形态上无需重写。它同时缺少递归条件树和 `IN`，并对日期、空值、
文本匹配、视图忽略、NULL/collation/tie 排序语义及复合字段的等价性留下边界。因此结论明确为
**部分可行**：H-2 只能 opt-in 地下推已验证的扁平子集，其余查询必须保持当前本地语义与 GET 路径。

### 1.5 E-2.6 API 事实实测台账（2026-09-29；2026-09-30 身份契约变更 + 负例清零后复跑）

专用写入 Base（与读侧隔离）；**2026-09-30 起主写入身份为应用身份**（`FEISHU_E26_WRITE_APP_ID` + `FEISHU_E26_WRITE_APP_SECRET` → `tenant_access_token`），`user_access_token` 备选模式保留在脚本中但本地已不再注入。脚本 `scripts/e2_6_api_facts.sh`（修复：`batch_create`/`batch_update` payload 须包 `{records:[…]}`）。非协作者身份为 `FEISHU_E26_NONCOLLAB_APP_ID` + `FEISHU_E26_NONCOLLAB_APP_SECRET`（**具备 `bitable:app` 写 scope、从未加入该 Base 协作者列表**，由 agent 经 `lark.registerApp()` 自建应用后扫码授权，方法见 §7）。探测结束后 `batch_delete` 清理成功（2002 条，分批删除均为 `code=0`）。

| CASE | HTTP | code | msg（脱敏） | 记录数 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `write_auth` | 200 | 0 | ok | N/A | 应用凭据换取 `tenant_access_token` 成功 |
| `batch_create_500` | 200 | 0 | success | 500 | 与官方上限一致 |
| `batch_create_501` | 200 | 0 | success | 501 | 服务端当前接受 501；契约仍冻结 500 |
| `batch_update_500` | 200 | 0 | success | 500 | |
| `batch_update_501` | 200 | 0 | success | 501 | |
| `batch_update_1000` | 200 | 0 | success | 1000 | 消解 G1：上限不是 500 |
| `batch_update_1001` | 200 | 1254104 | RecordAddOnceExceedLimit | N/A | 硬拒绝 |
| `readonly_field_write` | 200 | 0 | success | 1 | 向公式字段 `formula_column` 做 `batch_update` **未拒绝**（疑似静默忽略）；扩展 Bind 仍须拒绝只读列 |
| （附加）SingleSelect 未知选项 | 200 | 0 | success | 1 | OpenAPI `batch_create` **自动新增选项**（观测到 `status.options` 增长）；**不能**用作远端整批失败注入 |
| （附加）超大文本 cell | 200 | 1254130 | TooLargeCell | N/A | 可靠的远端整批失败注入（在线用例改用此路径） |
| （附加）number 收字符串 | 200 | 1254061 | NumberFieldConvFail | N/A | 端点拒绝；扩展 Bind 已拦截 VARCHAR→Number |
| `readonly_credential_write` | 400 | 99991672 | missing scope `[bitable:app, base:record:create]` | N/A | **负例（3）闭环（证明 scope 门先于资源 ACL）**：只读 scope 应用可换取 tenant token（`readonly_app_auth` code=0）但写入被拒，且拒绝理由是 **scope 缺失**而非资源 ACL；因未走到 ACL 层，**G2 不能由本行消解，仍待负例（4）** |
| `noncollab_app_auth` | 200 | 0 | ok | N/A | 非协作者应用（**有写 scope**、未加入该 Base 协作者）换取 token 成功 |
| `noncollab_app_write` | 403 | 91403 | Forbidden | N/A | 应用支路拒绝码已记录（证据等级 A，`403 / 91403`，脚本 `scripts/e2_6_api_facts.sh` 可复现）；「scope 已生效」正向对照为等级 B，机制解释有条件成立（接受等级 B 时写入需资源级协作者权限） |
| `noncollab_user_write` | SKIPPED | — | optional_noncollab_user_identity_not_provided | — | 用户身份支路未复测；应用支路拒绝码为等级 A，正向对照为等级 B，机制解释有条件成立 |

**正向对照（防止假阴性，2026-09-30 单独探测）**：非协作者应用先对**自己创建的** Base 做 `fields` 创建与 `records/batch_create`，均 `HTTP 200 / code=0`，证明其 `bitable:app` 写 scope 真实生效；同一 token 对专用写入 Base 的同一写请求则得 `403 / 91403`。若无此对照，(4) 的 `91403` 可能与"scope 未生效"混淆。

**读/写不对称（新观测，修正先前假设）**：该非协作者应用对专用写入 Base 的 `apps` 元信息、`tables` 列表、`fields` 列表、`records` 列表**全部返回 `200 / code=0`**，只有写被拒。即"非协作者"约束的是**写入**，读在本 Base 的当前分享设置下对同租户应用开放。这修正了此前"非协作者连读都不行"的推断。上述读成功为仓外一次性探测（证据等级 B），`scripts/e2_6_api_facts.sh` 不发这些读请求。

**E-2.6 状态**：五组 (1)(2)(3)(4)(5) 均有实测证据。G1：`batch_update` 硬上限 1000（1001 → `1254104`），`batch_create` 官方 500、服务端当前接受 501、硬上限未探，产品契约仍冻结 500。G2：应用支路拒绝码已记录（证据等级 A，`403 / 91403`，脚本 `scripts/e2_6_api_facts.sh` 可复现）；「scope 已生效」的自建 Base 正向对照与读端点成功是仓外一次性探测（证据等级 B），机制解释有条件成立（在接受等级 B 的前提下，写入需要资源级协作者权限）；用户身份支路未复测。

## 6. 当前真实环境验收台账（2026-09-13，2026-09-29 增量）

本节只记录当前环境的可核验证据。

| 范围 | 当前证据 | 判定 |
| --- | --- | --- |
| Debug 构建与扩展加载 | `make debug` 成功；独立 DuckDB `LOAD` 烟测成功 | 已通过 |
| Release 构建 | `make release` 成功 | 已通过 |
| 纯逻辑与离线 SQL | `make test_unit`：159+ cases；无凭据 Debug SQLLogicTest 在线文件按门控跳过 | 已通过 |
| 读侧真实 API | 应用已开通 Bitable scope（F-2 消解）；`make test_sql_online_debug`：`integration_first_table` 6 assertions、`integration_pushdown` 21 assertions 全绿 | 已通过 |
| 写侧 E-2.6 curl 探测 | §1.5：create/update 上限、只读字段、只读 scope 负例（`99991672`）与非协作者写拒绝（`91403`）均已实测；G2：应用支路拒绝码为等级 A（`403 / 91403`，脚本可复现），正向对照为等级 B，机制解释有条件成立；用户身份支路未复测 | 已通过 |
| 写侧 `FEISHU_WRITE_TEST_*` | 专用 Base + 五项变量（应用身份：`APP_ID`/`APP_SECRET`/`APP_TOKEN`/`TABLE_ID`/`PREFIX`）已注入本地 `.env.local`（未入库） | E-5.2 本地就绪 |
| 写侧 `integration_copy_writeback` | `make test_sql_write_debug`：**15 assertions 全绿**（E-5.3/5.4/5.5）；marker 已清理 | **本地验收通过** |
| GitHub `write-tests` | 2026-09-30 已换名为 `..._APP_ID` / `..._APP_SECRET`（删除废弃的 `..._USER_ACCESS_TOKEN`），与 `WriteTests.yml` 凭据检查一致；仅 `dev`/`main` 可引用；仓库级 Secret 为空 | E-5.6 凭据面就绪；**触发面不通**：`actions/workflows` 的 `total_count=0`（默认分支 `main` 无 `.github/`），真实 `gh workflow run` 须等 D-1/D-3；Required reviewers 延后到 D-0a |

## 7. 自建飞书测试应用的方法（原理，2026-09-30 实测可复现）

E-2.6 负例 (3)(4) 需要额外的飞书应用身份。手工在开放平台后台建应用会让 agent 无法自主推进；本项目验证了**由 agent 发起建应用、由人类扫码授权**的路径，全程无需人工登录开放平台。

**原理**
- 飞书提供 OAuth 2.0 设备授权（Device Authorization Grant），SDK 封装为 `lark.registerApp()`（`@larksuiteoapi/node-sdk`）。
- 调用方只发起并轮询，授权动作由用户在飞书客户端扫码完成；`client_id` / `client_secret` 由平台在授权成功后回传。**调用方全程不接触用户凭证，也不经过应用后台**。
- 两种模式：**新建**（`createOnly: true` + `appPreset.name/desc`）；**给既有应用增量开通权限**（传 `appId: 'cli_xxx'`，会重新下发 `client_secret`）。
- 权限范围在发起时声明：`addons: { preset: false, scopes: { tenant: [...] } }`（测试应用用 `bitable:app` + `drive:drive`）。
- 失败码：`access_denied`（用户拒绝）、`expired_token`（扫码超时，链接默认 3600s，需重新发起）、`abort`、`ECONNRESET`。

**落地约定**
- 凭据只落 0600 文件，不回显；再由 shell 直接写入 `.env.local`，值不进入对话。用完即删临时文件，避免凭据副本扩散。
- 一个应用只承担一个身份角色：读侧 / 写侧主身份 / 只读 scope 负例 / 非协作者负例各自独立，**绝不复用**——否则负例会被协作者关系污染。
- 负例应用「有写 scope 但不是协作者」这一形态，必须先**对自建 Base 成功写入一次**作正向对照，再打目标 Base；否则 `91403` 无法与"scope 未生效"区分。

## 8. I-1：`records/search` `sort` 与 DuckDB 本地 `ORDER BY`（2026-10-03，真实环境）

比较的是两条路径的**行序**（用合成标签 `r00`–`r15` 或夹具行号，不记录 `record_id` / token）：

1. `POST .../records/search`，body 带 `sort: [{field_name, desc}]`。
2. 同一批值按扩展的 Typed 投影放进 DuckDB 后执行 `ORDER BY`（默认空值语义，以及显式 `NULLS FIRST` / `NULLS LAST`）。受保护夹具另外用已编译扩展的 `read_bitable` 再对过一次。

DuckDB v1.5.5 本机实测：`ORDER BY x ASC` 与 `ORDER BY x DESC` 的默认都是 **NULLS LAST**（`[1, 2, NULL]` 与 `[2, 1, NULL]`）。下文“等价”指与这个默认值的行序一致，含同值的相对次序。

### 8.1 数据

- **受保护读夹具**（`FEISHU_TEST_*`，未改数据）：`pagination_table` 500 行，`文本列` / `序号列` 各 500 个互异非空值；`all_types_table` 3 行，含一个全空行。公式列 property 只有 `formatter: ""`，三行公式都是 SQL NULL。单选选项顺序是 High / Medium / Low，多选是 TagA / TagB / TagC，和字母序重合，**不能**用来区分“选项顺序”和“名字码点序”。
- **临时表**（读凭据在读 Base 上创建 `i1_sort_probe`，测完 `DELETE` 表；表名顺序恢复为 `pagination_table` / `empty_table` / `all_types_table`）：16 行，覆盖空值、大小写、中英混排、换行、emoji、负数、同值、以及**故意与字母序不同**的选项顺序（单选 Low / High / Medium，多选 TagC / TagA / TagB）。公式表达式为数字列的引用，读回形状是 `{"type":2,"value":[<number>]}`。`batch_create` 写入的空字符串和空数组读回时字段缺失，按 NULL 计，因此“空串 vs NULL”“空数组 vs NULL”这一对本身**不可比**。

未排序的 `records/search` 与 `GET /records` 都按创建顺序返回 `r00`…`r15`。同一次 ASC 连发两次，标签序相同。

### 8.2 逐类型结论

| 维度 | 结论 | 依据（实测，不是推断） |
| --- | --- | --- |
| 文本 `TEXT(1)`，含中英混排、大小写、换行、emoji、空值、ASC/DESC | **等价** | 临时表 16 行 ASC/DESC 与 DuckDB `ORDER BY` 标签序逐项相同；空值在两端都在最后。`pagination_table` 500 行 ASC/DESC 与 Unicode 码点稳定排序相同，例如 ASC 前缀为 `Row 1`、`Row 10`、`Row 100`，不是按数字大小把 `Row 2` 放在 `Row 10` 前。`all_types_table` 经扩展扫描后 ASC/DESC 也一致（`Normal text` < 含中文与 emoji 的文本 < NULL）。 |
| 数值 `NUMBER(2)`，含负数、0、小数、空值、ASC/DESC | **等价** | 临时表与 500 行 `序号列` 的 ASC/DESC 都与 DuckDB `DOUBLE` 序相同，空值在最后。 |
| 日期 `DATE_TIME(5)`，含同值与空值、ASC/DESC | **等价** | 临时表毫秒时间戳序与 DuckDB `epoch_ms(...)` 序相同；`all_types_table` 两枚时间戳加一个 NULL，扩展 `ORDER BY` 与 search 一致。 |
| 单选 `SINGLE_SELECT(3)` | **不等价** | 服务端按**选项定义顺序**（Low，High，Medium）排，空值在最后。DuckDB 按选项名的码点序（High，Low，Medium）。受保护夹具上两者碰巧一致，不能当成等价。 |
| 多选 `MULTI_SELECT(4)` | **不等价** | 服务端只按**第一个选项**的定义顺序分组，组内保持创建顺序，后续选项不参与比较。DuckDB 对整个 `VARCHAR[]` 做码点字典序。 |
| 公式 `FORMULA(20)` | **不等价** | 服务端按计算后的数值排，ASC/DESC 与数字列完全相同。扩展把公式存成 JSON，DuckDB `ORDER BY` 该 JSON 列的顺序与数值序不同（例如 `-100` 不再排在 `-1.5` 之前的同一位置）。夹具上三行都是 NULL，只能说明全空时次序不变，**不能**单独支撑等价。 |
| 同值次序 | 文本 / 数值 / 日期：**等价**；单选 / 多选 / 公式：键序已不等价 | 临时表里相等键在 ASC 与 DESC 中相对次序都不变，且等于创建顺序。DuckDB 对同一输入也保持输入中的相对次序（打乱插入顺序后，同值次序跟着变）。本表未排序扫描序就是创建序，所以两端一致。这是这次数据上的实测，不是“任意扫描序都稳定”的推断。 |
| `VIEW_ID` | **不等价**（不能和下推后的排序同时成立） | 无效 `view_id`（`vewNotAReal1`）无论是否带 `sort`，都是 HTTP 200、`code=1254005`、`msg=WrongViewId`。给临时表建了 `数字 > 0` 的视图后：只带 `view_id` 返回 9 行；同一 `view_id` 再带 `sort` 返回全部 16 行，且与不带 `view_id` 的 `sort` 次序相同。筛选被丢掉。`pagination_table` 的默认 Grid 无筛选，view 与全表都是 500 行，该夹具看不出这条差异。 |

`pagination_table` 上 `文本列` ASC/DESC、`序号列` ASC/DESC 与本地序 `strict=true`（各 500 行）。扩展对 `all_types_table` 的文本、数字、日期 ASC/DESC 也是 `strict=true`。

### 8.3 整体结论：**可做**

可下推的字段类型只有 **`TEXT(1)`、`NUMBER(2)`、`DATE_TIME(5)`**。条件：Typed 模式、排序表达式是这些列的直接引用、空值语义为 DuckDB 默认或 `NULLS LAST`、没有非默认 collation、没有 `VIEW_ID`，并且若存在 `WHERE` 则该谓词必须已经完整下推。`NULLS FIRST`、单选、多选、公式、复选框、电话以及其他未在上表标成等价的类型，全部回退本地排序。

电话、复选框、货币、进度、评分、创建时间没有单独做双路径，**不**纳入可下推集合。多键方面，读夹具上 `ORDER BY 文本列 ASC, 序号列 DESC` 与本地序一致；实现要求每一个键都属于上面三个类型，否则整条 `ORDER BY` 回退。

## 9. I-3：LINK / DUPLEX_LINK / USER 的 `batch_create` 请求体（2026-10-03，真实环境）

写入凭据打在专用写入 Base 上。每次都新建两张临时表（目标表 + 探针表），测完 `DELETE` 两张表。三次运行结束后，Base 表名都恢复为原来的一张 `write_test_table`，该表记录总数保持 900。下面不记录任何 id。

人员字段的合法值用的是**这个写入应用自己的机器人** `open_id`（`bot/v3/info` 返回，前缀 `ou_`，长度 35）。通讯录列用户接口返回 HTTP 400、`code=99991672`（缺 contact scope），所以没有雇员 `user_id`，也没有 `union_id`。

字段都是 `multiple=true`。`multiple=false` 未测。

### 9.1 实测

| 用例 | HTTP | code | msg | 记录是否创建 | 读回 |
| --- | --- | --- | --- | --- | --- |
| 单向关联，合法 `record_id` 的字符串数组 | 200 | 0 | success | 是 | id 与提交相同；展示文本等于目标行的文本 |
| 单向关联，不存在的 `rec000000000000` | 200 | 0 | success | 是 | id 原样在；展示文本为空 |
| 单向关联，探针表自己的 `record_id`（不是目标表） | 200 | 0 | success | 是 | id 原样在；展示文本为空 |
| 双向关联，合法 / 不存在 / 跨表，同样是字符串数组 | 200 | 0 | success | 是 | 合法值的 id 与展示文本一致，反向字段指向探针行；不存在与跨表的 id 也原样在 |
| 单向关联写成 `{"link_record_ids":[...]}` | 200 | 1254067 | LinkFieldConvFail | 否 | — |
| 单向关联数组里放 JSON null | 200 | 1254067 | LinkFieldConvFail | 否 | — |
| 单向关联空数组 `[]` | 200 | 0 | success | 是 | 空关联对象，没有 record id |
| 人员，`[{"id":"<open_id>"}]`，查询参数 `user_id_type=open_id` | 200 | 0 | success | 是 | id 与提交相同，展示名非空 |
| 人员，同上对象，**不带** `user_id_type` | 200 | 0 | success | 是 | id 与提交相同 |
| 人员，同一个 `open_id`，`user_id_type=union_id` | 200 | 1254066 | UserFieldConvFail | 否 | — |
| 人员，同一个 `open_id`，`user_id_type=user_id` | 200 | 1254066 | UserFieldConvFail | 否 | — |
| 人员，字符串数组 `[open_id]`（带或不带 `user_id_type=open_id`） | 200 | 1254066 | UserFieldConvFail | 否 | — |
| 人员，不存在的 `ou_`（对象或字符串） | 200 | 1254066 | UserFieldConvFail | 否 | — |
| 人员，JSON null 或 `{"id": null}` | 200 | 1254066 | UserFieldConvFail | 否 | — |
| 人员，空数组 `[]` | 200 | 0 | success | 是 | 该字段读回缺失 |

创建响应里的关联字段会先回显字符串数组；随后 `GET /records` 变成对象（含 `link_record_ids`、`table_id`、`text`）。一致标准是 id，不是两段 JSON 同构。

双向关联的空数组没有单独打。这是推断：写请求形状与单向相同，契约按单向处理。

### 9.2 整体结论：**可做**

- 关联：源类型 `LIST(VARCHAR)`，请求体是 `record_id` 字符串数组。不存在的 id 和跨表 id 服务端照单全收，扩展不另做存在性校验。
- 人员：源类型也是 `LIST(VARCHAR)`，请求体必须是 `[{"id":"<id>"}]`，不能是字符串数组。I-3 只实测了 `user_id_type=open_id`（以及不传，默认相同）配上真实 `open_id` 会成功；把这个 `open_id` 标成另外两种会 `1254066`。官方「新增多条记录」把可选值写成 `open_id`、`union_id`、`user_id`，默认 `open_id`，并要求 id 种类与参数一致。2026-10-03 契约改为跟着这三种取值走，不把未实测的两种排除在外。成功样本仍然只有 `open_id`。
- I-5 已实现这三列的 `COPY` 编码与 `user_id_type` 选项；契约见 `.specs/03_API_CONTRACT.md` §8.3 / §8.4。
