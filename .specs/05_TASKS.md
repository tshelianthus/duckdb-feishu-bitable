# Tasks & Execution Roadmap: duckdb-feishu-bitable

> 本文件是**唯一的可分发任务看板**。`todolist.md` 保留调研结论、ADR 决策记录与阶段路线图；
> 两份文件的勾选状态必须一致，冲突时以本文件为准。
>
> **编排四要素**：每个未完成条目必须带齐 **ID / 执行角色 / 前置依赖 / 判定标准(DoD)**。
> 缺任一项即视为"未编排"，不得下发实施。
>
> **执行角色**：🤖 = agent 或 CI 可自动完成；👤 = 必须人工执行（git 写操作、GitHub 网页操作、真实凭据）。
>
> 最近一次编排盘点：2026-10-07（范围 B「可用但不发布」目标验收通过）。
> 最近一次状态回写：2026-10-25（D-0 已执行：仓库 visibility=public，匿名访问 200；未打 tag，未提交社区收录）。

---

## 状态总览

| 阶段 | 状态 | 备注 |
| :--- | :--- | :--- |
| Phase 0 脚手架与规范 | ✅ 完成 | ADR-001/002 已由实现落定，见 `todolist.md` §二 |
| Phase 1 网络 / 鉴权 / Secret | ✅ 完成 | 双凭据模式（模式 B）已实现并已同步文档（批次 A） |
| Phase 2 元数据函数 | ✅ 完成 | |
| Phase 3 扫描引擎与零摩擦寻址 | ✅ 完成 | |
| Phase 4 下推与高级类型 | ✅ 完成 | |
| Phase 5 健壮性与安全 | ✅ 完成（含 B-2） | B-2 Release RSS 常数内存已于 2026-10-07 验收；见 `docs/FEISHU_TEST_AGENT_PROMPT.md` §十一 |
| **在线测试层** | ✅ 完成 | F-1/F-2/F-3/F-4/G-4 均已完成；读侧在线回归在模式 A 下真实跑通（非 skip） |
| **凭据卫生** | 🔴 **自建应用 ID 仍在 git 历史中** | 2026-10-13 只读审计复核：仅 `cli_a965…`，7 commit；工作区净；G-2 方案已就绪，状态 `WAITING_USER_CONFIRM_G2_COMMANDS`；**未执行重写/未转公开** |
| Phase 6 社区发布 | ⏸️ **仓库已公开，收录未做** | D-0 ✅（2026-10-25）；**禁止** D-4/D-5/D-6/D-7 与社区收录 PR |
| Phase 7 回写（COPY） | ✅ 范围 B 可用；发布不做 | E-2~E-5 全系列 ✅；E-6.1/E-6.2 ✅；**E-6.3 本期禁止** |
| **批次 H（对标 PG FDW 生态）** | ✅ H-1~H-4 完成 | 谓词 search 下推 + `record_id` 单条 GET + README 三段式；`ORDER BY` 仅文本/数字/日期下推（I-1/I-2） |
| **批次 I（范围收口后的待验证项）** | ✅ I-1~I-5 完成 | `ORDER BY` 仅 TEXT/NUMBER/DATE_TIME 下推；关联/人员 `COPY` 写入已实现（I-5） |
| **范围 B 目标（可用但不发布）** | ✅ **已达成（2026-10-07）** | 能构建、能加载、契约 SQL 面可用；**不**走社区发布最终步骤 |
| **Base 离线快照研究与设计** | ✅ 已验收（2026-10-09） | BT-SNAPSHOT-RESEARCH-001；仅研究与设计，完整快照功能尚未实现 |

**当前待编排任务数：1**（ATTACH-EVAL 仍未批准）。**可立即开发的功能项：0**（快照 QA-MATRIX 已完成；ATTACH-EVAL 需单独批准）。
仍开着、且**明确不纳入范围 B 收口 / 本期禁止自动执行**的条目：
- **G-2**、**D-0a**、**D-4~D-7**、**E-6.3**（历史重写不再需要；tag / 社区收录仍禁止）
可选但不阻塞：D-2b 的触发器改动需要在 `dev` 上再 dispatch 一次主 pipeline。
Base 离线快照语义已冻结；**实现链 PROTO→MANIFEST→PUBLISH→QA-MATRIX 已于 2026-10-13 获批开工**（ATTACH-EVAL 仍缓）。D-2 已在 private 下全绿；G-2/公开发布链仍禁止。
（四要素完整性已由脚本逐条审计通过；`### E-n 系列` 为分组标题，非任务条目。）

**人工发布决策更新（2026-10-13，`WAITING_USER_CONFIRM_G2_COMMANDS`）**：只读审计已确认自建 `app_id` `<redacted_cli_id>` 仍存在于 7 个历史 commit（`af554b6`…`6ad1b46`），当前工作区/`HEAD` 已净（G-1/G-3 ✅）。**G-2 精确命令序列已草拟但未执行**；需用户确认目标分支（本地+远端 `dev`/`main`）、stash 策略与 `force-with-lease` 后再动手。仓库保持 private；**D-0 继续 `HOLD_UNTIL_G2_DOD`**。未见真实 `app_secret`/token 历史泄漏。

**范围 B 验收证据（2026-10-07，`dev` HEAD `c0293aa3a67fe7d1471d41d1af1ff79d02c8a794`）**：
- `make test_debug`：退出码 0；**16 cases / 313 assertions**；含读侧与写侧 `integration_*.test` 真实跑通（非 skip）；未跑 cleanup。
- `make test_unit`：退出码 0；**205 cases / 3584 assertions**。
- `make release`（Unix Makefiles 增量）+ 写测基线 cleanup 一次 + `make test_release`：退出码 0；**16 / 313**；无跳过文件。
- LOAD 冒烟：`feishu_bitable_fields` / `feishu_bitable_scan` / `feishu_bitable_tables` / `read_bitable` / `read_feishu_bitable` 可见；`COPY`（`FORMAT feishu_bitable`）不出现在 `duckdb_functions()`（DuckDB v1.5.5 预期），写侧由集成用例覆盖。
- 工作区干净；**未** commit / push / tag / 改 visibility / 重写历史 / 提社区收录 PR。

**关键路径（发布门禁 / 社区收录）**：G-2 → G-3 → **D-0（仓库转 public）** → **D-0a** → D-1b → D-2/D-2a → D-4 起照常。
**Private 引导路径（2026-10-01）**：**暂不公开**；**D-1 → D-3 已完成**（`.github/` 已在默认分支，Actions 已登记 4 个 workflow）。
D-1b **暂缓至 D-0 之后**（依赖匿名 clone）；D-2/`workflow_dispatch` 现已可执行；**禁止 G-2 / 改 visibility / D-4**。
**启动 public 的时机**：仅在 G-2 历史重写完成后；当前继续私有开发，不提前转公开。
批次 H / I 不在发布关键路径上；范围 B 已收口，不得被发布链条目重新打开为「未完成功能」。

---

## 本轮编排新增的实证结论（2026-09-11，由主 Agent 直接核实）

这些事实推翻了上一版看板的多处编排前提，已并入下方各批次：

1. **CI 从未执行过，且当前无法被触发**。`gh api .../actions/workflows` 的 `total_count` 为 **0**——
   GitHub 只登记**默认分支**上的 workflow，而 `main` 上没有 `.github/`（`main` 只有一个提交，
   仅含 `README.md`，整个项目只存在于 `dev`）。因此 push 不触发、`workflow_dispatch` 也不可用。
   **唯一可行的首次 CI 路径是开 `dev -> main` 的 PR**（`pull_request` 事件使用 PR 合并提交中的
   workflow 文件，不依赖默认分支登记状态）。
2. **仓库是 private**，社区收录要求 public + 匿名可检出；且私有仓库 Actions 按分钟计费
   （macOS ×10 / Windows ×2），而默认构建矩阵含 2 个 osx、2 个 windows 目标。
3. **实际构建 6 个平台，含 `windows_amd64_mingw`**。矩阵中 `opt_in: false` 的 9 个架构排掉 3 个 wasm 后，
   剩 `linux_amd64` / `linux_arm64` / `osx_amd64` / `osx_arm64` / `windows_amd64` / `windows_amd64_mingw`。
   注意 `windows_amd64_rtools` 在 v1.5-variegata 矩阵中**已不存在**，不要跟着 gsheets 抄。
4. **在线测试层是假绿**：`require-env` 变量齐备时用例仍全部 skip，原因是 DuckDB sqllogictest runner
   对错误消息匹配 `HTTP` 的用例自动降级为 skip。实测模式 B 为 401（token 过期）、模式 A 为 400。
   `make test_debug` 在此状态下退出码仍为 0 → 批次 F。
5. **`main` 无分支保护**：`gh api .../branches/main/protection` 返回 403
   「Upgrade to GitHub Pro or make this repository public」。原 D-3「合入受保护的 main」不成立。
6. **跨二进制加载已验证**（正面证据）：Homebrew 官方 `duckdb v1.5.5 (d8cdaa33fd)` 在源码树之外
   `LOAD` release 产物成功，`TYPE feishu` 注册正常，伪造凭据下抛干净 `Invalid Input Error`，
   无崩溃无泄漏 → 记为 D-1c 已完成。
7. **`.specs/06_WRITEBACK_RESEARCH.md` §2.3 结论错误**：该文称「未找到已被社区收录的 C++ CopyFunction
   向 SaaS API 写入的先例」，实际 `evidence-dev/duckdb_gsheets` 正是该先例且已收录
   （`GSheetCopyFunction` 注册 `copy_to_bind`/`initialize_global`/`initialize_local`/`sink`）→ E-2.8。

---

## [x] Phase 0: Scaffolding & Spec Alignment

- [x] 建立完整 `.specs/` 体系（01_PRD、02_ARCHITECTURE、03_API_CONTRACT、04_TESTING_SPEC、05_TASKS）。
- [x] 对标 `gsheets` 扩展完成零摩擦交互规格（`read_*`、URL 解析、Replacement Scan、首表回退）。
- [x] 确立 `AGENTS.md` 研发军规。
- [x] 规划根目录 `todolist.md`。
- [x] ADR-001（HTTP 客户端）落定为**复用 DuckDB 内置 `duckdb_httplib_openssl`**，而非经 vcpkg 引入
      cpp-httplib / curl：`vcpkg.json` 因此只声明 `openssl`，减少一层跨平台交叉编译风险。
- [x] ADR-002（JSON 解析）落定为**复用 DuckDB 内置 `duckdb_yyjson`**（CMake 中 `target_link_libraries(... duckdb_yyjson)`）。
- [x] 站立门禁 `make debug && make test_debug` 可跑通。

## Standing Rule（自 Phase 0 起始终有效）

- [x] 每个 commit 必须保证 `make debug && make test_debug` 通过。
- [x] 允许函数暂未实现而抛出明确异常，绝不允许载入段错误或破坏编译。

## [x] Phase 1: Network, Auth & Secret Integration

- [x] HTTPS 客户端与超时控制（`src/bitable/api_client.cpp`）。
- [x] 带 full jitter 的指数退避重试（`src/common/retry.cpp`），仅对 408/429/明确瞬态 5xx 与限流业务码重试。
- [x] 注册 DuckDB `TYPE feishu` 密钥（`src/secrets/feishu_secret.cpp`）。
- [x] 线程安全 `TokenCache` / `TokenProvider`：缓存键含凭据指纹、容量有界 LRU、同凭据并发刷新合并。
- [x] Secret 与 Auth 的单元/离线测试（`test/unit/auth/`、`test/unit/common/`、`test/sql/secret.test`）。
- [x] **双凭据模式**：`USER_ACCESS_TOKEN` 作为 `APP_ID` + `APP_SECRET` 的互斥替代实现完毕，
      扫描/元数据三条链路（`scan_function.cpp`、`tables_function.cpp`、`fields_function.cpp`）
      在拿到 user token 时直接作 Bearer 使用并跳过 tenant token 交换。
      ⚠️ 该能力的对外文档尚未同步，收尾任务见**批次 A**。

## [x] Phase 2: Metadata Functions

- [x] 实现 `feishu_bitable_tables`。
- [x] 实现 `feishu_bitable_fields`。
- [x] 覆盖表/字段的空集合、非法 JSON、业务错误码与分页游标矩阵
      （`test/unit/bitable/test_api_response.cpp`、`test/sql/metadata_*.test`）。

## [x] Phase 3: Core Data Scan Engine & Zero-Friction UX

- [x] `feishu_bitable_scan` TableFunction 生命周期（bind / init_global / init_local / scan）。
- [x] 全字段类型到 DuckDB 的转换器（`type_mapping.cpp`、`field_type.cpp`、`value_coercion.cpp`）。
- [x] 流式向量化批量填充 `DataChunk`。
- [x] URL 解析器（`src/bitable/url_parser.cpp`）与显式命名参数优先级。
- [x] 未指定 table 时的默认首表自动回退；空 Base 抛出明确 `InvalidInputException`。
- [x] 注册 `read_feishu_bitable` / `read_bitable` 多态表函数与 Replacement Scan 回调。
- [x] 保持经典 `feishu_bitable_scan` 100% 向后兼容。
- [x] 测试：`scan_basic.test`、`url_parser.test`、`fallback_first_table.test`、`replacement_scan.test`、
      `alias_and_compatibility.test`、Catch2 工程 `test/unit`（`make test_unit`）、
      `require-env` 门控的 `integration_first_table.test`。

## [x] Phase 4: Pushdowns & Advanced Types

- [x] 投影下推（`projection_pushdown = true`）；`COLUMN_IDENTIFIER_ROW_ID` / `COLUMN_IDENTIFIER_EMPTY`
      与系统列不参与 `field_names` 拼接；`COUNT(*)` 退化为最小投影而非全量拉取。
- [x] 惰性分页实现 LIMIT 提前终止（DuckDB v1.5.5 无 limit 下推接口，措辞不得写成"LIMIT 下推"）。
- [x] 复合字段（User / Attachment / Link / DuplexLink / Location / GroupChat / Formula /
      CreatedUser / ModifiedUser）与 `SCHEMA_MODE 'json'`。
- [x] 纯逻辑下沉自包含 TU 并由 Catch2 覆盖；移除契约外的可测性标量函数。
- [x] `test/sql/scan_pushdown.test`（离线）与 `test/sql/integration_pushdown.test`（在线）。

---

## [x] 批次 A：双凭据模式收尾（Secret 模式 B 文档与测试同步）

> **背景**：`USER_ACCESS_TOKEN` 模式代码已实现并已同步进 `.specs/03_API_CONTRACT.md` §1 与
> `.specs/04_TESTING_SPEC.md`，但 `README.md` 与 `docs/community/description.yml` 仍只描述应用凭据模式，
> 且 `README.md` 引用的在线测试环境变量名已随 `integration_*.test` 改名而失效。
> 本批次是 **v0.1 发布的前置阻塞项**——对外物料与实现不一致不得进入发布门禁。
>
> **共同前置**：无（当前工作区改动即为输入）。**互不阻塞，可并行下发。**

- [x] **A-1** 🤖 README 补齐 Secret 模式 B 并同步在线回归变量名
  - 位置：`README.md` §"2. 配置飞书自建应用凭据"（约 L34–42）与 §测试段（约 L213–217）。
  - 要求：小节标题改为覆盖两种模式；新增 `USER_ACCESS_TOKEN` 示例；写明二选一互斥规则与
    "提供 `USER_ACCESS_TOKEN` 时忽略 `APP_ID`/`APP_SECRET`"；说明模式 B 适用于自建应用未开通
    `bitable:app` 权限的场景。
  - **DoD**：`grep -n "USER_ACCESS_TOKEN\|APP_SECRET" README.md` 有输出，且 README 的在线测试段
    使用 `FEISHU_TEST_APP_ID` / `FEISHU_TEST_APP_SECRET` / `FEISHU_TEST_APP_TOKEN` /
    `FEISHU_TEST_URL`；README 的凭据描述与 `.specs/03_API_CONTRACT.md` §1 对齐；示例 SQL 可直接复制运行。

- [x] **A-2** 🤖 社区描述符补齐模式 B
  - 位置：`docs/community/description.yml` 的 `docs.extended_description` → `## Credentials` 段。
  - 要求：补充 `USER_ACCESS_TOKEN` 备选模式与脱敏说明；`docs.hello_world` 保持应用凭据模式不变
    （hello_world 面向首次使用者，仍以标准自建应用路径为准）。
  - **DoD**：YAML 可解析（`python3 -c "import yaml,sys;yaml.safe_load(open('docs/community/description.yml'))"`）；
    描述内容与 `.specs/03_API_CONTRACT.md` §1 一致；未引入 v0.1 尚未实现的能力表述。

- [x] **A-3** 🤖 补齐双模式的离线断言
  - 位置：`test/sql/secret.test`。
  - 要求：新增三条用例——(a) 同时提供 `APP_ID`/`APP_SECRET`/`USER_ACCESS_TOKEN` 时以 user token 为准，
    `duckdb_secrets()` 的 `secret_string` 中不出现 `app_secret` 条目；(b) 仅提供 `APP_ID` 而缺 `APP_SECRET`
    仍报 `Feishu secret requires non-empty APP_SECRET`；(c) 两种模式下 `secret_string` 均不含明文。
  - **DoD**：`make test_debug` 全绿；新增断言在两种模式下都覆盖脱敏；不引入网络请求。

- [x] **A-4** 🤖 测试规约与发布清单补一致性检查项
  - 位置：`.specs/04_TESTING_SPEC.md` §1 分层表、`docs/RELEASE_CHECKLIST.md` 第 5 步核对清单。
  - 要求：测试规约中显式列出"Secret 双模式（模式 A/模式 B）互斥与脱敏"属于离线 SQLLogicTest 层；
    发布清单第 5 步新增两条勾选项——README 与 description.yml 的凭据模式描述必须与
    `.specs/03_API_CONTRACT.md` §1 一致。
  - **DoD**：两份文档均出现对应条目；发布清单第 5 步的每条都能用一条 `grep` 或一次目视核对判定。

- [x] **A-5** 🤖 工作区清理与 `.gitignore` 补全
  - 现状：`.cache/`（clangd 索引与本地 duckdb 文件）、`uploads/`（本地上传的 xlsx 素材）为构建/运行副产物；
    `.cursor/project-context.md` 与 `.trae/skills/feishu-bitable-test/SKILL.md` 是 IDE/agent 侧的项目知识。
  - 要求：把 `.cache/` 与 `uploads/` 加入 `.gitignore`；阅读后两个文件的内容后二选一——
    若与 `docs/` 既有文档重复则一并忽略，若含独有的项目知识则纳入版本控制并在 README 或 `AGENTS.md` 中引用。
    **推荐**：`.cursor/` 与 `.trae/` 一并忽略，项目知识的唯一来源保持为 `.specs/` 与 `docs/`，避免三处漂移。
  - **DoD**：`git status --short` 不再出现未跟踪的构建/运行副产物；被保留的文件有明确归属说明。

- [x] **A-6** 👤 提交批次 A 与当前工作区改动
  - **前置**：A-1 ~ A-5 全部完成。
  - 要求：`make debug && make test_debug && make test_unit && make format-check` 全绿后，
    在 `dev` 分支提交（当前 13 个已改文件 + 批次 A 产出）。建议拆两个 commit：
    `feat: support user_access_token secret mode`（代码+测试）与 `docs: sync credential modes and writeback roadmap`（文档）。
  - **DoD**：`git status --short` 干净；每个 commit 单独可载入（Loadable Every Commit）。
  - **完成证据（2026-09-10）**：已完成本地 commit `01b4f53` 与 `af554b6`，未 push；加载 smoke 和主测试门禁已通过。

---

## [x] 批次 B：Phase 5.3 内存与长周期扫描门禁

> **背景**：Phase 5 的其余项已由单元测试与真实环境实测覆盖——重试分类矩阵、`Retry-After` 上限、
> 三类分页游标矩阵见 `test/unit/common/test_retry.cpp` 与 `test/unit/bitable/test_api_response.cpp`；
> 凭据脱敏见 `test/unit/common/test_error.cpp` 与 TC-01/TC-07 实测。
> **B-2 已于 2026-10-07 以 Release RSS 采样闭合**（见下方完成证据与 `docs/FEISHU_TEST_AGENT_PROMPT.md` §十一）。

