# API Contract Specification: duckdb-feishu-bitable (v0.2 development contract)

所有函数在执行 `LOAD feishu_bitable;` 后注册到 DuckDB 的默认目录中。

---

## 1. Secret 类型与参数

支持两种互斥的凭据模式：

### 模式 A：应用凭据（APP_ID + APP_SECRET）

```sql
CREATE SECRET [secret_name] (
    TYPE feishu,
    APP_ID '<cli_xxxxxxxxxxxx>',
    APP_SECRET '<xxxxxxxxxxxxxxxxxxxxxxxxxxxx>'
);
```

- **参数规约**：
  - `APP_ID` (VARCHAR, 必填)：飞书自建应用的 App ID。
  - `APP_SECRET` (VARCHAR, 必填)：飞书自建应用的 App Secret。
- **鉴权方式**：通过 `tenant_access_token/internal` 接口换取 `tenant_access_token`。

### 模式 B：用户凭据（USER_ACCESS_TOKEN）

```sql
CREATE SECRET [secret_name] (
    TYPE feishu,
    USER_ACCESS_TOKEN '<u-xxxxxxxxxxxxxxxxxxxx>'
);
```

- **参数规约**：
  - `USER_ACCESS_TOKEN` (VARCHAR, 必填)：飞书委托用户访问令牌（如 lark-cli 内置应用签发的 user_access_token）。
- **鉴权方式**：直接作为 `Authorization: Bearer` 头使用，跳过 `tenant_access_token` 换取流程。
- **适用场景**：仅有用户委托令牌、自建应用未开通 `bitable:app` 等应用身份权限的环境。

- **互斥规则**：两种模式按用途二选一；若创建语句同时提供三项，`USER_ACCESS_TOKEN` 优先，`APP_ID`/`APP_SECRET` 不保存且不使用。
- **错误处理**：缺少任一模式的必填参数抛出 `Invalid Input Error`。

---

## 2. 元数据函数：`feishu_bitable_tables`

列出指定多维表格（App Token）下的所有数据表信息。

### 签名
```sql
feishu_bitable_tables(app_token VARCHAR, [, SECRET VARCHAR])
```

### 返回 Schema
| 列名 | 类型 | 说明 |
| :--- | :--- | :--- |
| `table_id` | `VARCHAR` | 数据表唯一标识符（以 `tbl` 开头） |
| `name` | `VARCHAR` | 数据表名称 |
| `revision` | `BIGINT` | 数据表版本修订号 |

---

## 3. 元数据函数：`feishu_bitable_fields`

列出指定多维表格中某张表的字段定义及元数据。

### 签名
```sql
feishu_bitable_fields(app_token VARCHAR, table_id VARCHAR [, SECRET VARCHAR, VIEW_ID VARCHAR])
```

### 返回 Schema
| 列名 | 类型 | 说明 |
| :--- | :--- | :--- |
| `field_id` | `VARCHAR` | 字段唯一标识符（以 `fld` 开头） |
| `field_name` | `VARCHAR` | 字段名称 |
| `type_id` | `INTEGER` | 飞书官方字段类型枚举 ID |
| `type_name` | `VARCHAR` | 字段类型可读名称 |
| `is_primary` | `BOOLEAN` | 是否为第一列索引主字段 |
| `description` | `VARCHAR` | 字段描述文本 |
| `property_json`| `JSON` | 字段详细配置属性（如选项列表、格式配置） |

---

## 4. 现代数据查询表函数：`read_feishu_bitable` 与 `read_bitable`

遵循 DuckDB 原生命名规范（如 `read_csv`、`read_parquet`）及 `gsheets` 扩展体验构建的统一读取表函数。`read_bitable` 为其完全等价的精简别名。

### 4.1 函数签名与多态重载 (Polymorphic Signatures)

#### 重载 1：单入参模式（浏览器 URL 或纯 `app_token`）
```sql
read_feishu_bitable(
    url_or_app_token VARCHAR
    [, table_id VARCHAR]
    [, secret VARCHAR]
    [, view_id VARCHAR]
    [, schema_mode VARCHAR]            -- 'typed' (默认) 或 'json'
    [, page_size INTEGER]              -- 默认 500
    [, include_system_columns BOOLEAN] -- 默认 false
)
```

