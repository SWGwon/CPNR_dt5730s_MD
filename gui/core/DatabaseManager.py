"""Transactional, versioned storage for DAQ run identity and lifecycle state."""

from __future__ import annotations

import datetime
import json
import os
import re
import sqlite3
import uuid
from contextlib import contextmanager


SCHEMA_VERSION = 4
DEFAULT_BUSY_TIMEOUT_MS = 5000

DAQ_TERMINAL_STATUSES = {
    "daq_completed", "daq_failed", "daq_cancelled", "daq_launch_failed",
}
PRODUCTION_TERMINAL_STATUSES = {
    "production_completed", "production_failed", "production_cancelled",
    "production_launch_failed",
}


class DatabaseError(RuntimeError):
    """Base error for run-history persistence failures."""


class DatabaseMigrationError(DatabaseError):
    """Raised when a run-history schema cannot be migrated safely."""


class DatabaseIdentityError(DatabaseError):
    """Raised when an update does not resolve to exactly one run record."""


def _utc_now():
    return (
        datetime.datetime.now(datetime.timezone.utc)
        .isoformat(timespec="seconds")
        .replace("+00:00", "Z")
    )


def _absolute_path(value):
    if not value:
        return ""
    return os.path.abspath(os.path.expanduser(os.fspath(value)))


def _infer_run_number(output_file):
    match = re.search(r"_run([0-9]+)(?:\D|$)", str(output_file))
    return int(match.group(1)) if match else 0