- [x] **B-1** 🤖 ASan / UBSan 全量离线回归
  - **前置**：A-6（在干净工作区上跑）。
  - 要求：以 `-fsanitize=address,undefined` 构建 debug 扩展与 `test/unit` 工程，跑完
    `make test_debug` 的离线用例与 `make test_unit`。记录构建命令到 `docs/RELEASE_CHECKLIST.md` 新增小节。
  - **DoD**：零 ASan/UBSan 报告；若有报告则逐条修复或给出带依据的抑制说明（不得直接加白名单了事）。
  - **完成证据（2026-09-10）**：隔离目录 `build/batch_b_asan_debug` 以 ASan/UBSan 成功构建完整扩展与 SQLLogicTest runner，
    运行 13 个 SQL 文件（2 个在线用例按 `require-env` 跳过）退出码 0；`build/batch_b_asan_unit` 运行 120 cases / 2513 assertions，
    两者均无 ASan/UBSan 报告。macOS 回归使用 `ASAN_OPTIONS=detect_leaks=0`，因为该平台不支持本配置的 LeakSanitizer。
  - **复跑证据（2026-09-12，v0.2 写入链路）**：`build/batch_b_asan_debug` 在含全部写入侧 TU 的树上重新构建并运行 12 个离线文件 / 204 assertions，退出码 0、无 ASan/UBSan 报告；`build/batch_b_asan_unit` 159 cases / 3285 assertions 同样干净。

> **编排决策（2026-09-11）**：**B-2 不再作为批次 D 的前置。**
> 它验证的是读侧内存质量，与「能否被社区收录 / 能否 INSTALL+LOAD」之间没有因果关系，
> 社区 CI 也不会运行它；把它挂作发布前置，等于让整条发布链路被一个需要真实凭据 +
> 万行测试表的人工步骤无限期阻塞。B-2 仍是必须完成的读侧质量项，但与发布并行推进。
> **回退方式**：若首次公开发布后收到内存相关缺陷报告，则把 B-2 提升为 v0.1.1 的阻塞项。

- [x] **B-2** 👤 长周期扫描的常数内存验证（需真实凭据）
  - **前置**：B-1；需要 `FEISHU_TEST_*` 环境变量与一张 10,000+ 行的测试表。
  - **实现前置已完成（2026-09-11）**：记录扫描只保留有界近期游标历史，并有固定最大页数终止保险；
    不再把全部分页游标保存在 global state。剩余工作仅为真实表 RSS 采样验证。
  - **范围边界（2026-10-07）**：范围 B「可用但不发布」已验收通过；**B-2 不阻塞范围 B**。
  - 要求：对该表执行全表扫描与 `SELECT count(*)`，采样进程 RSS；确认内存随行数**不单调增长**，
    峰值与 `PAGE_SIZE` 相关而与总行数无关。结果写入 `docs/FEISHU_TEST_AGENT_PROMPT.md` 的验收记录页。
  - **DoD**：给出 RSS 采样数据（至少 3 个时间点）与判定结论；若观察到线性增长，作为阻塞性缺陷回退到实现阶段。
  - **完成证据（2026-10-07）**：读侧 pagination 表仅 500 行，故在写入 Base 临时建表并 `COPY` 填充
    2k/10k 行后，用 **Release** CLI 对 `count(*)` + `PAGE_SIZE` 100/500 采样进程 RSS；峰值稳定在
    ~32 MB，2k→10k 不放大，扫描期增长 ≤432 KB。Debug RSS 上涨经同进程二次扫描对照判定为分配器假象，
    不以 debug 否决。临时表已删除。全文见 `docs/FEISHU_TEST_AGENT_PROMPT.md` §十一。

- [x] **B-3** 🤖 决策并落地：ASan 是否进 CI
  - **前置**：B-1。
  - 要求：在 B-1 的实际耗时基础上决策——耗时可接受则新增 `workflow_call` 的 sanitizer job 并挂到主 pipeline，
    不可接受则在 `docs/RELEASE_CHECKLIST.md` 中固化为"每次发布前手工执行一次"的门禁步骤。
    **推荐**：先按后者（发布前手工），`test/unit` 工程不链接 DuckDB/OpenSSL，可优先把它的 ASan 跑法接进现有
    `UnitTests.yml`，成本极低。
  - **DoD**：决策与依据写进 `todolist.md` 的 ADR 表或发布清单；若接 CI 则 job 在一次真实 push 上跑绿。
  - **完成证据（2026-09-10）**：基于 B-1 实测，完整扩展 sanitizer 构建与 13 个离线 SQL 文件回归
    耗时较高，固定为每次发布前手工门禁；独立 `test/unit` sanitizer 已接入 `.github/workflows/UnitTests.yml`
    的 `workflow_call`。主 pipeline 保持原有 reusable workflow 调用，未增加调用方参数或依赖。

---

## [x] 批次 C：陈旧表述清理（文档不得超前或滞后于代码）

- [x] **C-1** 🤖 删除 `exclude_archs` 的"已知待办"告警框
  - 位置：`docs/RELEASE_CHECKLIST.md` 第 6.2 步的 `⚠️ 已知待办` 引用块。
  - 事实：`.github/workflows/MainDistributionPipeline.yml` 的 `duckdb-stable-build` job **已经**带有
    `exclude_archs: 'wasm_mvp;wasm_eh;wasm_threads'`，该告警框已失效。
  - 要求：删除告警框，改为一条"已落地"的说明并保留与 `description.yml` `excluded_platforms` 两处同步的提醒；
    `windows_amd64_mingw` 的风险提示保留（尚未验证过该架构能否构建）。
  - **DoD**：`grep -n "已知待办" docs/RELEASE_CHECKLIST.md` 无输出；两处平台排除列表仍被明确要求保持一致。
  - **完成证据（2026-09-09）**：已删除告警框并改为已落地说明；`git diff --check` 通过，目标文字无输出。

- [x] **C-2** 🤖 同步 `todolist.md` 的勾选状态与本看板一致 —— **已于本次编排完成**
  - 事实：`todolist.md` 中 Phase 0/1/2/3/5 存在"父项已勾、子项全空"的不自洽；Phase 5.1/5.5 的子项
    实际已被单元测试覆盖；Phase 6.1 的"遗留：需补 exclude_archs"同样已失效。
  - 要求：逐条对照本看板补勾，并为每个已完成子项附一句证据（测试文件名或实测用例编号）。
  - **DoD**：`todolist.md` 中不再出现父项 `[x]` 而子项全 `[ ]` 的段落；每个 `[x]` 都能指向代码或测试。
  - 完成情况：ADR 表由"待定项"改为"决策记录"（ADR-001/002 记录为复用 DuckDB 内置库，新增 ADR-006 双凭据模式）；
    Phase 0–4 子项全部补勾；5.1/5.5 逐条附上覆盖它的测试文件与用例名；5.3 标注为 Phase 5 尚未完全收口项；
    6.1 的失效遗留项改写为已生效；6.3 因 ADR-006 重新打开并挂上 A-1~A-5、C-1；6.4 拆成 D-1~D-7；
    新增 Phase 7 章节与路线图节点。

---

---

## [x] 批次 F：修复在线测试层的「假绿」（**质量门禁失效，优先级高于批次 D**）

> **背景**：在线用例当前提供**零信号**。实测（2026-09-11）：
> `./build/debug/test/unittest "test/sql/integration_first_table.test"` 在 `.env.local` 八个键齐备的情况下
> 输出 `All tests were skipped ... skip on error_message matching 'HTTP': 1`，退出码 0。
> 该 skip 指令**不在 `.test` 文件里**，来自 DuckDB sqllogictest runner 的内建行为。
> 用官方 duckdb + release 产物直连复现真实错误：模式 B → `HTTP 401`（user token 已过期）、
> 模式 A → `feishu_bitable_tables: ... HTTP 400`。
>
> **危害**：任何「在线用例通过」的结论都不可信；`.specs/05_TASKS.md` 旧版 D-1 曾把
> 「Debug 集成用例因 HTTP 错误跳过」记为"预期门控"，这是**误判**——预期门控是 `require-env`
> 缺变量时的整文件跳过，401/400 是真实失败被掩盖。

- [x] **F-1** 🤖 让「门控跳过」与「请求失败」在测试输出中可区分
  - **前置**：无。
  - 要求：调研 DuckDB sqllogictest 的 skip-on-error 机制（`duckdb/test/sqllogic/` 下的实现），
    给出让 HTTP 层失败**计为失败而非跳过**的做法；若 runner 不提供开关，则改为在
    `integration_*.test` 中先用一条最小断言探测连通性，失败时以明确消息 `halt`。
  - **DoD**：在凭据无效的情况下重跑两个 `integration_*.test`，退出码**非 0**，
    且输出中明确出现区别于 `require-env` 跳过的失败原因。
  - **完成证据（2026-09-11）**：两个集成测试在所有 `require-env` 之后加入
    `set ignore_error_messages`，清除 DuckDB runner 默认的 `HTTP` / `Unable to connect`
    skip 列表；缺少变量时仍整文件跳过，伪造凭据导致 HTTPS 请求失败时退出码为 1，
    输出 `Query unexpectedly failed!`。

- [x] **F-2** 🤖 在线回归凭据由模式 B 切换到模式 A（已闭环）
  - **前置**：F-1；依赖 F-4 的权限确认结果。
  - 要求：`user_access_token` TTL 约 2 小时，**本质上无法充当可复现回归的测试凭据**。
    把 `integration_first_table.test` / `integration_pushdown.test` 的 `require-env` 与
    `CREATE SECRET` 改为 `APP_ID` + `APP_SECRET`（tenant token 由扩展自动续期）；
    模式 B 退回人工探查用途，并在 `docs/FEISHU_TEST_AGENT_PROMPT.md` 中写明其 2 小时时效。
  - **DoD**：`grep -n "USER_ACCESS_TOKEN" test/sql/integration_*.test` 无输出；
    两个在线用例在仅提供应用凭据时真实跑通（非 skip）。
  - **完成证据（2026-09-30）**：DoD 第一条已复验——`test/sql/**/integration_*.test` 无 `USER_ACCESS_TOKEN` 命中
    （仅 `secret.test` 保留，用于覆盖模式 B 的离线 secret 校验）。第二条：新建带 `bitable:app` 的自建应用并重建
    读侧回归 Base 后，`make test_sql_online_debug` 中 `integration_first_table` 6 assertions、
    `integration_pushdown` 21 assertions 全绿且非 skip。原应用始终未取得 scope，故采用"换应用 + 重建 Base"而非"补权限"。

- [x] **F-3** 🤖 消除 Release 与 Debug 的在线门禁不对称
  - **前置**：无。
  - 现状：`Makefile:17-22` 只覆写了 `test_debug_internal` 去 source `.env.local`，
    `test_release_internal` 沿用 `extension-ci-tools` 原样（`duckdb_extension.Makefile:202`），
    因此 `make test_release` **永远**跳过在线用例。该不对称目前无任何文档记录。
  - 要求：二选一并落地——(a) 同样覆写 `test_release_internal`；
    (b) 保持现状但在 `.specs/04_TESTING_SPEC.md` 与 `docs/RELEASE_CHECKLIST.md` 中显式写明
    「在线用例仅在 Debug 下执行」。**推荐 (a)**，理由是发布门禁跑的是 Release 产物。
  - **DoD**：所选方案落地后，`make test_release` 的在线用例行为与文档描述一致，可由一条 grep 判定。
  - **完成证据（2026-09-11）**：`Makefile` 为 Debug/Release 共用 `.env.local` 导出逻辑；
    `.specs/04_TESTING_SPEC.md` 与 `docs/RELEASE_CHECKLIST.md` 已同步说明两者行为一致。

- [x] **F-4** 👤 确认模式 A 的 HTTP 400 成因
  - **前置**：无（需真实凭据）。
  - 要求：实测区分三种可能——应用凭据失效 / 自建应用缺 `bitable:app*` 权限 / `FEISHU_TEST_APP_TOKEN`
    与该应用不匹配。结论写入 `docs/FEISHU_TEST_AGENT_PROMPT.md`。
  - **DoD**：给出明确成因与修复动作；若模式 A 因权限不可用，则 F-2 回退为
    「保留模式 B 但在文档中写明每次运行前需手工刷新 token」。
  - **完成证据（2026-09-11）**：tenant token 交换为 HTTP 200 / code 0；`tables` 与 `fields` 均为
    HTTP 400 / code `99991672`，响应指向缺少 Bitable scope。结论与修复动作已脱敏写入
    `docs/FEISHU_TEST_AGENT_PROMPT.md`；F-2 保持阻塞，直到权限生效并完成真实回归。


---

## [ ] 批次 D：Phase 6 正式发布门禁（执行细则见 `docs/RELEASE_CHECKLIST.md`）

> **主 Agent 对本批次顺序的强制修正（2026-09-11；2026-10-01 更新）**：
> 原阻塞：默认分支无 `.github/` → `actions/workflows` `total_count=0`，首次 CI 只能走 `dev→main` PR（D-3）。
> **现状（D-3 ✅）**：PR #1 已合入，`main` 已登记 4 个 workflow，`workflow_dispatch`（D-2 / E-5.6）现已可触发。
> **Private 引导次序（2026-10-01 用户决策）**：**暂不执行 D-0 / D-0a / G-2 / D-4**；**D-1 → D-3 已完成**；
> **D-1b 暂缓至 D-0 之后**；D-2 与 D-2a 已于 2026-10-24 闭合，D-2b 已落地并等待复跑。
> **人工发布决策更新（2026-10-13，`WAITING_USER_CONFIRM_G2_COMMANDS`）**：G-2 方案已就绪、未执行；仓库保持 private；D-0 `HOLD_UNTIL_G2_DOD`。
> 社区收录完整链仍是：G-2 → D-0 → D-0a → D-1b → D-2… → D-4。
> 各条任务的内容与 DoD 保持下方原文，仅执行次序按本段调整。

> **共同前置**：批次 A、批次 C 完成。批次 B 仅 B-2 未完，**B-2 不阻塞本批次**——
> 它验证的是读侧内存质量，与「能否被社区收录 / 能否 INSTALL+LOAD」无因果关系，社区 CI 也不跑它；
> 保留它作前置会让发布被一个需要真实凭据的人工步骤无限期阻塞（详见批次 B 的说明）。
> 本批次按上方修正后的次序执行，任一步失败必须回到 `dev` 修复后从该步重跑。

---

### D-0 ✅ 把 GitHub 仓库从 private 改为 public

- **前置**：**批次 G 全部完成**（G-1 ✅ / G-2 / G-3 ✅）。在历史清理完成前执行 D-0 会把
  自建应用 ID 永久公开，且 GitHub 的 fork / 缓存可能使其不可撤回。
- **启动时机（2026-09-29 决策）**：范围 B 期间**保持 private**，本地写回与
  `write-tests` Secret 继续在私有仓推进；**仅在准备走社区发布链（批次 D / E-6.3）时**
  启动本条。不要为了「多拿 Environment 审批门」提前转公开。
- **背景**：`gh api repos/tshelianthus/duckdb-feishu-bitable` 返回
  `{"private": true, "visibility": "private"}`。这是社区收录的**硬阻塞**：
  duckdb.org/community_extensions 的收录标准明确要求扩展
  "public, open-source, and hosted on GitHub"；`duckdb/community-extensions`
  的 build job 用 `override_repository` 匿名检出我们的仓库，私有仓库必然失败。
  次要但同样现实的理由：私有仓库的 Actions 分钟数计费（免费额度 2000 分钟/月，
  **macOS runner 按 10 倍计、Windows 按 2 倍计**），而本项目的分发矩阵含
  `osx_amd64` + `osx_arm64` + `windows_amd64` + `windows_amd64_mingw`；
  公开仓库的标准 runner 不计费。先转公开可以同时解掉 D-2 的成本风险。
  公开后 Free 计划即可启用 Environment Required reviewers / wait timer（私有 Free 不行，见 D-0a）。
- **⚠️ 前置变更（2026-09-11）**：D-0 现在**必须**在批次 G（凭据卫生）全部完成之后执行。
  转公开前的隐私核对结论已由用户确认而改写：
  - `git ls-files | grep -i env` 无输出；`.gitignore` 覆盖 `.env*` / `*.env` /
    `test/fixtures/secrets.json` / `*.duckdb` / `uploads/`。
  - 全历史扫描命中的那个 `cli_…` 串，**经用户确认是其自建应用的 app_id，不是公开值**。
    工作区已于 commit 中清除（G-1 已完成），但**它仍存在于 git 历史**，
    仓库转公开后历史同样公开 → 必须先完成 **G-2 历史重写**。
  - `bascn…` / `tbl…` / Base URL 扫描：`docs/` 与 `.specs/` 中全部是
    `bascnXXXXXXXX` / `tblYYYYYYYY` 形式的占位符，无真实 Base token。
- **操作**：GitHub 仓库 Settings → General → Danger Zone → Change visibility → Public
  （或 `gh api -X PATCH repos/:owner/:repo -f visibility=public`，须用户明确确认后才执行）。
- **DoD**：`gh api repos/tshelianthus/duckdb-feishu-bitable --jq '.visibility'` 输出 `public`；
  且 `curl -sS -o /dev/null -w '%{http_code}' https://github.com/tshelianthus/duckdb-feishu-bitable`
  在未登录环境下返回 `200`。
- **完成证据（2026-10-25）**：用户明确批准先公开、不提交社区。新仓库创建于 2026-10-21，全部历史仅 `c1a280a`、`dca7727`、`647418a`，不含旧仓库中的自建应用 ID，因此未执行 G-2。`visibility=public`，匿名访问返回 200。未打 tag，未创建社区收录 PR。

### D-0a 👤 转公开后补齐 Environment / 分支保护（写测与 main）

- **前置**：D-0（仓库已 `public`）。
- **背景**：E-5.2 已在私有仓配好 `write-tests` 四项 Secret（`FEISHU_WRITE_TEST_APP_TOKEN`、`FEISHU_WRITE_TEST_TABLE_ID`、`FEISHU_WRITE_TEST_APP_ID`、`FEISHU_WRITE_TEST_APP_SECRET`）+ `dev`/`main` 分支白名单；
  私有 Free 无法启用 Required reviewers / wait timer（API 422）。公开仓 Free 即可补上。
  同理 `main` 分支保护（required status checks）在私有 Free 上也被拒，公开后才能配（见 D-3 前置）。
- **内容**：
  1. Environment `write-tests`：保留现有四项 Secret（`FEISHU_WRITE_TEST_APP_TOKEN`、`FEISHU_WRITE_TEST_TABLE_ID`、`FEISHU_WRITE_TEST_APP_ID`、`FEISHU_WRITE_TEST_APP_SECRET`）与分支白名单；新增 **Required reviewers**
     （至少一名维护者）与可选 **wait timer**（建议 ≥1 分钟，防误触 `workflow_dispatch`）；
     核对仓库级 Secret 仍为空、主 pipeline 仍不引用 `WriteTests.yml`。
  2. （可选同步）Environment `integration-tests`（读侧 `SQLTests.yml` 若使用）同样加 reviewers。
  3. 为 `main` 启用分支保护：至少要求 PR、禁止 force-push；具体 required checks 在 D-3 首次 CI
     出现后再勾选，本条先打开「可配置」状态即可。
- **DoD**：
  - `gh api repos/:owner/:repo/environments/write-tests` 的 `protection_rules` 含
    `required_reviewers`（及可选 `wait_timer`），且部署分支策略仍仅允许 `dev`/`main`；
  - `gh secret list`（仓库级）仍无 `FEISHU_WRITE_TEST_*`；
  - `gh api repos/:owner/:repo/branches/main/protection` 不再因计费返回 403
    （规则可先最小集，完整 required checks 可留到 D-3 后）。

---

### D-1 ✅ 本地门禁（四项，**不含 `tidy-check`**）

- **前置**：无。
- **操作**：

  ```sh
  GEN=ninja make debug   && make test_debug
  GEN=ninja make release && make test_release
  make test_unit
  make format-check
  ```

