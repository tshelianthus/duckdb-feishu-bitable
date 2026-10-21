# Base Snapshot Contract (Approved Semantics, Not Implemented)

**Status:** `approved_semantics_unimplemented`  
**Task:** `BT-SNAPSHOT-CONTRACT-001`  
**Frozen at:** 2026-10-09  
**Authority:** This file is the frozen semantic contract for the first-phase Feishu Bitable offline Base snapshot. It does **not** authorize C++ implementation, SQL registration, or release claims.

Related sources:
- Research report: `docs/design/base-snapshot-review.md`
- Product family: `.product-dev/families/base-snapshot/README.md`
- Data contract summary: `.product-dev/data-contract.json`
- API contract summary: `.product-dev/api-contract.json`
- Existing implemented SQL surface: `.specs/03_API_CONTRACT.md`

---

## 0. Non-implementation boundary

This contract freezes **behavior and meaning**. The following remain **unimplemented and unregistered**:

- No new DuckDB SQL function is approved for implementation in this phase.
- No C++ snapshot orchestrator, StorageExtension, ATTACH catalog, or publish helper is approved by this document alone.
- First-phase users and any future orchestrator must rely on the **already frozen** SQL surface:
  - `feishu_bitable_tables`
  - `feishu_bitable_fields`
  - `read_feishu_bitable` / `read_bitable` / `feishu_bitable_scan`
  - ordinary DuckDB CTAS / local DDL against a destination `.duckdb` file

Optional future helper names in §8 are **semantic placeholders only** (`status=approved_semantics_unimplemented`). Using those names in README, community description, or `duckdb_functions()` before a separate implementation approval is a contract violation.

---

## 1. Frozen defaults (BT-SNAPSHOT-CONTRACT-001)

| Decision | Frozen default |
| :--- | :--- |
| Engineering path | **A**: orchestrate existing metadata/read functions with DuckDB CTAS into local materialized tables. `StorageExtension` / read-only `ATTACH` is **out of first phase** and needs a separate evaluation (`BT-SNAPSHOT-ATTACH-EVAL-001`). |
| Snapshot scope | Default = **all tables visible under the invoking credential at discovery start**. Explicit stable `table_id` subset is also allowed. |
| Undiscovered tables | **Unknown**, not empty, not successfully copied. |
| Destination | First phase prioritizes a **new `.duckdb` file**. Writing into an already-open / existing Catalog may be considered later and is **not committed** by this contract. |
| Drift / failure policy | **Strict**. Schema drift, permission denial, bad cursor/pagination, cancel, retry exhaustion, coercion/representation failure, or local write failure makes that table incomplete; the whole snapshot **must not** be labeled `complete`. |
| Destination already exists | **Default reject**. Only an explicit replace intent may run replace-all staging → validate → publish. |
| Completeness reporting | Three independent dimensions: **record coverage**, **representation fidelity**, **source-time consistency**. Do not collapse them. |
| Cross-table time claim | **Forbidden**. Do not claim a cross-table (or even per-table) source point-in-time transaction. |
| Attachments | **Metadata only**. No binary download. No offline URL availability promise. |
| Incremental sync | **Out of scope**. First refresh/rebuild is replace-all only. |
| SQL surface for first delivery | Existing read/metadata + user/orchestrator CTAS only. New helper names are placeholders, not approved implementations. |

---

## 2. What "complete snapshot" means

A snapshot is **authenticated-visible selected-scope complete** only when all of the following hold:

1. Scope discovery finished for the requested Base under the invoking Secret.
2. Every selected table discovered at start obtained field metadata.
3. Every selected table was locally materialized with ordinary DuckDB table contents (not a remote VIEW definition alone).
4. Every selected table consumed records pages until the source terminal page (`has_more=false`), including successful empty tables with zero rows.
5. Per-table accounting (rows/pages), identity mapping, and validation finished.
6. No selected table is incomplete under the strict policy in §5.
7. Manifest lifecycle state is `complete`, and each completeness dimension in §4 is reported without contradiction.