#### 重载 2：双位置入参兼容模式
```sql
read_feishu_bitable(
    app_token VARCHAR,
    table_id VARCHAR
    [, secret VARCHAR]
    [, view_id VARCHAR]
    [, schema_mode VARCHAR]            -- 'typed' (默认) 或 'json'
    [, page_size INTEGER]              -- 默认 500
    [, include_system_columns BOOLEAN] -- 默认 false
)
```

#### 别名
```sql
read_bitable(...)
```
`read_bitable` 与 `read_feishu_bitable` 参数签名、多态逻辑与执行行为 100% 一致。

### 4.1a 单记录等值查询优化（v0.3+，不改变 SQL 契约）

当 `include_system_columns = true` 且查询谓词**仅为** `record_id = '<id>'`（无其他谓词、非等值比较）时，
`read_feishu_bitable` / `read_bitable` / `feishu_bitable_scan` 走飞书「获取单条记录」接口
（`GET .../records/{record_id}`，单次请求），而非分页 List。公开函数名与参数不变；
不满足该形态的查询保持原有分页 List + 本地过滤行为。记录不存在时返回空结果集（与 SQL 过滤语义一致）。

### 4.1b 谓词 / 排序下推优化（v0.3+，不改变 SQL 契约）

当查询谓词属于 H-1 已验证的扁平子集（同层全 AND 或全 OR、≤50 个原子条件；字段限于
Text/Phone/Email/Barcode/SingleSelect/AutoNumber 的等值/不等值，Number/Currency/Progress/Rating
的等值与比较，Checkbox 等值，以及 DateTime/CreatedTime/ModifiedTime 的 `=` / `>` / `<`
（编码为官方 `ExactDate` + UTC 毫秒时间戳，运算符仅 `is` / `isGreater` / `isLess`）；
**不含**日期相对条件（Today/CurrentWeek 等）、日期 `!=` / `>=` / `<=`、空值、`IN`、`LIKE`、
嵌套 AND/OR、复合/公式字段），且**未**设置 `VIEW_ID` 时，扫描可改走飞书
`POST .../records/search`，在请求体中携带 `filter`（及可映射时的 `sort`，至多 100 键）。
公开函数名与参数不变。

优先级：sole `record_id =`（§4.1a）始终优先于 search；存在 `VIEW_ID`、或谓词不可完整下推时，
保持既有 `GET .../records` + DuckDB 本地过滤/排序。投影下推（`field_names`）在 search 路径上继续生效；
零列扫描仍请求一个最小字段。

SQL `ORDER BY` 由优化器改写接入 `sort`（至多 100 键），**仅当**以下条件全部成立：
每个排序键都是 Typed 模式的 `TEXT(1)`、`NUMBER(2)` 或 `DATE_TIME(5)` 列引用（不是表达式、
不是别的字段类型、也不是系统列）；方向为默认 / `ASC` / `DESC`，空值语义为 DuckDB 默认或显式
`NULLS LAST`（本构建实测默认即两端都是 `NULLS LAST`）；未指定非默认 collation；未设置 `VIEW_ID`；
`SCHEMA_MODE` 为 `typed`。若同时有 `WHERE`，该谓词必须已经完整走入 search，否则排序也不下推。
条件不满足时（含 `NULLS FIRST`、单选 / 多选 / 公式 / 复选框 / 电话等、JSON 模式、`VIEW_ID`）
保留 DuckDB 本地 `ORDER BY`。下推成功时本地排序算子被去掉，行序以服务端 `sort` 为准。
日期下推仅在常量可无歧义编码为 `["ExactDate","<epoch_ms>"]` 时启用；相对日期词与官方不支持的
日期运算符一律拒绝并回退 GET list。

### 4.2 多态分流与参数解析规则 (Polymorphic Resolution)
- **URL 分支判定**：
  - 若第一个入参包含协议头（以 `http://` 或 `https://` 开头），解析器将其作为飞书/Lark 多维表格 URL 处理。
  - 从 URL Path 中提取 `app_token`（如 `/base/<app_token>` 或 `/wiki/<wiki_token>`）。
  - 从 URL Query String 中提取 `table`（映射至 `table_id`）与 `view`（映射至 `view_id`）。
