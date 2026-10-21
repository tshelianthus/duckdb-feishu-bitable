# Testing this extension
This directory contains all the tests for this extension. The `sql` directory holds tests that are written as [SQLLogicTests](https://duckdb.org/dev/sqllogictest/intro.html). DuckDB aims to have most its tests in this format as SQL statements, so for the quack extension, this should probably be the goal too.

The root makefile contains targets to build and run all of these tests. To run the SQLLogicTests:
```bash
make test
```
or 
```bash
make test_debug
```

## Online test failures and environment gates (F-1)

Both `integration_first_table.test` and `integration_pushdown.test` keep all
`require-env` gates before any SQL. Missing variables skip the whole file with
`require-env <variable>` in the summary and exit code 0. An exported empty value
counts as present to the runner; it does not mean the credential is valid.

After those gates, `set ignore_error_messages` (with no values) clears the
runner's error-message skip list for the rest of that file. All existing
`statement ok` and `query` checks therefore fail on unexpected request errors,
including errors in later scans, with the original error and a nonzero exit code.

DuckDB v1.5.5 defaults to skipping errors containing `HTTP` (also matching
`HTTPS`) or `Unable to connect`. See `TestConfiguration::ErrorMessagesToBeSkipped`
in `duckdb/test/helpers/test_config.cpp`, `TestResultHelper::SkipErrorMessage` in
`duckdb/test/sqlite/result_helper.cpp`, and the `SQLLOGIC_SET` branch in
`duckdb/test/sqlite/sqllogic_test_runner.cpp`. The latter explicitly clears the
list before parsing optional values; `ExecuteFile` reloads defaults for each
file. This is a runner directive, not SQL `SET`. No connectivity probe is needed;
`halt` alone only stops parsing and does not mark a test failed.

Run the two files directly with an existing Debug runner from the repository
root (this bypasses the Makefile's `.env.local` loading). Use exact test names:

```bash
# Expected: exit 0, both files skipped for a missing APP_ID / APP_SECRET gate.
env -u FEISHU_TEST_APP_ID -u FEISHU_TEST_APP_SECRET \
  -u FEISHU_TEST_APP_TOKEN -u FEISHU_TEST_URL \
  build/debug/test/unittest 'test/sql/integration_first_table.test,test/sql/integration_pushdown.test'

# Deliberately invalid placeholders; no real credentials are needed.
# Expected: nonzero exit, both tests FAILED with the underlying request error.
env FEISHU_TEST_APP_ID=f1-invalid-app \
  FEISHU_TEST_APP_SECRET=f1-invalid-secret \
  FEISHU_TEST_APP_TOKEN=bascnXXXXXXXX \
  FEISHU_TEST_URL=https://example.feishu.cn/base/bascnXXXXXXXX \
  build/debug/test/unittest 'test/sql/integration_first_table.test,test/sql/integration_pushdown.test'
```

Verified on 2026-09-11 with the existing Debug runner: before the change, both
connection failures were skipped on `HTTP`, exit 0; after the change, both HTTPS
connection failures and live Feishu HTTP 400 responses produced two failed tests,
exit 2, with `Query unexpectedly failed!` and the underlying error. Missing each
required variable still skipped the affected files, exit 0. This verifies failure
and gating behavior; it is not evidence of a successful authenticated online run.
