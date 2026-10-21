#!/usr/bin/env python3
"""Offline regression checks for snapshot manifest helpers."""

import importlib.util
import io
import os
import tempfile
import unittest
from contextlib import ExitStack
from pathlib import Path
from unittest.mock import Mock, patch


MODULE_PATH = Path(__file__).with_name("snapshot_base_proto.py")
SPEC = importlib.util.spec_from_file_location("snapshot_base_proto", MODULE_PATH)
snapshot = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(snapshot)


class SnapshotManifestHelpersTest(unittest.TestCase):
    class FakeManifest:
        instances = []

        def __init__(self, duckdb_binary, database, snapshot_id):
            self.snapshot_id = snapshot_id
            self.database = database
            self.initialized = []
            self.snapshot_updates = []
            self.inserted_tables = []
            self.table_updates = []
            self.terminal_failures = []
            self.__class__.instances.append(self)

        def initialize(self, requested_scope, credential_mode, schema_mode, started_at):
            self.initialized.append((requested_scope, credential_mode, schema_mode, started_at))

        def set_snapshot(self, state, **values):
            self.snapshot_updates.append((state, values))

        def insert_table(self, row):
            self.inserted_tables.append(dict(row))

        def update_table(self, table_id, **values):
            self.table_updates.append((table_id, values))

        def terminal_failure(self, state, summary, table_id):
            self.terminal_failures.append((state, summary, table_id))

    def setUp(self):
        self.FakeManifest.instances = []

    def run_injected_snapshot(
        self, directory, discovered, metadata, row_counts, replace=False, output_exists=False, materialize=None
    ):
        output = Path(directory) / "snapshot.duckdb"
        if output_exists:
            output.write_text("previous snapshot", encoding="utf-8")
        staging = Path(directory) / "staging.duckdb"
        duckdb = Path(directory) / "duckdb"
        extension = Path(directory) / "feishu_bitable.duckdb_extension"
        duckdb.touch()
        extension.touch()
        events = []
        discovery_rows = [discovered]
        metadata_rows = iter(metadata)
        materialize = materialize or Mock()

        def metadata_query(*_args):
            if discovery_rows:
                return discovery_rows.pop()
            return next(metadata_rows)

        with ExitStack() as stack:
            stack.enter_context(
                patch(
                    "sys.argv",
                    [
                        "snapshot_base_proto.py",
                        "--app-token",
                        "<app_token>",
                        "--output",
                        str(output),
                        "--duckdb",
                        str(duckdb),
                        "--extension",
                        str(extension),
                    ]
                    + (["--replace"] if replace else []),
                )
            )
            stack.enter_context(patch.object(snapshot, "load_env_local", return_value=False))
            stack.enter_context(
                patch.object(snapshot, "credential_sql", return_value=("", ", SECRET = '<secret_name>'", "named_secret"))
            )
            stack.enter_context(patch.object(snapshot, "SnapshotManifest", self.FakeManifest))
            stack.enter_context(patch.object(snapshot, "staging_path_for", return_value=staging))
            stack.enter_context(patch.object(snapshot, "query_metadata_json", side_effect=metadata_query))
            stack.enter_context(patch.object(snapshot, "materialize_table", side_effect=materialize))
            stack.enter_context(patch.object(snapshot, "local_row_count", side_effect=row_counts))
            stack.enter_context(patch.object(snapshot, "read_parent_snapshot_id", return_value=None))
            stack.enter_context(
                patch.object(snapshot, "validate_complete_snapshot", side_effect=lambda *_args: events.append("validate"))
            )
            stack.enter_context(
                patch.object(snapshot, "publish_staging", side_effect=lambda *_args, **_kwargs: events.append("publish"))
            )
            with patch("sys.stderr", new_callable=io.StringIO) as stderr:
                result = snapshot.main()

        return result, output, events, stderr.getvalue(), self.FakeManifest.instances[-1], materialize

    def test_schema_fingerprint_is_stable_for_equivalent_property_json(self):
        first = [{"field_id": "fld_a", "type_id": 1, "property_json": '{"b":2,"a":1}'}]
        second = [{"field_id": "fld_a", "type_id": 1, "property_json": '{"a":1,"b":2}'}]
        changed = [{"field_id": "fld_a", "type_id": 2, "property_json": '{"a":1,"b":2}'}]

        self.assertEqual(snapshot.schema_fingerprint(first), snapshot.schema_fingerprint(second))
        self.assertNotEqual(snapshot.schema_fingerprint(first), snapshot.schema_fingerprint(changed))

    def test_page_accounting_keeps_empty_table_at_one_terminal_page(self):
        self.assertEqual(snapshot.derived_pages_consumed(0), 1)
        self.assertEqual(snapshot.derived_pages_consumed(500), 1)
        self.assertEqual(snapshot.derived_pages_consumed(501), 2)

    def test_empty_table_completes_with_terminal_page_and_three_dimensions(self):
        with tempfile.TemporaryDirectory() as directory:
            result, _output, events, _stderr, manifest, _materialize = self.run_injected_snapshot(
                directory,
                [{"table_id": "table_empty", "name": "Empty", "revision": 1}],
                [
                    [{"field_id": "field_a", "type_id": 1, "property_json": "{}"}],
                    [{"table_id": "table_empty", "name": "Empty", "revision": 1}],
                    [{"field_id": "field_a", "type_id": 1, "property_json": "{}"}],
                ],
                [0],
            )

        self.assertEqual(result, 0)
        self.assertEqual(events, ["validate", "publish"])
        materialized = [values for _table_id, values in manifest.table_updates if values.get("state") == "materialized"]
        self.assertEqual(materialized[0]["rows_materialized"], 0)
        self.assertEqual(materialized[0]["pages_consumed"], 1)
        complete = manifest.snapshot_updates[-1]
        self.assertEqual(complete[0], "complete")
        self.assertEqual(complete[1]["record_coverage_status"], "covered_for_selected_scope")
        self.assertEqual(complete[1]["representation_fidelity_status"], "all_tables_completed_selected_read_mapping")
        self.assertEqual(complete[1]["source_time_consistency_status"], "per_table_metadata_unchanged_cross_table_unproven")

    def test_injected_multi_page_records_are_accounted_before_complete_publish(self):
        with tempfile.TemporaryDirectory() as directory:
            result, _output, events, _stderr, manifest, materialize = self.run_injected_snapshot(
                directory,
                [{"table_id": "table_many", "name": "Many", "revision": 1}],
                [
                    [{"field_id": "field_a", "type_id": 1, "property_json": "{}"}],
                    [{"table_id": "table_many", "name": "Many", "revision": 1}],
                    [{"field_id": "field_a", "type_id": 1, "property_json": "{}"}],
                ],
                [1001],
            )

        self.assertEqual(result, 0)
        self.assertEqual(events, ["validate", "publish"])
        materialize.assert_called_once()
        materialized = [values for _table_id, values in manifest.table_updates if values.get("state") == "materialized"]
        self.assertEqual(materialized[0]["pages_consumed"], 3)
        self.assertEqual(materialized[0]["page_count_basis"], "lower_bound_from_rows_and_page_size_500")

    def test_injected_multi_table_and_field_discovery_results_complete_selected_scope(self):
        with tempfile.TemporaryDirectory() as directory:
            result, _output, events, _stderr, manifest, _materialize = self.run_injected_snapshot(
                directory,
                [
                    {"table_id": "table_page_one", "name": "One", "revision": 1},
                    {"table_id": "table_page_two", "name": "Two", "revision": 1},
                ],
                [
                    [
                        {"field_id": "field_a", "type_id": 1, "property_json": "{}"},
                        {"field_id": "field_b", "type_id": 1001, "property_json": "{}"},
                    ],
                    [
                        {"field_id": "field_c", "type_id": 4, "property_json": "{}"},
                        {"field_id": "field_d", "type_id": 99001, "property_json": "{}"},
                    ],
                    [
                        {"table_id": "table_page_one", "name": "One", "revision": 1},
                        {"table_id": "table_page_two", "name": "Two", "revision": 1},
                    ],
                    [
                        {"field_id": "field_a", "type_id": 1, "property_json": "{}"},
                        {"field_id": "field_b", "type_id": 1001, "property_json": "{}"},
                    ],
                    [
                        {"field_id": "field_c", "type_id": 4, "property_json": "{}"},
                        {"field_id": "field_d", "type_id": 99001, "property_json": "{}"},
                    ],
                ],
                [1, 0],
            )

        self.assertEqual(result, 0)
        self.assertEqual(events, ["validate", "publish"])
        self.assertEqual([row["table_id"] for row in manifest.inserted_tables], ["table_page_one", "table_page_two"])
        materialized = [values for _table_id, values in manifest.table_updates if values.get("state") == "materialized"]
        self.assertEqual([values["rows_materialized"] for values in materialized], [1, 0])

    def test_ctas_uses_full_existing_read_mapping_without_projection(self):
        with patch.object(snapshot, "run_duckdb", return_value="") as run_duckdb:
            snapshot.materialize_table(
                "duckdb",
                "extension.duckdb_extension",
                "",
                ", SECRET = '<secret_name>'",
                "<app_token>",
                Path("staging.duckdb"),
                "<table_id>",
                "local_table",
                "json",
            )

        sql = run_duckdb.call_args.args[2]
        self.assertIn("CREATE TABLE \"local_table\" AS SELECT * FROM read_feishu_bitable", sql)
        self.assertIn("SCHEMA_MODE = 'json'", sql)
        self.assertIn("INCLUDE_SYSTEM_COLUMNS = true", sql)

    def test_explicit_scope_keeps_requested_order_and_stable_local_names(self):
        selected = snapshot.validate_tables(
            [
                {"table_id": "tbl_b", "name": "Same Name", "revision": 1},
                {"table_id": "tbl_a", "name": "Same Name", "revision": 1},
            ],
            ["tbl_a", "tbl_b"],
        )

        self.assertEqual([row["table_id"] for row in selected], ["tbl_a", "tbl_b"])
        self.assertNotEqual(selected[0]["local_relation_name"], selected[1]["local_relation_name"])

    def test_credential_mode_never_returns_environment_value(self):
        with patch.dict(os.environ, {"FEISHU_SNAPSHOT_USER_ACCESS_TOKEN": "<user_access_token>"}, clear=True):
            setup, clause, mode = snapshot.credential_sql(None)

        self.assertEqual(mode, "user_access_token")
        self.assertIn("USER_ACCESS_TOKEN", setup)
        self.assertNotIn("<user_access_token>", mode)
        self.assertIn("SECRET", clause)

    def test_error_sanitization_removes_configured_credentials_and_bearer_values(self):
        secret_value = "sensitive-" + "credential"
        with patch.dict(os.environ, {"FEISHU_SNAPSHOT_USER_ACCESS_TOKEN": secret_value}, clear=True):
            sanitized = snapshot.sanitize_error_summary(
                "permission denied user_access_token=" + secret_value + " Bearer another-sensitive-value"
            )

        self.assertNotIn(secret_value, sanitized)
        self.assertNotIn("another-sensitive-value", sanitized)
        self.assertIn("user_access_token=<redacted>", sanitized)

    def test_existing_destination_is_rejected_without_explicit_replace(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "existing.duckdb"
            output.write_text("previous snapshot", encoding="utf-8")
            with patch("sys.argv", ["snapshot_base_proto.py", "--app-token", "<app_token>", "--output", str(output)]):
                with patch("sys.stderr", new_callable=io.StringIO) as stderr:
                    result = snapshot.main()

        self.assertEqual(result, 2)
        self.assertIn("pass --replace", stderr.getvalue())

    def test_explicit_replace_dry_run_keeps_existing_destination(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "existing.duckdb"
            output.write_text("previous snapshot", encoding="utf-8")
            with patch(
                "sys.argv",
                ["snapshot_base_proto.py", "--app-token", "<app_token>", "--output", str(output), "--replace", "--dry-run"],
            ):
                result = snapshot.main()

            self.assertEqual(output.read_text(encoding="utf-8"), "previous snapshot")

        self.assertEqual(result, 0)

    def test_explicit_replace_runs_staging_validation_before_publish(self):
        with tempfile.TemporaryDirectory() as directory:
            result, _output, events, _stderr, _manifest, _materialize = self.run_injected_snapshot(
                directory,
                [{"table_id": "table_replace", "name": "Replace", "revision": 1}],
                [
                    [{"field_id": "field_a", "type_id": 1, "property_json": "{}"}],
                    [{"table_id": "table_replace", "name": "Replace", "revision": 1}],
                    [{"field_id": "field_a", "type_id": 1, "property_json": "{}"}],
                ],
                [1],
                replace=True,
                output_exists=True,
            )

        self.assertEqual(result, 0)
        self.assertEqual(events, ["validate", "publish"])

    def test_field_drift_marks_snapshot_incomplete_and_never_publishes(self):
        with tempfile.TemporaryDirectory() as directory:
            result, _output, events, stderr, manifest, _materialize = self.run_injected_snapshot(
                directory,
                [{"table_id": "table_drift", "name": "Drift", "revision": 1}],
                [
                    [{"field_id": "field_a", "type_id": 1, "property_json": "{}"}],
                    [{"table_id": "table_drift", "name": "Drift", "revision": 2}],
                    [{"field_id": "field_a", "type_id": 2, "property_json": "{}"}],
                ],
                [1],
            )

        self.assertEqual(result, 1)
        self.assertEqual(events, [])
        self.assertIn("drift", stderr)
        self.assertEqual(manifest.terminal_failures[0][0], "incomplete")
        self.assertEqual(manifest.terminal_failures[0][2], "table_drift")
        self.assertNotIn("complete", [state for state, _values in manifest.snapshot_updates])

    def test_permission_denial_bad_cursor_and_write_failure_never_publish(self):
        failure_cases = (
            ("permission denied while listing records", "failed"),
            ("bad cursor in records pagination", "failed"),
            ("retry exhaustion while reading records", "failed"),
            ("local write failure after partial materialization", "incomplete"),
        )
        for failure_message, expected_state in failure_cases:
            with self.subTest(failure_message=failure_message), tempfile.TemporaryDirectory() as directory:
                if expected_state == "failed":
                    materialize = Mock(side_effect=snapshot.SnapshotError(failure_message))
                    row_counts = []
                else:
                    materialize = Mock()
                    row_counts = [1, snapshot.SnapshotError(failure_message)]
                result, _output, events, stderr, manifest, _materialize = self.run_injected_snapshot(
                    directory,
                    [
                        {"table_id": "table_first", "name": "First", "revision": 1},
                        {"table_id": "table_second", "name": "Second", "revision": 1},
                    ],
                    [
                        [{"field_id": "field_a", "type_id": 1, "property_json": "{}"}],
                        [{"field_id": "field_b", "type_id": 1, "property_json": "{}"}],
                    ],
                    row_counts,
                    materialize=materialize,
                )

                self.assertEqual(result, 1)
                self.assertEqual(events, [])
                self.assertIn(failure_message, stderr)
                self.assertEqual(manifest.terminal_failures[0][0], expected_state)
                self.assertEqual(manifest.terminal_failures[0][1], failure_message)
                self.assertEqual(manifest.terminal_failures[0][2], "table_second" if expected_state == "incomplete" else "table_first")

    def test_incomplete_terminal_state_reports_all_three_dimensions_as_non_complete(self):
        with patch.object(snapshot, "run_duckdb", return_value="") as run_duckdb:
            manifest = snapshot.SnapshotManifest("duckdb", Path("staging.duckdb"), "snapshot-id")
            manifest.terminal_failure("incomplete", "local write failure", "table-id")

        executed_sql = "\n".join(call.args[2] for call in run_duckdb.call_args_list)
        self.assertIn("record_coverage_status\" = 'not_covered'", executed_sql)
        self.assertIn("representation_fidelity_status\" = 'not_complete'", executed_sql)
        self.assertIn("source_time_consistency_status\" = 'unproven'", executed_sql)
        self.assertIn("lifecycle_state = 'incomplete'", executed_sql)

    def test_failure_redacts_error_before_manifest_or_stderr_and_does_not_publish(self):
        secret_value = "sensitive-" + "credential"
        with patch.dict(os.environ, {"FEISHU_SNAPSHOT_USER_ACCESS_TOKEN": secret_value}, clear=True):
            with tempfile.TemporaryDirectory() as directory:
                materialize = Mock(side_effect=snapshot.SnapshotError("permission denied token=" + secret_value))
                result, _output, events, stderr, manifest, _materialize = self.run_injected_snapshot(
                    directory,
                    [{"table_id": "table_secret", "name": "Secret", "revision": 1}],
                    [[{"field_id": "field_a", "type_id": 1, "property_json": "{}"}]],
                    [],
                    materialize=materialize,
                )

        self.assertEqual(result, 1)
        self.assertEqual(events, [])
        self.assertNotIn(secret_value, stderr)
        self.assertNotIn(secret_value, manifest.terminal_failures[0][1])
        self.assertIn("token=<redacted>", manifest.terminal_failures[0][1])

    def test_cancellation_marks_non_complete_staging_and_never_publishes(self):
        with tempfile.TemporaryDirectory() as directory:
            materialize = Mock(side_effect=KeyboardInterrupt())
            result, _output, events, stderr, manifest, _materialize = self.run_injected_snapshot(
                directory,
                [{"table_id": "table_cancel", "name": "Cancel", "revision": 1}],
                [[{"field_id": "field_a", "type_id": 1, "property_json": "{}"}]],
                [],
                materialize=materialize,
            )

        self.assertEqual(result, 130)
        self.assertEqual(events, [])
        self.assertIn("cancelled", stderr)
        self.assertEqual(manifest.terminal_failures[0][0], "cancelled")

    def test_publish_refuses_unvalidated_staging_and_preserves_target(self):
        with tempfile.TemporaryDirectory() as directory:
            staging = Path(directory) / "staging.duckdb"
            output = Path(directory) / "target.duckdb"
            staging.write_text("new snapshot", encoding="utf-8")
            output.write_text("previous snapshot", encoding="utf-8")

            with self.assertRaisesRegex(snapshot.SnapshotError, "not complete"):
                snapshot.publish_staging(staging, output, True, snapshot.file_identity(output), validated_complete=False)

            self.assertEqual(output.read_text(encoding="utf-8"), "previous snapshot")
            self.assertEqual(staging.read_text(encoding="utf-8"), "new snapshot")

    def test_explicit_replace_publishes_validated_staging(self):
        with tempfile.TemporaryDirectory() as directory:
            staging = Path(directory) / "staging.duckdb"
            output = Path(directory) / "target.duckdb"
            staging.write_text("new snapshot", encoding="utf-8")
            output.write_text("previous snapshot", encoding="utf-8")
            original_identity = snapshot.file_identity(output)

            snapshot.publish_staging(staging, output, True, original_identity, validated_complete=True)

            self.assertEqual(output.read_text(encoding="utf-8"), "new snapshot")
            self.assertFalse(staging.exists())

    def test_replace_detects_concurrent_target_change_and_retains_staging(self):
        with tempfile.TemporaryDirectory() as directory:
            staging = Path(directory) / "staging.duckdb"
            output = Path(directory) / "target.duckdb"
            staging.write_text("new snapshot", encoding="utf-8")
            output.write_text("previous snapshot", encoding="utf-8")
            original_identity = snapshot.file_identity(output)
            output.write_text("changed target", encoding="utf-8")

            with self.assertRaisesRegex(snapshot.SnapshotError, "changed while replacement staging"):
                snapshot.publish_staging(staging, output, True, original_identity, validated_complete=True)

            self.assertEqual(output.read_text(encoding="utf-8"), "changed target")
            self.assertEqual(staging.read_text(encoding="utf-8"), "new snapshot")

    def test_complete_manifest_validation_requires_complete_table_facts(self):
        snapshot_id = "snapshot-id"
        manifest_row = {
            "snapshot_id": snapshot_id,
            "lifecycle_state": "complete",
            "record_coverage_status": "covered_for_selected_scope",
            "representation_fidelity_status": "all_tables_completed_selected_read_mapping",
            "source_time_consistency_status": "per_table_metadata_unchanged_cross_table_unproven",
            "finished_at_or_null": "2026-10-13T00:00:00Z",
        }
        with patch.object(
            snapshot,
            "query_json",
            side_effect=[[manifest_row], [{"invalid_table_count": 0}], [{"missing_local_relation_count": 0}]],
        ) as query:
            snapshot.validate_complete_snapshot("duckdb", Path("staging.duckdb"), snapshot_id)

        self.assertEqual(query.call_count, 3)

    def test_complete_manifest_validation_rejects_incomplete_table_facts(self):
        snapshot_id = "snapshot-id"
        manifest_row = {
            "snapshot_id": snapshot_id,
            "lifecycle_state": "complete",
            "record_coverage_status": "covered_for_selected_scope",
            "representation_fidelity_status": "all_tables_completed_selected_read_mapping",
            "source_time_consistency_status": "per_table_metadata_unchanged_cross_table_unproven",
            "finished_at_or_null": "2026-10-13T00:00:00Z",
        }
        with patch.object(snapshot, "query_json", side_effect=[[manifest_row], [{"invalid_table_count": 1}]]):
            with self.assertRaisesRegex(snapshot.SnapshotError, "table completeness facts"):
                snapshot.validate_complete_snapshot("duckdb", Path("staging.duckdb"), snapshot_id)

    def test_complete_manifest_validation_rejects_missing_materialized_relation(self):
        snapshot_id = "snapshot-id"
        manifest_row = {
            "snapshot_id": snapshot_id,
            "lifecycle_state": "complete",
            "record_coverage_status": "covered_for_selected_scope",
            "representation_fidelity_status": "all_tables_completed_selected_read_mapping",
            "source_time_consistency_status": "per_table_metadata_unchanged_cross_table_unproven",
            "finished_at_or_null": "2026-10-13T00:00:00Z",
        }
        with patch.object(
            snapshot,
            "query_json",
            side_effect=[[manifest_row], [{"invalid_table_count": 0}], [{"missing_local_relation_count": 1}]],
        ):
            with self.assertRaisesRegex(snapshot.SnapshotError, "missing materialized relation"):
                snapshot.validate_complete_snapshot("duckdb", Path("staging.duckdb"), snapshot_id)


if __name__ == "__main__":
    unittest.main()