- **纯 Token 分支判定**：
  - 若第一个入参不含协议头，则判定为纯 `app_token`（例如 `bascnXXXXXXXX`）。
- **参数覆盖优先级**：
  - 显式命名参数（如 `table_id := 'tblXXX'`、`view_id := 'vewYYY'`）具有最高优先级，覆盖从 URL Query 中提取的值。
  - 双位置入参模式下，命名参数 `table_id` 优先于第二个位置入参。
- **命名参数取名 `table_id` 而非 `table`**：
  - `table` 是 DuckDB 解析器的保留关键字，`table := '...'` 直接触发 `Parser Error`，必须写成 `"table" := '...'`。
  - 对标 `gsheets`（其命名参数 `sheet`、`range`、`header` 均为非保留字，且不提供别名），本扩展同样只注册单一免引号参数名 `table_id`，与重载 2 的第二个位置入参同名。

### 4.3 未指定 table 时的默认首表回退语义 (Default First Table Fallback)
- **触发判定**：无论是单入参 URL 模式还是纯 `app_token` 模式，若调用未显式指定 `table_id` 且 URL 中未包含 `table` 查询参数，触发首表回退。
- **回退行为**：在 Bind 阶段自动向飞书元数据接口 `GET /apps/{app_token}/tables` 发起查询，获取该多维表格下的所有表，并默认绑定首个数据表（`tables[0].table_id`）。
- **空表异常**：若当前多维表格为空（即该 Base 不含任何数据表），抛出 `InvalidInputException("feishu_bitable: base contains no tables")`。

### 4.4 字段类型映射对照（Typed Mode）
| 飞书字段类型 | DuckDB 类型 | 行为与空值语义 |
| :--- | :--- | :--- |
| Text, Barcode, Phone, URL, Email | `VARCHAR` | 缺失或 null 返回 SQL NULL |
| AutoNumber | `VARCHAR` | 自动编号可携带格式化前缀（如 `AN-1`），转 DOUBLE 会丢失信息，故保持文本 |
| Number, Progress, Rating, Currency | `DOUBLE` | 缺失或 null 返回 SQL NULL |
| Checkbox | `BOOLEAN` | true/false，缺失为 false 或 NULL |
| DateTime, CreatedTime, ModifiedTime | `TIMESTAMP` | 毫秒时间戳转换为微秒 TIMESTAMP |
| SingleSelect | `VARCHAR` | 单选标签名 |
| MultiSelect | `LIST(VARCHAR)` | 标签数组列表 |
| User, Attachment, Link, DuplexLink, Location, GroupChat, Formula, CreatedUser, ModifiedUser | `JSON` | 保留完整结构化 JSON |
| 无法识别类型 / 未知字段 | `JSON` | 自动降级为 JSON，保证不丢失数据 |

---

## 5. 替换扫描机制：Replacement Scan (`FROM 'https://...'`)

提供对标 Google Sheets 与 DuckDB 本地文件零摩擦查询体验。

### 5.1 语法形式
用户无需显式调用 `read_feishu_bitable` 或 `read_bitable`，直接在 `FROM` 子句中使用飞书/Lark 多维表格 URL 字符串：
```sql
-- 指定 table 与 view
SELECT * FROM 'https://example.feishu.cn/base/bascn123456?table=tbl789012&view=vew345678';

-- 仅指定 base URL（自动回退至首表）
SELECT * FROM 'https://example.feishu.cn/base/bascn123456';

-- Lark 国际版域名
SELECT * FROM 'https://example.larksuite.com/base/bascn123456?table=tbl789012';
```

### 5.2 拦截与等价映射规则
- **拦截条件**：`FROM` 后的表标识符为常量字符串，以 `http://` 或 `https://` 开头，且匹配飞书/Lark 多维表格 URL 特征（包含 `.feishu.cn/base/`、`.larksuite.com/base/` 或 Wiki 关联 Base 路径）。
- **等价重写**：
  ```sql
  SELECT * FROM 'https://example.feishu.cn/base/bascnXXX?table=tblYYY'
  ```
  在 DuckDB AST 绑定阶段等价重写为：
  ```sql
  SELECT * FROM read_feishu_bitable('https://example.feishu.cn/base/bascnXXX?table=tblYYY')
  ```