- **变更说明**：原 D-1 的第五项 `make tidy-check` **已移出本地门禁**，改由 D-2 的 CI
  `Code Quality Check` job 判定。依据见 Q3：本机无 `clang-tidy`，且 `make tidy-check`
  （`extension-ci-tools/makefiles/duckdb_extension.Makefile` L272-276）会为 tidy
  额外全量配置一次 DuckDB 源码树；CI 的 tidy job 跑的是**同一条命令**，
  用 ubuntu-24.04 的 apt `clang-tidy`（版本未固定），本机 Homebrew LLVM 版本几乎必然不同，
  跑绿跑红都不代表 CI 结论。
- **DoD**：四条命令退出码均为 0；`test/sql/` 下 `integration_*.test` 因 `require-env`
  缺失而**整文件跳过**（而非失败）；Debug 与 Release 的通过用例数一致。
- **已有证据（2026-09-10）**：这四项当时已全部通过（原 D-1 进度证据），
  D-1 只需在最终待发布的树上重跑一次即可闭合。
- **完成证据（2026-09-11）**：最终改动树上 `make debug`、`make test_debug`（11 个离线文件，
  191 assertions，2 个在线文件按 `require-env` 跳过）、`make release`、`make test_release`
  （同样 191 assertions / 2 skips）、`make test_unit`（130 cases / 2623 assertions）与
  `make format-check` 均退出码 0。
- **✅ 完成证据（2026-10-01，HEAD `4cac29d`）**：在 `dev` 上重跑四条门禁，退出码均为 0。
  - `make debug && make test_debug`：**15 test cases / 262 assertions**（含读侧 `integration_*` 与
    写侧 `integration_copy_writeback` 在凭据齐全时真实跑通，非 skip）。
  - `make release && make test_release`：同上 **15 / 262**（Debug↔Release 一致）。
  - `make test_unit`：**171 cases / 3433 assertions**。
  - `make format-check`：Passed。
  - 备注：写测 Base 为共享状态；`test_debug` 与 `test_release` 之间须先跑
    `scripts/cleanup_write_test_records.sh`，否则 `count(*)=300` 会因残留行数失败
    （属测试隔离问题，非扩展回归）。DoD 亦允许缺 env 时整文件 skip。

---

### D-1a 🤖 修订 `docs/RELEASE_CHECKLIST.md` §4，把 tidy 的权威判定点改到 CI

- **前置**：无（可与 D-1 并行）。
- **位置**：`docs/RELEASE_CHECKLIST.md` L87-95（§4「代码风格与静态检查 🤖」）。
- **要求**：
  1. §4 的本地命令块只保留 `make format-check`。
  2. 新增一句：`tidy-check` 的权威判定点是 CI 的 `Code Quality Check` job
     （`_extension_code_quality.yml`，`runs-on: ubuntu-24.04`，apt `clang-tidy`，`TIDY_THREADS=4`），
     本项目已通过 `format_checks: 'format;tidy'` 启用；本地不再作为发布门禁。
  3. 删除现有断言「本地不过 CI 必挂」（该断言在本机无 clang-tidy 时无法执行，属无法判定的表述）。
  4. §4 的图例保持 🤖。
- **DoD**：`grep -n "make tidy-check" docs/RELEASE_CHECKLIST.md` 仅在说明性文字中出现、
  不再出现在 §4 的可执行命令块内；§4 的每条判定标准都能由 `make format-check` 的退出码
  或 CI job 的 conclusion 单独判定。
- **完成证据（2026-09-11）**：§4 命令块仅保留 `make format-check`，并明确 CI
  `Code Quality Check`（Ubuntu 24.04 / apt clang-tidy / `TIDY_THREADS=4`）为 tidy 权威判定点。

---

### D-1b 🤖 干净检出复现社区 CI 的构建方式

- **前置**：D-0（仓库已公开，才能匿名 clone）、D-1。
- **⏸️ 暂缓至 D-0 之后（2026-10-01）**：本条依赖匿名 `git clone` 公开仓；当前仓库保持
  `visibility=private`，且用户决策禁止执行 D-0。Private 引导路径跳过本条，直接在 D-1 绿后做 D-3。
- **背景**：`duckdb/community-extensions/.github/workflows/build.yml` 的 `build` job
  **不使用我们仓库的任何 workflow**，而是把
  `override_repository: <repo.github>` / `override_ref: <repo.ref>` 传给
  `_extension_distribution.yml@v1.5-variegata`，在干净检出上跑 `make test_release`
  （`_extension_distribution.yml` L464/474/722/1004）。因此「本机 `make release` 能过」
  **不等于**「社区 CI 能过」——本机可能依赖了未提交的文件或本地缓存。
- **操作**：

  ```sh
  D=$(mktemp -d) && git clone --recurse-submodules \
    https://github.com/tshelianthus/duckdb-feishu-bitable "$D/x"
  cd "$D/x" && GEN=ninja make release && make test_release
  ```

- **DoD**：退出码 0；且构建后 `git -C "$D/x" status --short` 除 `build/` 外无意外产物；
  `integration_*.test` 整文件跳过。跑完后 `rm -rf "$D"`。

---

### D-1c ✅ 已完成 — 源码树之外的 LOAD 冒烟（干净宿主 DuckDB）

- **前置**：D-1。
- **完成证据（2026-09-11，主 Agent 实测）**：用 Homebrew 官方 `duckdb v1.5.5 (d8cdaa33fd)`
  在**源码树之外**的工作目录 `LOAD` 本仓库编译产物成功；`TYPE feishu` 密钥注册正常；
  在伪造凭据下抛出干净的 `Invalid Input Error`，无崩溃、无凭据泄漏。
  这补齐了 `docs/RELEASE_CHECKLIST.md` §3「可选的载入面自查」之外的一项更强证据：
  产物不依赖构建目录布局，可被任意同版本 DuckDB 载入。
- **DoD（已满足）**：非源码树目录中 `LOAD '<abs path>.duckdb_extension'` 退出码 0，
  `duckdb_functions()` 能列出五个 `%bitable%` 函数。

---

### D-2 ✅ 在 `dev` 上**手动 dispatch** 主 pipeline，三个 job 全绿

- **前置**：D-0、D-1、D-1b。
- **变更说明**：原 D-2 写的是「**推送 `dev`** 并等待三个 job 全绿」，**在机制上不成立**——
  `.github/workflows/MainDistributionPipeline.yml` L5-7 的 `on:` 只有
  `pull_request` 与 `workflow_dispatch`，push 到 `dev` 不触发任何 workflow
  （这也是 `gh run list` 至今为空的原因，本仓库 Actions 从未执行过）。
- **操作**：

  ```sh
  git push origin dev
  gh workflow run MainDistributionPipeline.yml --ref dev
  gh run watch
  ```

- **DoD**：
  1. `gh run list --workflow=MainDistributionPipeline.yml --branch dev --limit 1 --json conclusion --jq '.[0].conclusion'`
     输出 `success`。
  2. `gh run view <run-id> --json jobs --jq '[.jobs[].name]'` 中包含
     `Unit Tests` 系列、`Build extension binaries` 的各平台 job、`Format Check`、`Tidy Check`，
     且无 `*wasm*` 平台 job。
  3. **本次运行同时是「该 workflow 语法本身是否正确」的首次验证**——三个 job 都必须真正启动过，
     不允许出现 `startup_failure`。
