#!/usr/bin/env python3
"""Materialize a Feishu Bitable Base into a DuckDB file with an internal manifest.

Only existing metadata/read functions plus CTAS are used. Existing destinations
are never changed unless --replace is explicit; replacement stages beside the
target and publishes only a reopened, complete manifest. This script does not
register SQL helpers or download attachment binaries.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import subprocess
import sys
import unicodedata
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
TEMPORARY_SECRET_NAME = "snapshot_proto_feishu"
MANIFEST_RELATION = "__feishu_bitable_snapshot_manifest"
TABLE_MANIFEST_RELATION = "__feishu_bitable_snapshot_tables"
MANIFEST_CONTRACT_VERSION = "BT-SNAPSHOT-CONTRACT-001@2026-10-09"
RECORD_PAGE_SIZE = 500
SENSITIVE_ENVIRONMENT_VARIABLES = (
    "FEISHU_SNAPSHOT_USER_ACCESS_TOKEN",
    "FEISHU_TEST_USER_ACCESS_TOKEN",
    "FEISHU_USER_ACCESS_TOKEN",
    "FEISHU_SNAPSHOT_APP_ID",
    "FEISHU_TEST_APP_ID",
    "FEISHU_APP_ID",
    "FEISHU_SNAPSHOT_APP_SECRET",
    "FEISHU_TEST_APP_SECRET",
    "FEISHU_APP_SECRET",
)


class SnapshotError(Exception):
    """An expected failure whose message is safe to persist and print."""


def file_identity(path: Path) -> Tuple[int, int, int, int]:
    """Return enough target metadata to detect an unsafe concurrent replacement."""
    try:
        status = path.stat()
    except OSError as exc:
        raise SnapshotError("cannot inspect destination before publish") from exc
    return status.st_dev, status.st_ino, status.st_size, status.st_mtime_ns


def sql_string(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def sql_identifier(value: str) -> str:
    return '"' + value.replace('"', '""') + '"'


def utc_timestamp() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def canonical_json(value: object) -> str:
    return json.dumps(value, ensure_ascii=True, separators=(",", ":"), sort_keys=True)


def schema_fingerprint(fields: List[Dict[str, object]]) -> str:
    """Hash ordered source field IDs/types/properties, excluding display names."""
    fingerprint_rows = []
    for field in fields:
        field_id = field.get("field_id")
        type_id = field.get("type_id")
        if not isinstance(field_id, str) or not field_id or not isinstance(type_id, int):
            raise SnapshotError("field metadata capture returned an invalid schema identity")
        property_json = field.get("property_json")
        if property_json is None:
            property_value = None
        elif isinstance(property_json, str):
            try:
                property_value = json.loads(property_json)
            except json.JSONDecodeError as exc:
                raise SnapshotError("field metadata capture returned invalid property JSON") from exc
        else:
            property_value = property_json
        fingerprint_rows.append({"field_id": field_id, "property": property_value, "type_id": type_id})
    return hashlib.sha256(canonical_json(fingerprint_rows).encode("utf-8")).hexdigest()


def derived_pages_consumed(rows_materialized: int) -> int:
    """Return the full-page lower bound; raw scan page telemetry is not public."""
    if rows_materialized < 0:
        raise SnapshotError("local row accounting returned a negative value")
    return max(1, math.ceil(rows_materialized / RECORD_PAGE_SIZE))


def load_env_local(path: Path) -> bool:
    if not path.is_file():
        return False
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise SnapshotError("cannot read .env.local") from exc
    for line in lines:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:].lstrip()
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key):
            continue
        if len(value) >= 2 and value[0] == value[-1] and value[0] in {"'", '"'}:
            value = value[1:-1]
        os.environ.setdefault(key, value)
    return True


def first_environment_value(names: Iterable[str]) -> Optional[str]:
    for name in names:
        value = os.environ.get(name)
        if value:
            return value
    return None


def sanitize_error_summary(summary: str) -> str:
    """Remove configured credentials and common token assignments from diagnostics."""
    sanitized = str(summary)
    for variable_name in SENSITIVE_ENVIRONMENT_VARIABLES:
        secret_value = os.environ.get(variable_name)
        if secret_value:
            sanitized = sanitized.replace(secret_value, "<redacted>")
    sanitized = re.sub(r"(?i)(bearer\s+)[^\s,;]+", r"\1<redacted>", sanitized)
    return re.sub(
        r"(?i)((?:app_id|app_secret|user_access_token|tenant_access_token|access_token|token)\s*[=:]\s*)"
        r"(?:'[^']*'|\"[^\"]*\"|[^\s,;]+)",
        r"\1<redacted>",
        sanitized,
    )


def credential_sql(secret_name: Optional[str]) -> Tuple[str, str, str]:
    """Return setup SQL, read clause, and a secret-free credential mode."""
    if secret_name:
        return "", ", SECRET = " + sql_string(secret_name), "named_secret"
    user_access_token = first_environment_value(
        ("FEISHU_SNAPSHOT_USER_ACCESS_TOKEN", "FEISHU_TEST_USER_ACCESS_TOKEN", "FEISHU_USER_ACCESS_TOKEN")
    )
    if user_access_token:
        return (
            "CREATE TEMPORARY SECRET "
            + sql_identifier(TEMPORARY_SECRET_NAME)
            + " (TYPE feishu, USER_ACCESS_TOKEN "
            + sql_string(user_access_token)
            + ");",
            ", SECRET = " + sql_string(TEMPORARY_SECRET_NAME),
            "user_access_token",
        )
    app_id = first_environment_value(("FEISHU_SNAPSHOT_APP_ID", "FEISHU_TEST_APP_ID", "FEISHU_APP_ID"))
    app_secret = first_environment_value(
        ("FEISHU_SNAPSHOT_APP_SECRET", "FEISHU_TEST_APP_SECRET", "FEISHU_APP_SECRET")
    )
    if bool(app_id) != bool(app_secret):
        raise SnapshotError("application credentials are incomplete; provide both APP_ID and APP_SECRET")
    if app_id and app_secret:
        return (
            "CREATE TEMPORARY SECRET "
            + sql_identifier(TEMPORARY_SECRET_NAME)
            + " (TYPE feishu, APP_ID "
            + sql_string(app_id)
            + ", APP_SECRET "
            + sql_string(app_secret)
            + ");",
            ", SECRET = " + sql_string(TEMPORARY_SECRET_NAME),
            "application_credentials",
        )
    return "", "", "default_secret_manager"


def normalize_display_name(display_name: object) -> str:
    value = unicodedata.normalize("NFKC", str(display_name or ""))
    value = re.sub(r"[^A-Za-z0-9]+", "_", value).strip("_").lower()
    return value or "table"


def stable_table_id_suffix(table_id: str) -> str:
    if re.fullmatch(r"[A-Za-z0-9_]+", table_id):
        return "id_" + table_id
    return "id_hex_" + table_id.encode("utf-8").hex()


def local_relation_name(display_name: object, table_id: str) -> str:
    return normalize_display_name(display_name) + "__" + stable_table_id_suffix(table_id)


def default_binary(relative_path: str, fallback: str) -> str:
    candidate = REPOSITORY_ROOT / relative_path
    return str(candidate) if candidate.is_file() else fallback


def run_duckdb(duckdb_binary: str, database: str, sql: str, stage: str, json_output: bool = False) -> str:
    command = [duckdb_binary, "-unsigned"]
    if json_output:
        command.append("-json")
    command.append(database)
    try:
        completed = subprocess.run(
            command, input=sql, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False
        )
    except OSError as exc:
        raise SnapshotError(stage + " could not start DuckDB") from exc
    if completed.returncode != 0:
        raise SnapshotError(stage + " failed; DuckDB did not complete the requested operation")
    return completed.stdout


def query_json(duckdb_binary: str, database: str, sql: str, stage: str) -> List[Dict[str, object]]:
    stdout = run_duckdb(duckdb_binary, database, sql + ";\n", stage, json_output=True)
    try:
        data = json.loads(stdout)
    except json.JSONDecodeError as exc:
        raise SnapshotError(stage + " produced unreadable metadata") from exc
    if not isinstance(data, list) or any(not isinstance(row, dict) for row in data):
        raise SnapshotError(stage + " produced unexpected metadata")
    return data


def query_metadata_json(
    duckdb_binary: str, extension_path: str, credential_setup: str, query: str, stage: str
) -> List[Dict[str, object]]:
    prelude = "LOAD " + sql_string(extension_path) + ";\n"
    if credential_setup:
        prelude += credential_setup + "\n"
    return query_json(duckdb_binary, ":memory:", prelude + query, stage)


def validate_tables(rows: List[Dict[str, object]], requested_table_ids: List[str]) -> List[Dict[str, object]]:
    by_id: Dict[str, Dict[str, object]] = {}
    for row in rows:
        table_id = row.get("table_id")
        if not isinstance(table_id, str) or not table_id:
            raise SnapshotError("table discovery returned an invalid table identity")
        if table_id in by_id:
            raise SnapshotError("table discovery returned a duplicate table identity")
        by_id[table_id] = row
    if requested_table_ids:
        if any(table_id not in by_id for table_id in requested_table_ids):
            raise SnapshotError("one or more requested table IDs were not visible during discovery")
        selected = [by_id[table_id] for table_id in requested_table_ids]
    else:
        selected = list(rows)
    used_names: Dict[str, str] = {}
    for row in selected:
        table_id = row["table_id"]
        assert isinstance(table_id, str)
        local_name = local_relation_name(row.get("name"), table_id)
        if local_name in used_names and used_names[local_name] != table_id:
            raise SnapshotError("local relation-name collision detected during planning")
        used_names[local_name] = table_id
        row["local_relation_name"] = local_name
    return selected


class SnapshotManifest:
    """Write the contract's semantic manifest relations without a public SQL API."""

    def __init__(self, duckdb_binary: str, database: Path, snapshot_id: str):
        self.duckdb_binary = duckdb_binary
        self.database = database
        self.snapshot_id = snapshot_id

    def execute(self, sql: str, stage: str = "manifest update") -> None:
        run_duckdb(self.duckdb_binary, str(self.database), sql + "\nCHECKPOINT;\n", stage)

    def initialize(self, requested_scope: str, credential_mode: str, schema_mode: str, started_at: str) -> None:
        self.execute(
            "CREATE TABLE " + sql_identifier(MANIFEST_RELATION) + " ("
            "snapshot_id VARCHAR PRIMARY KEY, contract_version VARCHAR NOT NULL, requested_scope VARCHAR NOT NULL, "
            "effective_discovered_scope VARCHAR NOT NULL, credential_mode_without_secret_value VARCHAR NOT NULL, "
            "schema_mode_or_mapping_profile VARCHAR NOT NULL, started_at VARCHAR NOT NULL, finished_at_or_null VARCHAR, "
            "lifecycle_state VARCHAR NOT NULL, record_coverage_status VARCHAR NOT NULL, "
            "representation_fidelity_status VARCHAR NOT NULL, source_time_consistency_status VARCHAR NOT NULL, "
            "refresh_parent_snapshot_id_or_null VARCHAR, failure_summary_sanitized VARCHAR);\n"
            "CREATE TABLE " + sql_identifier(TABLE_MANIFEST_RELATION) + " ("
            "snapshot_id VARCHAR NOT NULL, table_id VARCHAR NOT NULL, source_display_name VARCHAR, "
            "local_relation_name VARCHAR NOT NULL, discovery_status VARCHAR NOT NULL, metadata_started_at VARCHAR, "
            "metadata_finished_at VARCHAR, records_started_at_or_null VARCHAR, records_finished_at_or_null VARCHAR, "
            "rows_materialized BIGINT, pages_consumed BIGINT, page_count_basis VARCHAR, "
            "observed_revision_before_or_null VARCHAR, observed_revision_after_or_null VARCHAR, "
            "field_schema_fingerprint_before VARCHAR, field_schema_fingerprint_after_or_null VARCHAR, state VARCHAR NOT NULL, "
            "record_coverage_status VARCHAR NOT NULL, representation_fidelity_status VARCHAR NOT NULL, "
            "source_time_consistency_status VARCHAR NOT NULL, sanitized_error_or_null VARCHAR, "
            "PRIMARY KEY (snapshot_id, table_id));\n"
            "INSERT INTO " + sql_identifier(MANIFEST_RELATION) + " VALUES ("
            + ", ".join(
                [
                    sql_string(self.snapshot_id), sql_string(MANIFEST_CONTRACT_VERSION), sql_string(requested_scope),
                    sql_string("[]"), sql_string(credential_mode), sql_string(schema_mode), sql_string(started_at),
                    "NULL", sql_string("planned"), sql_string("not_started"), sql_string("not_started"),
                    sql_string("unproven"), "NULL", "NULL",
                ]
            ) + ");",
            "manifest initialization",
        )

    def set_snapshot(self, state: str, **values: Optional[str]) -> None:
        assignments = ["lifecycle_state = " + sql_string(state)]
        for column, value in values.items():
            assignments.append(sql_identifier(column) + " = " + ("NULL" if value is None else sql_string(value)))
        self.execute(
            "UPDATE " + sql_identifier(MANIFEST_RELATION) + " SET " + ", ".join(assignments)
            + " WHERE snapshot_id = " + sql_string(self.snapshot_id) + ";"
        )

    def insert_table(self, row: Dict[str, object]) -> None:
        table_id = row["table_id"]
        local_name = row["local_relation_name"]
        assert isinstance(table_id, str)
        assert isinstance(local_name, str)
        revision = row.get("revision")
        self.execute(
            "INSERT INTO " + sql_identifier(TABLE_MANIFEST_RELATION) + " ("
            "snapshot_id, table_id, source_display_name, local_relation_name, discovery_status, "
            "observed_revision_before_or_null, state, record_coverage_status, representation_fidelity_status, "
            "source_time_consistency_status) VALUES ("
            + ", ".join(
                [
                    sql_string(self.snapshot_id), sql_string(table_id),
                    "NULL" if row.get("name") is None else sql_string(str(row.get("name"))), sql_string(local_name),
                    sql_string("discovered"), "NULL" if revision is None else sql_string(str(revision)),
                    sql_string("discovering"), sql_string("not_started"), sql_string("not_started"),
                    sql_string("unproven"),
                ]
            ) + ");"
        )

    def update_table(self, table_id: str, **values: Optional[object]) -> None:
        assignments = []
        for column, value in values.items():
            encoded = "NULL" if value is None else (str(value) if isinstance(value, int) else sql_string(str(value)))
            assignments.append(sql_identifier(column) + " = " + encoded)
        self.execute(
            "UPDATE " + sql_identifier(TABLE_MANIFEST_RELATION) + " SET " + ", ".join(assignments)
            + " WHERE snapshot_id = " + sql_string(self.snapshot_id) + " AND table_id = " + sql_string(table_id) + ";"
        )

    def terminal_failure(self, state: str, summary: str, table_id: Optional[str]) -> None:
        summary = sanitize_error_summary(summary)
        if table_id:
            self.update_table(
                table_id, state=state, record_coverage_status="cancelled" if state == "cancelled" else "not_covered",
                representation_fidelity_status="not_complete", source_time_consistency_status="unproven",
                sanitized_error_or_null=summary,
            )
        self.set_snapshot(
            state, finished_at_or_null=utc_timestamp(), record_coverage_status="not_complete",
            representation_fidelity_status="not_complete", source_time_consistency_status="unproven",
            failure_summary_sanitized=summary,
        )