- **首表回退继承**：若 Replacement Scan 传入的 URL 未包含 `table` 查询参数，无缝继承 4.3 节的默认首表回退逻辑。

---

## 6. 经典数据查询表函数：`feishu_bitable_scan`（完全向后兼容）

为保障既有脚本与生产管道的稳定运行，保留原始底层的 `feishu_bitable_scan` 契约，100% 向后兼容。

### 签名
```sql
feishu_bitable_scan(
    app_token VARCHAR,
    table_id VARCHAR
    [, SECRET VARCHAR]
    [, VIEW_ID VARCHAR]
    [, SCHEMA_MODE VARCHAR]          -- 'typed' (默认) 或 'json'
    [, PAGE_SIZE INTEGER]            -- 默认 500
    [, INCLUDE_SYSTEM_COLUMNS BOOLEAN] -- 默认 false
)
```

### 兼容性承诺
- 要求显式传入 `app_token` 和 `table_id` 双位置入参。
- 底层复用相同的向量化扫描引擎与凭证流转机制。
- 继承 §4.1a 的单记录等值查询优化（需 `INCLUDE_SYSTEM_COLUMNS = true`）。
- 继承 §4.1b 的谓词 / 排序下推优化（扁平子集 + 无 `VIEW_ID`）。

---

## 7. 通用语义与错误规范
- **NULL 传播**：记录中缺失的字段严格打上 `ValidityMask` 标记为 SQL NULL。
- **Zero Panic**：所有网络失败（401、403、404、5xx）及解析错误均抛出 `InvalidInputException` 或 `IOException`，严禁进程崩溃。
- **脱敏安全**：任何抛出的异常消息中严禁出现完整的 App Secret 或 Token。
- **Catalog 元数据**：公开入口通过 `CreateTableFunctionInfo` / `CreateScalarFunctionInfo` 写入
  每个 overload 的 `description`、`examples` 与可选 `categories`；社区扩展页面与 SQL 侧
  agent 发现面均来自该注册元数据，而非 `description.yml` 模板。
  - **标量**：必须填写真实 `parameter_names`（禁止 `col0`）。本扩展唯一标量为 §7.1。
  - **带命名参数的表函数**：`parameter_names` **保持为空**。DuckDB 1.5.x 的
    `GetParameterNames` 在 `parameter_names` 非空时用扁平行列对齐 `GetParameterTypes`
    （含 `unordered_map` 的 named_parameters）；注册时快照的顺序在 `TableFunction` 拷贝后
    不稳定，硬填要么名/类型错位，要么把命名参数也变成 `colN`。留空则走 fallback：
    位置参数显示为 `col0`/`col1`，命名参数保留真实名（`secret` / `view_id` / …）。
    这是上游限制（见 duckdb#15876），不是本扩展疏漏；`description` / `examples` 仍必须填。
  - `parameter_types`（FunctionDescription 内）只列位置参数类型，供 overload 匹配。

### 7.1 内部 load-smoke 标量（非公开 API）

```sql
feishu_bitable(name VARCHAR) -> VARCHAR
```

- **用途**：站立门禁 / 载入冒烟；返回 `'feishu_bitable ready: ' || name`。
- **元数据**：`description` 标明 “Internal load-smoke helper...; not part of the public API.”；
  `parameter_names = {"name"}`（DuckDB v1.5.5 的 `FunctionEntry` 不持久化 `CreateInfo.comment`，
  故以 description 为准）；`categories = {"internal"}`。
- **契约边界**：不出现在文档的公开入口表中；不作为业务调用面。

---

## Base Snapshot (approved semantics, not implemented)