Non-claims:

- Does **not** assert that inaccessible, hidden, deleted-after-start, or otherwise undiscoverable tables anywhere in a tenant were copied.
- Does **not** assert Feishu product-state backup (views, dashboards, automations, permissions ACLs, formulas as executable expressions, attachment bytes, etc.).
- Does **not** assert a transactional source snapshot across tables or within a table while pages were fetched.

---

## 3. Scope and discovery

### 3.1 Requested scope

Allowed request shapes:

1. **Default whole-visible-Base**: discover all tables currently returned by authenticated table listing for `<app_token>`.
2. **Explicit subset**: caller supplies one or more stable `table_id` values. Only those IDs are selected; discovery must still prove each ID is visible/readable under the same credential, or the snapshot ends incomplete/failed for that ID.

`app_token` and credentials come from the existing Secret Manager path. Examples must use placeholders only:

```sql
-- Existing implemented surface used by a future orchestrator / manual workflow.
SELECT * FROM feishu_bitable_tables('<app_token>', SECRET = '<secret_name>');
SELECT * FROM feishu_bitable_fields('<app_token>', '<table_id>', SECRET = '<secret_name>');
CREATE TABLE local_tbl_xxxx AS
SELECT * FROM read_feishu_bitable(
    '<app_token>',
    '<table_id>',
    SECRET = '<secret_name>',
    INCLUDE_SYSTEM_COLUMNS = true
);
```

### 3.2 Effective discovered scope

- Discovery timestamp is the start of snapshot discovery, not the finish of materialization.
- Tables that appear only after discovery start are outside the frozen selected scope unless a later approved refresh redesign says otherwise.
- A table absent from discovery is **unknown**, never treated as an empty successful copy.
- A discovered table whose metadata or records call is denied is a **selected-scope failure**, not "skipped success".

### 3.3 Identity basis

| Entity | Stable identity | Local naming |
| :--- | :--- | :--- |
| Base | Non-secret Base identity / opaque locator for provenance only | Destination file path chosen by caller |
| Table | Source `table_id` | Deterministic collision-safe local relation name from normalized display name + stable-ID suffix |
| Field | Source `field_id` | Deterministic collision-safe local column name; ordered schema fingerprint uses IDs/types/properties |
| Record | Source `record_id` (+ `table_id`) | Must be retained for snapshot identity/validation; first-phase scan path should use `INCLUDE_SYSTEM_COLUMNS = true` or an equivalent approved identity capture |

Display names may collide, rename, or use reserved words. Refresh and drift comparison use stable IDs, never names alone.

---

## 4. Completeness dimensions (separate)

These dimensions are independent. Success on one never implies success on another.

### 4.1 Record coverage (`scope_completeness` / record coverage)

Whether every authenticated-visible selected table and every one of its record pages was locally materialized through the terminal page.

- Empty readable tables count as covered with `rows_materialized = 0`.
- Partial paging, cancel, permission failure, bad cursor, or local write failure => not covered for that table.

### 4.2 Representation fidelity

Whether local values preserve the chosen read mapping without silent loss.

- Default mapping follows the existing Typed read semantics in `.specs/03_API_CONTRACT.md`.
- Composite / unknown / formula-returned values use JSON as returned by the records API where the existing mapper already does so.
- `SCHEMA_MODE = 'json'` may be selected explicitly; the snapshot must record which mode was used.
- Formula columns store returned results only; they do **not** store/rebuild formulas or promise offline recalculation.
- A coercion failure, unsupported representation drop, or silent truncation makes that table incomplete under the strict policy.

### 4.3 Source-time consistency

Whether source metadata/data stayed unchanged while requests ran, to the evidence available.

- Record per-table start/end times and before/after metadata observations (field schema fingerprint; revision when the API returns one).
- Before/after differences are **drift evidence**, not transactional snapshot tokens.
- Cross-table consistency is reported as **unproven** unless a future source API offers and an approved implementation uses a documented snapshot/transaction token.

---

## 5. Strict drift and failure policy