class DatabaseManager:
    """Own durable identity and independent DAQ/production stage states."""

    def __init__(self, db_path, *, busy_timeout_ms=DEFAULT_BUSY_TIMEOUT_MS):
        self.db_path = _absolute_path(db_path)
        self.busy_timeout_ms = int(busy_timeout_ms)
        if self.busy_timeout_ms < 0:
            raise ValueError("busy_timeout_ms must be non-negative")
        self.init_db()

    def _open_connection(self):
        try:
            connection = sqlite3.connect(
                self.db_path,
                timeout=self.busy_timeout_ms / 1000.0,
                isolation_level=None,
            )
            connection.row_factory = sqlite3.Row
            connection.execute(f"PRAGMA busy_timeout={self.busy_timeout_ms}")
            connection.execute("PRAGMA foreign_keys=ON")
            connection.execute("PRAGMA journal_mode=WAL")
            connection.execute("PRAGMA synchronous=FULL")
            return connection
        except sqlite3.Error as exc:
            raise DatabaseError(
                f"Cannot open/configure run-history database {self.db_path}: {exc}"
            ) from exc

    @contextmanager
    def _connection(self, *, write=False):
        connection = self._open_connection()
        try:
            if write:
                connection.execute("BEGIN IMMEDIATE")
            yield connection
            if write:
                connection.commit()
        except sqlite3.Error as exc:
            if connection.in_transaction:
                connection.rollback()
            raise DatabaseError(
                f"Run-history transaction failed for {self.db_path}: {exc}"
            ) from exc
        except Exception:
            if connection.in_transaction:
                connection.rollback()
            raise
        finally:
            connection.close()

    def init_db(self):
        os.makedirs(os.path.dirname(self.db_path) or ".", exist_ok=True)
        with self._connection(write=True) as connection:
            version = int(connection.execute("PRAGMA user_version").fetchone()[0])
            if version > SCHEMA_VERSION:
                raise DatabaseMigrationError(
                    f"Database schema version {version} is newer than supported "
                    f"version {SCHEMA_VERSION}."
                )

            table_exists = connection.execute(
                "SELECT 1 FROM sqlite_master WHERE type='table' "
                "AND name='run_history'"
            ).fetchone()
            if not table_exists:
                self._create_schema(connection, "run_history")
                self._create_indexes(connection)
            elif version < SCHEMA_VERSION:
                self._migrate_legacy_schema(connection)
            else:
                self._validate_schema(connection)
                self._create_indexes(connection)
            connection.execute(f"PRAGMA user_version={SCHEMA_VERSION}")

    @staticmethod
    def _create_schema(connection, table_name):
        connection.execute(f"""
            CREATE TABLE {table_name} (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                run_uuid TEXT NOT NULL UNIQUE,
                run_number INTEGER NOT NULL,
                segment_kind TEXT NOT NULL,
                segment_index INTEGER NOT NULL,
                status TEXT NOT NULL,
                exit_code INTEGER,
                daq_status TEXT NOT NULL,
                daq_exit_code INTEGER,
                daq_error_message TEXT,
                production_status TEXT NOT NULL,
                production_exit_code INTEGER,
                production_error_message TEXT,
                metadata_path TEXT,
                root_file TEXT,
                created_at TEXT NOT NULL,
                updated_at TEXT NOT NULL,
                start_time TEXT,
                output_file TEXT,
                output_path_key TEXT,
                hv TEXT,
                config_path TEXT,
                config_dump TEXT,
                env_metadata TEXT,
                daq_summary TEXT,
                production_summary TEXT,
                error_message TEXT,
                migrated_legacy INTEGER NOT NULL DEFAULT 0
                    CHECK (migrated_legacy IN (0, 1))
            )
        """)

    @staticmethod
    def _create_indexes(connection):
        # Failed/cancelled attempts stay in history but do not block a retry.
        # Migrated rows are excluded because old DBs may contain duplicates.
        connection.execute("""
            CREATE UNIQUE INDEX IF NOT EXISTS uq_run_history_live_output
            ON run_history(output_path_key)
            WHERE migrated_legacy = 0
              AND output_path_key IS NOT NULL
              AND status IN (
                  'daq_launching', 'daq_running', 'daq_completed',
                  'production_launching', 'production_completed'
              )
        """)
        connection.execute("""
            CREATE UNIQUE INDEX IF NOT EXISTS uq_run_history_completed_root
            ON run_history(root_file)
            WHERE migrated_legacy = 0
              AND root_file IS NOT NULL AND root_file <> ''
              AND production_status = 'production_completed'
        """)
        connection.execute("""
            CREATE INDEX IF NOT EXISTS idx_run_history_output_path_key
            ON run_history(output_path_key)
        """)
        connection.execute("""
            CREATE INDEX IF NOT EXISTS idx_run_history_run_number
            ON run_history(run_number, segment_kind, segment_index)
        """)
        connection.execute("""
            CREATE INDEX IF NOT EXISTS idx_run_history_status
            ON run_history(status, updated_at)
        """)

    @staticmethod
    def _required_columns():
        return {
            "id", "run_uuid", "run_number", "segment_kind", "segment_index",
            "status", "exit_code", "daq_status", "daq_exit_code",
            "daq_error_message", "production_status", "production_exit_code",
            "production_error_message", "metadata_path", "root_file",
            "created_at", "updated_at", "start_time", "output_file",
            "output_path_key", "hv", "config_path", "config_dump",
            "env_metadata", "daq_summary", "production_summary",
            "error_message", "migrated_legacy",
        }

    def _validate_schema(self, connection):
        columns = {
            row["name"] for row in connection.execute(
                "PRAGMA table_info(run_history)"
            ).fetchall()
        }
        missing = sorted(self._required_columns() - columns)
        if missing:
            raise DatabaseMigrationError(
                "run_history is marked current but required columns are missing: "
                + ", ".join(missing)
            )

    def _legacy_path_base(self):
        database_dir = os.path.dirname(self.db_path)
        if os.path.basename(database_dir) == "data":
            return os.path.dirname(database_dir)
        return database_dir

    def _output_path_key(self, value):
        if value is None or str(value) == "":
            return None
        path = os.path.expanduser(os.fspath(value))
        if not os.path.isabs(path):
            path = os.path.join(self._legacy_path_base(), path)
        return os.path.normcase(os.path.abspath(os.path.normpath(path)))

    @staticmethod
    def _legacy_value(values, name, default=None):
        return values[name] if name in values else default

    def _migrate_legacy_schema(self, connection):
        migration_table = f"run_history__schema_v{SCHEMA_VERSION}"
        if connection.execute(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",
            (migration_table,),
        ).fetchone():
            raise DatabaseMigrationError(
                f"Unexpected migration table already exists: {migration_table}"
            )

        old_columns = {
            row["name"] for row in connection.execute(
                "PRAGMA table_info(run_history)"
            ).fetchall()
        }
        legacy_core = {"id", "start_time", "output_file", "hv", "config_dump"}
        if not legacy_core.issubset(old_columns):
            missing = sorted(legacy_core - old_columns)
            raise DatabaseMigrationError(
                "run_history does not match a supported legacy schema; "
                "refusing unsafe migration. Missing: " + ", ".join(missing)
            )
        sequence_exists = connection.execute(
            "SELECT 1 FROM sqlite_master WHERE type='table' "
            "AND name='sqlite_sequence'"
        ).fetchone()
        sequence_row = (
            connection.execute(
                "SELECT seq FROM sqlite_sequence WHERE name='run_history'"
            ).fetchone()
            if sequence_exists else None
        )
        previous_sequence = int(sequence_row[0]) if sequence_row else 0
        rows = connection.execute(
            "SELECT * FROM run_history ORDER BY id"
        ).fetchall()
        self._create_schema(connection, migration_table)

        seen_uuids = set()
        for ordinal, row in enumerate(rows, start=1):
            values = dict(row)
            raw_id = values.get("id")
            old_id = int(raw_id if raw_id is not None else ordinal)
            output_file = self._legacy_value(values, "output_file")
            start_time = self._legacy_value(values, "start_time")
            candidate_uuid = self._legacy_value(values, "run_uuid", "")
            candidate_uuid = "" if candidate_uuid is None else str(candidate_uuid)
            if not candidate_uuid or candidate_uuid in seen_uuids:
                candidate_uuid = str(uuid.uuid5(
                    uuid.NAMESPACE_URL,
                    f"cpnr-run-history:{self.db_path}:{old_id}:"
                    f"{output_file!r}:{start_time!r}",
                ))
            seen_uuids.add(candidate_uuid)

            migrated_at = _utc_now()
            run_number = self._legacy_value(values, "run_number", 0)
            segment_kind = self._legacy_value(values, "segment_kind", "legacy")
            segment_index = self._legacy_value(values, "segment_index", old_id)
            status = self._legacy_value(values, "status", "legacy_unknown")
            daq_status = self._legacy_value(
                values, "daq_status", "legacy_unknown"
            )
            production_status = self._legacy_value(
                values, "production_status", "legacy_unknown"
            )
            created_at = self._legacy_value(values, "created_at", migrated_at)
            updated_at = self._legacy_value(values, "updated_at", created_at)
            output_path_key = self._legacy_value(values, "output_path_key")
            if not output_path_key:
                output_path_key = self._output_path_key(output_file)

            # Only newly required identity metadata is synthesized. All
            # nullable historical payload fields are copied without coercion.
            if run_number is None:
                run_number = 0
            if segment_kind is None:
                segment_kind = "legacy"
            if segment_index is None:
                segment_index = old_id
            if status is None:
                status = "legacy_unknown"
            if daq_status is None:
                daq_status = "legacy_unknown"
            if production_status is None:
                production_status = "legacy_unknown"
            if created_at is None:
                created_at = migrated_at
            if updated_at is None:
                updated_at = created_at

            connection.execute(f"""
                INSERT INTO {migration_table} (
                    id, run_uuid, run_number, segment_kind, segment_index,
                    status, exit_code, daq_status, daq_exit_code,
                    daq_error_message, production_status,
                    production_exit_code, production_error_message,
                    metadata_path, root_file, created_at, updated_at,
                    start_time, output_file, output_path_key, hv, config_path,
                    config_dump, env_metadata, daq_summary,
                    production_summary, error_message, migrated_legacy
                ) VALUES (
                    ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,
                    ?, ?, ?, ?, ?, ?, ?, ?, ?, 1
                )
            """, (
                old_id, candidate_uuid, int(run_number), str(segment_kind),
                int(segment_index), str(status),
                self._legacy_value(values, "exit_code"), str(daq_status),
                self._legacy_value(values, "daq_exit_code"),
                self._legacy_value(values, "daq_error_message"),
                str(production_status),
                self._legacy_value(values, "production_exit_code"),
                self._legacy_value(values, "production_error_message"),
                self._legacy_value(values, "metadata_path"),
                self._legacy_value(values, "root_file"), str(created_at),
                str(updated_at), start_time, output_file, output_path_key,
                self._legacy_value(values, "hv"),
                self._legacy_value(values, "config_path"),
                self._legacy_value(values, "config_dump"),
                self._legacy_value(values, "env_metadata"),
                self._legacy_value(values, "daq_summary"),
                self._legacy_value(values, "production_summary"),
                self._legacy_value(values, "error_message"),
            ))

        connection.execute("DROP TABLE run_history")
        connection.execute(
            f"ALTER TABLE {migration_table} RENAME TO run_history"
        )
        maximum_id = int(connection.execute(
            "SELECT COALESCE(MAX(id), 0) FROM run_history"
        ).fetchone()[0])
        connection.execute("DELETE FROM sqlite_sequence WHERE name='run_history'")
        connection.execute(
            "INSERT INTO sqlite_sequence(name, seq) VALUES('run_history', ?)",
            (max(previous_sequence, maximum_id),),
        )
        self._create_indexes(connection)
        self._validate_schema(connection)
        migrated_ids = [
            int(row[0]) for row in connection.execute(
                "SELECT id FROM run_history ORDER BY id"
            ).fetchall()
        ]
        original_ids = [int(dict(row)["id"]) for row in rows]
        if migrated_ids != original_ids:
            raise DatabaseMigrationError(
                "Run-history migration did not preserve the exact row identities."
            )
        foreign_key_errors = connection.execute(
            "PRAGMA foreign_key_check"
        ).fetchall()
        if foreign_key_errors:
            raise DatabaseMigrationError(
                "Run-history migration failed foreign_key_check."
            )
        quick_check = connection.execute("PRAGMA quick_check").fetchone()[0]
        if quick_check != "ok":
            raise DatabaseMigrationError(
                f"Run-history migration failed quick_check: {quick_check}"
            )

    @staticmethod
    def _validate_segment(segment_kind, segment_index):
        allowed = {"single", "batch", "threshold_scan", "legacy"}
        if segment_kind not in allowed:
            raise ValueError(
                f"segment_kind must be one of {sorted(allowed)}: {segment_kind}"
            )
        if int(segment_index) < 0:
            raise ValueError("segment_index must be non-negative")

    @staticmethod
    def _new_uuid(value=None):
        if value is None:
            return str(uuid.uuid4())
        try:
            return str(uuid.UUID(str(value)))
        except (AttributeError, TypeError, ValueError) as exc:
            raise ValueError(f"run_uuid is not a valid UUID: {value}") from exc

    def record_run_start(
        self, output_file, env_dict, config_path, *, run_number=None,
        segment_kind="single", segment_index=1, metadata_path=None,
        run_uuid=None, status="daq_launching",
    ):
        """Insert one immutable run identity and return its integer row id."""

        output_file = _absolute_path(output_file)
        if not output_file:
            raise ValueError("output_file is required")
        config_path = _absolute_path(config_path)
        metadata_path = _absolute_path(
            metadata_path or f"{output_file}.run.json"
        )
        run_number = (
            _infer_run_number(output_file) if run_number is None else int(run_number)
        )
        if run_number < 0:
            raise ValueError("run_number must be non-negative")
        self._validate_segment(segment_kind, segment_index)
        if status not in {"launching", "daq_launching"}:
            raise ValueError("new runs must start in daq_launching status")
        run_uuid = self._new_uuid(run_uuid)
        now = _utc_now()

        config_dump = ""
        if config_path and os.path.exists(config_path):
            with open(config_path, "r", encoding="utf-8") as config_file:
                config_dump = config_file.read()
        env = dict(env_dict or {})
        env_json = json.dumps(env, ensure_ascii=False, allow_nan=False)
        hv_fallback = env.get("Applied HV") or env.get("AppliedHV") or "Unknown"

        with self._connection(write=True) as connection:
            cursor = connection.execute("""
                INSERT INTO run_history (
                    run_uuid, run_number, segment_kind, segment_index, status,
                    exit_code, daq_status, daq_exit_code, daq_error_message,
                    production_status, production_exit_code,
                    production_error_message, metadata_path, root_file,
                    created_at, updated_at, start_time, output_file,
                    output_path_key, hv, config_path, config_dump,
                    env_metadata, error_message, migrated_legacy
                ) VALUES (
                    ?, ?, ?, ?, 'daq_launching', NULL, 'daq_launching', NULL,
                    NULL, 'not_started', NULL, NULL, ?, NULL, ?, ?, ?, ?, ?,
                    ?, ?, ?, ?, NULL, 0
                )
            """, (
                run_uuid, run_number, segment_kind, int(segment_index),
                metadata_path, now, now, now, output_file,
                self._output_path_key(output_file), str(hv_fallback),
                config_path, config_dump, env_json,
            ))
            run_id = int(cursor.lastrowid)
        return run_id

    def get_run(self, run_id):
        with self._connection() as connection:
            row = connection.execute(
                "SELECT * FROM run_history WHERE id = ?", (int(run_id),)
            ).fetchone()
        if row is None:
            raise DatabaseIdentityError(f"Run id does not exist: {run_id}")
        return dict(row)

    def get_run_uuid(self, run_id):
        return str(self.get_run(run_id)["run_uuid"])

    def resolve_run_identity(self, run_id, *, run_uuid=None, output_file=None):
        """Resolve an id plus optional immutable identity guards to one row."""

        clauses = ["id = ?"]
        values = [int(run_id)]
        if run_uuid is not None:
            clauses.append("run_uuid = ?")
            values.append(str(run_uuid))
        if output_file is not None:
            clauses.append("output_path_key = ?")
            values.append(self._output_path_key(output_file))
        with self._connection() as connection:
            rows = connection.execute(
                "SELECT * FROM run_history WHERE " + " AND ".join(clauses),
                values,
            ).fetchall()
        if len(rows) != 1:
            raise DatabaseIdentityError(
                "Run identity did not resolve to exactly one row: "
                f"id={run_id}, uuid={run_uuid!r}, output={output_file!r}."
            )
        return dict(rows[0])

    def find_run_id_by_output(self, output_file):
        output_key = self._output_path_key(output_file)
        if output_key is None:
            return None
        with self._connection() as connection:
            rows = connection.execute(
                "SELECT id FROM run_history WHERE output_path_key = ? "
                "ORDER BY id", (output_key,),
            ).fetchall()
        if not rows:
            return None
        if len(rows) != 1:
            raise DatabaseIdentityError(
                f"Raw output resolves to {len(rows)} run rows, not exactly one: "
                f"{_absolute_path(output_file)}"
            )
        return int(rows[0]["id"])

    @staticmethod
    def _require_one(cursor, description):
        if cursor.rowcount != 1:
            raise DatabaseIdentityError(
                f"Expected exactly one run row for {description}; "
                f"updated {cursor.rowcount}."
            )

    def _identity_where(self, run_id, run_uuid=None, output_file=None):
        clauses = ["id = ?"]
        values = [int(run_id)]
        if run_uuid is not None:
            clauses.append("run_uuid = ?")
            values.append(str(run_uuid))
        if output_file is not None:
            clauses.append("output_path_key = ?")
            values.append(self._output_path_key(output_file))
        return " AND ".join(clauses), values

    def mark_daq_launch_failed(
        self, run_id, error_message, *, run_uuid=None, output_file=None
    ):
        where, identity_values = self._identity_where(
            run_id, run_uuid, output_file
        )
        with self._connection(write=True) as connection:
            cursor = connection.execute(f"""
                UPDATE run_history
                SET status = 'daq_launch_failed', exit_code = -1,
                    daq_status = 'daq_launch_failed', daq_exit_code = -1,
                    daq_error_message = ?, error_message = ?, updated_at = ?
                WHERE {where} AND daq_status = 'daq_launching'
                  AND production_status = 'not_started'
            """, [str(error_message), str(error_message), _utc_now()]
                 + identity_values)
            self._require_one(cursor, f"DAQ launch identity id={run_id}")

    def finalize_daq_run(
        self, run_id, *, status, exit_code, summary_dict=None,
        metadata_path=None, error_message=None, run_uuid=None,
        output_file=None,
    ):
        if status not in DAQ_TERMINAL_STATUSES - {"daq_launch_failed"}:
            raise ValueError(f"invalid DAQ terminal status: {status}")
        summary_json = (
            json.dumps(dict(summary_dict), ensure_ascii=False, allow_nan=False)
            if summary_dict is not None else None
        )
        where, identity_values = self._identity_where(
            run_id, run_uuid, output_file
        )
        updates = [
            "status = ?", "exit_code = ?", "daq_status = ?",
            "daq_exit_code = ?", "daq_error_message = ?",
            "error_message = ?", "updated_at = ?",
        ]
        values = [
            status, exit_code, status, exit_code, error_message,
            error_message, _utc_now(),
        ]
        if summary_json is not None:
            updates.append("daq_summary = ?")
            values.append(summary_json)
        if metadata_path is not None:
            updates.append("metadata_path = ?")
            values.append(_absolute_path(metadata_path))
        values.extend(identity_values)
        with self._connection(write=True) as connection:
            cursor = connection.execute(
                f"UPDATE run_history SET {', '.join(updates)} "
                f"WHERE {where} AND daq_status IN "
                "('daq_launching', 'daq_running') "
                "AND production_status = 'not_started'", values,
            )
            self._require_one(cursor, f"DAQ terminal identity id={run_id}")

    def begin_production(self, run_id, *, run_uuid=None, output_file=None):
        where, identity_values = self._identity_where(
            run_id, run_uuid, output_file
        )
        with self._connection(write=True) as connection:
            cursor = connection.execute(f"""
                UPDATE run_history
                SET status = 'production_launching', exit_code = NULL,
                    production_status = 'production_launching',
                    production_exit_code = NULL,
                    production_error_message = NULL,
                    error_message = NULL, updated_at = ?
                WHERE {where} AND daq_status = 'daq_completed'
                  AND production_status IN (
                      'not_started', 'production_launch_failed',
                      'production_failed', 'production_cancelled'
                  )
            """, [_utc_now()] + identity_values)
            self._require_one(cursor, f"production launch identity id={run_id}")

    def finalize_production_run(
        self, run_id, *, status, exit_code, summary_dict=None,
        root_file=None, error_message=None, run_uuid=None, output_file=None,
    ):
        if status not in PRODUCTION_TERMINAL_STATUSES:
            raise ValueError(f"invalid production terminal status: {status}")
        summary_json = (
            json.dumps(dict(summary_dict), ensure_ascii=False, allow_nan=False)
            if summary_dict is not None else None
        )
        where, identity_values = self._identity_where(
            run_id, run_uuid, output_file
        )
        updates = [
            "status = ?", "exit_code = ?", "production_status = ?",
            "production_exit_code = ?", "production_error_message = ?",
            "error_message = ?", "updated_at = ?",
        ]
        values = [
            status, exit_code, status, exit_code, error_message,
            error_message, _utc_now(),
        ]
        if summary_json is not None:
            updates.append("production_summary = ?")
            values.append(summary_json)
        if root_file is not None:
            updates.append("root_file = ?")
            values.append(_absolute_path(root_file))
        values.extend(identity_values)
        with self._connection(write=True) as connection:
            cursor = connection.execute(
                f"UPDATE run_history SET {', '.join(updates)} "
                f"WHERE {where} AND production_status = "
                "'production_launching'", values,
            )
            self._require_one(cursor, f"production terminal identity id={run_id}")

    def update_run_status(
        self, run_id, status, *, exit_code=None, metadata_path=None,
        error_message=None,
    ):
        """Narrow compatibility wrapper for pre-v4 GUI callers."""

        if status in {"launch_failed", "daq_launch_failed"}:
            return self.mark_daq_launch_failed(run_id, error_message or "")
        if status == "production_launching":
            return self.begin_production(run_id)
        raise ValueError(
            "Generic lifecycle rewrites are not allowed; use a stage-specific API"
        )

    def update_daq_summary(
        self, run_id, summary_dict, *, status=None, exit_code=None,
        metadata_path=None,
    ):
        if status is not None:
            return self.finalize_daq_run(
                run_id, status=status, exit_code=exit_code,
                summary_dict=summary_dict, metadata_path=metadata_path,
            )
        summary_json = json.dumps(
            dict(summary_dict or {}), ensure_ascii=False, allow_nan=False
        )
        with self._connection(write=True) as connection:
            cursor = connection.execute(
                "UPDATE run_history SET daq_summary = ?, updated_at = ? "
                "WHERE id = ?", (summary_json, _utc_now(), int(run_id)),
            )
            self._require_one(cursor, f"id={run_id}")

    def update_production_summary(
        self, raw_file_path, summary_dict, *, run_id=None,
        status="production_completed", exit_code=0, root_file=None,
        error_message=None,
    ):
        """Compatibility API that still requires one unambiguous identity."""

        if run_id is None:
            run_id = self.find_run_id_by_output(raw_file_path)
            if run_id is None:
                raise DatabaseIdentityError(
                    f"No run row exists for raw output: "
                    f"{_absolute_path(raw_file_path)}"
                )
        self.finalize_production_run(
            run_id, status=status, exit_code=exit_code,
            summary_dict=summary_dict, root_file=root_file,
            error_message=error_message, output_file=raw_file_path,
        )