首期 Base 离线快照语义见 [`.specs/07_BASE_SNAPSHOT_CONTRACT.md`](07_BASE_SNAPSHOT_CONTRACT.md)。该契约批准的是行为语义，**不表示运行时已支持**：当前仍只可使用本文件已定义的 `feishu_bitable_tables`、`feishu_bitable_fields`、`read_feishu_bitable` / `read_bitable` / `feishu_bitable_scan` 与普通 DuckDB CTAS。尚未实现或注册 Base 快照编排函数，也没有 `ATTACH` / StorageExtension 快照支持。

---

## 8. 回写契约：`COPY ... TO`（v0.2 正式契约）

> **状态**：本节自 v0.2 起为**已冻结的正式契约**，由批次 E-2 定稿（E-2.1 ~ E-2.5、E-2.7）；
> 当前开发分支已接通显式 COPY 的范围 B 代码路径，但专用写入 Base 的在线验收尚未完成，也不执行社区发布。
> 读侧（§1 ~ §7）不受本节影响：写入只能在显式 `COPY` 语句中触发，绝无隐式回写路径。

### 8.1 语法与目标解析

```sql
COPY <src> TO '<target>' (FORMAT feishu_bitable [, table_id '<tbl...>'] [, secret '<name>'] [, batch_size <n>] [, user_id_type 'open_id|union_id|user_id']);
```

- 格式名固定为 `feishu_bitable`，且**必须显式书写** `(FORMAT feishu_bitable)`；
  本扩展不做文件扩展名自动推断，也不注册任何别名。
- 两种上游都支持：`COPY <table> TO ...` 与 `COPY (SELECT ...) TO ...`。
- `<target>` 接受两种形态（与读侧 §4.1 / §4.2 完全对称，亦与社区先例 gsheets 的
  `COPY <t> TO '<URL>' (FORMAT gsheet)` 一致）：
  - (a) 飞书 / Lark 多维表格**浏览器 URL**：`https://<tenant>.feishu.cn/base/<app_token>?table=<table_id>`，
    host 规则、path 关键字（`base` / `wiki`）与解析失败语义复用 `src/bitable/url_parser.cpp`。
  - (b) **裸 `app_token`**（形如 `bascnXXXXXXXX`）。
- 解析优先级与读侧一致：显式 `table_id` 选项**始终覆盖** URL 的 `table` 查询参数。
- URL 中的 `view` 查询参数被**忽略**（视图对追加写入无意义），且本扩展**不提供 `view_id` 选项**。
- **写侧不做首表回退**（与读侧 §4.3 明确不对称）：URL 未携带 `table` 且未提供 `table_id` 选项时，
  Bind 阶段直接报错，绝不猜测目标表。
- **Replacement Scan 不参与写入**（§5）：URL 出现在 `FROM` 与出现在 `COPY ... TO` 是两条独立路径，
  前者由 §5 处理，后者由本节处理。
- 非本格式的 `COPY`（`FORMAT csv` / `parquet` / 缺省 `FORMAT`）不受影响，
  缺 `FORMAT` 时不得被误判为本扩展。

### 8.2 选项表

| 选项名 | LogicalType | CopyOptionMode | 默认值 | 缺省 / 非法时行为 |
| :--- | :--- | :--- | :--- | :--- |
| `table_id` | `VARCHAR` | `WRITE_ONLY` | 空 | URL 未携带 `table` 且本项为空 → Bind 报错 |
| `secret` | `VARCHAR` | `WRITE_ONLY` | 空串 | 空串 = 按 DuckDB Secret 解析规则取 `TYPE feishu` 的默认 secret（§1；命名不存在 → Bind 报错） |
| `batch_size` | `BIGINT` | `WRITE_ONLY` | `500` | 越界（下界 `1`，上界 `500`）→ Bind 报错 |
| `user_id_type` | `VARCHAR` | `WRITE_ONLY` | `open_id` | 只接受 `open_id`、`union_id`、`user_id` 这三个小写值；其他值（含空串、大小写变体）→ Bind 报错。整次 `batch_create` 都带上这个查询参数（含默认值） |

- 选项名**大小写不敏感**（DuckDB 使用 `case_insensitive_map_t`）。
- 未知选项由 DuckDB `copy_options` 机制自动产生
  `Unrecognized option "<x>" for feishu_bitable` 并附候选建议，本扩展不自行拦截。