Default policy is **strict**.

Mark the affected table incomplete / failed and forbid snapshot-level `complete` when any of the following occurs for a selected table:

- permission / authorization denial on metadata or records
- malformed pagination / bad cursor / non-terminal incomplete page sequence
- schema fingerprint or observed revision differs between pre-read and post-read observations
- representation/coercion failure
- retry exhaustion
- cancellation
- local DuckDB write/validation failure

Allowed terminal outcomes for the whole snapshot:

| Outcome | Meaning |
| :--- | :--- |
| `complete` | All selected tables succeeded under §§2–5; all three dimensions report without contradiction. |
| `incomplete` | At least one selected table has partial useful local data or diagnostic residue, but the snapshot is not complete. |
| `failed` | Snapshot could not satisfy selected scope; may have little or no useful local data. |
| `cancelled` | Explicit cancel before complete validation/publish. |

Partial local data may be retained only as staging/diagnostic material and must remain labeled non-complete.

A deferred **best-effort** policy may be considered later, but is **not** the first-phase default. Any best-effort mode would still have to report the three dimensions separately and still must not claim point-in-time Base backup.

---

## 6. Destination, staging, and replace rules

### 6.1 First-phase destination

- Preferred target: a **new** local `.duckdb` file path that does not already exist.
- Opening a saved snapshot file later does **not** silently refresh Feishu data.
- Writing snapshot tables into an already populated / currently attached Catalog is a **future option only** and is not promised by this contract.

### 6.2 Destination already exists

- **Default:** reject. Do not overwrite, append, or mutate the existing file/catalog in place.
- **Explicit replace only:** allow a replace-all workflow:
  1. write a new staging database beside the target (or equivalent isolated staging location),
  2. discover → materialize → validate under this contract,
  3. publish only after validation says `complete` (or an explicitly requested non-complete diagnostic publish design is separately approved — first phase should not soft-publish incomplete as the target),
  4. leave the previous target untouched until publish succeeds.

Atomic cross-platform file replacement, crash recovery, and open-handle behavior are **implementation risks to verify later**; this contract does not invent a guarantee beyond "do not claim publish success before validation and safe handle/checkpoint close".

### 6.3 Refresh

- First implementation refresh mode: **explicit replace-all** into new staging, then publish on complete validation.
- Incremental / append / delete CDC sync is **deferred** and must not be inferred from metadata revision values.
- `created_at` / `successful_refresh_at` are manifest facts only.

---

## 7. Local layout and manifest (semantic, not SQL API)

Manifest relation names are **internal semantic requirements**, not a frozen user SQL API in this phase.

### 7.1 Snapshot-level required facts

- `snapshot_id`
- `contract_version`
- `requested_scope`
- `effective_discovered_scope`
- `credential_mode_without_secret_value`
- `schema_mode_or_mapping_profile`
- `started_at`
- `finished_at_or_null`
- `lifecycle_state`
- `record_coverage_status`
- `representation_fidelity_status`
- `source_time_consistency_status`
- `refresh_parent_snapshot_id_or_null`
- `failure_summary_sanitized`

### 7.2 Per-table required facts

- `table_id`
- `source_display_name`
- `local_relation_name`
- `discovery_status`
- `metadata_started_at` / `metadata_finished_at`
- `records_started_at_or_null` / `records_finished_at_or_null`
- `rows_materialized`
- `pages_consumed`
- `observed_revision_before_or_null` / `observed_revision_after_or_null`
- `field_schema_fingerprint_before` / `field_schema_fingerprint_after_or_null`
- `state`
- `sanitized_error_or_null`

### 7.3 Lifecycle states

`planned` → `discovering` → `metadata_captured` → `materializing` → `validating` → `complete`  
plus terminal `cancelled` / `failed` / `incomplete`.

Transition rules:

