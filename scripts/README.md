# E-2.6 API fact probe

`e2_6_api_facts.sh` probes the Feishu Bitable records endpoints directly with
`curl` and `jq`; it does not load or modify the DuckDB extension. Use a
dedicated disposable write-test Base and table, never the read regression Base.

The script requires these environment variables:

- `FEISHU_E26_APP_TOKEN`, `FEISHU_E26_TABLE_ID`
- `FEISHU_E26_WRITABLE_FIELD`: an existing writable text-like field
- `FEISHU_E26_READONLY_FIELD`: an existing formula, auto-number, or system field
- One primary write identity: `FEISHU_E26_WRITE_APP_ID` plus
  `FEISHU_E26_WRITE_APP_SECRET`, or `FEISHU_E26_WRITE_USER_ACCESS_TOKEN`

Run the probe once with the permitted application identity and once with the
permitted user access token to cover both primary credential modes. The
optional read-only and non-collaborator identities can be supplied on either
run; missing optional identities are reported as `SKIPPED`, never as passes.

Optional identities enable additional rejection probes. If absent, the case is
reported as `SKIPPED`, never as a pass:

- `FEISHU_E26_READONLY_APP_ID` plus `FEISHU_E26_READONLY_APP_SECRET`: an app
  with only the read-only Bitable scope.
- `FEISHU_E26_NONCOLLAB_APP_ID` plus `FEISHU_E26_NONCOLLAB_APP_SECRET`: an app
  that is not a collaborator of the dedicated Base.
- `FEISHU_E26_NONCOLLAB_USER_ACCESS_TOKEN`: a user that is not a collaborator.

`FEISHU_E26_API_BASE_URL` is optional and defaults to the official Feishu API
origin. The script validates the target path identifiers and uses HTTPS only.

Run it from the repository root after exporting values in the calling shell:

```sh
scripts/e2_6_api_facts.sh
```

Output is limited to a case label, HTTP status, Feishu `code`, redacted `msg`,
and `data.records` count/cursor evidence. Request payloads, response bodies,
tokens, app secrets, and `Authorization` headers are never printed. The probe
creates marker records in batches of at most 500 and attempts `batch_delete`
cleanup on normal completion and on abnormal exit. A cleanup failure is
reported as an API evidence line and gives the script a non-zero exit status.

The script intentionally records boundary behavior without assuming whether
`batch_update` accepts 500, 501, 1000, or 1001 records. It does not update
`.specs/06_WRITEBACK_RESEARCH.md`; observed results must be reviewed before
that document is changed.

The protected writeback workflow uses `cleanup_write_test_records.sh` before
and after the SQLLogicTest run. It requires the four `FEISHU_WRITE_TEST_*`
credential/target variables (`APP_ID`, `APP_SECRET`, `APP_TOKEN`, `TABLE_ID`)
plus the non-secret `FEISHU_WRITE_TEST_PREFIX`, and deletes only text-field
values beginning with that prefix from the dedicated table. The script
exchanges the app credentials for a `tenant_access_token` itself and never
prints the credentials or the token. The I-5 USER round-trip also needs
`FEISHU_WRITE_TEST_OPEN_ID` in the test job (cleanup does not use it); that
value is stored as a `write-tests` environment secret for log redaction, not
because it authenticates.
