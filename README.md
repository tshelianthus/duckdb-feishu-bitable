# duckdb-feishu-bitable

DuckDB 飞书多维表格读写扩展。可直接使用 DuckDB SQL 查询飞书多维表格（Bitable），并通过显式
`COPY ... (FORMAT feishu_bitable)` 向指定数据表批量追加记录，无需中间库转存与离线文件同步。

> **当前状态**：开发分支已实现范围 B 的显式、append-only `COPY` 写回代码路径，但真实写入 Base 的
> 在线验收尚未完成。本项目尚未进入 DuckDB Community Extensions；本次不执行 merge、tag、GitHub Release
> 或社区收录 PR。写回不会由查询、Replacement Scan 或未指定 `FORMAT` 的 `COPY` 隐式触发。

本仓库基于 [DuckDB Extension Template](https://github.com/duckdb/extension-template)，遵循 Community Extensions 构建与发布规范。

---

## 核心特性

* **原生 SQL 查询**：将飞书多维表格映射为 DuckDB 关系模型，支持过滤、聚合与本地数据关联。
* **安全凭据隔离**：基于 DuckDB Secret 管理应用凭据或委托用户令牌；应用 token 自动刷新且不落盘，敏感值统一脱敏。
* **双模式解析**：
  * **Typed Mode（默认）**：基于飞书字段元数据映射为 DuckDB 原生类型。
  * **JSON Mode**：保留原始字段结构，适配动态 Schema 与复杂嵌套字段。
* **零摩擦查询（对标 gsheets）**：直接粘贴浏览器 URL 查询，支持 `read_feishu_bitable` / `read_bitable` 现代命名、`FROM 'https://...'` 替换扫描与未指定数据表时的首表自动回退。
* **流式低内存开销**：分页拉取与 DataChunk 向量化流式对接，支持投影下推；每次调用只拉取一页，`LIMIT` 满足后不再发起后续分页请求。
* **企业级弹性**：读侧内置 HTTP 429 退避重试与网络超时恢复；写侧只重试确定未提交或明确限流的请求。
* **受控批量追加**：显式 `COPY` 使用 Feishu `records/batch_create`，按 1–500 行串行分批；只在收到确定成功响应后推进已确认进度，结果未知时停止并要求人工核对。

---

## 快速上手

### 1. 安装与加载

```sql
-- 当前尚未发布到 community；先按“本地构建”章节构建，再加载本地产物
LOAD './build/release/extension/feishu_bitable/feishu_bitable.duckdb_extension';
```

### 2. 配置飞书凭据（应用凭据或用户令牌二选一）

模式 A 使用飞书自建应用的 `APP_ID` + `APP_SECRET`，通过
`tenant_access_token/internal` 换取 `tenant_access_token`：

```sql
CREATE SECRET company_feishu (
    TYPE feishu,
    APP_ID '<your_app_id>',
    APP_SECRET '<your_app_secret>'
);
```

模式 B 直接使用委托用户令牌 `USER_ACCESS_TOKEN`，跳过 tenant token 换取，适合自建应用尚未开通
`bitable:app` 等应用身份权限、但已有用户令牌（例如 lark-cli 内置应用签发令牌）的环境：

```sql
CREATE SECRET user_feishu (
    TYPE feishu,
    USER_ACCESS_TOKEN '<your_user_access_token>'
);
```

两种模式按用途互斥；若创建语句同时提供三项，`USER_ACCESS_TOKEN` 优先，`APP_ID` / `APP_SECRET` 不会保存或使用。
`APP_SECRET` 与 `USER_ACCESS_TOKEN` 均由 DuckDB Secret
Manager 脱敏，`duckdb_secrets()` 不会返回明文。
两种模式缺少所需的非空参数时均抛出 `Invalid Input Error`。

### 3. 元数据发现

```sql
-- 查看指定多维表格中的数据表
SELECT *
FROM feishu_bitable_tables('app_token', SECRET = 'company_feishu');

-- 查看指定数据表的字段定义
SELECT *
FROM feishu_bitable_fields('app_token', 'table_id', SECRET = 'company_feishu', VIEW_ID = 'vew345678');
```

* `feishu_bitable_tables` 返回：`table_id`、`name`、`revision`；命名参数：`SECRET`。
* `feishu_bitable_fields` 返回：`field_id`、`field_name`、`type_id`、`type_name`、`is_primary`、
  `description`、`property_json`；命名参数：`SECRET`、`VIEW_ID`。

### 4. 数据查询与导出

```sql
-- 基础查询
SELECT customer_name, amount, created_at
FROM feishu_bitable_scan('app_token', 'table_id', SECRET = 'company_feishu')
WHERE amount >= 1000
LIMIT 50;

-- 关联本地数据后直接导出为 Parquet
COPY (
    SELECT remote.customer_name, local.city, remote.amount
    FROM feishu_bitable_scan('app_token', 'table_id', SECRET = 'company_feishu') AS remote
    LEFT JOIN read_parquet('city_mapping.parquet') AS local
      ON remote.city_id = local.id
) TO 'export.parquet' (FORMAT PARQUET, COMPRESSION ZSTD);
```

### 5. 显式批量追加写回（范围 B）

目标表必须显式给出；写侧不会像读侧那样自动选择第一张表。源查询的列名必须与目标飞书字段名一致：

```sql
COPY (
    SELECT
        'example'::VARCHAR AS "名称",
        42.5::DOUBLE AS "金额",
        true::BOOLEAN AS "已确认"
) TO '<app_token>' (
    FORMAT feishu_bitable,
    table_id '<table_id>',
    secret 'company_feishu',
    batch_size 500
);
```

目标也可写成包含 `table` 参数的飞书/Lark Base URL；显式 `table_id` 始终优先。当前只做
「往已有表里追加行」，不做改行、删行、整表覆盖、自动建表或自动加字段。

**可以写入的列**：文本类、数字类、复选框、日期时间、单选、多选、人员、单向/双向关联。
人员列用 `LIST(VARCHAR)`，扩展会编成 `[{"id":"..."}]`；默认按 `open_id` 解释，也可在
`COPY` 选项里写 `user_id_type 'union_id'` 或 `'user_id'`。关联列同样用 `LIST(VARCHAR)`，
元素是目标行的 `record_id`。

**写不了、会直接报错的列**（不是 bug，是刻意不做）：公式、创建/修改时间、创建/修改人、
自动编号（飞书本身只读）；以及附件、地理位置、群组（本扩展永不支持写入）。

写回需要编辑权限：应用或授权用户必须持有「查看、评论、编辑和管理多维表格」（常见 scope 为 `bitable:app`），并且是该 Base 的协作者；**仅有读取权限不能写入**（依据 `.specs/06_WRITEBACK_RESEARCH.md` §1.1 与 `.specs/03_API_CONTRACT.md` §8）。E-2.6 只实测了应用身份；用户身份沿用官方权限表，本轮未复测。

单批失败按整批处理，但多个批次之间没有事务：前面已确认成功的批次不会因后续批次失败而回滚。
读超时、断连、5xx 或无法验证响应时，扩展不会自动重放该批次，以免重复创建记录；错误会给出已确认的批次/行数
和结果未知的行区间。重跑前先核对远端实际状态，例如：

```sql
SELECT count(*)
FROM read_feishu_bitable('<app_token>', table_id => '<table_id>');
```

---

## 表函数接口

### `read_feishu_bitable` / `read_bitable`

推荐入口，命名对齐 DuckDB 原生的 `read_csv` / `read_parquet`。`read_bitable` 是完全等价的别名。

```sql
-- 直接粘贴浏览器地址栏 URL：自动解析 app_token / table / view
SELECT * FROM read_feishu_bitable(
    'https://example.feishu.cn/base/bascn123456?table=tbl789012&view=vew345678'
);

-- 仅给出 Base URL 或 app_token：自动回退到该多维表格的第一张数据表
SELECT * FROM read_bitable('https://example.feishu.cn/base/bascn123456');
SELECT * FROM read_bitable('bascn123456');

-- 兼容双位置入参，并支持全部可选命名参数
SELECT * FROM read_feishu_bitable(
    'bascn123456',
    'tbl789012',
    SECRET = 'company_feishu',
    VIEW_ID = 'vew345678',
    SCHEMA_MODE = 'typed',
    PAGE_SIZE = 500,
    INCLUDE_SYSTEM_COLUMNS = true
);
```

显式命名参数优先级高于 URL 中的查询参数：

```sql
-- 实际扫描 tblNew，URL 中的 tblOld 被覆盖；view 仍沿用 URL 中的 vewOld
SELECT * FROM read_feishu_bitable(
    'https://example.feishu.cn/base/bascn123456?table=tblOld&view=vewOld',
    table_id := 'tblNew'
);
```

### 替换扫描：`FROM 'https://...'`

飞书 / Lark 多维表格 URL 可以像本地 CSV、Parquet 文件一样直接出现在 `FROM` 子句中，
扩展会在绑定阶段透明重写为 `read_feishu_bitable('<url>')`：

```sql
SELECT * FROM 'https://example.feishu.cn/base/bascn123456?table=tbl789012';
SELECT * FROM 'https://example.larksuite.com/base/bascn123456';  -- 回退首表
```

拦截范围限定为 `*.feishu.cn` 与 `*.larksuite.com` 域名下的 `/base/`、`/wiki/` 路径，
其余表名与文件格式完全交还 DuckDB 原生处理。Lark URL 使用 Lark API 区域；Wiki URL
会先解析 Wiki 节点的底层 Bitable `obj_token`，再读取对应 Base。

### `feishu_bitable_scan`（经典接口，100% 向后兼容）

```sql
SELECT * FROM feishu_bitable_scan(
    'app_token',
    'table_id',
    SECRET = 'company_feishu',
    VIEW_ID = 'vew345678',         -- 可选：限定视图
    SCHEMA_MODE = 'typed',         -- 可选：'typed' (默认) 或 'json'
    PAGE_SIZE = 500,               -- 可选：每页获取数量，取值 1–500
    INCLUDE_SYSTEM_COLUMNS = true  -- 可选：追加 record_id / created_time / updated_time 列
);
```

该接口要求显式传入 `app_token` 与 `table_id` 双位置入参，不做 URL 解析、不触发首表回退，
也不注册 `TABLE_ID` 命名参数（`table_id` 只作为第二个位置入参）。

### 命名参数一览

`read_feishu_bitable` / `read_bitable` / `feishu_bitable_scan` 共享以下命名参数：

| 参数 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `SECRET` | `VARCHAR` | 自动选择 DuckDB Secret Manager 匹配的 `TYPE feishu` Secret | 指定 Secret 名称 |
| `VIEW_ID` | `VARCHAR` | 无 | 限定视图 |
| `SCHEMA_MODE` | `VARCHAR` | `'typed'` | `'typed'` 或 `'json'` |
| `PAGE_SIZE` | `INTEGER` | `500` | 每页记录数，1–500 |
| `INCLUDE_SYSTEM_COLUMNS` | `BOOLEAN` | `false` | 追加 `record_id`、`created_time`、`updated_time` |

`read_feishu_bitable` / `read_bitable` 另有 `TABLE_ID`（`VARCHAR`）。
命名参数大小写不敏感，`NAME = 'value'` 与 `NAME := 'value'` 两种写法等价。
`table` 是 DuckDB 解析器保留字，因此参数名取 `table_id` 而非 `table`。

---

## 功能范围与边界

### 实现能力（Implementation）

飞书自建应用身份鉴权（`tenant_access_token`）或委托用户令牌鉴权；应用凭据交换使用 POST，数据与元数据读取保持只读 GET；另支持只读流式扫描、投影下推、惰性分页（`LIMIT` 满足后不再请求后续页）、429 重试，扁平标量谓词（含可编码的日期 `ExactDate` 比较）在无 `VIEW_ID` 时可走 `records/search`，以及显式 `COPY ... (FORMAT feishu_bitable)` 的 append-only 批量追加写回。

### 支持的类型（Supported Types）

| 飞书字段类型 | DuckDB 类型 (Typed Mode) |
| --- | --- |
| 文本、电话、超链接、条码、邮箱、自动编号、单选 | `VARCHAR` |
| 数字、货币、进度、评分 | `DOUBLE` |
| 复选框 | `BOOLEAN` |
| 日期时间、创建时间、最后更新时间 | `TIMESTAMP`（毫秒时间戳转为微秒） |
| 多选 | `VARCHAR[]` |
| 人员、附件、单向关联、双向关联、地理位置、群组、创建人、修改人、公式 | `JSON` |
| 未知 / 无法识别类型 | `JSON` |

`SCHEMA_MODE = 'json'` 时所有列统一返回 `JSON`，用于动态 Schema 与深层嵌套字段。
记录中缺失的字段一律以 SQL `NULL` 输出。

### 限制（Limitations）

注意：DuckDB v1.5.5 的 TableFunction 没有 LIMIT 下推接口，本扩展**不做** LIMIT 下推；
提前终止是"每次扫描调用只拉取一页、DuckDB 拿够行数后不再回调"的自然结果。

谓词下推仅覆盖已验证的扁平子集。SQL `ORDER BY` 只对 Typed 模式的文本、数字、日期列下推（默认空值在最后，且不能带 `VIEW_ID`）；其他类型、`NULLS FIRST` 和带 `VIEW_ID` 的排序仍在 DuckDB 本地执行。日期字段只下推可编码为官方 `ExactDate` 的
`=` / `>` / `<`，相对日期与日期 `!=` / `>=` / `<=`、空值、`IN`、`LIKE`、嵌套 AND/OR、
复合字段及带 `VIEW_ID` 的查询仍走 GET list + 本地过滤。

**永远不做**：跨批次事务、整表覆盖写、自动建表、`INSERT INTO` 语法、写侧猜第一张表、
附件上传/下载、地理位置与群组写入、浏览器 OAuth 弹窗、电子表格/云文档/知识库/消息、增量 CDC。

**暂时不做**（有真实需求再评估）：改已有记录、删记录。

人员与关联已经可以写；我们只在真实环境里验过匹配的 `open_id` 和合法关联 `record_id`，
`union_id` / 雇员 `user_id` 还没有成功样本。读附件时 JSON 里已有 `file_token` 等元数据，
但扩展不代下文件；需要文件本体时请自行拿 `file_token` 调飞书下载接口。

---

## 本地构建

需要先配置 [vcpkg](https://vcpkg.io/)（本扩展依赖 OpenSSL）：

```shell
git clone https://github.com/Microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
export VCPKG_TOOLCHAIN_PATH=`pwd`/vcpkg/scripts/buildsystems/vcpkg.cmake
```

然后：

```sh
git submodule update --init --recursive
GEN=ninja make          # 构建
make test               # 运行 SQL 测试（SQLLogicTest）
make test_unit          # 运行单元测试（Catch2，独立构建，无需完整 DuckDB）
make test_all           # 两者一并运行
```

`test/sql/integration_*.test` 为对接真实飞书应用的端到端测试，默认跳过；本地导出
`FEISHU_TEST_APP_ID`、`FEISHU_TEST_APP_SECRET`、`FEISHU_TEST_APP_TOKEN`、`FEISHU_TEST_URL`
后自动纳入执行，也可在受保护环境运行 `make test_sql_online_debug`。集成测试使用模式 A，
自建应用需开通 `bitable:app:readonly` / `bitable:app` / `base:table:read` 任一权限；模式 B
的短时效 `USER_ACCESS_TOKEN` 仅用于人工探查。变量命名与
`docs/FEISHU_TEST_AGENT_PROMPT.md` 的联调任务书保持一致。

产物：

* `./build/release/duckdb` — 已预加载本扩展的 DuckDB shell
* `./build/release/extension/feishu_bitable/feishu_bitable.duckdb_extension`

---

## 开发状态

已落地：`TYPE feishu` Secret 与 Token 缓存、`feishu_bitable_tables` / `feishu_bitable_fields` 元数据函数、
`feishu_bitable_scan` 向量化流式扫描、投影下推，以及对标 gsheets 的零摩擦交互
（URL 解析、`read_feishu_bitable` / `read_bitable`、替换扫描、首表回退）；范围 B 的显式 COPY 写回代码路径
已接通目标/字段 Bind、值编码、批次累积、`batch_create` 发送、成功响应条数核对与写侧重试分类。

仍待验证：专用写入 Base 的写后读回、批次边界和整批失败/跨批次部分完成在线用例，以及结果未知后不重放的
独立状态机测试。读侧在线回归仍受测试应用 Bitable 权限配置影响。

---

## 社区发布（本次不执行）

当前仅保留发布材料作为后续准备；尚未 merge 到发布分支、打 tag、创建 GitHub Release 或向
`duckdb/community-extensions` 提交收录 PR，也不要把下面的占位描述符当成已发布状态。

* 社区描述符：[`docs/community/description.yml`](docs/community/description.yml)
  —— 提交社区 PR 时复制到 `duckdb/community-extensions` 的 `extensions/feishu_bitable/description.yml`。
  其中 `repo.ref` 目前是占位符，需在打出 tag `v0.1.0` 后回填完整 commit SHA。
* 发布门禁清单：[`docs/RELEASE_CHECKLIST.md`](docs/RELEASE_CHECKLIST.md)。
* 平台排除：`wasm_mvp;wasm_eh;wasm_threads`（WASM 沙箱无原生 TLS 套接字，本扩展依赖 OpenSSL 直连
  `https://open.feishu.cn`）。

---

## License

[MIT License](LICENSE)