- 选项类型错误（如 `batch_size 'abc'`）由 DuckDB 的选项转换层拒绝，扩展可假定进入 Bind 的值类型正确。
- **不提供 `view_id`**：视图对追加写入无意义。

### 8.3 写入类型映射表

| 飞书字段类型（type id） | 接受的 DuckDB 源类型 | 越界 / 不可转换时的行为 |
| :--- | :--- | :--- |
| TEXT(1)、BARCODE(99001)、PHONE(13)、URL(15)、EMAIL(99005) | `VARCHAR` | 非 `VARCHAR` 源列 → Bind 报错；URL 在 API 请求体中编码为同时包含 `link` / `text` 的对象，两者均取该 VARCHAR 值 |
| NUMBER(2)、PROGRESS(99002)、CURRENCY(99003)、RATING(99004) | `FLOAT` / `DOUBLE` 及可无损转换的整数类型（`TINYINT`~`BIGINT`、`UTINYINT`~`UBIGINT`、`HUGEINT`） | 越出 `DOUBLE` 有限范围、整数不能无损表示为 `DOUBLE`、或值为 NaN / ±Inf → 运行时报错（§8.4） |
| CHECKBOX(7) | `BOOLEAN` | 非 `BOOLEAN` 源列 → Bind 报错 |
| DATE_TIME(5) | `TIMESTAMP` / `TIMESTAMPTZ` | 毫秒时间戳越界 → 运行时报错（§8.4） |
| SINGLE_SELECT(3) | `VARCHAR` | 非 `VARCHAR` 源列 → Bind 报错 |
| MULTI_SELECT(4) | `LIST(VARCHAR)` | 含 NULL 元素的 LIST → 运行时报错（§8.4） |
| LINK(18)、DUPLEX_LINK(21) | `LIST(VARCHAR)` | 非 `LIST(VARCHAR)` → Bind 报错。每个元素是 `record_id` 字符串；请求体是 JSON 字符串数组。空列表、含 NULL、不存在或跨表 id 见 §8.4 |
| USER(11) | `LIST(VARCHAR)` | 非 `LIST(VARCHAR)` → Bind 报错。每个元素原样作为 `id`。请求体为 `[{"id":"<id>"}]`，`id` 的种类由 §8.2 的 `user_id_type` 决定，默认 `open_id`，也可以是 `union_id` 或 `user_id`。空列表与服务端拒绝见 §8.4 |

上表 LINK / DUPLEX_LINK / USER 三行是 2026-10-03 冻结的契约，I-5 已实现。关联的请求体来自 I-3 实测。人员字段的 `user_id_type` 跟着官方「新增多条记录」的三个可选值；在线成功样本仍只有匹配的 `open_id`（见 §8.4）。

**时区规则（唯一确定语句）**：`TIMESTAMP` 按 **UTC** 解释（其微秒值直接视为 UTC epoch 微秒），
`TIMESTAMPTZ` 先归一化到 UTC，二者最后都换算为**毫秒** epoch（`micros / 1000`）提交；
行为**不取决于**会话 `TimeZone` 设置。

### 8.4 特殊输入的唯一行为

以下输入各只有一种确定行为，实现不得"视情况而定"：