- Cancellation, retry exhaustion, permission failure, malformed pagination, representation failure, or local write failure ends in `cancelled`, `failed`, or `incomplete` and must not assign `complete`.
- Record page tokens belong only to the active request sequence. They are diagnostics at most and are **not** restart-safe checkpoints after process restart or credential change.
- Staging data published early, or partial staging retained for diagnosis, must keep a non-complete manifest state.

### 7.4 Secrets and attachments

- Never store `app_id`, `app_secret`, `user_access_token`, bearer tokens, or raw Secret Manager values in the snapshot file, manifest, logs, or error text.
- Attachment field JSON returned by the records API (token/name/type/size/URL when supplied) may be retained as ordinary field data.
- Do **not** download attachment binaries.
- Do **not** promise that a returned URL remains usable offline.
- The local file may still contain business/personal data; filesystem access control is the deployer's responsibility.

---

## 8. SQL surface status

### 8.1 Approved for first-phase use (already implemented elsewhere)

| Entry | Role in snapshot workflow |
| :--- | :--- |
| `feishu_bitable_tables` | Discover authenticated-visible tables |
| `feishu_bitable_fields` | Capture per-table field metadata / fingerprints |
| `read_feishu_bitable` / `read_bitable` / `feishu_bitable_scan` | Read selected table records for CTAS materialization |
| DuckDB CTAS / local DDL | Create durable local tables inside the destination `.duckdb` |

### 8.2 Future optional helpers (placeholders only)

The following names are reserved as **semantic placeholders** for a later implementation approval. They are **not** registered, not callable, and not part of the current public catalog.

| Placeholder name | Intended role | Status |
| :--- | :--- | :--- |
| `feishu_bitable_snapshot_base` | Orchestrate discover → materialize → validate → publish for one Base/new file | `approved_semantics_unimplemented` |
| `feishu_bitable_snapshot_status` | Read manifest / completeness dimensions for a snapshot file or snapshot_id | `approved_semantics_unimplemented` |

Any implementation task that wants to register these (or different) names must obtain a separate approval after this contract and must update `.specs/03_API_CONTRACT.md` with real signatures before coding.

### 8.3 Explicitly not approved by this contract

- `ATTACH '...' (TYPE feishu_bitable)` / StorageExtension catalog browsing as the offline snapshot mechanism
- incremental sync SQL
- attachment binary fetch SQL
- claiming VIEW persistence as a complete snapshot

---

## 9. Cost and memory expectations (non-benchmark promises)

- Record request baseline for selected non-empty tables, when source is stable, pages are full, page size is constant, and retries are absent: sum over tables of `ceil(records / effective_page_size)`.
- Add paginated table discovery and per-table field discovery; empty tables still need at least one records request to observe termination.
- Retries, short pages, drift rechecks, and source changes increase request count and duration.
- Memory target: preserve bounded page/vector buffering; do not accumulate an entire Base in memory before writing.
- File size, wall time, and rate limits require representative measurement before any numeric promise.

---

## 10. Acceptance matrix hooks for later tasks

Future implementation/QA tasks (`BT-SNAPSHOT-PROTO-001`, `BT-SNAPSHOT-MANIFEST-001`, `BT-SNAPSHOT-PUBLISH-001`, `BT-SNAPSHOT-QA-MATRIX-001`) must cover at least:

- table/field pagination and empty tables
- multi-page records, missing-field NULL, composite/unknown/formula-returned values
- renamed/duplicate local names and field drift
- permission denial, bad cursor, retry exhaustion, cancel, local write failure
- complete vs incomplete manifest reporting across the three dimensions
- destination-exists default reject and explicit replace staging→validate→publish
- checkpoint/handle close and reopen validation before publish success claims
- secret redaction and `scripts/check_no_secrets.sh` on all examples

No item above is satisfied by this contract document alone.

---

## 11. Change control

- Changing any frozen default in §1 requires a new contract task and explicit user/PM approval.
- Research documents remain historical evidence; if they disagree with this file on a frozen default, **this file wins** until revised.
- Implementation PRs that add SQL names, ATTACH, incremental sync, or binary attachments without updating this contract and `.specs/03_API_CONTRACT.md` are out of scope.