def materialize_table(
    duckdb_binary: str, extension_path: str, credential_setup: str, secret_clause: str, app_token: str,
    database: Path, table_id: str, local_name: str, schema_mode: str,
) -> None:
    sql_parts = ["LOAD " + sql_string(extension_path) + ";"]
    if credential_setup:
        sql_parts.append(credential_setup)
    scan = (
        "read_feishu_bitable(" + sql_string(app_token) + ", " + sql_string(table_id) + secret_clause
        + ", SCHEMA_MODE = " + sql_string(schema_mode) + ", PAGE_SIZE = " + str(RECORD_PAGE_SIZE)
        + ", INCLUDE_SYSTEM_COLUMNS = true)"
    )
    sql_parts.append("CREATE TABLE " + sql_identifier(local_name) + " AS SELECT * FROM " + scan + ";")
    run_duckdb(duckdb_binary, str(database), "\n".join(sql_parts) + "\nCHECKPOINT;\n", "materialization")


def local_row_count(duckdb_binary: str, database: Path, local_name: str) -> int:
    rows = query_json(
        duckdb_binary, str(database), "SELECT count(*) AS rows_materialized FROM " + sql_identifier(local_name),
        "local row accounting",
    )
    if len(rows) != 1 or not isinstance(rows[0].get("rows_materialized"), int):
        raise SnapshotError("local row accounting produced an invalid count")
    return rows[0]["rows_materialized"]


