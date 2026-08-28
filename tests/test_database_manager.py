import os
import sqlite3
import sys
import tempfile
import unittest
import uuid
from pathlib import Path
from unittest import mock


PROJECT_ROOT = Path(__file__).resolve().parents[1]
GUI_DIR = PROJECT_ROOT / "gui"
if str(GUI_DIR) not in sys.path:
    sys.path.insert(0, str(GUI_DIR))

from core.DatabaseManager import (  # noqa: E402
    DatabaseError,
    DatabaseIdentityError,
    DatabaseManager,
    DatabaseMigrationError,
    SCHEMA_VERSION,
)


class DatabaseManagerTests(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.project = Path(self.temporary_directory.name)
        self.data_dir = self.project / "data"
        self.data_dir.mkdir()
        self.db_path = self.data_dir / "run_history.db"
        self.config_path = self.project / "run.conf"
        self.config_path.write_text("[Digitizer]\nRecordLength = 1024\n")

    def tearDown(self):
        self.temporary_directory.cleanup()

    def record(self, manager, name="sample_run007.dat", **kwargs):
        return manager.record_run_start(
            self.data_dir / name,
            {"Operator": "tester", "Applied HV": "1800 V"},
            self.config_path,
            run_number=7,
            segment_kind="batch",
            segment_index=2,
            **kwargs,
        )

    def test_new_schema_pragmas_identity_and_independent_stage_facts(self):
        manager = DatabaseManager(self.db_path, busy_timeout_ms=3210)
        run_id = self.record(manager)
        run = manager.get_run(run_id)

        self.assertEqual(run["status"], "daq_launching")
        self.assertEqual(run["daq_status"], "daq_launching")
        self.assertEqual(run["production_status"], "not_started")
        self.assertEqual(run["run_number"], 7)
        self.assertEqual(run["segment_kind"], "batch")
        self.assertEqual(run["segment_index"], 2)
        self.assertEqual(run["migrated_legacy"], 0)
        self.assertEqual(str(uuid.UUID(run["run_uuid"])), run["run_uuid"])
        self.assertEqual(
            run["output_path_key"],
            os.path.abspath(self.data_dir / "sample_run007.dat"),
        )

        with sqlite3.connect(self.db_path) as connection:
            self.assertEqual(
                connection.execute("PRAGMA user_version").fetchone()[0],
                SCHEMA_VERSION,
            )
            self.assertEqual(
                connection.execute("PRAGMA journal_mode").fetchone()[0], "wal"
            )
            columns = {
                row[1]: row for row in connection.execute(
                    "PRAGMA table_info(run_history)"
                )
            }
            for name in (
                "run_uuid", "run_number", "segment_kind", "segment_index",
                "status", "daq_status", "production_status", "created_at",
                "updated_at",
            ):
                self.assertEqual(columns[name][3], 1, name)
        with manager._connection() as connection:
            self.assertEqual(
                connection.execute("PRAGMA busy_timeout").fetchone()[0], 3210
            )

        manager.finalize_daq_run(
            run_id,
            status="daq_completed",
            exit_code=0,
            summary_dict={"events": 20},
            run_uuid=run["run_uuid"],
            output_file=run["output_file"],
        )
        manager.begin_production(
            run_id,
            run_uuid=run["run_uuid"],
            output_file=run["output_file"],
        )
        manager.finalize_production_run(
            run_id,
            status="production_failed",
            exit_code=9,
            error_message="converter failed",
            run_uuid=run["run_uuid"],
            output_file=run["output_file"],
        )
        final = manager.get_run(run_id)
        self.assertEqual(final["daq_status"], "daq_completed")
        self.assertEqual(final["daq_exit_code"], 0)
        self.assertEqual(final["production_status"], "production_failed")
        self.assertEqual(final["production_exit_code"], 9)
        self.assertEqual(final["status"], "production_failed")

    def test_guarded_transitions_reject_stale_or_wrong_identity(self):
        manager = DatabaseManager(self.db_path)
        run_id = self.record(manager)
        original = manager.get_run(run_id)

        with self.assertRaises(DatabaseIdentityError):
            manager.begin_production(run_id)
        with self.assertRaises(DatabaseIdentityError):
            manager.finalize_daq_run(
                run_id,
                status="daq_completed",
                exit_code=0,
                run_uuid=str(uuid.uuid4()),
            )
        self.assertEqual(manager.get_run(run_id)["daq_status"], "daq_launching")

        manager.finalize_daq_run(
            run_id, status="daq_completed", exit_code=0,
            run_uuid=original["run_uuid"],
        )
        with self.assertRaises(DatabaseIdentityError):
            manager.finalize_daq_run(
                run_id, status="daq_failed", exit_code=1,
            )
        with self.assertRaises(DatabaseIdentityError):
            manager.finalize_daq_run(
                999999, status="daq_failed", exit_code=1,
            )
        self.assertEqual(manager.get_run(run_id)["daq_status"], "daq_completed")

    def test_live_output_is_unique_but_launch_failure_can_be_retried(self):
        manager = DatabaseManager(self.db_path)
        first_id = self.record(manager)
        with self.assertRaises(DatabaseError):
            self.record(manager)
        self.assertEqual(manager.get_run(first_id)["daq_status"], "daq_launching")

        first_uuid = manager.get_run_uuid(first_id)
        manager.mark_daq_launch_failed(
            first_id,
            "exec missing",
            run_uuid=first_uuid,
            output_file=self.data_dir / "sample_run007.dat",
        )
        second_id = self.record(manager)
        self.assertGreater(second_id, first_id)
        with self.assertRaises(DatabaseIdentityError):
            manager.find_run_id_by_output(self.data_dir / "sample_run007.dat")

    def create_legacy_database(self):
        with sqlite3.connect(self.db_path) as connection:
            connection.execute("""
                CREATE TABLE run_history (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    start_time TEXT,
                    output_file TEXT,
                    hv TEXT,
                    config_dump TEXT,
                    env_metadata TEXT,
                    daq_summary TEXT,
                    production_summary TEXT
                )
            """)
            connection.execute(
                "INSERT INTO run_history VALUES(?, ?, ?, ?, ?, ?, ?, ?)",
                (1, None, "data/legacy.dat", "", None, "한글", "{}", None),
            )
            connection.execute(
                "INSERT INTO run_history VALUES(?, ?, ?, ?, ?, ?, ?, ?)",
                (2, "", "data/legacy.dat", None, "", None, None, "bad-json"),
            )
            connection.execute(
                "UPDATE sqlite_sequence SET seq=100 WHERE name='run_history'"
            )

    def test_legacy_migration_preserves_payload_sequence_and_relative_key(self):
        self.create_legacy_database()
        manager = DatabaseManager(self.db_path)
        first = manager.get_run(1)
        second = manager.get_run(2)

        self.assertIsNone(first["start_time"])
        self.assertEqual(first["output_file"], "data/legacy.dat")
        self.assertEqual(first["hv"], "")
        self.assertIsNone(first["config_dump"])
        self.assertEqual(first["env_metadata"], "한글")
        self.assertEqual(first["daq_summary"], "{}")
        self.assertIsNone(first["production_summary"])
        self.assertEqual(second["start_time"], "")
        self.assertIsNone(second["hv"])
        self.assertEqual(second["config_dump"], "")
        self.assertEqual(second["production_summary"], "bad-json")

        for row in (first, second):
            self.assertEqual(row["status"], "legacy_unknown")
            self.assertEqual(row["daq_status"], "legacy_unknown")
            self.assertEqual(row["production_status"], "legacy_unknown")
            self.assertEqual(row["migrated_legacy"], 1)
            self.assertEqual(
                row["output_path_key"],
                os.path.abspath(self.project / "data/legacy.dat"),
            )
        self.assertNotEqual(first["run_uuid"], second["run_uuid"])
        with self.assertRaises(DatabaseIdentityError):
            manager.find_run_id_by_output(self.project / "data/legacy.dat")

        with sqlite3.connect(self.db_path) as connection:
            self.assertEqual(
                connection.execute(
                    "SELECT seq FROM sqlite_sequence WHERE name='run_history'"
                ).fetchone()[0],
                100,
            )
        new_id = self.record(manager, name="new_run007.dat")
        self.assertEqual(new_id, 101)

    def test_failed_migration_rolls_back_schema_data_and_version(self):
        self.create_legacy_database()
        with mock.patch.object(
            DatabaseManager,
            "_create_indexes",
            side_effect=RuntimeError("injected migration failure"),
        ):
            with self.assertRaisesRegex(RuntimeError, "injected"):
                DatabaseManager(self.db_path)

        with sqlite3.connect(self.db_path) as connection:
            self.assertEqual(connection.execute("PRAGMA user_version").fetchone()[0], 0)
            columns = [
                row[1] for row in connection.execute(
                    "PRAGMA table_info(run_history)"
                )
            ]
            self.assertEqual(
                columns,
                ["id", "start_time", "output_file", "hv", "config_dump",
                 "env_metadata", "daq_summary", "production_summary"],
            )
            self.assertEqual(
                connection.execute("SELECT COUNT(*) FROM run_history").fetchone()[0],
                2,
            )
            self.assertIsNone(connection.execute(
                "SELECT name FROM sqlite_master WHERE "
                "name='run_history__schema_v4'"
            ).fetchone())

        manager = DatabaseManager(self.db_path)
        self.assertEqual(manager.get_run(1)["migrated_legacy"], 1)

    def test_five_column_non_autoincrement_legacy_schema_migrates(self):
        with sqlite3.connect(self.db_path) as connection:
            connection.execute("""
                CREATE TABLE run_history (
                    id INTEGER PRIMARY KEY,
                    start_time TEXT,
                    output_file TEXT,
                    hv TEXT,
                    config_dump TEXT
                )
            """)
            connection.execute(
                "INSERT INTO run_history VALUES(?, ?, ?, ?, ?)",
                (9, "old", "relative.dat", "1900 V", "legacy config"),
            )

        manager = DatabaseManager(self.db_path)
        row = manager.get_run(9)
        self.assertEqual(row["start_time"], "old")
        self.assertEqual(row["output_file"], "relative.dat")
        self.assertEqual(row["hv"], "1900 V")
        self.assertEqual(row["config_dump"], "legacy config")
        self.assertEqual(row["migrated_legacy"], 1)
        self.assertEqual(
            manager.find_run_id_by_output(self.project / "relative.dat"), 9
        )

    def test_future_version_is_refused_without_changes(self):
        with sqlite3.connect(self.db_path) as connection:
            connection.execute("CREATE TABLE run_history(id INTEGER PRIMARY KEY)")
            connection.execute(f"PRAGMA user_version={SCHEMA_VERSION + 1}")
        with self.assertRaises(DatabaseMigrationError):
            DatabaseManager(self.db_path)
        with sqlite3.connect(self.db_path) as connection:
            self.assertEqual(
                connection.execute("PRAGMA user_version").fetchone()[0],
                SCHEMA_VERSION + 1,
            )
            self.assertEqual(
                [row[1] for row in connection.execute(
                    "PRAGMA table_info(run_history)"
                )],
                ["id"],
            )

    def test_unknown_version_zero_table_is_not_misidentified_as_legacy(self):
        with sqlite3.connect(self.db_path) as connection:
            connection.execute(
                "CREATE TABLE run_history(id INTEGER PRIMARY KEY, payload BLOB)"
            )
            connection.execute(
                "INSERT INTO run_history VALUES(1, X'000102')"
            )
        with self.assertRaisesRegex(
            DatabaseMigrationError, "supported legacy schema"
        ):
            DatabaseManager(self.db_path)
        with sqlite3.connect(self.db_path) as connection:
            self.assertEqual(connection.execute("PRAGMA user_version").fetchone()[0], 0)
            self.assertEqual(
                connection.execute(
                    "SELECT hex(payload) FROM run_history WHERE id=1"
                ).fetchone()[0],
                "000102",
            )


if __name__ == "__main__":
    unittest.main()