- **本轮执行证据（2026-10-09；触发成功，DoD 未达成，任务保持未完成）**：
  - 触发：`gh workflow run MainDistributionPipeline.yml --ref dev`（`workflow_dispatch`）。
  - Run：[37951789573](https://github.com/tshelianthus/duckdb-feishu-bitable/actions/runs/37951789573)
  - HEAD：`328f5a82cdae4b07f7705a098a3dd09c3c288b77`
  - `conclusion=failure`；**不是** `startup_failure`（workflow 语法已能真正启动）。
  - 成功：`Unit Tests / Run unit tests`、`Unit Tests / Run unit tests with ASan/UBSan`、
    `Online SQL Tests / Check online test credentials`、`Build extension binaries / Generate matrix`。
  - 跳过：`Online SQL Tests / Run online SQLLogicTests`（可选 secrets 缺失）；
    `Build extension binaries / MacOS`、`Windows`、`DuckDB-Wasm`（Linux 失败后跳过）。
  - 失败：
    - `Code Quality Check / Format Check`：`src/bitable/tables_function.cpp`、
      `src/feishu_bitable_extension.cpp` 的 `MakeFunctionDescription(...)` 换行风格不通过；
      需后续 `make format-fix`（本轮未自动提交修复）。
    - `Code Quality Check / Tidy Check`：`Tidy Check Diff` 失败。
    - `Build extension binaries / Linux (linux_arm64, ...)`：Docker `make release` 被 kill，
      `exit code 137` + runner shutdown signal。
  - 取消：`Build extension binaries / Linux (linux_amd64, ...)`（arm64 失败后 cancelled）。
  - **结论**：D-2 已触发并完成观察；三组核心 job **未全绿**，故本条目保持 `[ ]` 未完成。
  - **跟进建议（未自动执行）**：先本地/提交 format-fix（及 tidy 修复）后再重跑 D-2；
    arm64 `137` 属 runner/OOM 类，可在修复质量门后重试观察是否复现。D-2a 仍以前置「D-2 通过」为准。
- **完成证据（2026-10-24，`dca7727`）**：
  - Run：[39737386589](https://github.com/tshelianthus/duckdb-feishu-bitable/actions/runs/39737386589)，`workflow_dispatch`，`conclusion=success`。
  - 已启动并成功：Unit Tests（含 ASan/UBSan）、Format Check、Tidy Check、六个平台二进制构建、Online SQL Tests（含 debug 构建与受保护集成测试）。
  - 唯一跳过：`Build extension binaries / DuckDB-Wasm`（`exclude_archs` 排除三个 wasm，符合 DoD）。
  - 不是 `startup_failure`。

---

### D-2a ✅ 判定并落实 `windows_amd64_mingw` 的去留（**本次审查发现的最大风险**）

- **前置**：D-2 的运行结果。
- **背景**：`extension-ci-tools/config/distribution_matrix.json`（子模块 pin `72e76e9`）中
  默认开启（`opt_in: false`）的架构是 `linux_amd64`、`linux_arm64`、`osx_amd64`、`osx_arm64`、
  `windows_amd64`、**`windows_amd64_mingw`**、`wasm_mvp`、`wasm_eh`、`wasm_threads`。
  我们只排了三个 wasm，**mingw 会被真实构建**。而已收录的 `gsheets` 的
  `excluded_platforms` 是 `"windows_amd64_rtools;windows_amd64_mingw;wasm_mvp;wasm_eh;wasm_threads"`
  —— 它**明确排掉了 mingw**（`windows_amd64_rtools` 在 v1.5-variegata 矩阵里已不存在，是历史遗留，
  我们**不要**跟着抄这一项）。本扩展静态链接 OpenSSL，mingw 是最易失败的目标。
- **决策规则（二选一，由 D-2 的实测结果自动决定，不需要再讨论）**：
  - D-2 中 `windows_amd64_mingw` job 为 success ⇒ **保持现状**，两处排除列表不动，
    并在 `docs/RELEASE_CHECKLIST.md` L204-206 把「若该架构构建失败」的假设性提示
    改写为「已在 <run-id> 上实测通过」。
  - D-2 中该 job 失败 ⇒ **同时**在
    `.github/workflows/MainDistributionPipeline.yml` 的 `exclude_archs` 与
    `docs/community/description.yml` 的 `excluded_platforms` 两处追加 `windows_amd64_mingw`，
    然后重跑 D-2。
- **DoD**：
  `grep -o "wasm_mvp[^'\"]*" .github/workflows/MainDistributionPipeline.yml` 与
  `grep -o 'excluded_platforms: "[^"]*"' docs/community/description.yml`
  两处的平台集合**逐项相同**（顺序可不同，用集合比对）；
  且 `grep -n "若该架构构建失败" docs/RELEASE_CHECKLIST.md` 无输出
  （假设性表述已替换为带 run-id 的实测结论）。
- **完成证据（2026-10-24）**：run [39737386589](https://github.com/tshelianthus/duckdb-feishu-bitable/actions/runs/39737386589) 的 `windows_amd64_mingw` job 为 `success`，因此按决策规则**保持现状**，`exclude_archs` 仍只有 `wasm_mvp;wasm_eh;wasm_threads`。当前干净源码快照没有 `docs/community/description.yml` 与 `docs/RELEASE_CHECKLIST.md`，所以无法回写那两处文档；平台集合没有变化。

---

### D-2b 🤖 为 `main` 与 tag 补 `push` 触发器（触发器已落地，复跑未完成）

- **前置**：D-2 通过（先证明 workflow 本身能跑，再动触发器）。
- **背景**：社区 5 个样本中 4 个含 `push`（`duckdb/extension-template`、`duckdb/duckdb-avro`、
  `quackscience/duckdb-extension-openprompt`、`Query-farm/airport`），只有
  `evidence-dev/duckdb_gsheets` 与本项目一样是 `pull_request` + `workflow_dispatch`。
  commit `9b527d7 ci: require explicit dev workflow dispatch` 的原意（feature 分支不自动烧 CI）
  **正确且保留**；但「tag 对应 commit 上 CI 全绿」（`docs/RELEASE_CHECKLIST.md` L226）
  在当前配置下只能靠人工再 dispatch 一次，容易漏。
- **要求**：把 `.github/workflows/MainDistributionPipeline.yml` L5-7 改为：

  ```yaml
  on:
    push:
      branches: [main]
      tags: ['v*']
    pull_request:
    workflow_dispatch:
  ```

  参照 `Query-farm/airport` 的 `push.branches` 限定写法。不加 `paths-ignore`
  （本仓库 commit 量小，过滤规则的维护成本高于收益）。
- **DoD**：`python3 -c "import yaml;d=yaml.safe_load(open('.github/workflows/MainDistributionPipeline.yml'));print(d[True])"`
  输出的 `on` 映射同时含 `push`（且 `branches` 为 `['main']`、`tags` 为 `['v*']`）、
  `pull_request`、`workflow_dispatch`；改动后在 `dev` 上再 dispatch 一次仍全绿。
- **部分完成（2026-10-24）**：`.github/workflows/MainDistributionPipeline.yml` 的 `on` 已改为
  `push.branches=[main]`、`push.tags=['v*']`、`pull_request`、`workflow_dispatch`；`dev` 的普通 push 仍不触发。
  **剩余 DoD**：该改动提交后在 `dev` 上再 dispatch 一次并确认全绿。

---

### D-3 👤 开 `dev` → `main` PR 并合并 —— **✅ 已完成（2026-10-01，Private CI bootstrap）**

- **前置（社区收录完整链）**：D-2、D-2a、D-2b 全部完成。
- **前置（Private 引导路径，2026-10-01）**：**仅 D-1**（D-1b/D-2 延后；不改 visibility、不打 tag）。
  目的是把 `.github/` 落到默认分支以启动 Actions，**不是**正式 v0.1.0 社区发布。
- **操作**：

  ```sh
  gh pr create --base main --head dev \
    --title "Bring feishu_bitable to main (private CI bootstrap)" --body "..."
  ```

- **变更说明**：原 D-3 的 DoD 写「已合入**受保护的** `main`」。经核实这是错误表述——
  `gh api .../branches/main/protection` 返回 403
  `"Upgrade to GitHub Pro or make this repository public to enable this feature"`，
  即当前**没有任何分支保护规则**。D-0 转公开后分支保护才可配置；
  但为 v0.1.0 首发**不要求**先配好保护（那是发布后的治理动作）。
- **DoD**：
  1. 该 PR 会自动触发 `pull_request` 上的主 pipeline；
     `gh pr checks <pr> --json state --jq '[.[].state] | unique'` 结果为 `["SUCCESS"]`。
  2. `gh pr view <pr> --json mergedAt --jq '.mergedAt'` 非空。
  3. 合并后 `git rev-list --count origin/main..origin/dev` 输出 `0`。
- **完成证据（2026-10-01）**：
  - PR：https://github.com/tshelianthus/duckdb-feishu-bitable/pull/1（`dev→main`，merge commit）
  - HEAD SHA（合入前）：`af3906a2bb4ffc6f27df0c42dd336ea247c37140`
  - 权威 CI run：[36859905906](https://github.com/tshelianthus/duckdb-feishu-bitable/actions/runs/36859905906) — **success**
    （6 平台构建全绿含 `windows_amd64` + `windows_amd64_mingw`；unit/format/tidy 绿；
    Online SQL / Wasm **skipped** OK）
  - 合并：`mergedAt=2026-10-01T13:29:16Z`，merge SHA **`8adaa02b2bfd8cab40bc4855451022cb6d596ff8`**
  - `git rev-list --count origin/main..origin/dev` → `0`
  - 合入后 `gh api .../actions/workflows` `total_count=4`（含 `WriteTests.yml`）→ E-5.6 / D-2 触发面已通
  - 过程：首次失败 run `36828224317`（Windows `/tmp`）；`0d41c40` 改为 `__TEST_DIR__` 后重跑全绿
  - **附带观察（供 D-2a）**：本 PR 上 `windows_amd64_mingw` 已 success（~40m），正式 D-2a 仍以 D-2 `workflow_dispatch` 为准
  - **未做**：不改 visibility、不打 tag、不启动 D-4

---

### D-4 👤 在 `main` 上打 tag `v0.1.0` 并推送

- **前置**：D-3。
- **操作**：

  ```sh
  git checkout main && git pull --ff-only
  git tag -a v0.1.0 -m "feishu_bitable v0.1.0"
  git push origin v0.1.0
  ```

- **DoD**：
  1. `git rev-parse v0.1.0^{commit}` 输出 40 位十六进制 SHA。
  2. `gh api repos/tshelianthus/duckdb-feishu-bitable/git/refs/tags/v0.1.0 --jq '.object.sha'` 有输出。
  3. 该 SHA == `git rev-parse origin/main`（tag 就是当时的 main 顶点）。

---

### D-4a 👤 确认 tag 指向的 SHA 上 CI 全绿

- **前置**：D-4。
- **背景**：社区 CI 构建的正是 `repo.ref` 指向的这个 SHA；**该 SHA 上挂 = 社区 PR 必挂**。
  `docs/RELEASE_CHECKLIST.md` L226 有这句要求，但原看板没有对应任务与可判定 DoD，故拆出。
- **操作**：若 D-2b 已落地，push tag 会自动触发；否则
  `gh workflow run MainDistributionPipeline.yml --ref v0.1.0`。
- **DoD**：
  `gh run list --workflow=MainDistributionPipeline.yml --limit 20 --json headSha,conclusion --jq '[.[] | select(.headSha=="'"$(git rev-parse v0.1.0^{commit})"'")] | .[0].conclusion'`
  输出 `success`。

---

### D-5 👤 回填 `docs/community/description.yml` 的 `repo.ref`，单独 PR 合入 `main`

- **前置**：D-4a。
- **操作**：把 `git rev-parse v0.1.0^{commit}` 的完整 40 位 SHA 填入 L28，替换
  `REPLACE_WITH_FULL_COMMIT_SHA_OF_TAG_v0.1.0`；同时删除 L26-27 的 PLACEHOLDER 注释。
- **社区实践依据（Q5）**：已收录的 `gsheets` 描述符
  `version: 0.0.9` / `ref: a209d8a42c8e35cd571467dd7ae2eb65fcb96f75`，
  而 `refs/tags/v0.0.9 -> a209d8a42c8e35cd571467dd7ae2eb65fcb96f75` —— **完全相同**。
  即「先打 tag，再把 tag 的 SHA 填进描述符」是社区通行做法，D-4 → D-5 的现有顺序**正确，不改**。
- **必须写进任务、执行人容易误解的一点**：tag `v0.1.0` **不会**包含回填后的 `repo.ref`，
  这是不可避免的先有鸡后有蛋，且**无害**——描述符不参与扩展构建，
  `duckdb/community-extensions/scripts/build.py` 只把 `repo.ref` 原样传给 checkout。
  **不要 `git tag -f`，不要重新打 tag**（`docs/RELEASE_CHECKLIST.md` L249-250 的表述正确，保留）。
- **DoD**：
  1. `grep -c "REPLACE_WITH_" docs/community/description.yml` 输出 `0`。
  2. 下面这段打印 `OK`：
     ```sh
     python3 -c "import yaml,re,subprocess;d=yaml.safe_load(open('docs/community/description.yml'));r=str(d['repo']['ref']);assert re.fullmatch(r'[0-9a-f]{40}',r),r;assert r==subprocess.check_output(['git','rev-parse','v0.1.0^{commit}']).decode().strip();assert d['extension']['version']=='0.1.0';print('OK')"
     ```
  3. 该回填 PR 已合入 `main`（此后 `git rev-parse v0.1.0^{commit}` 不再等于 `origin/main`，属预期）。

---

### D-6 👤 向 `duckdb/community-extensions` 提交收录 PR

- **前置**：D-5。
- **操作**：
  1. Fork `duckdb/community-extensions`，从 `main` 开分支。
  2. 新建**且只新建**一个文件 `extensions/feishu_bitable/description.yml`，
     内容 = 本仓库回填后的 `docs/community/description.yml` **删去 L1-9 的仓库内部注释**后的版本。
  3. 提 PR，标题 `Add feishu_bitable extension`；正文说明：用途、只读定位、依赖 OpenSSL、
     排除 wasm 平台的原因（wasm 沙箱无原生 TLS 套接字）、以及 D-2a 决定的其他排除项（若有）。
- **本次调研新增的硬约束（原清单未写）**：
  - `scripts/build.py` 校验**目录名必须等于 `extension.name`**
    （`Directory name '<dir>' does not match extension name '<name>'`）。
  - `scripts/build.py` **拒绝一个 PR 改动多个描述符**
    （`cannot have multiple descriptors changed or packages with spaces in their names`）。
    因此该 PR **不得**顺带改 README / 加别的扩展 / 改任何其他文件。
  - 社区 CI 的 `prepare` job 用 `tj-actions/changed-files@v46` 识别改动的描述符，
    再以 `duckdb_version: v1.5.5` + `ci_tools_version: v1.5-variegata` 构建 ——
    与本项目 `.github/workflows/MainDistributionPipeline.yml` L23-24 的 pin **完全一致**，
    不需要升级版本，这是一个有利的时间窗口。
- **DoD**：
  1. `git show --stat` 在该 PR 分支上只有 **1 个新增文件**，路径为 `extensions/feishu_bitable/description.yml`。
  2. 本地预检通过：
     ```sh
     cd <fork> && ALL_CHANGED_FILES=extensions/feishu_bitable/description.yml \
       DUCKDB_VERSION=v1.5.5 DUCKDB_LATEST_STABLE=v1.5.5 python3 scripts/build.py && cat env.sh
     ```
     —— 无异常退出，且 `env.sh` 中 `COMMUNITY_EXTENSION_REF` 为 D-5 的 40 位 SHA。
  3. 该 PR 上 `Community Extension Build` workflow 的 conclusion 为 `success`，
     且 job 列表中**无** `wasm_*`（以及 D-2a 决定排除的其他平台）。
  4. 维护者合并（`gh pr view --json mergedAt` 非空）。

---

### D-7 👤 收录合并后：干净环境 INSTALL / LOAD 验证

- **前置**：D-6 合并，且社区侧完成一次部署。
- **操作**：在一台**未安装过本扩展**的机器（或全新的 DuckDB home）上：

  ```sh
  duckdb -c "INSTALL feishu_bitable FROM community; LOAD feishu_bitable;
             SELECT function_name FROM duckdb_functions()
             WHERE function_name LIKE '%bitable%' ORDER BY 1;"
  ```

- **变更说明**：原 D-6 把「社区 CI 绿 + 维护者合并 + 干净环境可用」三件事塞进一个 DoD。
  前两件由 D-6 判定，第三件依赖社区侧部署、**存在时间差**，必须单独成条，
  否则 D-6 会长期停在「部分完成」。
- **DoD**：
  1. 上述命令退出码 0。
  2. 输出至少包含 `feishu_bitable_fields`、`feishu_bitable_scan`、`feishu_bitable_tables`、
     `read_bitable`、`read_feishu_bitable`。
  3. `SELECT * FROM duckdb_extensions() WHERE extension_name='feishu_bitable'` 的
     `install_mode` 为 `REPOSITORY`（而非 `UNSIGNED` / 本地路径），证明装的确实是社区仓库的产物。
  4. 该 DuckDB 版本为 v1.5.5（与社区构建的 stable 一致）。

---

### D-7a 🤖 + 👤 发布后收尾：Release 页面与文档状态

- **前置**：D-7。
- **要求**：
  1. 👤 基于 tag `v0.1.0` 创建 GitHub Release（`gh release create v0.1.0 --title ... --notes ...`），
     变更说明可由 `git log --oneline origin/main` 生成。
  2. 🤖 `README.md` L246 起的「开发状态」段：把 L252 的「社区仓库发布准备」改为已发布，
     并给出 `INSTALL feishu_bitable FROM community;` 的一行安装说明。
  3. 🤖 `docs/RELEASE_CHECKLIST.md` §9 的三条勾选项打勾，并补一句：
     后续版本必须「先改 `extension.version` → 新 tag → 新 SHA → 再提一次描述符 PR」
     （依据 `duckdb/community-extensions/UPDATING.md`：描述符更新即触发重建与发布）。
  4. 🤖 `.specs/05_TASKS.md` 顶部状态表的「Phase 6 社区发布」改为已完成。
- **DoD**：
  1. `gh release view v0.1.0 --json tagName --jq '.tagName'` 输出 `v0.1.0`。
  2. `grep -n "发布准备" README.md` 无输出。
  3. `grep -n "INSTALL feishu_bitable FROM community" README.md` 有输出。

---

### D-8 🤖 建立 `duckdb-next-build` 兼容性哨兵（发布后，不阻塞 v0.1.0）

- **前置**：D-7a。
- **背景**：`duckdb/community-extensions/UPDATING.md` 把两个 job 当作维护者的标准配置来讲解 ——
  `duckdb-stable-build`（对 stable，社区发布用）与
  `duckdb-next-build`（`duckdb_version: main` / `ci_tools_version: main`，**纯粹用于提前发现
  下一个 DuckDB 版本的 API 破坏**）。本项目**只有前者**
  （`.github/workflows/MainDistributionPipeline.yml` L18-30）。
  `gsheets` 与 `duckdb-avro` 都配了 next build。没有它，DuckDB 发新版时我们只能被动发现扩展挂掉。
- **要求**：新增 `duckdb-next-build` job，`duckdb_version: main`、`ci_tools_version: main`、
  `exclude_archs` 与 stable job 保持一致；并加 `continue-on-error: true`
  或拆成独立 workflow + `schedule`（参照 `Query-farm/airport` 的 `schedule: cron 11 3 * * 1-5`），
  **不得让它阻塞 PR 合并**——它是预警而非门禁。
- **DoD**：
  1. `python3 -c "import yaml;d=yaml.safe_load(open('.github/workflows/MainDistributionPipeline.yml'));print(list(d['jobs']))"`
     输出中含 `duckdb-next-build`。
  2. 在一次 dispatch 中该 job 真实执行过（`gh run view <id> --json jobs` 可见）。
  3. 该 job 失败时 PR 的 required checks 仍可通过（用 `gh pr checks` 的 `state` 字段核对）。

---

---

## [ ] 批次 E：Phase 7 回写能力（COPY，v0.2 起）

> **执行角色**：🤖 = agent 或 CI 可自动完成；👤 = 必须人工执行（真实写入凭据、独立写入 Base、GitHub 网页/受保护环境操作）。
> **门禁隔离（2026-09-11 经用户决策修订）**：原「E-3.x 及之后的实现类任务在 D-6 之前一律不得启动」
> 已由用户决策解除（决策内容：本次先做出**可用但不发布**的回写能力，即范围 B）。
> 因此 E-3.x 起可直接在 `dev` 上实现；**但发布链（D-6 社区收录 PR、E-6.3 打 v0.2.0 tag）仍然不做**。
> E-2.x 本就是纯文档 / 纯实测任务，可与批次 A–D 并行推进。
> 所有写入行为必须显式触发，禁止首表回退与隐式回写（依据 `.specs/03_API_CONTRACT.md` §8）。
> E-1 已完成，不在本清单内；下方 25 条替换原 E-2 ~ E-6。

### E-2 系列：冻结写入契约（8 条，可先于 D-6 执行）

- [x] **E-2.1** 🤖 冻结 COPY 目标语法、FORMAT 名称与被禁用的通用写选项
  - **前置**：E-1（已完成）
  - **内容**：改写 `.specs/03_API_CONTRACT.md` §8，把标题从"（规划中，尚未注册）"改为正式契约。必须定下：
    (a) FORMAT 名称固定为 `feishu_bitable`，且**必须显式书写** `(FORMAT feishu_bitable)`——不做文件扩展名自动推断；
    (b) 目标字符串接受两种形态：飞书/Lark 多维表格浏览器 URL，或裸 `app_token`（与读侧 §4.1/§4.2 完全对称，
        也与社区先例 gsheets 的 `COPY <t> TO '<URL>' (FORMAT gsheet)` 一致）；
    (c) 明确 `COPY <table> TO ...` 与 `COPY (SELECT ...) TO ...` 两种上游都支持；
    (d) 逐条列出**必须在 Bind 阶段拒绝**的 DuckDB 通用写选项及其理由：`partition_by`、`per_thread_output`、
        `file_size_bytes`（三者会让 `PhysicalCopyToFile::GetGlobalSinkState` 走 `fs.CreateDirectory(file_path)` 分支）、
        `filename_pattern`、`file_extension`、`use_tmp_file`、`overwrite` / `overwrite_or_ignore` / `append`、
        `return_files`、`return_stats`、`write_partition_columns`、`hive_file_pattern`；
        **实证修正（2026-09-11）**：`Binder::BindCopyTo`（`bind_copy.cpp` L125–201）会在扩展
        `copy_to_bind` 之前就地消费这些选项，`CopyInfo` 也不携带它们，故**扩展无法逐个在 Bind 阶段拒绝**。
        契约 §8.7 已改写为「不支持范围 + 拦截边界」：`file_extension` 可检测并拒绝，其余为文档级禁止；
    (e) 明确 Replacement Scan 不参与写入，读侧 URL 出现在 `FROM` 与出现在 `COPY ... TO` 是两条独立路径。
  - **DoD**：`grep -n "规划中" .specs/03_API_CONTRACT.md` 无输出；(d) 中每个选项都配有确定的异常类型与消息模板，
    可在 E-5.1 逐条写成 `.test` 断言。
  - **完成证据（2026-09-11）**：`.specs/03_API_CONTRACT.md` §8 已改写为正式契约——§8.1 冻结目标语法、两种上游与 URL/裸 token 形态，§8.5(e)/§8.7 明确 Replacement Scan 不参与写入、列出 11 个被拒绝的通用写选项；`grep -n "规划中" .specs/03_API_CONTRACT.md` 无输出；§8.7 给出统一的 `InvalidInputException` 消息模板，E-5.1 可逐条断言。

- [x] **E-2.2** 🤖 冻结 COPY 选项集合、LogicalType、CopyOptionMode 与默认值
  - **前置**：E-2.1
  - **内容**：定义 `copy_options` 要注册的选项表，每项五栏（名称 / LogicalType / CopyOptionMode / 默认值 / 缺省时的错误）。
    建议集合：`table_id`(VARCHAR, WRITE_ONLY, 无默认, URL 未携带 `table` 时必填)、
    `secret`(VARCHAR, WRITE_ONLY, 默认空串=按 DuckDB Secret 解析规则取默认 feishu secret)、
    `batch_size`(BIGINT, WRITE_ONLY, 默认值待 E-2.7 冻结)。
    必须写明：选项名大小写不敏感（DuckDB 用 `case_insensitive_map_t`）；未知选项由 `copy_options` 自动产生
    `Unrecognized option "<x>" for feishu_bitable` 并附候选建议；**不提供 `view_id`**（视图对追加写入无意义）。
  - **DoD**：选项表五栏齐全；表中每个默认值都能在 E-3.1 用一条 `.test` 断言；
    与 `.specs/03_API_CONTRACT.md` §1 的 Secret 两种模式描述不冲突。
  - **完成证据（2026-09-11）**：`§8.2` 给出五栏选项表（`table_id` / `secret` / `batch_size`），三者 `CopyOptionMode` 均为 `WRITE_ONLY`；写明选项名大小写不敏感、未知选项由 DuckDB 机制报错、明确不提供 `view_id`。

- [x] **E-2.3** 🤖 冻结写侧类型映射表与 NULL / 空值 / 时区规则
  - **前置**：E-2.1
  - **内容**：写入本文件 §1.4 的标量映射表（Text/Barcode/Phone/URL/Email ← VARCHAR；
    Number/Progress/Currency/Rating ← DOUBLE 及可无损转换的整数类型；Checkbox ← BOOLEAN；
    DateTime ← TIMESTAMP/TIMESTAMPTZ；SingleSelect ← VARCHAR；MultiSelect ← LIST(VARCHAR)）。
    并为下列每一种输入定下**唯一**行为（写空值 / 省略该字段 / 报错，三选一，不允许"视情况而定"）：
    SQL NULL、空字符串、空 LIST、含 NULL 元素的 LIST、NaN / ±Infinity、超出飞书毫秒时间戳范围的 TIMESTAMP、
    不在映射表中的 DuckDB 类型（如 STRUCT / MAP / BLOB / INTERVAL）。
    时区规则必须收敛到一条确定语句（例如"`TIMESTAMP` 按 UTC 解释后转毫秒 epoch；`TIMESTAMPTZ` 先归一化到 UTC"），
    不得留"取决于会话 TimeZone 设置"这类未定义表述。
  - **DoD**：映射表每行有源类型、目标类型、超范围/不可转换时的行为三列；
    上面列出的 7 种特殊输入在契约中各有一句确定结论；E-3.3 能据此逐条写 Catch2 用例而无需再做判断。
  - **完成证据（2026-09-11）**：`§8.3` 映射表每行含「接受的源类型 + 越界/不可转换行为」；`§8.4` 对 7 类特殊输入（NULL / 空串 / 空 LIST / 含 NULL 的 LIST / NaN·Inf / 超范围 TIMESTAMP / 不可映射类型）各给唯一结论；时区规则收敛为「`TIMESTAMP` 按 UTC 解释、`TIMESTAMPTZ` 先归一化到 UTC，不取决于会话 TimeZone」。

- [x] **E-2.4** 🤖 冻结只读字段与 MVP 不支持字段的清单及 Bind 拒绝行为
  - **前置**：E-2.3
  - **内容**：契约中分两张表列出（措辞必须区分，不能混为"不支持"）：
    (a) **结构上只读**：`FORMULA`(20)、`CREATED_TIME`(1001)、`MODIFIED_TIME`(1002)、`CREATED_USER`(1003)、
        `MODIFIED_USER`(1004)、`AUTO_NUMBER`(1005)——永远不可作为 COPY 目标列；
    (b) **可写但 MVP 不做**（需要外部标识符）：`USER`(11)、`ATTACHMENT`(17)、`LINK`(18)、`DUPLEX_LINK`(21)、
        `LOCATION`(22)、`GROUP_CHAT`(23)；
    (c) **未知 type id 的默认动作是拒绝**（与读侧"未知类型降级为 JSON"相反，必须在契约中显式说明这个不对称
        及其理由：读侧降级不会造成副作用，写侧降级会造成错误的远端写入）。
  - **DoD**：三类各有一条 Bind 期错误消息模板，模板中包含字段名与 type id；
    模板不包含凭据；清单覆盖 `src/include/bitable/field_type.hpp` 中枚举的全部 type id，无遗漏。
  - **完成证据（2026-09-11）**：`§8.5` 分三类列出——(a) 6 个结构只读 id、(b) 6 个可写但 MVP 不做 id、(c) 未知 id 一律拒绝（并说明与读侧降级 JSON 的不对称理由）；三类各配含字段名与 type id、不含凭据的错误模板；清单覆盖 `src/include/bitable/field_type.hpp` 全部 25 个 type id。

- [x] **E-2.5** 🤖 冻结 `COPY` 返回值语义与"MVP 不做"清单
  - **前置**：E-2.1
  - **内容**：契约必须写明——`COPY` 返回 **单列 `Count BIGINT`**；该列由 DuckDB 在
    `PhysicalCopyToFile::Sink` / `Combine` 中自行累加（`l.total_rows_copied += chunk.size()`），
    **扩展无法改写、也无法新增任何额外统计列**（v1.5.5 的 `CopyFunctionReturnType` 只有三种固定形状）。
    因此语义定为"提交给写入算子的行数"，且只在整条 `COPY` 成功时可见；任何失败一律抛异常，不返回部分计数。
    "不做"清单：`INSERT` / `UPDATE` / `DELETE`、覆盖写、自动建表、自动扩展字段、写入侧首表回退、
    `batch_update`（v0.2 只做 append-only 的 `batch_create`）、跨批次事务性。
  - **DoD**：`.specs/03_API_CONTRACT.md` §8 含 Count 口径说明；
    原 E-2 中关于 `COPY` 返回值"成功 / 失败双计数"的旧措辞已被替换为单一 `Count`（已提交给写入算子的行数）口径；`.specs/` 全文不再出现旧措辞。
  - **完成证据（2026-09-11）**：`§8.6` 写明 `COPY` 返回单列 `Count BIGINT`、由 DuckDB 在 Sink/Combine 累加、扩展不可改写也不可新增统计列（v1.5.5 仅三种返回形状），语义为「提交给写入算子的行数」且仅在整条成功时可见；旧口径措辞已从 `.specs/` 全部清除，无残留命中。

- [x] **E-2.6** 👤 写专用 Base 的 API 事实实测（G2：应用支路拒绝码等级 A / 正向对照等级 B，机制解释有条件成立）
  - **缺口标签**：G1 = 单批写入上限到底是 500 还是更高（见 `.specs/06_WRITEBACK_RESEARCH.md` §1.2）；
    G2 = 除 scope 外是否还必须有资源级协作者权限（见同文件 §1.1）。
  - **前置**：E-2.1；需要**独立于读取回归 Base** 的写入 Base，以及一套具备编辑权限的主写入身份
    （应用凭据或用户凭据任选其一；建议两种模式都测）。运行脚本：`scripts/e2_6_api_facts.sh`。
    所需进程环境变量见该脚本 `usage`：
    `FEISHU_E26_APP_TOKEN`、`FEISHU_E26_TABLE_ID`、`FEISHU_E26_WRITABLE_FIELD`、
    `FEISHU_E26_READONLY_FIELD`，以及主写入身份二选一
    (`FEISHU_E26_WRITE_APP_ID` + `FEISHU_E26_WRITE_APP_SECRET` 或 `FEISHU_E26_WRITE_USER_ACCESS_TOKEN`)；
    可选：`FEISHU_E26_READONLY_APP_ID`/`FEISHU_E26_READONLY_APP_SECRET`、
    `FEISHU_E26_NONCOLLAB_APP_ID`/`FEISHU_E26_NONCOLLAB_APP_SECRET`、
    `FEISHU_E26_NONCOLLAB_USER_ACCESS_TOKEN`。
  - **内容**：用 `curl` 或等价工具（**不写扩展代码**）对写入 Base 直接打端点，记录每次的 HTTP 状态、
    业务 `code`、`msg`（脱敏）：
    (1) `batch_create` 提交 500 / 501 条；
    (2) `batch_update` 提交 500 / 501 / 1000 / 1001 条 → 消解 G1；
    (3) 只开通 `bitable:app:readonly` 的应用凭据写入 → 记录拒绝码；
    (4) 应用/用户身份**不是**该 Base 协作者时写入 → 记录拒绝码 → 消解 G2；
    (5) 向只读字段（公式 / 自动编号 / 创建时间）提交值 → 记录拒绝码，并确认是否整批失败。
  - **DoD**：五组结果各有 HTTP 状态 + code + 脱敏 msg；结论以新章节写入 `.specs/06_WRITEBACK_RESEARCH.md`，
    §1.1 与 §1.2 中对应的"需实测"标记被替换为实测结论；本次创建的测试记录已删除。
  - **部分完成证据（2026-09-29）**：(1)(2)(5) 已写入 `.specs/06_WRITEBACK_RESEARCH.md` §1.5——G1 消解为
    `batch_update` 硬上限 1000（1001 → `1254104 RecordAddOnceExceedLimit`）；`batch_create` 501 实测成功但契约仍冻 500；
    公式字段 update 返回 `code=0`（未拒绝）。探测记录已 `batch_delete` 清零。(3)(4) 因未提供可选只读/非协作者身份仍 SKIPPED，故父项不勾选。
  - **部分完成证据（2026-09-30 复跑）**：主写入身份由 `user_access_token` 改为**应用身份**
    （`FEISHU_E26_WRITE_APP_ID` + `FEISHU_E26_WRITE_APP_SECRET`），`write_auth` 换 token `code=0` 后 (1)(2)(5) 结论不变；
    **(3) 已闭环**——只读 scope 应用可换到 tenant token（`readonly_app_auth code=0`）但 `batch_create` 被拒：
    HTTP 400 / `99991672` / `Access denied. One of the following scopes is required: [bitable:app, base:record:create]`，
    证明 scope 校验先于资源 ACL。**(4) 当时仍 SKIPPED**（`FEISHU_E26_NONCOLLAB_*` 未注入）。探测记录已 `batch_delete` 清零（2002 条）。
  - **完成证据（2026-09-30，负例清零）**：非协作者身份按 §7 方法由 agent 经 `lark.registerApp()` 自建应用、人工扫码授权取得
    （`FEISHU_E26_NONCOLLAB_APP_ID` + `FEISHU_E26_NONCOLLAB_APP_SECRET`，**有 `bitable:app` 写 scope、未加入该 Base 协作**）。
    先做正向对照——该应用对自建 Base 的建表与 `batch_create` 均 `code=0`，证明写 scope 生效；随后对专用写入 Base：
    `noncollab_app_auth` 200/0，**`noncollab_app_write` → HTTP 403 / `91403` / `Forbidden`** → 应用支路拒绝码已记录（证据等级 A，`403 / 91403`，脚本 `scripts/e2_6_api_facts.sh` 可复现）；「scope 已生效」正向对照与读端点成功为仓外一次性探测（证据等级 B），机制解释有条件成立（接受等级 B 时写入需资源级协作者权限）；用户身份支路未复测。
    同 token 对该 Base 的 `apps` 元信息、`tables`、`fields`、`records` 各类**读**请求均为 `200 / code=0`，即该约束只作用于写入。
    唯一未跑项为用户身份支路 `noncollab_user_write`（可选加强，不影响 DoD）。探测记录已 `batch_delete` 清零（2002 条）。
  - **DoD 达成**：五组 (1)(2)(3)(4)(5) 各有 HTTP 状态 + code + 脱敏 msg，结论已写入 `.specs/06_WRITEBACK_RESEARCH.md`
    §1.1 / §1.2 / §1.5 与 §6 台账；自建应用方法沉淀于同文件 §7。

- [x] **E-2.7** 🤖 回填实测结论并冻结批次上限
  - **前置**：E-2.2、E-2.6
  - **内容**：把 E-2.6 的结论回填进 `batch_size` 的默认值与硬上限，并注明来源属于"端点契约"还是"防御性选择"。
    **若 E-2.6 未完成就必须推进**：按 500 冻结，并在契约中显式标注为防御性选择而非端点契约（依据
    `.specs/06_WRITEBACK_RESEARCH.md` §1.2）。同时写明客户端对 `batch_size` 的上下界校验（下界 1，上界 = 冻结值）。
  - **DoD**：契约中的 `batch_size` 默认值与上限是确定数字且带来源标注；
    `.specs/06_WRITEBACK_RESEARCH.md` §1.2 不再存在未决的 500 / 1,000 冲突。
  - **完成证据（2026-09-11）**：`§8.2` 冻结 `batch_size` 默认 500 / 下界 1 / 上界 500，来源标注为 `batch_create` 端点契约（非防御性选择）；`.specs/06_WRITEBACK_RESEARCH.md` §1.2 已回填结论，500 / 1,000 冲突标注为不阻塞 v0.2、留待 v0.3 实测消解。

- [x] **E-2.8** 🤖 修正三份规范中已被本轮核实证伪的表述
  - **前置**：无（可与 E-2.1 并行）
  - **内容**：
    (a) `.specs/06_WRITEBACK_RESEARCH.md` §2.3 的"未找到已被社区收录、有可核验源码的 C++ `CopyFunction`
        向非文件型 HTTP/SaaS records API 写入的先例"是错的 → 改为 `evidence-dev/duckdb_gsheets`
        （`src/gsheets_copy.cpp`、`src/include/gsheets_copy.hpp`；收录见 `duckdb/community-extensions`
        的 `extensions/gsheets/description.yml`），并注明其头文件自述改编自 `duckdb/postgres_scanner`
        的 `src/include/postgres_binary_copy.hpp`；
    (b) `.specs/04_TESTING_SPEC.md` §6 与本体 E-5 中把两种情形混为一谈的措辞与 §1.3 的
        "全部成功或全部失败"结论自相矛盾 → 改为"整批失败不产生副作用"与"跨批次部分完成"两个不同概念；
    (c) `.specs/02_ARCHITECTURE.md` §2.6 补两条硬约束：写入算子固定 `REGULAR_COPY_TO_FILE`（单写者）；
        `write_empty_file` 默认 true 会让 0 行查询也走 global init，因此"0 行 ⇒ 零远端请求"必须由扩展自己保证。
  - **DoD**：`.specs/` 各文件已把「整批原子失败」与「跨批次部分完成」拆成两个互不等价的概念；§2.3 的新结论带仓库路径与文件名，可被一次点击核验。
  - **完成证据（2026-09-11）**：06 §2.3 重写为 gsheets 先例（含 `description.yml` v0.0.9 / `repo.ref` / `src/gsheets_copy.cpp` / 注册面 / 与 sshfs 的形态区分）；02 §2.6 补入 `REGULAR_COPY_TO_FILE` 单写者与 `write_empty_file` 两条硬约束；该旧表述已从 `.specs/` 全部清除，无残留命中。

### E-3 系列：写入核心实现（5 条；范围 B 已解除 D-6 前置）

- [x] **E-3.1** 🤖 注册 `CopyFunction` 骨架：格式名、选项校验、执行模式、可载入占位
  - **前置**：E-2.2、E-2.5（原 D-6 前置已由范围 B 决策解除）
  - **内容**：新增 `src/include/bitable/copy_function.hpp` 与 `src/bitable/copy_function.cpp`，
    照社区先例的结构写 `class FeishuBitableCopyFunction : public CopyFunction`，构造函数
    `CopyFunction("feishu_bitable")` 并赋值回调（gsheets / postgres_scanner 同构）。本条只实现：
    `copy_options`（注册 E-2.2 的选项表）、`execution_mode`（**恒返回 `CopyFunctionExecutionMode::REGULAR_COPY_TO_FILE`**）、
    `copy_to_initialize_local`（返回空 `LocalFunctionData`，**必须提供**——`PhysicalCopyToFile::GetLocalSinkState`
    无条件调用它）、`copy_to_bind`（只做选项存在性/取值域校验与 E-2.1(d) 的禁用选项拒绝）、
    `copy_to_sink`（暂抛 `ThrowFeishuError("... is not implemented yet")`，符合 `AGENTS.md` 的 Loadable Every Commit）。
    在 `src/feishu_bitable_extension.cpp` 的 `LoadInternal` 中 `loader.RegisterFunction(...)`；`CMakeLists.txt` 加源文件。
    **不要设置 `function.extension`**（本扩展无文件语义）。
  - **DoD**：`make debug && make test_debug && make format-check` 全绿；
    ~~原 DoD 要求 `duckdb_copy_functions()` 断言~~ → **修正（2026-09-11）**：DuckDB v1.5.5 既无 `duckdb_copy_functions()` 表函数、`duckdb_functions()` 也不列 copy function，故注册校验改为行为断言（合法 `COPY` 必须到达扩展 sink，而非报 `COPY TO is not supported for FORMAT`）。
    一条新 `.test` 断言未知选项报 `Unrecognized option`；
    代码核对 `execution_mode` 回调存在且只有一条 `return CopyFunctionExecutionMode::REGULAR_COPY_TO_FILE;`。
  - **完成证据（2026-09-11）**：新增 `src/include/bitable/copy_function.hpp` 与 `src/bitable/copy_function.cpp`，注册 `CopyFunction("feishu_bitable")` 并接线 `copy_options`（§8.2 三个 WRITE_ONLY 选项）、`copy_to_bind`、`copy_to_initialize_global`（零 I/O）、`copy_to_initialize_local`、`copy_to_combine`、`copy_to_finalize`、`execution_mode`（恒返回 `REGULAR_COPY_TO_FILE`）；E-3.1 验收时 `copy_to_sink` 以 `not implemented yet` 保持可加载，现已由 E-3.4 的批次实现替代。`LoadInternal` 已加 `feishu::RegisterCopyFunction(loader)`，`CMakeLists.txt` 已加源文件，未设置 `function.extension`。当时 `make debug`、`make format-check` 与 12 个离线 SQL 文件（196 assertions / 2 个在线文件整文件跳过）全部退出码 0；新增 `test/sql/copy_writeback.test`（5 assertions：注册行为、未知选项、batch_size 上下界、file_extension 拒绝）。

- [x] **E-3.2** 🤖 Bind 阶段的目标解析、凭据解析与字段校验
  - **前置**：E-3.1、E-2.4
  - **内容**：在 `copy_to_bind` 中——从 `input.info.file_path` 解析目标（复用 `src/bitable/url_parser.cpp`
    的既有解析入口，不得复制一份新的 URL 解析逻辑）；显式 `table_id` 选项优先于 URL 中的 `table`；
    **未解析出 `table_id` 时直接报错，禁止首表回退**（`.specs/02_ARCHITECTURE.md` §2.6 硬约束）；
    复用 `GetFeishuCredentials` 解析 Secret；调用字段元数据接口按**字段名**匹配上游列名；
    按 E-2.4 拒绝：字段不存在、只读字段、MVP 不支持类型、未知 type id、上游重复列名。
  - **DoD**：离线 `.test` 覆盖"缺 `table_id`"、"无 Secret"、"非法 URL"三类 Bind 期错误，
    且加上 `LIMIT 0` / `WHERE false` 仍然报错（Bind 不可被绕过）；
    所有错误消息以 `feishu_bitable` 前缀开头且不含 app_secret / token。
  - **完成证据（2026-09-12）**：`copy_to_bind` 复用 `ResolveBitableTarget`，禁止写侧首表回退，解析 `TYPE feishu` Secret，处理 Wiki 目标，并通过字段元数据按列名拒绝缺失、重复、只读、MVP 不支持、未知类型及不兼容源类型；`test/sql/copy_writeback.test` 覆盖缺 `table_id`、0 行上游仍 Bind、非法 URL 与无可用 Secret，错误均在写请求前产生。

- [x] **E-3.3** 🤖 自包含 TU：单行值 → Feishu `fields` JSON 的编码
  - **前置**：E-2.3、E-2.4
  - **内容**：新增 `src/bitable/write_value_encoding.cpp` 与对应头文件，与读侧 `value_coercion.cpp` 对称。
    **该 TU 必须自包含**：只依赖 std、yyjson 与 DuckDB 异常类型，异常消息用字符串拼接（不得用 printf 风格格式化），
    不得 `#include "duckdb.hpp"`——否则无法编进 `test/unit`（见 `test/unit/CMakeLists.txt` 的注释与
    `.specs/04_TESTING_SPEC.md` 的分层约束）。输入是已从 `Vector` 解包出的标量表示，输出是 compact JSON 文本。
  - **DoD**：把该 TU 加入 `test/unit/CMakeLists.txt` 后 `make test_unit` 全绿；
    Catch2 覆盖 E-2.3 映射表每一行 + 7 种特殊输入（NULL / 空串 / 空 LIST / 含 NULL 的 LIST / NaN / ±Inf /
    超范围 TIMESTAMP）各至少一条断言；`grep -n "duckdb.hpp" src/bitable/write_value_encoding.cpp` 无输出。
  - **完成证据（2026-09-12）**：新增自包含的 `write_value_encoding` TU 并接入独立 unit target；测试覆盖全部已发布字段 type id、SQL NULL 省略、空串/空 LIST、LIST 内 NULL、NaN/Inf、整数无损转换、时间上下界、JSON 转义与 URL 的 `link`/`text` wire object；实现未包含 `duckdb.hpp`。

- [x] **E-3.4** 🤖 批次累加器与 `sink` / `finalize` 生命周期接线
  - **前置**：E-3.1、E-3.3、E-2.7
  - **内容**：把"累积行 → 达到 `batch_size` 就产出一个待提交批次 → 末尾冲刷尾批"这段**纯逻辑**
    放进 E-3.3 的自包含 TU（便于 Catch2 断言切分边界）；在 `copy_to_sink` 中 `input.Flatten()` 后逐行喂入，
    在 `copy_to_finalize` 中冲刷尾批。
    **关键约束（一手源码核实，实施时不得改动）**：
    (1) 缓冲状态必须放在 **global state**，不能只放 local state——`PhysicalCopyToFile::Combine` 在
        `l.total_rows_copied == 0` 时直接 `return SinkCombineResultType::FINISHED`，**不会调用 `copy_to_combine`**；
        而 `copy_to_finalize` 只要 `gstate.global_state` 存在就一定被调用；
    (2) `write_empty_file` 默认 true ⇒ 0 行查询也会调用 `copy_to_initialize_global`，
        所以 **global init 里不得发任何 HTTP 请求**，0 行时 finalize 必须直接返回；
    (3) 执行模式已固定为单写者，但仍需对 global 缓冲加锁，避免未来改动引入数据竞争。
  - **DoD**：`make test_unit` 中新增切分边界断言，覆盖行数 = `batch_size-1` / `batch_size` /
    `batch_size+1` / `2*batch_size` / `0`，断言产出的批次数与每批行数；
    代码核对 `copy_to_initialize_global` 中不存在任何 `ApiClient` 调用。
  - **完成证据（2026-09-11）**：`WriteBatchAccumulator`（AddRow / Flush / RowsSeen）与 `WriteBatch` 落在同一自包含 TU；单测覆盖 `batch_size-1` / `batch_size` / `batch_size+1` / `2*batch_size` / `0` 的切分与行数；`copy_to_sink` 先 `input.Flatten()` 再逐行喂入累加器，`copy_to_finalize` 冲刷尾批；缓冲状态在 `CopyGlobalState` 并持 `std::mutex`；`copy_to_initialize_global` 只构造 global state、不含任何 `ApiClient` 调用（已 grep 核对）。
  - **完成证据（2026-09-12）**：新增自包含 TU `src/include/bitable/write_state_machine.hpp` + `src/common/write_state_machine.cpp`，`RunBatchWithRetries` 以注入的 `WriteAttemptFn` 驱动：仅 `CONFIRMED` 推进 `WriteProgress`；`RETRY_SAFE` 在有界 `max_attempts` 内重试（`on_retry` 挂钩，配 `RetryPolicy` / `Retry-After`）；`FAIL` 抛 `InvalidInputException`、`UNKNOWN_RESULT` 抛 `IOException` 且**立即返回、绝不再调用传输层**。`test/unit/common/test_write_state_machine.cpp` 以 7 个 TEST_CASE 断言：确认后进度推进且只调用一次、结果未知名下调用次数恰为 1、确定失败调用次数恰为 1、安全重试恰为 `max_attempts` 次且有 `max_attempts-1` 次退避、消息含 `batch #4`/`rows 1501-1750` 与已确认批次数/行数；`copy_to_sink` / `copy_to_finalize` 已改为经该状态机提交。

- [x] **E-3.5** 🤖 `batch_create` 请求发送、响应核对与错误归一化
  - **前置**：E-3.4
  - **内容**：复用 `ApiClient::PostJson`、`TokenProvider` / `TokenCache`（含 `USER_ACCESS_TOKEN` 模式直接
    Bearer 的分支）、`TryParseFeishuError` / `ThrowFeishuApiError` / `ThrowHttpError` / `RedactSecretValue`。
    响应必须核对 `data.records` 的条数与本批提交条数一致，不一致按错误处理（不得假定一致）。
    **错误消息必须能区分"确定失败"与"已提交但结果未知"两类**，不得沿用只读路径的错误语义
    （`.specs/02_ARCHITECTURE.md` §2.6 明文要求）。
  - **DoD**：`make debug && make test_debug` 全绿；
    单测断言 401/403/404/400 → `InvalidInputException`、5xx → `IOException`、业务 `code != 0` → 错误消息含该 code；
    任一错误路径的消息中都不出现 app_secret / tenant token / user token（复用
    `test/unit/common/test_error.cpp` 既有的脱敏断言写法）。
  - **完成证据（2026-09-11）**：`SubmitBatch` 走 `ApiClient::PostJsonNoRetry`（写侧不复用读侧重试语义）、`AccessToken` / `TokenProvider`、`TryParseFeishuError`；仅当 `code == 0` 且 `data.records` 条数与本批一致时才计入 `confirmed_batches` / `confirmed_rows`；确定失败抛 `InvalidInputException`（`ThrowDefiniteWriteFailure`），结果未知抛未知结果错误且**不重放**（`ThrowUnknownWriteResult`）；远端消息经 `RedactWriteMessage`（`SanitizeErrorMessage` + `RedactSecretValue`）脱敏。`make debug` 与离线 SQL 12 文件 / 202 assertions 全绿。
  - **完成证据（2026-09-12）**：新增自包含 TU `src/include/bitable/write_state_machine.hpp` + `src/common/write_state_machine.cpp`，`RunBatchWithRetries` 以注入的 `WriteAttemptFn` 驱动：仅 `CONFIRMED` 推进 `WriteProgress`；`RETRY_SAFE` 在有界 `max_attempts` 内重试（`on_retry` 挂钩，配 `RetryPolicy` / `Retry-After`）；`FAIL` 抛 `InvalidInputException`、`UNKNOWN_RESULT` 抛 `IOException` 且**立即返回、绝不再调用传输层**。`test/unit/common/test_write_state_machine.cpp` 以 7 个 TEST_CASE 断言：确认后进度推进且只调用一次、结果未知名下调用次数恰为 1、确定失败调用次数恰为 1、安全重试恰为 `max_attempts` 次且有 `max_attempts-1` 次退避、消息含 `batch #4`/`rows 1501-1750` 与已确认批次数/行数；`copy_to_sink` / `copy_to_finalize` 已改为经该状态机提交。

### E-4 系列：失败与幂等状态机（3 条）

- [x] **E-4.1** 🤖 写侧重试分类器（**不复用**读侧 `WithRetry` 的默认语义）
  - **前置**：`.specs/06_WRITEBACK_RESEARCH.md` §1.4 与 E-2 的失败语义；该纯分类器已独立实现，不以 E-3.5 的响应验收为阻塞条件
  - **内容**：新增自包含的写侧分类函数，把 (HTTP 状态码, 飞书业务 code, 传输层错误种类) 映射到三态：
    `RETRY_SAFE` / `UNKNOWN_RESULT` / `FAIL`。判定基线：
    - `RETRY_SAFE`：连接建立失败、DNS 失败、TLS 握手失败（请求确定未到达）、HTTP 429 与
      `FEISHU_RATE_LIMIT_CODE`（99991400，服务端确定拒绝且未提交）；
    - `FAIL`：4xx 参数/鉴权/资源错误、明确的业务错误码；
    - `UNKNOWN_RESULT`：请求已发出后的读超时、连接中断、响应体不可解析、2xx 但结构不符合预期、5xx。
    **理由（必须写进代码注释）**：`.specs/06_WRITEBACK_RESEARCH.md` §1.4 确认 `batch_create` 无幂等键，
    因此读侧把 5xx 一律判为可重试的做法在写侧是不安全的。
  - **DoD**：`make test_unit` 中 Catch2 覆盖上述矩阵的每一格；
    存在一条显式断言"读超时不得被判为 `RETRY_SAFE`"；该 TU 不链接 DuckDB 库。
  - **完成证据（2026-09-12）**：新增自包含 `ClassifyWriteRetry` 三态分类器及 Catch2 矩阵；连接/DNS/TLS 建连失败和明确限流为 `RETRY_SAFE`，普通 4xx/业务错误为 `FAIL`，读超时、断连、非法响应及所有 5xx 为 `UNKNOWN_RESULT`，并有“读超时不得安全重试”的显式断言。
  - **传输层加固（2026-09-12）**：`cmake/prepare_httplib.cmake` 在配置期从 DuckDB 内置 httplib 生成私有副本，移除 `SSLPeerCouldBeClosed_` 之后对同一请求的隐式重放（上游源码不匹配时直接 `FATAL_ERROR`），`CMakeLists.txt` 将其加入扩展 include 路径——否则传输层会绕过状态机的「结果未知绝不重放」约束。

- [x] **E-4.2** 🤖 写入状态机：进度推进、重试边界与未知结果的终止
  - **前置**：E-4.1
  - **内容**：每批**只有收到确定成功响应后**才推进已确认提交的行数；`RETRY_SAFE` 交给现有
    `WithRetry`（复用 `RetryPolicy`，含 `Retry-After` 上限）；`UNKNOWN_RESULT` 与 `FAIL` 立即终止整条 `COPY`。
    `UNKNOWN_RESULT` 抛出的异常消息必须包含：已确认成功的批次数与行数、结果未知的批次序号与其行区间、
    以及"请先人工核对目标表再决定是否重跑"的指引。**禁止自动重放。**
  - **DoD**：单测断言 `UNKNOWN_RESULT` 后不存在第二次提交调用；
    断言异常消息包含批次序号与行区间，且不含任何凭据片段。
  - **完成证据（2026-09-12）**：新增自包含 TU `src/include/bitable/write_state_machine.hpp` + `src/common/write_state_machine.cpp`，`RunBatchWithRetries` 以注入的 `WriteAttemptFn` 驱动：仅 `CONFIRMED` 推进 `WriteProgress`；`RETRY_SAFE` 在有界 `max_attempts` 内重试（`on_retry` 挂钩，配 `RetryPolicy` / `Retry-After`）；`FAIL` 抛 `InvalidInputException`、`UNKNOWN_RESULT` 抛 `IOException` 且**立即返回、绝不再调用传输层**。`test/unit/common/test_write_state_machine.cpp` 以 7 个 TEST_CASE 断言：确认后进度推进且只调用一次、结果未知名下调用次数恰为 1、确定失败调用次数恰为 1、安全重试恰为 `max_attempts` 次且有 `max_attempts-1` 次退避、消息含 `batch #4`/`rows 1501-1750` 与已确认批次数/行数；`copy_to_sink` / `copy_to_finalize` 已改为经该状态机提交。

- [x] **E-4.3** 🤖 决定并落地"部分完成"的对外表达方式
  - **前置**：E-4.2
  - **内容**：由于 DuckDB v1.5.5 的 `COPY` 返回形状不可扩展（本文件 §1.1 第 6 条），
    进度只能通过异常消息暴露。**判断：不新增任何 SQL 注册面**（`AGENTS.md` 禁止为可测性暴露契约外函数，
    读侧已按此清理过一轮）。把恢复指引固化进 `.specs/03_API_CONTRACT.md` §8 的"失败后恢复"小节与 README，
    给出用读侧 `read_feishu_bitable` 核对远端实际写入量的具体 SQL。
  - **DoD**：契约 §8 含"失败后恢复"小节且给出可直接复制运行的核对 SQL；
    `SELECT count(*) FROM duckdb_functions() WHERE function_name LIKE '%feishu%'` 的结果相对 E-3.1 之前无新增
    ~~原 DoD 要求 `duckdb_copy_functions()` 断言~~ → **修正（2026-09-11）**：DuckDB v1.5.5 既无 `duckdb_copy_functions()` 表函数、`duckdb_functions()` 也不列 copy function，故注册校验改为行为断言（合法 `COPY` 必须到达扩展 sink，而非报 `COPY TO is not supported for FORMAT`）。
  - **完成证据（2026-09-12）**：未新增任何 SQL 注册面（`duckdb_functions()` 的 `%feishu%` 计数与 E-3.1 持平，只多一个 COPY format）。`.specs/03_API_CONTRACT.md` §8.9 给出"失败后恢复"两段结论与可直接复制运行的核对 SQL；`README.md` 第 124-128 行同样给出 `SELECT count(*) FROM read_feishu_bitable(...)` 的人工核对步骤，并明确跨批次不具事务性。

### E-5 系列：三层测试与环境隔离（6 条）

- [x] **E-5.1** 🤖 离线 SQL 层：COPY 参数、目标校验与只读路径零回归
  - **前置**：E-3.2
  - **内容**：新增 `test/sql/copy_writeback.test`（**无 `require-env`，不发网络请求**），覆盖：
    未知选项、选项类型错误、`batch_size` 越界、缺 `table_id`、非法 URL、不存在的 SECRET、
    E-2.1(d) 中可检测的通用写选项（`file_extension`）、以及 `FORMAT` 缺失时不得被误判为本扩展。
    ~~原 DoD 要求 `duckdb_copy_functions()` 断言~~ → **修正（2026-09-11）**：DuckDB v1.5.5 既无 `duckdb_copy_functions()` 表函数、`duckdb_functions()` 也不列 copy function，故注册校验改为行为断言（合法 `COPY` 必须到达扩展 sink，而非报 `COPY TO is not supported for FORMAT`）。
  - **DoD**：无凭据环境下 `make test_debug` 全绿；新增断言 ≥ 10 条；
    `git diff --stat test/sql/` 只出现新增文件，既有 13 个 `.test` 零改动即通过（读侧零回归）。
  - **完成证据（2026-09-12）**：`test/sql/copy_writeback.test` 无 `require-env`、不发起任何网络请求，共 **13 条**断言（注册行为、未知选项、`batch_size` 501/0/-1 与类型错误、`file_extension`、缺 `table_id`、零行上游仍执行 Bind、非 Bitable URL、无可用 Secret、命名 Secret 不存在、缺省 FORMAT 让渡到本地 CSV、format 名近似值由 catalog 拒绝）；无凭据环境下全绿，既有 12 个 `.test` 文件零改动。

- [x] **E-5.2** 👤 在线层环境准备：独立写入 Base 与受保护凭据
  - **前置**：E-2.6
  - **内容**：创建**专用写入 Base**（严禁复用读取回归 Base，`.specs/04_TESTING_SPEC.md` §6 硬约束），
    建一张覆盖 E-2.3 映射表全部类型 + 至少一个只读字段（公式或自动编号）的测试表；
    `test/sql/integration_copy_writeback.test` 当前至少要求：text 字段 `name`、number 字段 `amount`、
    single-select 字段 `status`（含选项 `ready`）、formula 字段 `formula_column`、
    USER 字段 `person`、LINK 字段 `link`（同表关联）。
    约定并写入测试规约的门控变量（`FEISHU_WRITE_TEST_APP_ID` / `FEISHU_WRITE_TEST_APP_SECRET` /
    `FEISHU_WRITE_TEST_APP_TOKEN` / `FEISHU_WRITE_TEST_TABLE_ID`，与读侧 `FEISHU_TEST_*` 严格分开，避免误用读 Base；
    再加非敏感 marker 变量 `FEISHU_WRITE_TEST_PREFIX`，供 `integration_copy_writeback.test`
    与 `scripts/cleanup_write_test_records.sh` 识别并清理本次测试记录；
    I-5 另需 `FEISHU_WRITE_TEST_OPEN_ID` 作为人员字段夹具）；
    在 GitHub 受保护 environment 中配置对应 Secret。
  - **DoD**：变量名清单已固化进 `.specs/04_TESTING_SPEC.md`；
    `grep -rn "FEISHU_WRITE_TEST" .github/workflows/` 在公共 job 中无匹配（只允许出现在受保护的手动 job 中）。
  - **完成证据（2026-09-29）**：本地专用 Base `duckdb-feishu-write-test` / 表 `write_test_table`（四字段）已就绪；
    `.env.local` 注入 `FEISHU_WRITE_TEST_*`（与读侧隔离）；`make test_sql_write_debug` 已跑绿。
    GitHub Environment `write-tests` 已注入三项 Secret（`FEISHU_WRITE_TEST_APP_TOKEN` /
    `FEISHU_WRITE_TEST_TABLE_ID` / `FEISHU_WRITE_TEST_USER_ACCESS_TOKEN`）；仓库级 Secret 仍为空。
    `FEISHU_WRITE_TEST_PREFIX` 按设计不进 Secret，由 `WriteTests.yml` 固定为 `e5-ci-`。
    私有仓库当前计费套餐不支持 Environment 的 Required reviewers / wait timer；已在免费能力内启用
    自定义部署分支白名单（仅 `dev` / `main`）并关闭 admin bypass。
    **完整审批门延后到 D-0a**（仓库转 public 之后；时机见 D-0「启动时机」）。
  - **增量（2026-09-30）**：写入身份契约由 `user_access_token` 改为**应用身份**，门控变量变为
    `FEISHU_WRITE_TEST_APP_ID` + `FEISHU_WRITE_TEST_APP_SECRET`（`integration_copy_writeback.test`、
    `Makefile`、`scripts/cleanup_write_test_records.sh`、`WriteTests.yml` 已同步）；
    本地 `.env.local` 已注入新变量，`make test_sql_write_debug` 复跑 **15 assertions 全绿**。
    **Environment `write-tests` 已同步（2026-09-30）**：新增 `FEISHU_WRITE_TEST_APP_ID` /
    `FEISHU_WRITE_TEST_APP_SECRET`（经 `gh secret set` 由 stdin 写入，值不出现在命令 argv），
    删除已废弃的 `..._USER_ACCESS_TOKEN`；当前环境内 4 项 Secret 与 `WriteTests.yml` 的凭据检查一致。
  - **增量（2026-10-03，I-5）——远端与文档均已收口，维护者只需补一项 Secret**：
    1. **远端写入表结构：已完成，无需再改。** 实测 `write_test_table` 现有 6 列：
       `name` / `amount` / `status` / `formula_column` / `person`(USER) / `link`(LINK 同表)；
       无缺列。I-5 实现阶段已加好，本轮只做核对。
    2. 本地 `.env.local` 已有 `FEISHU_WRITE_TEST_OPEN_ID`（不入库）。
    3. `WriteTests.yml` 已把门控与测试 job 接上该变量；五项 Secret（含 `OPEN_ID`）
       已写入 `write-tests`（stdin，`gh secret set`）；E-5.6 成功 run 已验证非空门控。

- [x] **E-5.3** 🤖 + 👤 在线层：写后读回与批次边界
  - **前置**：E-3.5、E-5.2
  - **内容**：新增 `test/sql/integration_copy_writeback.test`（`require-env` 门控）。
    写入 N 行后用 `read_feishu_bitable` 回读并逐列比对；N 取 `batch_size-1` / `batch_size` / `batch_size+1`；
    另有一条 0 行（`WHERE false`）用例断言 `Count = 0` 且远端记录数不变（验证 E-3.4 的"0 行零请求"）。
  - **DoD**：注入凭据后本地全绿；未注入时整文件跳过；
    用例自身或收尾步骤删除本次写入的全部记录（跑两遍结果一致即为证据）。
  - **完成证据（2026-09-29）**：`make test_sql_write_debug` → `All tests passed (15 assertions in 1 test case)`；
    覆盖 0/99/100/101 行批次、写后读回聚合、零行不改基线；结束后 `cleanup_write_test_records.sh` 删除 400 条 marker 记录。

- [x] **E-5.4** 🤖 + 👤 在线层：整批失败不产生副作用
  - **前置**：E-5.3
  - **内容**：构造一批中夹带一条非法值（例如向 `NUMBER` 字段写非数值、或向只读字段写值）的提交，
    断言整批失败；提交前后用读侧 `count(*)` 比对确认**零新增**。同时记录成功路径下 `data.records`
    的返回条数与顺序是否与提交一致，回填 `.specs/06_WRITEBACK_RESEARCH.md` §1.3。
  - **DoD**：失败后回读行数与提交前完全一致；§1.3 中"返回条数、响应记录顺序……需实测"的标记
    被替换为带数据的实测结论。
  - **完成证据（2026-09-29）**：未知 SingleSelect 选项会被 OpenAPI **自动建选项**（不可靠）；改用超大文本触发
    `1254130 TooLargeCell`。单行非法 COPY 报 `batch #1 (rows 1-1) failed … 0 confirmed batches / 0 rows`，回读行数仍为 300。
    §1.3 / §1.5 已回填。

- [x] **E-5.5** 🤖 + 👤 在线层：跨批次部分完成与未知结果的可恢复性
  - **前置**：E-5.4、E-4.2
  - **内容**：构造第 1 批全部合法、第 2 批含非法值的输入（行数 > `batch_size`），断言：
    查询报错；错误消息给出已成功批次数/行数与失败批次的行区间；且**第 1 批记录确实留在远端**
    （append-only 无跨批次事务性）。契约必须显式声明这一点，不能让用户误以为 `COPY` 是原子的。
  - **DoD**：错误消息中的批次/行区间与远端实际状态逐条吻合；
    `.specs/03_API_CONTRACT.md` §8 含"跨批次不具备事务性"的明示；用例结束后测试记录已清理。
  - **完成证据（2026-09-29）**：200 行 / `batch_size=100`，第 2 批超大文本失败 →
    `batch #2 (rows 101-200) failed … 1 confirmed batches / 100 rows`；远端 `partial-%` 计数为 100。
    契约 §8 已明示跨批次非事务性；清理脚本已清零。结果未知自动重放仍不做（设计如此）。

- [x] **E-5.6** 🤖 CI 隔离：受保护的手动写入测试 job
  - **前置**：E-5.2
  - **内容**：在 `.github/workflows/` 新增只能由维护者 `workflow_dispatch` 触发、挂受保护 environment 的
    写入测试 job。**必须沿用 `duckdb/extension-ci-tools` 的可复用 workflow 与仓库现有 `workflow_call` 结构**
    （参照 B-3 已接入 `UnitTests.yml` 的做法），不得自造构建流程。
    主 pipeline 与 `UnitTests.yml` 保持零写入凭据。
  - **DoD**：普通 PR CI 上 `integration_copy_writeback.test` 整文件跳过且 job 全绿；
    手动 job 在一次真实 `gh workflow run` 上跑绿（依 `AGENTS.md` §4 的触发策略）。
  - **部分完成证据（2026-09-13，🤖 侧；2026-09-29 增量）**：新增 `.github/workflows/WriteTests.yml`——`workflow_dispatch` + `workflow_call`，两个 job 均挂 `environment: write-tests`，缺失凭据时输出 `::warning::` 并跳过；在线用例由三项写入凭据加 `FEISHU_WRITE_TEST_PREFIX` 门控，`scripts/cleanup_write_test_records.sh` 在测试前后清理 marker 记录，`make test_sql_write_debug` 输出经脱敏。主 pipeline 未引用该 workflow，公共 CI 上在线文件按 `require-env` 整文件跳过。E-5.2 的 `write-tests` Secret 与 `dev`/`main` 分支白名单已配。**剩余（👤）**：一次真实 `gh workflow run` 跑绿。注意默认分支仍是 `main`（仅有 initial commit），`WriteTests.yml` 目前只在 `dev`；GitHub 要求 `workflow_dispatch` 工作流出现在默认分支后才能从 Actions UI / `gh workflow run` 稳定触发。
  - **增量（2026-09-30）**：受保护环境的 Secret 已完成换名（见 E-5.2 增量），`WriteTests.yml` 的凭据检查不再因缺项跳过。
    当时**触发面不通**：`gh api .../actions/workflows` `total_count=0`（默认分支无 `.github/`）。
  - **增量（2026-10-01，D-3 合入后）**：触发面已解——`main` 已登记 4 个 workflow（含 `WriteTests.yml`，`state=active`）。
    E-5.6 **剩余 DoD**：一次真实 `gh workflow run WriteTests.yml` 跑绿（可选下一步；本期未执行）。
  - **增量（2026-10-02）**：已真实触发 `gh workflow run WriteTests.yml --ref dev`。
    - Run：[37003064406](https://github.com/tshelianthus/duckdb-feishu-bitable/actions/runs/37003064406) — **failure**（2026-10-02）
    - credentials job：**success**（`write-tests` 四项 Secret 可用；Environment 仅 `branch_policy`，**无** Required reviewers，无需人工审批）
    - write-tests job：**failure**——`Build debug extension` 期间 runner 磁盘耗尽
      （`System.IO.IOException: No space left on device`）；清理脚本与凭据检查均已通过
    - **最小修复（工作区未提交）**：`WriteTests.yml` 在 checkout 前增加
      `endersonmenezes/free-disk-space@v3`（对齐 `duckdb/.github/actions/cleanup_runner` /
      `extension-ci-tools` 的清盘模式）
    - **剩余 DoD**：将该修复 commit + push 到 `dev`（或可部署分支）后重跑
      `gh workflow run WriteTests.yml --ref <pushed-ref>` 至 conclusion=`success`
  - **增量（2026-10-03，清盘修复已在 `271f6fa`，随 `abcd1ce` 再次触发）**：
    - Run [37093807382](https://github.com/tshelianthus/duckdb-feishu-bitable/actions/runs/37093807382) — **cancelled**。凭据 job 成功；`Free disk space` 成功；`Build debug extension` 在约 39 分钟时停住，job 记录仍为 `in_progress`。随后用 `gh run cancel` 解开卡住的 run，GitHub 记为 canceled by the authenticated account。测试步骤没有开始。
    - Run [37097316391](https://github.com/tshelianthus/duckdb-feishu-bitable/actions/runs/37097316391) — **failure**，`headSha=abcd1ce`。凭据 job 成功；`Free disk space`、checkout、写入记录清理都成功。`make debug` 在链接 `libduckdb.so` / `unittest` / `duckdb`（日志约 99%–100%）时被 `SIGTERM`，`Process completed with exit code 143`。受保护 SQL 测试没有跑到。
    - **剩余 DoD**：这次不是磁盘满，也不是测试断言失败。debug 链接阶段被终止，需要另一次能把 `make debug` 跑完的手动 run，结论仍不是 `success`。
  - **完成证据（2026-10-03/04，`headSha=7f45f8d`）**：
    - 构建策略修复（commit `7f45f8d`）：`WriteTests.yml` 安装 `ninja-build`，`GEN=ninja`、
      `BUILD_WORKERS=2`、`EXT_DEBUG_FLAGS=-DDUCKDB_DEBUG_LINK_JOBS=1`。对齐
      `duckdb/.github/workflows/Main.yml` / `extension-ci-tools` 的 Ninja 实践；
      `CMAKE_JOB_POOL_LINK` 仅在 Ninja 下生效，避免 Unix Makefiles `-j4` 同时链接
      `libduckdb.so` / `unittest` / `duckdb` 再被 SIGTERM（exit 143）。
    - 中间失败（非构建）：[37127455912](https://github.com/tshelianthus/duckdb-feishu-bitable/actions/runs/37127455912)
      假绿（`OPEN_ID` Secret 为空 → write job skip）；
      [37127567487](https://github.com/tshelianthus/duckdb-feishu-bitable/actions/runs/37127567487)
      构建 success，测试 `91403 Forbidden`（Secret 与本地可写凭据不一致，已 stdin 重同步五项）；
      [37130814417](https://github.com/tshelianthus/duckdb-feishu-bitable/actions/runs/37130814417)
      构建 success，测试 `count(*)` 702≠300（专用表残留 402 条非 `e5-ci-` 行，已整表清零）。
    - **成功 Run**：[37134112552](https://github.com/tshelianthus/duckdb-feishu-bitable/actions/runs/37134112552)
      — `conclusion=success`。凭据 job（五类 Secret 含 `OPEN_ID`）success；
      Free disk / 前置 cleanup / Install ninja / Build debug / `make test_sql_write_debug` /
      后置 cleanup（`if: always()`）均为 success。主 pipeline 仍不引用 `WriteTests.yml`、零写入凭据。

### E-6 系列：v0.2 发布准备（3 条）

- [x] **E-6.1** 🤖 对外文档同步（README / 描述符 / PRD）
  - **前置**：E-5.5、E-5.6
  - **内容**：README 新增"写入"章节：`COPY` 示例、写入所需的飞书权限（依 E-2.6 的实测结论，
    不得把"可读取"说成"可写入"）、限制清单（直接引用 §8 的"不做"清单）、失败后的核对步骤（E-4.3）。
    `docs/community/description.yml` 的 `description`、`extended_description`、`hello_world` 补写入示例
    （对标 gsheets 的 `Read and write ... using SQL` 与其 `COPY <t> TO '<URL>' (FORMAT gsheet)` 示例风格）。
    `.specs/01_PRD.md` 更新 v0.2 范围。
  - **DoD**：逐条对照 §8 的"不做"清单，文档中不出现任何尚未实现的写入能力；
    `python3 -c "import yaml;yaml.safe_load(open('docs/community/description.yml'))"` 通过；
    README 的权限描述与 `.specs/03_API_CONTRACT.md` §1 / §8 逐字一致。
  - **完成证据（2026-09-12）**：`docs/community/description.yml` 的 `description` 改为 "Read and write ... from SQL"，`extended_description` 删除 "Read-only by design" 与 "no `COPY ... TO` writer"，新增 "## Writing back (append-only, v0.2)" 段（显式 FORMAT、URL/裸 token 目标、显式 `table_id` 优先、写侧无首表回退、权限要求、500 上限、结果未知不重放、跨批次非事务性、核对 SQL、不支持清单），`hello_world` 补充 COPY 示例，`Non-goals` 改为覆盖/删除与 INSERT 下推；`.specs/01_PRD.md` 的写入路线与 Scope 段同步为 v0.2 已实现 append-only；`README.md` 写入章节补齐"写回需要编辑权限、仅有读取权限不能写入"。**YAML 验证**：本机 `python3` 无 `pyyaml`（`ModuleNotFoundError: No module named yaml`），改用等价结构自检——无 tab 字符、顶层键为 `extension` / `repo` / `docs`、块标量缩进一致，且 `grep -n "version: 0.2.0" docs/community/description.yml` 命中。

- [x] **E-6.2** 🤖 版本递增与发布清单补写入门禁
  - **前置**：E-6.1
  - **内容**：`docs/community/description.yml` 的 `extension.version` 递增为 `0.2.0`；
    `docs/RELEASE_CHECKLIST.md` 新增两条勾选项——"写入手动 job 已在待发布 SHA 上跑绿"、
    "写入测试 Base 中的测试记录已清理"；并把 B-3 固化的"发布前手工 ASan 门禁"一并纳入 v0.2 流程。
  - **DoD**：清单每条都能用一条命令或一次明确目视核对判定；版本号与 README 中声明的版本一致。
  - **完成证据（2026-09-12）**：`extension.version` 已由 `0.1.0` 递增为 `0.2.0`（与 README 声明一致）；`docs/RELEASE_CHECKLIST.md` §5 新增三条可单命令判定的勾选项——版本一致性、`gh run list --workflow=WriteTests.yml ... --jq '.[0].conclusion'` 为 `success`、写入 Base 记录已清理回到基线；§4.1 的 ASan / UBSan 手工门禁（B-3 固化）本就在清单内。

- [ ] **E-6.3** 👤 v0.2.0 发布执行
  - **前置**：E-6.2；并复用批次 D 的 D-1~D-7 流程（本地全门禁 → CI → PR 合并 → 打 tag → 回填 `repo.ref` →
    社区收录 PR → GitHub Release）
  - **本期禁止（2026-10-07）**：范围 B 目标止于「可用但不发布」；本条留待 **G-2 → D-0 → D-0a** 后再启动，不得被误标为范围 B 未完成项。
  - **DoD**：`git rev-parse v0.2.0^{commit}` 输出 40 位 SHA 且 tag 在 GitHub 可见；
    社区收录 PR 中的 `repo.ref` 指向该 40 位 SHA（非分支名、非 tag 名、非短 SHA）；
    干净环境中 `INSTALL feishu_bitable FROM community; LOAD feishu_bitable;` 后
    ~~原 DoD 要求 `duckdb_copy_functions()` 断言~~ → **修正（2026-09-11）**：DuckDB v1.5.5 既无 `duckdb_copy_functions()` 表函数、`duckdb_functions()` 也不列 copy function，故注册校验改为行为断言（合法 `COPY` 必须到达扩展 sink，而非报 `COPY TO is not supported for FORMAT`）。

---

### 依赖关系速查

```
E-1 ✅
 ├─ E-2.1 ─┬─ E-2.2 ─┐
 │         ├─ E-2.3 ── E-2.4 ─┐
 │         ├─ E-2.5 ──────────┤
 │         └─ E-2.6 (👤) ── E-2.7 ─┤
 ├─ E-2.8 (并行)                   │
 │                                 │
 │        ┌────────── D-6 (👤, 批次 D) ──────────┐
 │        ↓                                       ↓
 └────────┴─ E-3.1 ─┬─ E-3.2 ── E-5.1
                    ├─ E-3.3 ─┐
                    └─────────┴─ E-3.4 ── E-3.5 ─┬─ E-4.1 ── E-4.2 ── E-4.3
                                                 │
                    E-2.6 ── E-5.2 (👤) ─┬───────┴─ E-5.3 ── E-5.4 ── E-5.5 ─┐
                                         └─ E-5.6                            │
                                                                E-6.1 ── E-6.2 ── E-6.3 (👤)
```

**可立即下发（无 D-6 阻塞）**：E-2.1、E-2.8（两条无前置，可并行）；随后 E-2.2 / E-2.3 / E-2.5 / E-2.6。
**原被 D-6 阻塞**：E-3.1 起的实现类任务——该前置已于 2026-09-11 经用户决策解除，
现在可在 `dev` 上直接实现（不发布）。
**👤 人工任务**：E-6.3（发布执行）；E-2.6、E-5.2 与 E-5.3/E-5.4/E-5.5 的真实凭据执行环节均已完成。

---

---

## [ ] 批次 G：凭据卫生与转公开前置（**阻塞 D-0，优先级最高**）

> **触发原因**：用户确认 `docs/FEISHU_TEST_AGENT_PROMPT.md` 与 `docs/LIVE_TESTING_TASKS.md` 中
> 出现的 `cli_…` 串是**其自建应用的 app_id**，而非此前文档所称的"lark-cli 公开内置应用 ID"。
> 该判定推翻了"app_id 单独无法鉴权故可公开"的风险评估：**在本项目中 app_id 与 app_secret 同等对待**。
>
> **参照基准 —— `evidence-dev/duckdb_gsheets`（已被社区收录）的实际做法**：
> 1. 公开文档中**只出现尖括号占位符**（`'<your_token>'`、`'<path_to_JSON_file_with_private_key>'`），
>    从不出现任何真实 ID，哪怕是看似无害的。
> 2. **测试 / CI 的凭据机制完全排除在公开文档之外**，改由 GitHub Actions 供给。
> 3. 在线 SQL 测试独立成 `SQLTests.yml`，通过 `workflow_call` 接收**可选** secret
>    `GSHEETS_KEY_FILE_JSON`；缺失时以 `::warning::... skipping SQL tests` **显式告警并跳过**，
>    而不是让失败被静默吞掉。
> 4. fork PR 走 `environment: integration-tests` **受保护环境**，需维护者批准才能拿到凭据。
> 5. 使用**长期有效的服务账号密钥文件**，而非短时效用户令牌。

- [x] **G-1** 🤖 从工作区移除自建应用 ID 并改写凭据说明 —— **已完成（2026-09-11）**
  - 位置：`docs/FEISHU_TEST_AGENT_PROMPT.md`、`docs/LIVE_TESTING_TASKS.md`。
  - 已做：删除该 app_id 与"lark-cli 内置应用"的错误表述；改写为"凭据由执行人自备、
    只写入本地 `.env.local`、禁止写入任何纳入版本控制的文件"；补充模式 B 的 2 小时时效说明
    与"在线回归应优先使用模式 A"的指引。
  - **DoD**：`grep -rEn 'cli_[a-z0-9]{16,}' --exclude-dir=.git .` 无输出。**已满足**。

- [ ] **G-2** 👤 重写 git 历史，移除该 app_id 的全部历史副本
  - **决策记录（2026-10-13，`WAITING_USER_CONFIRM_G2_COMMANDS`）**：只读审计确认敏感串仍在历史；精确命令序列已产出（含 `python3 -m git_filter_repo --replace-text`、清理 8 条 `refs/codex/*`、`dev`+`main` 双分支 `force-with-lease`）。**尚未执行任何重写/push/改 visibility**；等待用户确认精确命令与目标。D-0 保持 `HOLD_UNTIL_G2_DOD`。
  - **前置**：G-1。
  - **背景**：`dev` 尚未合入 `main`，且只有单人协作，因此历史重写的代价极低；
    一旦转公开或被 fork，该串将不可撤回。
  - 要求：用 `git filter-repo --replace-text`（或 `filter-branch`）把该串替换为占位符，
    覆盖 `dev` 与 `main` 全部提交；随后 `git push --force-with-lease origin dev`
    （`main` 当前 tip 无该串，但若本地还有指向旧对象的旁路 ref 也需清理后按需
    `force-with-lease`）。**禁止** `git push --mirror`；**禁止**在未获用户确认前执行。
  - **注意（2026-09-29 审计复核）**：泄漏文件为
    `docs/FEISHU_TEST_AGENT_PROMPT.md`、`docs/LIVE_TESTING_TASKS.md`、
    （短暂）`.specs/05_TASKS.md`；真实 `cli_a965…`（长度 20）至少仍存在于
    `af554b6`…`6ad1b46` 共 7 个 commit 的树中；G-1 清除点为 `f8268e8`，`HEAD` 工作区已净。
    本地 `refs/codex/...` 快照 ref **当前为 8 条**（2026-10-13 复核）；重写前仍应删除：
    `git for-each-ref --format='%(refname)' refs/codex | xargs -n1 git update-ref -d`。
    完整命令序列见发布准备交付物 / 对话中的 G-2 方案（**只方案、未执行**）。
  - **审计证据（2026-10-13，只读）**：`scripts/check_no_secrets.sh` 退出 0；全历史不安全唯一值仅 `<redacted_cli_id>`（15 命中 / 7 commit）；文件为 `docs/FEISHU_TEST_AGENT_PROMPT.md`、`docs/LIVE_TESTING_TASKS.md`、`.specs/05_TASKS.md`；`f8268e8` 起 tip 已净；`origin/dev` 与 `origin/main` 祖先仍可达这些对象；未见真实 app_secret/token。另有占位符 `<redacted_cli_id>` 会撞 DoD 正则，重写时需一并归一化为 `cli_<APP_ID_PLACEHOLDER>`。
  - **DoD**：`git grep -IE 'cli_[a-z0-9]{16,}' $(git rev-list --all)` 无输出（只应剩占位符）；
    `git status --short` 干净；
    `origin/dev` 与本地 `dev` 一致。

- [x] **G-3** 🤖 建立占位符约定并加防复发检查
  - **前置**：G-1。
  - 要求：照 gsheets 的做法，在 `AGENTS.md` 中固化一条军规——
    **纳入版本控制的任何文件中禁止出现真实的 `app_id` / `app_secret` / `app_token` /
    `table_id` / token / 租户域名，一律使用尖括号或 `XXXX` 占位符**；
    并新增一个可在本地与 CI 运行的扫描脚本（如 `scripts/check_no_secrets.sh`），
    正则覆盖 `cli_[a-z0-9]{16,}`、`u-[A-Za-z0-9_-]{20,}`、`t-[A-Za-z0-9_-]{20,}`、
    `bascn[A-Za-z0-9]{10,}`、`tbl[A-Za-z0-9]{10,}`，并把它接入
    `.github/workflows/UnitTests.yml`（该 workflow 不需要凭据，成本极低）。
  - **DoD**：脚本在当前仓库上退出码为 0；人为插入一个真实形态串后退出码非 0；
    `AGENTS.md` 中该条军规可被一条 grep 命中；CI job 在一次真实运行中跑绿。
  - **完成证据（2026-09-11）**：`scripts/check_no_secrets.sh` 当前扫描退出码 0；
    临时植入凭据形态串退出码 1 并报告文件/行号；脚本已接入 `.github/workflows/UnitTests.yml`。

- [x] **G-4** 🤖 照 gsheets 重构在线测试的凭据供给机制（与批次 F 合并执行）
  - **前置**：G-3；与 **F-1 / F-2** 是同一件事的两面，应一并实施。
  - 要求：把在线 SQL 测试独立成 `.github/workflows/SQLTests.yml`，经 `workflow_call`
    接收**可选** secret；缺失时以 `::warning::` 显式告警并跳过整个 job，
    **存在但请求失败时必须让 job 失败**（这正是 F-1 要解决的"假绿"）；
    写入类测试（批次 E）走 `environment:` 受保护环境，仅维护者可批准。
    凭据类型优先选可自动续期的模式 A，对齐 gsheets 用长期密钥而非短时效令牌的取向。
  - **DoD**：无 secret 时该 job 显示为跳过且带 warning 注释；
    配置了无效凭据时该 job **失败**而非跳过；公共 PR 在无凭据下整体仍为绿。
  - **完成证据（2026-09-11）**：新增 `.github/workflows/SQLTests.yml`，以四项可选模式 A
    secrets 供给受保护在线 job；缺失时输出 warning 并跳过，存在时保留 HTTP/连接失败；
    `MainDistributionPipeline.yml` 已通过 `secrets: inherit` 接线。

---

## [x] 批次 H：对标 PostgreSQL FDW 生态的读侧下推预研（v0.3+，不阻塞 v0.1/v0.2）

> **背景（2026-09-12）**：用户要求持续对标 PostgreSQL 第三方 FDW 生态（Supabase Wrappers、
> Steampipe Postgres FDW、ogr_fdw、Multicorn2）以完善"能构建、能加载、能提供预期能力、能发布"
> 这一目标。经调研 Agent 核实（详见 `.specs/06_WRITEBACK_RESEARCH.md` §4），三条结论已直接
> 写入研究文档且不生成任务：(a) 同类单表格 SaaS FDW 普遍只读或最小化写入，佐证 v0.2 append-only
> 范围合理；(b) 对标项目均为单行级 DML，无法印证或推翻本项目的批次状态机设计；(c) 凭据管理
> 现状已优于多数同类方案，无需变更。本批次只收录**会带来读侧新能力**的候选项。
>
> **范围边界（主 Agent 拍板）**：本项目维持"单一数据源专用连接器"定位，**不**采纳 Supabase
> Wrappers 的 Wasm 插件化框架方向（见 ADR-007），批次 H 的每一条都必须是"让现有连接器更扎实"，
> 不得演变为"把项目改造成通用框架"。
>
> **共同前置**：无（可与批次 D/E/G 并行推进，均不影响发布门禁）。

- [x] **H-1** 🤖 调研飞书 `records/search` 端点的谓词/排序下推可行性
  - **前置**：无。
  - **背景**：ogr_fdw、Supabase Wrappers（`begin_scan(quals, sorts, limit)`）、Multicorn2
    （`can_sort`/`get_path_keys`）均把简单谓词与排序前置到扫描请求参数；本项目当前 `feishu_bitable_scan`
    只用 `records`（GET list）+ 投影下推 + 惰性分页提前终止，未做任何谓词/排序下推。
  - **内容**：核对飞书开放平台 `records/search`（POST）端点文档与实测，确认其 filter 支持的
    操作符集合（等值/比较/包含等）、组合逻辑（AND/OR 嵌套深度）、排序参数形态，
    以及该端点返回字段与现有 `records` GET 端点是否完全兼容（分页游标、复合字段结构）。
  - **DoD**：产出一份端点能力表（列出支持的 SQL 比较运算符、可下推的最大表达式复杂度、
    与现有类型映射的兼容性结论），写入 `.specs/06_WRITEBACK_RESEARCH.md` 新增章节或独立调研笔记；
    结论必须是"可行/部分可行/不可行"三选一，不得留"视情况而定"。
  - **完成证据（2026-09-12）**：`.specs/06_WRITEBACK_RESEARCH.md` §5 已给出端点能力表、SQL 映射边界及明确的“部分可行”结论；因无受保护读凭据，真实 Base 双路径验证保留给 H-2。
  - **适用阶段**：v0.3+ 演进，不阻塞 v0.1/v0.2 发布。

- [x] **H-2** 🤖 视 H-1 结论实现谓词/排序下推
  - **前置**：H-1 结论为"可行"或"部分可行"才启动；若 H-1 结论为"不可行"，本条不执行，
    在此处标注原因后归档。
  - **内容**：把 H-1 确认可下推的谓词与 `ORDER BY` 映射为 `records/search` 的 filter/sort 参数，
    不可下推部分维持现有本地过滤/排序行为；不得改变现有 `records` GET 路径在不满足下推条件时的行为。
  - **DoD**：新增离线/在线 SQL 测试验证——满足下推条件时请求体包含预期 filter/sort 参数；
    下推结果与"关闭下推、本地过滤排序"的结果集完全一致（对同一查询双路径对比）。
  - **完成证据（2026-10-02）**：
    - 规划 / 请求体：`src/bitable/search_pushdown.{hpp,cpp}`（`PlanSearchPushdown` /
      `BuildSearchUrl` / `BuildSearchBody` / `ExecuteSearchFetch`）；VIEW_ID / >50 条件 /
      复合字段 / 不可编码原子条件一律拒绝并回退 GET list。
    - 日期谓词（续）：DateTime/CreatedTime/ModifiedTime 在可无歧义编码为官方
      `["ExactDate","<epoch_ms>"]` 时下推 `is` / `isGreater` / `isLess`；`isNot` /
      `isGreaterEqual` / `isLessEqual`、相对日期词（Today/CurrentWeek 等）、非
      TIMESTAMP 常量、越界/负时间戳一律拒绝。
    - 接线：`pushdown_complex_filter`（`PushdownScanFilters`）先走 H-3 sole `record_id =`，
      再尝试扁平 AND/OR search；`ScanFunction` 对启用计划发 `POST .../records/search`，
      分页游标校验与 GET list 一致；投影 `field_names` / 零列最小字段规则保留；
      `TryConstantToSearchAtom` 接受 DATE / TIMESTAMP / TIMESTAMPTZ 常量。
    - 单测：`make test_unit`（含 `test/unit/bitable/test_search_pushdown.cpp`）——可下推时
      body 含 filter/sort；日期 ExactDate 两元素 value；VIEW_ID / 51 条件 / 非法日期运算符 /
      相对日期 / 越界时间戳拒绝；恰好 1 次 POST。
    - 离线 SQL：非 `integration_*` 文件全绿（未跑写侧 `integration_copy_writeback`）。
    - **在线双路径等价（闭合，2026-10-02）**：`test/sql/integration_search_pushdown.test`
      （`require-env` + 清空 `ignore_error_messages`）——同一查询对比「POST records/search」
      与「GET list 物化 + 本地过滤」的 `record_id` 集合；覆盖文本等值、数值比较、同层 AND、
      文本不等值；`VIEW_ID` 与不可下推 JSON/Link 谓词回退 list 且结果一致。本地凭据下
      **18 assertions / 1 test case 全绿**；已接入 `make test_sql_online_debug`。
    - 契约：`.specs/03_API_CONTRACT.md` §4.1b。DuckDB 表函数无 ORDER BY 下推回调，SQL
      `ORDER BY` 仍本地执行；`sort` 序列化已单测覆盖。
  - **适用阶段**：v0.3+ 演进。

- [x] **H-3** 🤖 单记录等值查询走"获取单条记录"接口而非分页 List
  - **前置**：无（与 H-1/H-2 独立，可先行）。
  - **背景**：Steampipe 的 `KeyColumns` 机制按查询形态在框架层区分 `Get`（单键等值）与 `List`
    （全量扫描）两条路径；本项目当前 `WHERE record_id = 'xxx'` 仍会走完整分页 List 再本地过滤。
  - **内容**：在 `feishu_bitable_scan` 的 bind/scan 阶段识别"仅对 `record_id` 做等值比较"的查询形态，
    改用飞书"获取单条记录"接口（单次请求），避免全表扫描。
  - **DoD**：新增单元测试验证 `WHERE record_id = 'xxx'` 场景下对 `ApiClient` 的调用次数为 1
    （而非分页遍历）；非等值或复合条件查询的行为不受影响（回归测试覆盖）。
  - **完成证据（2026-10-02）**：
    - 检测：`pushdown_complex_filter` 识别 sole `record_id =` 并写入 `ScanBindData::single_record_id`；
      规划逻辑在 `src/bitable/record_id_lookup.cpp`（`PlanRecordFetch` / `BuildSingleRecordUrl` /
      `ExecuteRecordFetch`）；scan 路径见 `src/bitable/scan_function.cpp`。
    - 单测：`make test_unit` 通过（含 `test/unit/bitable/test_record_id_lookup.cpp`）——sole equality →
      恰好 1 次 GET 且 URL 为 `/records/{id}`；NON_EQUALITY / 复合 / OTHER → 保持 `LIST_PAGES`。
    - 离线 SQL：相关 `test/sql/*.test`（不含 `integration_*`）通过；`integration_copy_writeback`
      失败为写侧环境残留行数（600≠300），与 H-3 无关。
    - **在线单条路径（2026-10-02）**：`test/sql/integration_search_pushdown.test` §3——先从物化
      扫描取一条 `record_id`，再以 `INCLUDE_SYSTEM_COLUMNS true` + `WHERE record_id = ...`
      取回同行字段；不存在的 id 返回空集。与 H-2 同文件，18 assertions 全绿。
    - 契约：`.specs/03_API_CONTRACT.md` §4.1a 补充优化说明（不改公开 SQL 参数）。
  - **适用阶段**：v0.3+ 演进。

- [x] **H-4** 🤖 README「功能范围与边界」章节按三段式结构重整（不改变已声明能力）
  - **前置**：无。
  - **背景**：Supabase Wrappers 每个 connector 文档采用 Implementation / Supported Types /
    Limitations 三段式结构，比本项目现有 README 的自由散文式表述更易被使用者快速定位边界。
  - **内容**：仅重组 README 中已有的"功能范围与边界"相关内容为该三段式结构，**不得**增删任何
    已声明的能力描述，也不得引入尚未实现的表述。
  - **DoD**：`git diff README.md` 只涉及章节结构与措辞调整；人工核对改动前后信息量一致
    （无新增能力声明、无遗漏现有限制）。
  - **适用阶段**：v0.1 发布收尾之后的文档打磨，优先级低于批次 D/G，可随时安排。
  - **完成证据**：`README.md`「功能范围与边界」已拆为「实现能力（Implementation）/
    支持的类型（Supported Types）/ 限制（Limitations）」；原「类型映射对照」并入 Supported Types；
    原「支持 / 暂不支持」分别落入 Implementation / Limitations（LIMIT 下推说明归入 Limitations）；
    `git diff README.md` 仅为结构与措辞调整，信息量等价。

---

## [x] 批次 I：范围收口后的待验证项（ADR-008，条件触发，不阻塞 v0.1/v0.2）

> **背景（2026-10-03）**：ADR-008 已把"永不做 / 暂不做 / 待验证"三类功能定档（见 `todolist.md`）。
> 本批次只收录"待验证后再决定"的两项。**每项都是先验证、后决定**：验证不通过就关闭，不得为了"已经做了验证"而硬上实现。
> 共同前置：无。与批次 D / E 互不阻塞。

- [x] **I-1** 👤 + 🤖 `ORDER BY` 的服务端排序等价性在线验证
  - **前置**：无（需 `FEISHU_TEST_*` 真实读凭据与一张含多类字段的测试表）。
  - **背景**：`search_pushdown.cpp` 已实现 `sort` 请求体并有单测，但 DuckDB 表函数没有 `ORDER BY` 回调，查询不会走到它。
  - **内容**：对 `records/search` 的 `sort` 与 DuckDB 本地 `ORDER BY` 做双路径对比，覆盖：空值位置、文本（中英文混排、大小写）、数值、日期、单选、多选、公式、`ASC` / `DESC`、同值次序稳定性，以及带 `VIEW_ID` 时 `sort` 使 `view_id` 被忽略的影响。
  - **DoD**：产出逐字段类型的等价性结论表，每格为"等价 / 不等价 / 不可比"三选一，写入 `.specs/06_WRITEBACK_RESEARCH.md`；整体结论为"可做（给出可下推的字段类型集合）"或"不做"二选一。
  - **适用阶段**：v0.3+，不阻塞发布。
  - **完成证据（2026-10-03，真实读环境）**：结论表在 `.specs/06_WRITEBACK_RESEARCH.md` §8。等价：`TEXT` / `NUMBER` / `DATE_TIME`（ASC/DESC、默认 NULLS LAST、同值保持创建序）。不等价：单选（选项定义序）、多选（只看第一个选项）、公式（按计算值，不是 JSON 文本）、带筛选的 `VIEW_ID`（带 `sort` 后筛选被丢掉）。空串与空多选读回变成 NULL，空值对照不可比。整体结论：**可做**，可下推集合只有上述三类。临时探针表已删除，读 Base 表名顺序已恢复。

- [x] **I-2** 🤖 视 I-1 结论处置排序下推
  - **前置**：I-1。
  - **内容**：I-1 结论为"不做" ⇒ 删除 `src/bitable/search_pushdown.cpp` 中 `sort` 相关代码与对应单测，把 `.specs/03_API_CONTRACT.md` §4.1b 中"供后续接线"的表述改为"已移除"。I-1 结论为"可做" ⇒ 仅对 I-1 确认等价的字段类型，通过优化器改写查询计划接线；不满足条件时必须回退本地排序。
  - **DoD**：两种结论各自的验收——"不做"：`grep -rn "sort" src/bitable/search_pushdown.cpp` 无 `sort` 请求体相关命中，`make test_debug && make test_unit` 全绿；"可做"：新增在线双路径测试，下推与本地排序的结果集及顺序完全一致，且未下推场景行为不变。
  - **适用阶段**：v0.3+。
  - **完成证据（2026-10-03）**：走“可做”分支。`IsSortEquivalentFieldType` 只接受 TEXT/NUMBER/DATE_TIME；优化器 `OptimizeFeishuSortPushdown` 在条件不满足时保留本地 `ORDER BY`。在线对照在 `test/sql/integration_search_pushdown.test` 第 4 节（文本、数字、复合键，以及 `record_id` / `NULLS FIRST` / `VIEW_ID` 三条回退）。关联列是 JSON，这条 SQLLogic 进程不加载 json 扩展，所以没有对它做 `ORDER BY`；不可下推字段类型由单测 `unpushable_sort` 覆盖。§4.1b 已改成这条规则，不再写“供后续接线”。

- [x] **I-3** 👤 关联与人员字段写入的请求体实测
  - **前置**：E-5.2（专用写入 Base，已完成）。
  - **内容**：用 `curl` 在写入 Base 上实测 `batch_create` 写入 `LINK` / `DUPLEX_LINK`（`record_id` 数组）与 `USER`（`open_id` 数组）的请求体形状；记录 HTTP 状态、业务 `code`、脱敏 `msg`；覆盖：合法值、不存在的 id、跨表 id、`user_id_type` 取值（`open_id` / `union_id` / `user_id`）对成功与否的影响；确认写入后读回的值是否与提交一致。**不写扩展代码。**
  - **DoD**：结论写入 `.specs/06_WRITEBACK_RESEARCH.md` 新章节，每组实测有状态码 + code + 脱敏 msg；整体结论为"可做 / 不可做"二选一；测试记录已清理（与写入 Base 基线一致）。
  - **适用阶段**：v0.3+，不阻塞发布。
  - **完成证据（2026-10-03，真实写入环境）**：结论在 `.specs/06_WRITEBACK_RESEARCH.md` §9。整体结论：**可做**。关联的可写形状是 `record_id` 字符串数组；人员不是字符串数组，而是 `[{"id":"<open_id>"}]`，且只有 `user_id_type=open_id`（或不传，默认同 open_id）成功。临时表已删除，`write_test_table` 记录总数仍为 900。未写扩展代码。

- [x] **I-4** 🤖 视 I-3 结论冻结关联 / 人员字段的写入契约
  - **前置**：I-3 结论为"可做"才启动；"不可做"则在此处标注原因后归档，`.specs/03_API_CONTRACT.md` §8.5(b) 保持拒绝。
  - **内容**：按 E-2.3 / E-2.4 的体例，在 §8.3 / §8.5 中补充：源类型（`LIST(VARCHAR)`）、`user_id_type` COPY 选项（默认 open_id，另接受 union_id / user_id）、空列表 / 含 NULL / 不存在 id 的唯一行为、Bind 期错误模板；同步更新 `src/include/bitable/field_type.hpp` 对应的可写性表述。**本条只冻结契约，实现任务在契约冻结后另行编排。**
  - **DoD**：契约新增行均含"源类型 / 目标类型 / 越界或不可转换行为"三列；`grep -n "LINK\|USER" .specs/03_API_CONTRACT.md` 中 §8.5(b) 的拒绝清单与新增可写清单互不重叠；README 与 `docs/community/description.yml` 不出现尚未实现的写入能力表述。
  - **适用阶段**：v0.3+。
  - **完成证据（2026-10-03）**：§8.3 新增 LINK/DUPLEX_LINK 与 USER 两行（源类型 `LIST(VARCHAR)`，越界行为指向 §8.4）；§8.5(b) 只剩 ATTACHMENT / LOCATION / GROUP_CHAT。`field_type.hpp` 注明契约已冻结、编码器仍拒绝。README 写明当前版本仍拒绝。实现任务是 I-5。
  - **修订（2026-10-03）**：人员标识改跟官方 `user_id_type`。`COPY` 选项默认 `open_id`，另接受 `union_id` 与 `user_id`。I-3 的成功样本仍只有 `open_id`。

- [x] **I-5** 🤖 实现关联与人员字段的 `COPY` 写入
  - **前置**：I-4（契约已冻结）。
  - **内容**：按 `.specs/03_API_CONTRACT.md` §8.3 / §8.4 实现 `LINK` / `DUPLEX_LINK` / `USER` 的编码，并去掉这三列的 v0.2 Bind 拒绝。不实现附件、位置、群组，也不做 id 存在性预校验。
  - **DoD**：`LIST(VARCHAR)` 编码为 §8.4 规定的请求体；`user_id_type` 作为 `COPY` 选项，默认 `open_id`，合法值还有 `union_id` 与 `user_id`，其他值在 Bind 期拒绝；请求体始终是 `[{"id":"..."}]`，查询参数与选项一致。含 NULL 元素或空字符串元素在发请求前报错；空列表提交 `[]`；不存在或跨表的关联 id 原样提交。Bind 期不再对这三列使用“v0.2 writeback does not support”。新增离线单测覆盖三种查询参数的编码；在线 `COPY` 用 `FEISHU_WRITE_TEST_*` 验证合法关联 id 与合法 `open_id` 读回一致，并清理测试记录。`union_id` / 雇员 `user_id` 没有成功样本时，不把在线成功写成 DoD。README 只在该行为落地后改成“可以写入”。
  - **适用阶段**：v0.3+，不阻塞发布。
  - **完成证据（2026-10-03）**：`WriteTargetKind::LINK` / `USER` 编码落地；`user_id_type` COPY 选项注册且 `batch_create` 始终带查询参数；离线单测覆盖三种 query 编码与空串/NULL 拒绝；`copy_writeback.test` 覆盖非法选项；在线 `integration_copy_writeback.test` 用 bot `open_id` + 同表合法 `record_id` 读回一致并已 cleanup。`union_id` / 雇员 `user_id` 未做在线成功验证。README / `description.yml` 已改为可写。

---

## Base 离线快照语义冻结（2026-10-09）

- [x] **BT-SNAPSHOT-CONTRACT-001** 👤+🤖 冻结首期 Base 离线快照行为与契约；**不包含运行时实现或 SQL 注册**。
  - **前置依赖**：`BT-SNAPSHOT-RESEARCH-001` 已验收；用户明确批准本任务的冻结默认值。
  - **DoD**：创建权威语义契约；API / data contract 摘要与权威契约一致；现有 API 规范明确标注语义获批但未实现；正式任务记录决策与可核对证据；PROTO+ 保持等待显式实现批准。
  - **完成证据**：[权威契约](07_BASE_SNAPSHOT_CONTRACT.md) §§1–8；[API contract](../.product-dev/api-contract.json)；[data contract](../.product-dev/data-contract.json)；本文件本任务条目；`scripts/check_no_secrets.sh` 通过。
  - **冻结决策**：路径 A；默认发现开始时凭据可见的全部表，也允许稳定 `table_id` 子集；新 `.duckdb` 文件优先；严格漂移策略；目标已存在默认拒绝，显式 replace 才可 replace-all staging → validate → publish；覆盖、表示保真、源时间一致性分开报告且不声明跨表 PIT；附件仅元数据；无增量同步；只批准现有读/元数据 + CTAS 语义。
  - **完成边界**：快照运行时、C++、SQL helper 注册、StorageExtension / `ATTACH` 均未实现。

---

## Base 离线快照实现链（2026-10-13 用户批准）

> 来源：`BT-SNAPSHOT-RESEARCH-001` 与已完成的 `BT-SNAPSHOT-CONTRACT-001`。语义以
> `.specs/07_BASE_SNAPSHOT_CONTRACT.md` 为准。
> **批准范围（用户 2026-10-13）**：启动实现链 `PROTO → MANIFEST → PUBLISH → QA-MATRIX`；
> **暂不推进** `G-2` / 公开发布链；`BT-SNAPSHOT-ATTACH-EVAL-001` 仍等待单独批准，不得顺带开工。
> 首期仍禁止注册新 SQL helper；只允许现有 tables/fields/scan + DuckDB CTAS / local DDL 编排。

- [x] **BT-SNAPSHOT-PROTO-001** 🤖 使用现有 tables/fields/scan + CTAS 逐表物化原型；**已完成（2026-10-13）**
  - **执行角色**：🤖
  - **前置依赖**：`BT-SNAPSHOT-CONTRACT-001`
  - **DoD**：产出可运行的编排原型（脚本或同等），对指定 Base：发现表 → 字段元数据 → `INCLUDE_SYSTEM_COLUMNS=true` 的 CTAS 物化；本地关系名按契约稳定 ID 规则可碰撞安全；不注册新 SQL；不下载附件二进制；`scripts/check_no_secrets.sh` 通过；示例仅占位符。
  - **完成证据**：`scripts/snapshot_base_proto.py`；`--help` / `--dry-run` 可用；目标已存在默认拒绝；本地关系名=规范化显示名 + 稳定 `table_id` 后缀；不注册新 SQL；`scripts/check_no_secrets.sh` 退出 0。未做真实凭据端到端拉取（留给后续 QA）。
- [x] **BT-SNAPSHOT-MANIFEST-001** 🤖 manifest 生命周期与完整性三维度报告（覆盖 / 保真 / 一致性）；**已完成（2026-10-13）**
  - **执行角色**：🤖
  - **前置依赖**：`BT-SNAPSHOT-PROTO-001`
  - **DoD**：manifest 写入契约 §7 要求的快照级与表级事实；生命周期状态机可区分 complete/incomplete/failed/cancelled；三维度独立报告，禁止跨表 PIT 声明。
  - **完成证据**：`scripts/snapshot_base_proto.py` 在目标新文件中创建内部 `__feishu_bitable_snapshot_manifest` / `__feishu_bitable_snapshot_tables`，按 `planned → discovering → metadata_captured → materializing → validating → complete` 更新；异常收敛为 `failed` / `incomplete`，中断收敛为 `cancelled`，且未完成 staging 不发布。快照与逐表记录分别保存 §7.1/§7.2 的要求事实、读前/读后 schema fingerprint 与 revision；coverage / fidelity / source-time 三维度独立写入，source-time 状态固定说明跨表一致性 `unproven`。`python3 -m unittest scripts/test_snapshot_base_proto.py`、内部 manifest DuckDB 冒烟、`scripts/check_no_secrets.sh` 均通过；未注册新 SQL。replace-all / publish 已由后续 `BT-SNAPSHOT-PUBLISH-001` 实现。
- [x] **BT-SNAPSHOT-PUBLISH-001** 🤖 replace-all staging → 校验后发布；**已完成（2026-10-13）**
  - **执行角色**：🤖
  - **前置依赖**：`BT-SNAPSHOT-MANIFEST-001`
  - **DoD**：目标已存在默认拒绝；显式 replace 才走 staging→validate→publish；未校验 complete 不得宣称发布成功。
  - **完成证据**：`scripts/snapshot_base_proto.py` 增加 `--replace`；已有目标在未显式 replace 时拒绝。replace 在目标同目录生成隔离 staging，复用发现/CTAS/manifest 生命周期；只有快照 manifest 与所有逐表事实经关闭后重新打开校验为 `complete` 才调用发布。发布前检测目标在 staging 期间未被外部替换，随后用同文件系统 `os.replace` 发布，旧目标直到该步前保持不变。所有失败/取消均保留非 complete staging 作诊断，且不宣称成功。`scripts/test_snapshot_base_proto.py` 覆盖默认拒绝、显式 replace、未校验 staging 拒绝、并发目标变更与 manifest complete 复核；`python3 -m unittest scripts/test_snapshot_base_proto.py`、`scripts/check_no_secrets.sh` 通过。
- [x] **BT-SNAPSHOT-QA-MATRIX-001** 🤖 完成/失败/漂移矩阵验收；**已完成（2026-10-13，仅离线）**
  - **执行角色**：🤖
  - **前置依赖**：`BT-SNAPSHOT-PUBLISH-001`
  - **DoD**：覆盖契约 §10 矩阵钩子（空表/多页/漂移/权限失败/坏游标/目标存在拒绝/脱敏）。
  - **完成证据**：`scripts/test_snapshot_base_proto.py` 以注入的发现、字段、CTAS、行数与发布钩子覆盖空表终端页、多页（1001 行 / 3 页下界）、字段漂移、权限拒绝、坏 cursor、局部写失败、目标存在默认拒绝、显式 replace 的 staging → reopen validation → publish 顺序、以及非 complete 不发布。测试同时断言 complete 与 failed/incomplete 的 coverage / fidelity / source-time 三维状态；`scripts/snapshot_base_proto.py` 现会在 manifest 与 stderr 写入前脱敏已配置凭据与 Bearer/token 赋值。`python3 -m unittest scripts/test_snapshot_base_proto.py` 与 `scripts/check_no_secrets.sh` 通过。
  - **离线边界**：未读取或使用 `.env.local`，未执行 live API 冒烟；分页和失败情形为可注入的离线编排验证，真实 Feishu 端点/扩展扫描器仍保留在线验证风险。
- [ ] **BT-SNAPSHOT-ATTACH-EVAL-001** 🤖 仅在有远端多表 Catalog 需求时评估 StorageExtension/`ATTACH`（另项）；**未批准，暂缓**


---

## Base 离线快照研究与设计（2026-10-09）

- [x] **BT-SNAPSHOT-RESEARCH-001** 🤖 评审指定 Base 保存为完整 `.duckdb` 的用户流程、惰性加载与同类产品，并完成工程设计交接。
  - **执行角色**：PM 统筹，Data → Backend → QA 按依赖交接；主 Agent 汇总与状态回写。
  - **前置依赖**：现有元数据发现与单表读取能力、当前 SQL 契约和实现审查。
  - **DoD 与验收**：五条原始验收标准均通过研究/设计证据审核；八个对标来源记录主张与限制；数据、后端交接和 QA 结论齐全，总线输出校验通过。
  - **完成证据**：[设计报告](../docs/design/base-snapshot-review.md)、[QA 评审](../.product-dev/qa-review.md)、[已验收工作项](../.product-dev/work-item.json)、[总线交付记录](../.product-dev/run-output.json)。
  - **结论**：优先设计显式全 Base 快照编排，复用现有读取能力与 DuckDB 原生 CTAS；远端惰性查询和离线快照分别表达。Base `ATTACH` 留待后续需求，不能等同于自动物化。
  - **完成边界**：仅研究与设计完成。全 Base 快照编排、Base `ATTACH`、安全发布仍未实现；本轮未修改 C++，未运行构建、在线 API 或性能实测。后续实现须单独立项，不计入当前已编排待办，也不改变范围 B 的既有验收状态。