def table_lookup(rows: List[Dict[str, object]]) -> Dict[str, Dict[str, object]]:
    return {row["table_id"]: row for row in rows if isinstance(row.get("table_id"), str) and row["table_id"]}


def staging_path_for(output_path: Path) -> Path:
    """Keep staging beside the target so replacement remains on one filesystem."""
    return output_path.parent / ("." + output_path.name + ".snapshot-staging-" + uuid.uuid4().hex + ".duckdb")


def validate_complete_snapshot(duckdb_binary: str, staging_path: Path, snapshot_id: str) -> None:
    """Reopen and verify the persisted manifest before a staging file can publish."""
    manifest_rows = query_json(
        duckdb_binary,
        str(staging_path),
        "SELECT snapshot_id, lifecycle_state, record_coverage_status, "
        "representation_fidelity_status, source_time_consistency_status, finished_at_or_null "
        "FROM " + sql_identifier(MANIFEST_RELATION) + " WHERE snapshot_id = " + sql_string(snapshot_id),
        "complete manifest validation",
    )
    if len(manifest_rows) != 1:
        raise SnapshotError("complete manifest validation could not find exactly one snapshot")
    manifest_row = manifest_rows[0]
    expected_snapshot = {
        "snapshot_id": snapshot_id,
        "lifecycle_state": "complete",
        "record_coverage_status": "covered_for_selected_scope",
        "representation_fidelity_status": "all_tables_completed_selected_read_mapping",
        "source_time_consistency_status": "per_table_metadata_unchanged_cross_table_unproven",
    }
    if any(manifest_row.get(key) != value for key, value in expected_snapshot.items()):
        raise SnapshotError("complete manifest validation rejected snapshot-level completeness facts")
    if not isinstance(manifest_row.get("finished_at_or_null"), str) or not manifest_row["finished_at_or_null"]:
        raise SnapshotError("complete manifest validation found no finished timestamp")

    table_rows = query_json(
        duckdb_binary,
        str(staging_path),
        "SELECT count(*) AS invalid_table_count FROM " + sql_identifier(TABLE_MANIFEST_RELATION)
        + " WHERE snapshot_id = " + sql_string(snapshot_id)
        + " AND (state <> 'complete' OR record_coverage_status <> 'covered_by_terminal_scan' "
        "OR representation_fidelity_status <> 'read_mapping_completed_without_reported_coercion_failure' "
        "OR source_time_consistency_status <> 'metadata_unchanged_during_table_read_cross_table_unproven')",
        "complete manifest table validation",
    )
    if len(table_rows) != 1 or table_rows[0].get("invalid_table_count") != 0:
        raise SnapshotError("complete manifest validation rejected one or more table completeness facts")

    relation_rows = query_json(
        duckdb_binary,
        str(staging_path),
        "SELECT count(*) AS missing_local_relation_count FROM " + sql_identifier(TABLE_MANIFEST_RELATION)
        + " AS manifest_tables LEFT JOIN information_schema.tables AS local_tables"
        " ON local_tables.table_schema = 'main'"
        " AND local_tables.table_name = manifest_tables.local_relation_name"
        " AND local_tables.table_type = 'BASE TABLE'"
        " WHERE manifest_tables.snapshot_id = " + sql_string(snapshot_id)
        + " AND local_tables.table_name IS NULL",
        "complete manifest relation validation",
    )
    if len(relation_rows) != 1 or relation_rows[0].get("missing_local_relation_count") != 0:
        raise SnapshotError("complete manifest validation found a missing materialized relation")