| 输入 | 唯一行为 |
| :--- | :--- |
| SQL NULL | **省略该字段**（请求体的 `fields` 中不出现该 key，远端保持该字段为空） |
| 空字符串 `''` | 文本类字段（TEXT/PHONE/URL/EMAIL/BARCODE/SINGLE_SELECT）写空串；其余类型运行时报错 |
| 空 `LIST`（多选） | 写**空数组** `[]`（等价于把多选字段清空） |
| 空 `LIST`（LINK / DUPLEX_LINK / USER） | 同样提交 `[]`。单向关联与人员已实测为 HTTP 200、`code=0`：关联读回为空关联对象，人员读回时该字段缺失。双向关联的空列表未单测，契约与单向相同 |
| 空字符串元素的 `LIST`（LINK / DUPLEX_LINK / USER） | **运行时报错**，不发请求。与多选（MULTI_SELECT）不同：多选仍允许空字符串元素 |
| 含 NULL 元素的 `LIST` | **运行时报错**，不静默丢弃元素，也不发请求。服务端若收到关联数组中的 JSON null：HTTP 200、`code=1254067`、`msg=LinkFieldConvFail`；人员字段的 JSON null 或 `{"id": null}`：HTTP 200、`code=1254066`、`msg=UserFieldConvFail`。记录都不创建 |
| 不存在或跨表的关联 `record_id` | **原样提交**。服务端 HTTP 200、`code=0` 并原样存下，读回展示文本为空。扩展不做存在性预校验 |
| 人员 `LIST` 的元素 | 每个非空字符串原样放进 `{"id":"..."}`，不按 `ou_` / `on_` 前缀改写，也不预校验是否存在。空字符串元素运行时报错，不发请求。种类与 `user_id_type` 不一致、跨应用 `open_id`、或不存在的 id，由服务端 HTTP 200、`code=1254066`、`msg=UserFieldConvFail` 拒绝，整条记录不创建。官方参数说明写明 `user_id` 需要 scope `contact:user.employee_id:readonly`。I-3 只实测了匹配的 `open_id` 能成功；`union_id` 与雇员 `user_id` 没有成功样本，契约仍按官方三种取值接受 |
| `NaN` / `±Infinity` | **运行时报错** |
| 超出飞书毫秒范围的 `TIMESTAMP` | **运行时报错**；可接受区间为 1970-01-01T00:00:00Z ~ 9999-12-31T23:59:59.999Z，即**早于 epoch 的微秒值（`micros < 0`）与超过 `FEISHU_MAX_TIMESTAMP_MILLIS`（= 253402300799999）的值都拒绝**；上界是防御性选择，见 `src/include/bitable/write_value_encoding.hpp` |
| 不在 §8.3 映射表中的 DuckDB 类型（`STRUCT` / `MAP` / `BLOB` / `INTERVAL` / 嵌套 LIST 等） | **Bind 阶段拒绝该列**（列类型不可写，不进入扫描） |

### 8.5 写不了的字段（两类原因，勿混谈）

措辞必须区分「飞书本身只读」与「本扩展刻意永不写」，不得笼统写成"不支持"：

**(a) 飞书本身只读**——永远不可作为 `COPY` 目标列：

| type id | 字段类型 |
| :--- | :--- |
| 20 | `FORMULA` |
| 1001 | `CREATED_TIME` |
| 1002 | `MODIFIED_TIME` |
| 1003 | `CREATED_USER` |
| 1004 | `MODIFIED_USER` |
| 1005 | `AUTO_NUMBER` |

**(b) 本扩展永不写入**（ADR-008；不是「以后再说」）——`COPY` 绑定时直接报错：

| type id | 字段类型 | 通俗名 |
| :--- | :--- | :--- |
| 17 | `ATTACHMENT` | 附件 |
| 22 | `LOCATION` | 地理位置 |
| 23 | `GROUP_CHAT` | 群组 |

人员（`USER`）与关联（`LINK` / `DUPLEX_LINK`）**不在此列**，可写规则见 §8.3 / §8.4。

**(c) 未知 type id 一律报错**——与读侧"未知类型降级为 JSON"（§4.4）**有意不对称**：
读侧降级不产生副作用，写侧降级会造成错误的远端写入。

三类各自的 Bind 期错误消息模板（模板中**不含任何凭据**；第二条文案沿用历史字符串，语义即「本扩展不写这类字段」）：

```
feishu_bitable: column "<col>" maps to read-only Feishu field "<field>" (type <id>) and cannot be a COPY target
feishu_bitable: column "<col>" maps to Feishu field "<field>" (type <id>) which v0.2 writeback does not support
feishu_bitable: column "<col>" maps to unknown Feishu field type <id>; refusing to write
```

`FeishuFieldTypeId` 的 25 个 type id 由 §8.3 可写行、本节 (a)(b) 与未知 type id 拒绝共同覆盖，无遗漏。

### 8.6 `COPY` 返回值语义

