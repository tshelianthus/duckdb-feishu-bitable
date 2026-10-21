# PRD: duckdb-feishu-bitable (Feishu Bitable Extension)

## 1. Vision & Objectives
- **Target**: 构建高性能、低内存占用、企业级弹性的 DuckDB 飞书多维表格（Bitable）读写扩展；v0.1 先交付只读扫描，后续以 `COPY` 批量回写为写入 MVP。
- **Positioning**: 允许用户直接在 DuckDB 中通过 SQL 查询飞书多维表格数据，无需编写中间同步脚本或通过 CSV/Excel 中转，无缝支持过滤、聚合与本地数据（如 Parquet/SQLite）关联分析。
- **Tech Stack**: **C++17**，基于 DuckDB 官方 `duckdb/extension-template` 与 CMake / vcpkg 工具链。
- **Distribution**: 遵循 DuckDB 社区扩展规范，正式发布至 `duckdb/community-extensions`。

## 2. Rationale: Why DuckDB Extension
- **Analytical Agility**: 飞书多维表格广泛用于企业业务协同，但缺乏强大的 OLAP 分析能力；通过 DuckDB 扩展可直接进行复杂 SQL 汇总、窗口函数分析及本地持久化导出。
- **Vectorized In-Memory Execution**: DuckDB 的 DataChunk 批处理模型使得百万级飞书多维表格数据查询速度远超传统 Python 脚本循环拉取。

## 3. User Experience & Scenarios (对标 gsheets 零摩擦交互)
- **Zero-Friction URL Input (浏览器 URL 直填)**:
  - 对标 Google Sheets (`gsheets`) 扩展体验，解决飞书多维表格参数碎片化（`app_token`、`table_id`、`view_id` 分散）的交互痛点。
  - 用户可直接将浏览器地址栏完整 URL 传入 `read_feishu_bitable('https://<domain>.feishu.cn/base/<app_token>?table=<table_id>&view=<view_id>')`，系统自动解析定位目标多维表格。
- **Replacement Scan (无感知替换扫描)**:
  - 允许用户直接在 SQL 中书写 `SELECT * FROM 'https://<domain>.feishu.cn/base/...'`。
  - 依托 DuckDB 替换扫描机制在解析期透明重写为表函数调用，使查询云端飞书多维表格与本地 Parquet/CSV 体验完全一致，无需显式调用表函数。
- **Default First Table Fallback (默认首表回退)**:
  - 当 URL 或调用参数未指定具体数据表时，自动拉取该多维表格的元数据并默认绑定首张数据表（`tables[0]`），与 Google Sheets 默认读取 Sheet 0 的用户心智一致。
- **Dual-Track Compatibility (双轨并存兼容)**:
  - 既提供现代化规范命名的 `read_feishu_bitable(...)` / `read_bitable(...)` 及 URL 直填体验；
  - 同时 100% 保持经典 `feishu_bitable_scan(app_token, table_id)` 契约的向下兼容性，保障既有脚本与生产管道的稳定运行。

- **Writeback Roadmap (回写路线)**:
  - 写入能力不纳入 v0.1 只读社区发布门禁，作为后续兼容演进；**v0.2 已实现**从 DuckDB 查询结果通过显式 `COPY ... TO '<URL 或 app_token>' (FORMAT feishu_bitable, table_id '...', secret '...', batch_size N)` 批量追加到**已存在**的指定表（append-only，单批上限 500，契约见 `.specs/03_API_CONTRACT.md` §8）。
  - `INSERT`、`UPDATE`、`DELETE`、覆盖写、跨表同步和自动建表不属于写入 MVP；必须在批量追加稳定后分别立项。

## 4. Scope Boundaries & Non-Goals
- **Read-Only Scope (v0.1) 与 append-only 写入 (v0.2)**：v0.1 仅提供只读查询能力；v0.2 通过**显式** `COPY ... TO` 增加 append-only 批量追加，契约已冻结于 `.specs/03_API_CONTRACT.md` §8，并有独立的三层测试门禁。除此之外不得添加任何隐式写入路径（写侧无首表回退，Replacement Scan 也不参与写入）。
- **No Non-Bitable Support**: 暂不支持飞书普通电子表格（Sheets）、云文档（Docs）、知识库、即时消息等其他飞书套件资源。
- **No Personal OAuth Flow**: 鉴权仅聚焦于飞书企业自建应用凭据（`app_id` + `app_secret` 获取 `tenant_access_token`），暂不支持弹出式个人用户 OAuth 授权码重定向流程。
- **No Attachment Binary Download (不下载附件二进制)**: 附件字段（type 17）仅以 `JSON` 形式返回飞书记录接口原样给出的元数据数组（`file_token` / `name` / `size` / `type` / `url` / `tmp_url`），扩展不会下载文件内容，也不提供 `BLOB` 列。理由：素材临时下载链接接口单次上限 5 个 `file_token`，在按页流式扫描中会退化为 O(行数/5) 次串行 HTTP 调用；文件字节物化进向量会击穿内存模型；且下载素材需要额外申请「下载云文档中的图片和附件」权限，会让所有只做元数据查询的用户被迫扩大授权范围。若后续要支持，应以「`file_token` → 预签名临时链接」的独立辅助函数形式立项，而不是把字节塞进扫描结果。
- **Single C++ Toolchain**: 严禁引入二次语言构建栈（如 Rust/Cargo 等）。