def publish_staging(
    staging_path: Path,
    output_path: Path,
    replace: bool,
    original_target_identity: Optional[Tuple[int, int, int, int]],
    validated_complete: bool,
) -> None:
    """Publish a validated staging database without ever mutating a target in place."""
    if not validated_complete:
        raise SnapshotError("refusing to publish staging whose manifest is not complete")
    try:
        if replace:
            if original_target_identity is not None and file_identity(output_path) != original_target_identity:
                raise SnapshotError("destination changed while replacement staging was in progress")
            os.replace(str(staging_path), str(output_path))
        else:
            os.link(str(staging_path), str(output_path))
            try:
                staging_path.unlink()
            except OSError:
                # The new target is already a validated hard link; retaining staging is safe diagnostics residue.
                pass
    except FileExistsError as exc:
        raise SnapshotError("destination already exists; pass --replace for replace-all staging") from exc
    except OSError as exc:
        raise SnapshotError("cannot publish the completed staging destination") from exc


def read_parent_snapshot_id(duckdb_binary: str, output_path: Path) -> Optional[str]:
    """Preserve an existing complete snapshot identity when the target is one of ours."""
    try:
        rows = query_json(
            duckdb_binary,
            str(output_path),
            "SELECT snapshot_id FROM " + sql_identifier(MANIFEST_RELATION)
            + " WHERE lifecycle_state = 'complete' ORDER BY started_at DESC LIMIT 1",
            "existing snapshot manifest inspection",
        )
    except SnapshotError:
        return None
    if len(rows) != 1 or not isinstance(rows[0].get("snapshot_id"), str) or not rows[0]["snapshot_id"]:
        return None
    return rows[0]["snapshot_id"]


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Prototype: materialize a Bitable Base with an internal manifest.")
    parser.add_argument("--app-token", required=True, help="Bitable app token (never printed by this script)")
    parser.add_argument("--output", required=True, type=Path, help=".duckdb destination; existing files require --replace")
    parser.add_argument("--table", action="append", default=[], help="stable table ID subset; repeat to select several")
    parser.add_argument("--secret", help="existing DuckDB TYPE feishu Secret name; skips environment credentials")
    parser.add_argument("--schema-mode", choices=("typed", "json"), default="typed", help="read mapping stored in manifest")
    parser.add_argument("--duckdb", help="DuckDB CLI path (defaults to local debug build when available)")
    parser.add_argument("--extension", help="extension binary path (defaults to local debug build when available)")
    parser.add_argument("--dry-run", action="store_true", help="validate plan without creating a target or requesting Feishu")
    parser.add_argument(
        "--replace",
        action="store_true",
        help="explicitly replace an existing destination after isolated staging and complete-manifest validation",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    output_path = args.output.expanduser()
    if output_path.suffix != ".duckdb":
        print("snapshot prototype failed: destination must have a .duckdb suffix", file=sys.stderr)
        return 2
    if output_path.exists() and not args.replace:
        print("snapshot prototype failed: destination already exists; pass --replace for replace-all staging", file=sys.stderr)
        return 2
    if output_path.exists() and not output_path.is_file():
        print("snapshot prototype failed: destination must be a regular file when --replace is used", file=sys.stderr)
        return 2
    if not output_path.parent.is_dir():
        print("snapshot prototype failed: destination directory does not exist", file=sys.stderr)
        return 2
    if not args.app_token or any(not table_id for table_id in args.table):
        print("snapshot prototype failed: app token and table IDs must not be empty", file=sys.stderr)
        return 2
    if len(set(args.table)) != len(args.table):
        print("snapshot prototype failed: duplicate --table values are not allowed", file=sys.stderr)
        return 2
    if args.dry_run:
        print("Dry run: no target was created and no Feishu request was made.")
        print("Plan: manifest planned -> discovery -> metadata -> CTAS -> validation -> complete.")
        print("Manifest reports coverage, fidelity, and source-time consistency separately; cross-table time is unproven.")
        print("Destination policy: existing targets are rejected unless --replace requests isolated staging and publish.")
        return 0

    staging_path: Optional[Path] = None
    manifest: Optional[SnapshotManifest] = None
    current_table_id: Optional[str] = None
    materialized_any = False
    selected: List[Dict[str, object]] = []
    original_target_identity: Optional[Tuple[int, int, int, int]] = None
    try:
        load_env_local(REPOSITORY_ROOT / ".env.local")
        credential_setup, secret_clause, credential_mode = credential_sql(args.secret)
        duckdb_binary = args.duckdb or default_binary("build/debug/duckdb", "duckdb")
        extension_path = args.extension or default_binary(
            "build/debug/extension/feishu_bitable/feishu_bitable.duckdb_extension",
            str(REPOSITORY_ROOT / "build/release/extension/feishu_bitable/feishu_bitable.duckdb_extension"),
        )
        if not Path(duckdb_binary).exists() and os.path.sep in duckdb_binary:
            raise SnapshotError("DuckDB CLI was not found")
        if not Path(extension_path).is_file():
            raise SnapshotError("extension binary was not found; build the extension or pass --extension")

        if output_path.exists():
            original_target_identity = file_identity(output_path)
        parent_snapshot_id = read_parent_snapshot_id(duckdb_binary, output_path) if output_path.exists() else None

        requested_scope = canonical_json(
            {"kind": "explicit_table_ids" if args.table else "all_visible_at_discovery", "table_ids": args.table}
        )
        staging_path = staging_path_for(output_path)
        manifest = SnapshotManifest(duckdb_binary, staging_path, str(uuid.uuid4()))
        manifest.initialize(requested_scope, credential_mode, args.schema_mode, utc_timestamp())
        if parent_snapshot_id is not None:
            manifest.set_snapshot("planned", refresh_parent_snapshot_id_or_null=parent_snapshot_id)
        manifest.set_snapshot("discovering")

        discovered = query_metadata_json(
            duckdb_binary, extension_path, credential_setup,
            "SELECT table_id, name, revision FROM feishu_bitable_tables(" + sql_string(args.app_token) + secret_clause + ")",
            "table discovery",
        )
        selected = validate_tables(discovered, args.table)
        manifest.set_snapshot(
            "discovering", effective_discovered_scope=canonical_json(
                [{"local_relation_name": row["local_relation_name"], "table_id": row["table_id"]} for row in selected]
            ),
        )
        for row in selected:
            manifest.insert_table(row)

        for row in selected:
            current_table_id = row["table_id"]
            assert isinstance(current_table_id, str)
            manifest.update_table(current_table_id, metadata_started_at=utc_timestamp(), state="metadata_capturing")
            fields = query_metadata_json(
                duckdb_binary, extension_path, credential_setup,
                "SELECT * FROM feishu_bitable_fields(" + sql_string(args.app_token) + ", "
                + sql_string(current_table_id) + secret_clause + ")", "field metadata capture",
            )
            row["field_schema_fingerprint_before"] = schema_fingerprint(fields)
            manifest.update_table(
                current_table_id, metadata_finished_at=utc_timestamp(),
                field_schema_fingerprint_before=row["field_schema_fingerprint_before"], state="metadata_captured",
            )

        manifest.set_snapshot("metadata_captured")
        manifest.set_snapshot("materializing")
        for row in selected:
            current_table_id = row["table_id"]
            local_name = row["local_relation_name"]
            assert isinstance(current_table_id, str)
            assert isinstance(local_name, str)
            manifest.update_table(current_table_id, records_started_at_or_null=utc_timestamp(), state="materializing")
            materialize_table(
                duckdb_binary, extension_path, credential_setup, secret_clause, args.app_token, staging_path,
                current_table_id, local_name, args.schema_mode,
            )
            row_count = local_row_count(duckdb_binary, staging_path, local_name)
            materialized_any = True
            manifest.update_table(
                current_table_id, records_finished_at_or_null=utc_timestamp(), rows_materialized=row_count,
                pages_consumed=derived_pages_consumed(row_count),
                page_count_basis="lower_bound_from_rows_and_page_size_500", state="materialized",
                record_coverage_status="covered_by_terminal_scan",
                representation_fidelity_status="read_mapping_completed_without_reported_coercion_failure",
            )

        manifest.set_snapshot("validating")
        after_discovery = table_lookup(query_metadata_json(
            duckdb_binary, extension_path, credential_setup,
            "SELECT table_id, name, revision FROM feishu_bitable_tables(" + sql_string(args.app_token) + secret_clause + ")",
            "post-materialization table discovery",
        ))
        for row in selected:
            current_table_id = row["table_id"]
            assert isinstance(current_table_id, str)
            after_row = after_discovery.get(current_table_id)
            if after_row is None:
                raise SnapshotError("post-materialization discovery could not find a selected table")
            after_fields = query_metadata_json(
                duckdb_binary, extension_path, credential_setup,
                "SELECT * FROM feishu_bitable_fields(" + sql_string(args.app_token) + ", "
                + sql_string(current_table_id) + secret_clause + ")", "post-materialization field metadata capture",
            )
            after_fingerprint = schema_fingerprint(after_fields)
            before_revision = row.get("revision")
            after_revision = after_row.get("revision")
            manifest.update_table(
                current_table_id, observed_revision_after_or_null=None if after_revision is None else str(after_revision),
                field_schema_fingerprint_after_or_null=after_fingerprint,
            )
            if before_revision != after_revision or after_fingerprint != row["field_schema_fingerprint_before"]:
                raise SnapshotError("source metadata drift was observed during materialization")
            manifest.update_table(
                current_table_id, state="complete",
                source_time_consistency_status="metadata_unchanged_during_table_read_cross_table_unproven",
            )

        manifest.set_snapshot(
            "complete", finished_at_or_null=utc_timestamp(), record_coverage_status="covered_for_selected_scope",
            representation_fidelity_status="all_tables_completed_selected_read_mapping",
            source_time_consistency_status="per_table_metadata_unchanged_cross_table_unproven",
            failure_summary_sanitized=None,
        )
        validate_complete_snapshot(duckdb_binary, staging_path, manifest.snapshot_id)
        publish_staging(staging_path, output_path, args.replace, original_target_identity, validated_complete=True)
        staging_path = None
    except KeyboardInterrupt:
        if manifest is not None:
            try:
                manifest.terminal_failure("cancelled", "snapshot cancelled by caller", current_table_id)
            except SnapshotError:
                pass
        print("snapshot prototype cancelled", file=sys.stderr)
        if staging_path is not None and staging_path.exists():
            print("Non-complete staging retained for diagnosis at " + str(staging_path) + ".", file=sys.stderr)
        return 130
    except SnapshotError as exc:
        terminal_state = "incomplete" if materialized_any else "failed"
        safe_summary = sanitize_error_summary(str(exc))
        if manifest is not None:
            try:
                manifest.terminal_failure(terminal_state, safe_summary, current_table_id)
            except SnapshotError:
                pass
        print("snapshot prototype failed: " + safe_summary, file=sys.stderr)
        if staging_path is not None and staging_path.exists():
            print("Non-complete staging retained for diagnosis at " + str(staging_path) + ".", file=sys.stderr)
        return 1

    print("Snapshot materialized " + str(len(selected)) + " table(s) and published validated staging into " + str(output_path) + ".")
    print("Internal manifest reports coverage, fidelity, and source-time separately; cross-table consistency is unproven.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