- `COPY` 返回**单列 `Count BIGINT`**。该列由 DuckDB 在 `PhysicalCopyToFile::Sink` / `Combine` 中
  自行累加（`l.total_rows_copied += chunk.size()`），**扩展无法改写、也无法新增任何额外统计列（例如失败计数列）**
  （DuckDB v1.5.5 的 `CopyFunctionReturnType` 只有 `CHANGED_ROWS` /
  `CHANGED_ROWS_AND_FILE_LIST` / `WRITTEN_FILE_STATISTICS` 三种固定形状）。
- 因此语义定为"**提交给写入算子的行数**"，且**只在整条 `COPY` 成功时可见**；
  任何失败一律抛异常，不返回部分计数。

### 8.7 通用写选项：不支持范围与拦截边界（v1.5.5 源码实测修正）

下列 DuckDB 通用写选项对远端 Bitable 写入无意义，**本格式不支持**：
`partition_by`、`per_thread_output`、`file_size_bytes`、`filename_pattern`、`file_extension`、
`use_tmp_file`、`overwrite` / `overwrite_or_ignore` / `append`、`return_files`、`return_stats`、
`write_partition_columns`、`hive_file_pattern`。

**拦截边界的实测结论（DuckDB v1.5.5，源码核验）**：`Binder::BindCopyTo`
（`duckdb/src/planner/binder/statement/bind_copy.cpp` L125–201）会在调用本扩展的 `copy_to_bind`
**之前**逐个就地消费上述选项（它们只存在于 Binder 的局部变量），且 `CopyInfo`
（`duckdb/src/include/duckdb/parser/parsed_data/copy_info.hpp`）不携带这些字段。
因此本扩展在 `copy_to_bind` 阶段**看不到**它们，无法逐个在 Bind 阶段报错。

确定可行的防线只有两条：

1. 用户显式传 `file_extension` 时，其值会写入 `CopyFunctionBindInput::file_extension`
   （`bind_copy.cpp` L160）；扩展在 `copy_to_bind` 中检测到该值不等于 `feishu_bitable`
   即抛 `InvalidInputException`。
2. 其余选项属**文档级禁止**：使用它们不会得到本扩展的支持性保证；若与分区 / 并行语义组合，
   还可能让 DuckDB 对目标字符串执行本地目录操作，属未定义行为。

若要用"逐个在 Bind 阶段拒绝"的严格语义，必须改用 `CopyFunction::plan` 自建逻辑计划
（`copy_function.hpp` 的 `copy_to_plan_t`），v0.2 不采用——与社区先例 gsheets 的取舍一致。

`file_extension` 被改动时的消息模板：

```
feishu_bitable: option "file_extension" is not supported for remote Bitable writeback
```

### 8.8 v0.2 "不做"清单

**永不做**（见 `todolist.md` ADR-008）：跨批次事务性、覆盖写、自动建表、自动扩展字段、
写入侧首表回退、`INSERT INTO` 语法、附件 / 地理位置 / 群组字段写入。

**暂不做、条件触发**：`UPDATE` / `DELETE` / `batch_update`（v0.2 只做 append-only 的 `batch_create`）。
仅在出现真实需求且批次 E 在线验证全部收口后，才以 `COPY` 显式模式评估。

**已实现（I-5）**：关联（`LINK` / `DUPLEX_LINK`）与人员（`USER`）按 §8.3 / §8.4 可写；
`user_id_type` 默认 `open_id`，另接受 `union_id` / `user_id`。在线成功样本仍只有匹配的 `open_id`。

### 8.9 失败后的恢复

- 批次一旦成功即已落在远端，**跨批次不具备事务性**：失败时不得假设远端被回滚。
- HTTP 超时、连接断开或响应无法解析都属"**结果未知**"，扩展**不会**自动重放（§见 `.specs/06_WRITEBACK_RESEARCH.md` §1.4）。
- 核对远端实际写入量的推荐做法（可直接复制运行）：

```sql
-- 用读侧函数比对远端真实行数；<app_token> / <table_id> 使用占位符
SELECT count(*) FROM read_feishu_bitable('<app_token>', table_id => '<table_id>');
```

- `batch_size` 的冻结来源见 §8.2：**500 取自 `batch_create` 的端点契约**，属端点事实而非防御性选择。
