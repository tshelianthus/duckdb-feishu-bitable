# 本地 CLI 冒烟：只用打包好的 release DuckDB

把本文件交给本地 agent。目标是在仓库根目录用已经打进 DuckDB 的 release 程序做一次离线 CLI 冒烟。不要访问飞书，不要使用真实凭据。

## 用哪个程序

用这个：

```text
./build/release/duckdb
```

这是打包版本。扩展已经链进这个可执行文件，启动后函数就在，不需要 `LOAD`。

不要用这些：

- 不要 `LOAD` `feishu_bitable.duckdb_extension`，无论它在 `build/release/extension/` 还是 `build/debug/extension/`。
- 不要加 `-unsigned`。那个参数是为了加载外部扩展文件。
- 不要改用 `./build/debug/duckdb`。debug 构建没准备好时，去编 release，不要拿 debug 代替。
- 不要运行 `make test_unit`、`make test_debug`、`make test_release`、`make test_sql_online_debug`。这次只测 CLI。

## 不要做的事

- 不要读、改、提交 `.env.local`，也不要在命令行或日志里写真实的 `app_id`、`app_secret`、`app_token`、`table_id`、访问令牌或租户域名。
- 不要发网络请求。下面每条 SQL 都必须在访问飞书之前结束。
- 不要 `git commit`、`git push`、打 tag、改仓库可见性，也不要提社区收录 PR。
- 报错原文里如果意外出现凭据，先打码再写入报告。

## 准备 release 程序

在仓库根目录执行。已有 `./build/release/duckdb` 且是这次源码编出来的，就不要重编。

子模块和 vcpkg 按 `README.md` 的「本地构建」准备：

```sh
git submodule update --init --recursive
```

`VCPKG_TOOLCHAIN_PATH` 必须指向本机的 `vcpkg/scripts/buildsystems/vcpkg.cmake`。没有这个变量时，按 README 克隆并 bootstrap vcpkg 2025.12.12，然后导出变量。不要猜路径。

然后：

```sh
export GEN=ninja
export BUILD_WORKERS=2
make release
```

`ninja` 不存在时，去掉 `GEN=ninja`，只保留 `BUILD_WORKERS=2` 再执行 `make release`。链接被系统杀掉时，不要提高并行度，保持 `BUILD_WORKERS=2` 重跑。

编完后确认文件存在：

```sh
test -x ./build/release/duckdb
```

下面所有命令的程序都是 `./build/release/duckdb`。某条命令退出码或报错文本不符合预期时，停下来，把命令、退出码和完整输出写进报告。不要改用扩展文件把失败补成成功。

## 1. 扩展已经在进程里

```sh
./build/release/duckdb -c "SELECT version();"
```

期望：退出码 0，版本是 `v1.5.5`。

```sh
./build/release/duckdb -c "SELECT function_name FROM duckdb_functions() WHERE function_name IN ('feishu_bitable_tables','feishu_bitable_fields','feishu_bitable_scan','read_feishu_bitable','read_bitable') GROUP BY function_name ORDER BY function_name;"
```

期望：退出码 0，五行按这个顺序出现：

```text
feishu_bitable_fields
feishu_bitable_scan
feishu_bitable_tables
read_bitable
read_feishu_bitable
```

```sh
./build/release/duckdb -c "SELECT feishu_bitable('world');"
```

期望：退出码 0，结果是 `feishu_bitable ready: world`。`feishu_bitable(VARCHAR)` 只是内部加载探针，不是公开查询接口。

这三步里都没有 `LOAD`。函数列表少任何一行，就判定打包版本没有带上扩展，停止。

## 2. Secret 能建，密钥不回显

```sh
./build/release/duckdb -c "CREATE TEMPORARY SECRET feishu_valid (TYPE feishu, APP_ID 'cli_test_app', APP_SECRET 'super-secret-value'); SELECT type, provider, secret_string NOT LIKE '%super-secret-value%' AS secret_redacted FROM duckdb_secrets() WHERE name = 'feishu_valid'; SELECT sum(entry = 'app_secret=redacted') FROM (SELECT unnest(split(secret_string, ';')) AS entry FROM duckdb_secrets() WHERE name = 'feishu_valid');"
```

期望：退出码 0。`type` 是 `feishu`，`provider` 是 `config`，`secret_redacted` 是 `true`，最后的合计是 `1`。输出里不能出现 `super-secret-value`。

缺 `APP_ID` 必须失败：

```sh
./build/release/duckdb -c "CREATE TEMPORARY SECRET feishu_missing_id (TYPE feishu, APP_SECRET 'never-shown');"
```

期望：退出码非 0，报错包含 `Feishu secret requires non-empty APP_ID`，并且不包含 `never-shown`。

## 3. 读入口在联网前就被拦住

非法域名：

```sh
./build/release/duckdb -c "SELECT * FROM read_feishu_bitable('https://example.com/base/bascn123456');"
```

期望：退出码非 0，报错包含 `input is not hosted on a Feishu or Lark domain`。

合法形状的 URL，但会话里没有 Secret：

```sh
./build/release/duckdb -c "SELECT * FROM read_feishu_bitable('https://sample.feishu.cn/base/bascn123456?table=tbl789012&view=vew345678');"
```

期望：退出码非 0，报错包含 `No feishu secret is available`。这表示 URL 已经通过解析，并在凭据这一步停下。

## 4. COPY 格式已注册，同样不联网

未知选项：

```sh
./build/release/duckdb -c "COPY (SELECT 1 AS id) TO 'bascnXXXXXXXX' (FORMAT feishu_bitable, not_an_option 'x');"
```

期望：退出码非 0，报错包含 `Unrecognized option` 和 `not_an_option`。

选项合法，但没有 Secret：

```sh
./build/release/duckdb -c "COPY (SELECT 1 AS id) TO 'bascnXXXXXXXX' (FORMAT feishu_bitable, table_id 'tblYYYYYYYY', user_id_type 'open_id');"
```

期望：退出码非 0，报错包含 `feishu_bitable: no TYPE feishu secret is available`。出现 `COPY TO is not supported for FORMAT` 则失败。

批次上限：

```sh
./build/release/duckdb -c "COPY (SELECT 1 AS id) TO 'bascnXXXXXXXX' (FORMAT feishu_bitable, batch_size 501);"
```

期望：退出码非 0，报错包含 `batch_size` 和 `between 1 and 500`。

## 报告

用这个结构回复，不要贴构建日志全文：

```text
程序: ./build/release/duckdb
是否执行过 LOAD: 否
version: v1.5.5 或实际输出
函数列表: 通过或缺失的名字
feishu_bitable('world'): 实际单元格
Secret 脱敏: 通过或失败
缺 APP_ID: 退出码和报错中的那一句
非法域名: 退出码和报错中的那一句
合法 URL 无 Secret: 退出码和报错中的那一句
COPY 未知选项: 退出码和报错中的那一句
COPY 无 Secret: 退出码和报错中的那一句
batch_size 501: 退出码和报错中的那一句
结论: 通过 / 失败
```

八项检查都符合上面的期望，结论才是通过。这次通过只说明打包版本能启动、SQL 入口和写入口在本地被正确拦住。它不是飞书在线读写测试。
