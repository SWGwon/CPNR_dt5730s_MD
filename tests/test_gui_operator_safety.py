import os
import shutil
import hashlib
import json
import signal
import sys
import tempfile
import time
import unittest
import uuid
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


PROJECT_ROOT = Path(__file__).resolve().parents[1]
GUI_DIR = PROJECT_ROOT / "gui"
if str(GUI_DIR) not in sys.path:
    sys.path.insert(0, str(GUI_DIR))

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

try:
    from PyQt6.QtCore import QProcess, QSettings
    from PyQt6.QtGui import QCloseEvent
    from PyQt6.QtWidgets import QApplication
except ImportError:  # pragma: no cover - optional outside the GUI environment
    QProcess = None
    QSettings = None
    QApplication = None
    QCloseEvent = None


class DummyDatabaseManager:
    def __init__(self, *_args, **_kwargs):
        pass

    def update_production_summary(self, *_args, **_kwargs):
        pass

    def find_run_id_by_output(self, *_args, **_kwargs):
        return None


class FailingTerminalDatabase(DummyDatabaseManager):
    def begin_production(self, *_args, **_kwargs):
        pass

    def finalize_production_run(self, *_args, **_kwargs):
        raise RuntimeError("database disk unavailable")


class RecordingDaqDatabase(DummyDatabaseManager):
    def __init__(self, *, fail=False):
        self.fail = fail
        self.finalizations = []
        self.launch_failures = []

    def finalize_daq_run(self, run_id, **kwargs):
        self.finalizations.append((run_id, kwargs))
        if self.fail:
            raise RuntimeError("database is locked")

    def mark_daq_launch_failed(self, run_id, message, **kwargs):
        self.launch_failures.append((run_id, message, kwargs))
        if self.fail:
            raise RuntimeError("database is locked")


class RecordingProductionDatabase(DummyDatabaseManager):
    def __init__(self):
        self.begins = []
        self.finalizations = []

    def begin_production(self, run_id, **kwargs):
        self.begins.append((run_id, kwargs))

    def finalize_production_run(self, run_id, **kwargs):
        self.finalizations.append((run_id, kwargs))


class MismatchedContextDatabase(DummyDatabaseManager):
    def __init__(self):
        self.resolve_calls = 0
        self.find_calls = 0
        self.begin_calls = 0

    def resolve_run_identity(self, *_args, **_kwargs):
        self.resolve_calls += 1
        raise RuntimeError("UUID mismatch")

    def find_run_id_by_output(self, *_args, **_kwargs):
        self.find_calls += 1
        return 99

    def begin_production(self, *_args, **_kwargs):
        self.begin_calls += 1


def make_terminal_run_fixture(root, run_number, status="completed"):
    root = Path(root)
    raw = root / f"sample_run{run_number:03d}.dat"
    metadata = Path(f"{raw}.run.json")
    config = Path(f"{raw}.config.conf")
    frontend = root / "frontend_dt5730"
    raw_payload = b"r" * 24
    raw.write_bytes(raw_payload)
    config.write_text("[Digitizer]\nTriggerPolarity = 1\n", encoding="utf-8")
    frontend.write_bytes(b"frontend-test-binary")
    context = {
        "raw_file": str(raw),
        "metadata_path": str(metadata),
        "run_number": run_number,
        "config_path": str(config),
        "config_sha256": hashlib.sha256(config.read_bytes()).hexdigest(),
        "frontend_path": str(frontend),
        "frontend_sha256": hashlib.sha256(frontend.read_bytes()).hexdigest(),
    }
    termination = {
        "completed": "operator_stop",
        "failed": "failure",
        "cancelled": "cancelled_before_start",
    }[status]
    failure_reason = "hardware readout failed" if status == "failed" else None
    document = {
        "schema_version": 2,
        "run_number": run_number,
        "acquisition_status": status,
        "termination_reason": termination,
        "failure_reason": failure_reason,
        "requested_raw_output_path": str(raw),
        "raw_output_path": str(raw),
        "raw_output_published": True,
        "raw_output_finalized": True,
        "raw_finalization_error": None,
        "raw_output_size_bytes": len(raw_payload),
        "raw_event_bytes": len(raw_payload),
        "recorded_events": 1,
        "last_complete_offset": len(raw_payload),
        "raw_output_sha256": hashlib.sha256(raw_payload).hexdigest(),
        "metadata_path": str(metadata),
        "config_path": str(config),
        "config_sha256": context["config_sha256"],
        "binary_path": str(frontend),
        "binary_sha256": context["frontend_sha256"],
    }
    metadata.write_text(json.dumps(document), encoding="utf-8")
    return context, document

@unittest.skipIf(QApplication is None, "PyQt6 is not installed")
class GuiOperatorSafetyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.settings_dir = tempfile.TemporaryDirectory()
        for settings_format in (
            QSettings.Format.NativeFormat,
            QSettings.Format.IniFormat,
        ):
            QSettings.setPath(
                settings_format,
                QSettings.Scope.UserScope,
                cls.settings_dir.name,
            )
        cls.app = QApplication.instance() or QApplication([])

    @classmethod
    def tearDownClass(cls):
        cls.settings_dir.cleanup()

    def wait_for(self, predicate, timeout=3.0):
        deadline = time.monotonic() + timeout
        while not predicate() and time.monotonic() < deadline:
            self.app.processEvents()
            time.sleep(0.005)
        self.app.processEvents()
        self.assertTrue(predicate(), "condition was not reached before timeout")

    def test_zero_finite_stop_conditions_are_rejected(self):
        from widgets.DaqTab import validate_stop_condition

        validate_stop_condition(0, 0, 0)
        with self.assertRaisesRegex(ValueError, "Max Events"):
            validate_stop_condition(1, 0, 3600)
        with self.assertRaisesRegex(ValueError, "Max Time"):
            validate_stop_condition(2, 100, 0)
        validate_stop_condition(1, 1, 0)
        validate_stop_condition(2, 0, 1)

    def test_daq_start_blocks_zero_event_limit_before_runtime_launch(self):
        from widgets import DaqTab as daq_module

        source = Path(__file__).resolve().parents[1] / "config" / (
            "dt5730s_inorganic.conf"
        )
        with tempfile.TemporaryDirectory() as directory:
            config_path = Path(directory) / "run.conf"
            shutil.copy2(source, config_path)
            with (
                mock.patch.object(
                    daq_module, "find_project_root", return_value=Path(directory)
                ),
                mock.patch.object(
                    daq_module, "DatabaseManager", DummyDatabaseManager
                ),
            ):
                tab = daq_module.DaqTab()
            tab.config_input.setText(str(config_path))
            tab.combo_stop_cond.setCurrentIndex(1)
            tab.spin_events.setValue(0)
            with (
                mock.patch.object(tab, "validate_runtime_artifacts") as runtime,
                mock.patch.object(daq_module.QMessageBox, "critical") as critical,
            ):
                tab.start_daq_sequence()
            runtime.assert_not_called()
            self.assertTrue(tab.btn_start.isEnabled())
            self.assertIn("Max Events", critical.call_args.args[2])
            tab.disk_timer.stop()
            tab.deleteLater()

    def test_daq_storage_preview_tracks_selected_output_and_exact_raw_size(self):
        from widgets import DaqTab as daq_module

        source = Path(__file__).resolve().parents[1] / "config" / (
            "dt5730s_ls_coin.conf"
        )
        with tempfile.TemporaryDirectory() as project_dir, \
             tempfile.TemporaryDirectory() as output_dir:
            config_path = Path(project_dir) / "run.conf"
            shutil.copy2(source, config_path)
            with (
                mock.patch.object(
                    daq_module, "find_project_root",
                    return_value=Path(project_dir),
                ),
                mock.patch.object(
                    daq_module, "DatabaseManager", DummyDatabaseManager
                ),
            ):
                tab = daq_module.DaqTab()
            selected = Path(output_dir) / "usb_run.dat"
            tab.config_input.setText(str(config_path))
            tab.output_input.setText(str(selected))
            tab.combo_stop_cond.setCurrentIndex(1)
            tab.spin_events.setValue(200000)
            tab.update_disk_space()

            plan = tab.last_storage_plan
            self.assertIsNotNone(plan)
            self.assertEqual(plan["output_parent"], str(selected.parent))
            self.assertEqual(plan["event_bytes"], 24 + 4 * 512 * 2)
            self.assertEqual(
                plan["expected_total_bytes"], (24 + 4 * 512 * 2) * 200000
            )
            self.assertIn(str(selected.parent), tab.lbl_output_storage.text())
            self.assertIn("planned RAW", tab.lbl_output_storage.text())
            tab.disk_timer.stop()
            tab.deleteLater()

    def test_config_edit_emits_path_scoped_dirty_and_save_clears_it(self):
        from widgets.ConfigTab import ConfigTab

        source = Path(__file__).resolve().parents[1] / "config" / (
            "dt5730s_inorganic.conf"
        )
        with tempfile.TemporaryDirectory() as directory:
            config_path = Path(directory) / "operator_test.conf"
            shutil.copy2(source, config_path)
            tab = ConfigTab()
            dirty_events = []
            tab.configDirtyChanged.connect(
                lambda path, dirty: dirty_events.append((path, dirty))
            )
            tab.load_file(str(config_path))
            self.assertFalse(tab.is_dirty())

            editable_row = next(
                row for row in range(tab.table.rowCount())
                if tab.table.item(row, 0).text() == "SoftwareDSP"
                and tab.table.item(row, 1).text() == "BaselineSamples"
            )
            tab.table.item(editable_row, 2).setText("151")
            self.app.processEvents()
            self.assertTrue(tab.is_dirty())
            self.assertIn("UNSAVED", tab.lbl_current_file.text())
            self.assertEqual(
                dirty_events[-1], (str(config_path.resolve()), True)
            )

            with mock.patch(
                "widgets.ConfigTab.QMessageBox.critical"
            ) as critical:
                tab.save_config()
            critical.assert_not_called()
            self.assertFalse(tab.is_dirty())
            self.assertEqual(
                dirty_events[-1], (str(config_path.resolve()), False)
            )
            self.assertIn(
                "BaselineSamples = 151",
                config_path.read_text(encoding="utf-8"),
            )
            tab.deleteLater()

    def test_config_atomic_commit_failure_preserves_original_and_dirty_state(self):
        from widgets.ConfigTab import ConfigTab

        class CommitFailingSaveFile:
            instances = []

            def __init__(self, path):
                self.path = path
                self.payload = b""
                self.opened = False
                self.cancelled = False
                self.direct_fallback = None
                self.__class__.instances.append(self)

            def setDirectWriteFallback(self, enabled):
                self.direct_fallback = enabled

            def open(self, _mode):
                self.opened = True
                return True

            def write(self, payload):
                self.payload = bytes(payload)
                return len(self.payload)

            def commit(self):
                return False

            def errorString(self):
                return "simulated atomic commit failure"

            def isOpen(self):
                return self.opened

            def cancelWriting(self):
                self.cancelled = True
                self.opened = False

        source = Path(__file__).resolve().parents[1] / "config" / (
            "dt5730s_inorganic.conf"
        )
        with tempfile.TemporaryDirectory() as directory:
            config_path = Path(directory) / "atomic_failure.conf"
            shutil.copy2(source, config_path)
            original_bytes = config_path.read_bytes()
            tab = ConfigTab()
            tab.load_file(str(config_path))
            editable_row = next(
                row for row in range(tab.table.rowCount())
                if tab.table.item(row, 0).text() == "SoftwareDSP"
                and tab.table.item(row, 1).text() == "BaselineSamples"
            )
            tab.table.item(editable_row, 2).setText("152")
            self.app.processEvents()
            self.assertTrue(tab.is_dirty())

            with (
                mock.patch(
                    "widgets.ConfigTab.QSaveFile",
                    CommitFailingSaveFile,
                ),
                mock.patch(
                    "widgets.ConfigTab.QMessageBox.critical"
                ) as critical,
            ):
                tab.save_config()

            staged = CommitFailingSaveFile.instances[-1]
            self.assertEqual(staged.path, str(config_path.resolve()))
            self.assertFalse(staged.direct_fallback)
            self.assertIn(b"BaselineSamples = 152", staged.payload)
            self.assertTrue(staged.cancelled)
            self.assertEqual(config_path.read_bytes(), original_bytes)
            self.assertTrue(tab.is_dirty())
            self.assertIn("UNSAVED", tab.lbl_current_file.text())
            self.assertEqual(critical.call_args.args[1], "Save Failed")
            self.assertIn(
                "simulated atomic commit failure",
                critical.call_args.args[2],
            )
            tab.deleteLater()

    def test_daq_blocks_dirty_only_for_the_selected_config(self):
        from widgets.DaqTab import DaqTab

        with tempfile.TemporaryDirectory() as directory:
            selected = Path(directory) / "selected.conf"
            other = Path(directory) / "other.conf"
            fake_tab = SimpleNamespace(
                proj_dir=directory,
                config_dirty=True,
                dirty_config_path=str(selected.resolve()),
            )
            with self.assertRaisesRegex(ValueError, "Save .conf"):
                DaqTab.ensure_selected_config_is_saved(fake_tab, selected)
            DaqTab.ensure_selected_config_is_saved(fake_tab, other)

    def test_daq_start_blocks_matching_unsaved_config(self):
        from widgets import DaqTab as daq_module

        source = Path(__file__).resolve().parents[1] / "config" / (
            "dt5730s_inorganic.conf"
        )
        with tempfile.TemporaryDirectory() as directory:
            config_path = Path(directory) / "dirty.conf"
            shutil.copy2(source, config_path)
            with (
                mock.patch.object(
                    daq_module, "find_project_root", return_value=Path(directory)
                ),
                mock.patch.object(
                    daq_module, "DatabaseManager", DummyDatabaseManager
                ),
            ):
                tab = daq_module.DaqTab()
            tab.config_input.setText(str(config_path))
            tab.set_config_dirty(str(config_path), True)
            with (
                mock.patch.object(tab, "validate_runtime_artifacts") as runtime,
                mock.patch.object(daq_module.QMessageBox, "critical") as critical,
            ):
                tab.start_daq_sequence()
            runtime.assert_not_called()
            self.assertTrue(tab.btn_start.isEnabled())
            self.assertIn("Save .conf", critical.call_args.args[2])
            tab.disk_timer.stop()
            tab.deleteLater()

    def test_mainwindow_propagates_config_dirty_state_to_daq(self):
        from widgets import DaqTab as daq_module
        from widgets import ProductionTab as production_module
        from windows.MainWindow import MainWindow

        source = Path(__file__).resolve().parents[1] / "config" / (
            "dt5730s_inorganic.conf"
        )
        with tempfile.TemporaryDirectory() as directory:
            config_path = Path(directory) / "mainwindow.conf"
            shutil.copy2(source, config_path)
            with (
                mock.patch.object(
                    daq_module, "DatabaseManager", DummyDatabaseManager
                ),
                mock.patch.object(
                    production_module, "DatabaseManager", DummyDatabaseManager
                ),
            ):
                window = MainWindow()
            window.config_tab.load_file(str(config_path))
            editable_row = next(
                row for row in range(window.config_tab.table.rowCount())
                if window.config_tab.table.item(row, 0).text() == "SoftwareDSP"
                and window.config_tab.table.item(row, 1).text()
                == "BaselineSamples"
            )
            window.config_tab.table.item(editable_row, 2).setText("152")
            self.app.processEvents()
            self.assertTrue(window.daq_tab.config_dirty)
            self.assertEqual(
                window.daq_tab.dirty_config_path, str(config_path.resolve())
            )
            window.daq_tab.disk_timer.stop()
            window.monitor_tab.cleanup()
            window.deleteLater()

    def make_production_tab(self, project_root):
        from widgets import ProductionTab as production_module

        with (
            mock.patch.object(
                production_module, "find_project_root",
                return_value=Path(project_root),
            ),
            mock.patch.object(
                production_module, "DatabaseManager", DummyDatabaseManager
            ),
        ):
            return production_module.ProductionTab()

    @unittest.skipUnless(os.name == "posix", "QProcess shell test is POSIX-only")
    def test_production_reads_real_stderr_channel_and_recovers_on_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            tab = self.make_production_tab(directory)
            tab._run_active = True
            tab.btn_run.setEnabled(False)
            tab.process.start(
                "/bin/sh",
                ["-c", "printf 'stderr-only <bad&>\\n' >&2; exit 7"],
            )
            self.wait_for(
                lambda: tab.process.state() == QProcess.ProcessState.NotRunning
                and tab.btn_run.isEnabled()
            )
            rendered = tab.log_console.toPlainText()
            self.assertIn("stderr-only <bad&>", rendered)
            self.assertIn("Exited with Code: 7", rendered)
            self.assertEqual(tab.progress_bar.format(), "Failed (exit 7)")
            self.assertFalse(tab._run_active)
            tab.deleteLater()

    def test_production_failed_to_start_restores_controls(self):
        with tempfile.TemporaryDirectory() as directory:
            tab = self.make_production_tab(directory)
            tab._run_active = True
            tab.btn_run.setEnabled(False)
            tab.process.start("/definitely/not/a/real/converter", [])
            self.wait_for(
                lambda: tab.process.state() == QProcess.ProcessState.NotRunning
                and tab.btn_run.isEnabled()
            )
            rendered = tab.log_console.toPlainText()
            self.assertIn("Process Error", rendered)
            self.assertIn("FailedToStart", rendered)
            self.assertEqual(tab.progress_bar.format(), "Failed to start")
            self.assertFalse(tab._run_active)
            tab.deleteLater()

    def test_terminal_metadata_requires_matching_launch_identity(self):
        from widgets.DaqTab import load_terminal_run_metadata

        with tempfile.TemporaryDirectory() as directory:
            context, document = make_terminal_run_fixture(directory, 12)
            observed, identity = load_terminal_run_metadata(
                context["metadata_path"], context
            )
            self.assertEqual(observed, document)
            self.assertEqual(len(identity["sha256"]), 64)

            document["binary_sha256"] = "0" * 64
            Path(context["metadata_path"]).write_text(
                json.dumps(document), encoding="utf-8"
            )
            with self.assertRaisesRegex(ValueError, "binary_sha256"):
                load_terminal_run_metadata(context["metadata_path"], context)

    def make_daq_tab(self, project_root):
        from widgets import DaqTab as daq_module

        with (
            mock.patch.object(
                daq_module, "find_project_root", return_value=Path(project_root)
            ),
            mock.patch.object(
                daq_module, "DatabaseManager", DummyDatabaseManager
            ),
        ):
            return daq_module.DaqTab()

    def test_daq_db_failure_does_not_block_terminal_ui_recovery(self):
        with tempfile.TemporaryDirectory() as directory:
            context, _document = make_terminal_run_fixture(directory, 3)
            tab = self.make_daq_tab(directory)
            tab.db = RecordingDaqDatabase(fail=True)
            tab.current_run_id = 4
            tab.current_run_uuid = str(uuid.uuid4())
            tab.current_run_context = context
            tab.current_batch = 1
            tab.total_batches = 1
            emitted = []
            tab.runContextReady.connect(emitted.append)
            tab.btn_start.setEnabled(False)

            tab.on_batch_finished(0)

            self.assertTrue(tab.btn_start.isEnabled())
            self.assertEqual(len(emitted), 1)
            self.assertIn("DB Error", tab.terminal.toPlainText())
            self.assertEqual(
                tab.db.finalizations[0][1]["status"], "daq_completed"
            )
            tab.disk_timer.stop()
            tab.deleteLater()

    def test_daq_terminal_sidecar_truth_precedes_exit_code_and_stop_intent(self):
        cases = (
            ("completed", 7, True, "daq_completed"),
            ("failed", 0, True, "daq_failed"),
            ("cancelled", 9, False, "daq_cancelled"),
        )
        for index, (truth, exit_code, stopped, expected) in enumerate(
            cases, start=20
        ):
            with self.subTest(truth=truth, exit_code=exit_code, stopped=stopped), \
                 tempfile.TemporaryDirectory() as directory:
                context, _document = make_terminal_run_fixture(
                    directory, index, truth
                )
                tab = self.make_daq_tab(directory)
                tab.db = RecordingDaqDatabase()
                tab.current_run_id = index
                tab.current_run_uuid = str(uuid.uuid4())
                tab.current_run_context = context
                tab.current_batch = 1
                tab.total_batches = 1
                tab.stop_requested = stopped
                tab.daq_process = SimpleNamespace(
                    process_started=True,
                    launch_cancelled_before_process=False,
                    failure_message="",
                )

                tab.on_batch_finished(exit_code)

                self.assertEqual(
                    tab.db.finalizations[0][1]["status"], expected
                )
                if expected == "daq_completed":
                    self.assertIn(
                        "Terminal sidecar truth takes precedence",
                        tab.terminal.toPlainText(),
                    )
                tab.disk_timer.stop()
                tab.deleteLater()

    def test_daq_started_exit_without_terminal_sidecar_is_unknown_not_cancelled(self):
        with tempfile.TemporaryDirectory() as directory:
            tab = self.make_daq_tab(directory)
            tab.db = RecordingDaqDatabase()
            tab.current_run_id = 31
            tab.current_run_uuid = str(uuid.uuid4())
            tab.current_run_context = {
                "raw_file": str(Path(directory) / "interrupted.dat"),
                "metadata_path": str(Path(directory) / "missing.run.json"),
                "run_number": 31,
            }
            tab.current_batch = 1
            tab.total_batches = 1
            tab.stop_requested = True
            tab.daq_process = SimpleNamespace(
                process_started=True,
                launch_cancelled_before_process=False,
                failure_message="",
            )

            tab.on_batch_finished(-signal.SIGKILL)

            terminal = tab.db.finalizations[0][1]
            self.assertEqual(terminal["status"], "daq_failed")
            self.assertIn("interrupted/unknown", terminal["error_message"])
            self.assertIn("Lifecycle Unknown", tab.terminal.toPlainText())
            tab.disk_timer.stop()
            tab.deleteLater()

    def test_daq_prelaunch_cancel_is_known_without_frontend_sidecar(self):
        with tempfile.TemporaryDirectory() as directory:
            tab = self.make_daq_tab(directory)
            tab.db = RecordingDaqDatabase()
            tab.current_run_id = 32
            tab.current_run_uuid = str(uuid.uuid4())
            tab.current_run_context = {
                "raw_file": str(Path(directory) / "never_started.dat"),
                "metadata_path": str(Path(directory) / "missing.run.json"),
                "run_number": 32,
            }
            tab.current_batch = 1
            tab.total_batches = 1
            tab.daq_process = SimpleNamespace(
                process_started=False,
                launch_cancelled_before_process=True,
                failure_message="",
            )

            tab.on_batch_finished(-signal.SIGINT)

            self.assertEqual(
                tab.db.finalizations[0][1]["status"], "daq_cancelled"
            )
            tab.disk_timer.stop()
            tab.deleteLater()

    def test_production_db_failure_still_recovers_controls(self):
        with tempfile.TemporaryDirectory() as directory:
            tab = self.make_production_tab(directory)
            tab.db = FailingTerminalDatabase()
            tab._active_db_run_id = 10
            tab._active_run_uuid = "run-uuid"
            tab._db_production_begun = True
            tab._run_active = True
            tab.current_raw_file = str(Path(directory) / "input.dat")
            tab.current_root_output = ""
            tab.btn_run.setEnabled(False)

            tab.process.start(sys.executable, ["-c", "pass"])
            self.wait_for(
                lambda: tab.process.state() == QProcess.ProcessState.NotRunning
                and tab.btn_run.isEnabled()
            )

            self.assertTrue(tab.btn_run.isEnabled())
            self.assertFalse(tab._run_active)
            self.assertFalse(tab._db_terminal_recorded)
            self.assertIn("DB Error", tab.log_console.toPlainText())
            tab.deleteLater()

    def test_production_debug_exit_never_mutates_production_lifecycle(self):
        with tempfile.TemporaryDirectory() as directory:
            tab = self.make_production_tab(directory)
            tab.db = RecordingProductionDatabase()
            tab._active_db_run_id = 10
            tab._active_run_uuid = str(uuid.uuid4())
            tab._run_active = True
            tab._active_debug_mode = True
            tab.btn_run.setEnabled(False)
            tab.btn_stop.setEnabled(True)

            tab.process.start(sys.executable, ["-c", "pass"])
            self.wait_for(
                lambda: tab.process.state() == QProcess.ProcessState.NotRunning
                and tab.btn_run.isEnabled()
            )

            self.assertEqual(tab.db.begins, [])
            self.assertEqual(tab.db.finalizations, [])
            self.assertEqual(tab.progress_bar.format(), "Debug session ended")
            self.assertIn(
                "no production ROOT completion was recorded",
                tab.log_console.toPlainText(),
            )
            tab.deleteLater()

    def test_validated_production_output_precedes_racing_stop_intent(self):
        with tempfile.TemporaryDirectory() as directory:
            root_output = Path(directory) / "race_completed.root"
            tab = self.make_production_tab(directory)
            tab.db = RecordingProductionDatabase()
            tab._active_db_run_id = 11
            tab._active_run_uuid = str(uuid.uuid4())
            tab._db_production_begun = True
            tab._run_active = True
            tab._cancel_requested = True
            tab._active_debug_mode = False
            tab.current_raw_file = str(Path(directory) / "input.dat")
            tab.current_root_output = str(root_output)
            tab.btn_run.setEnabled(False)
            emitted = []
            tab.rootOutputReady.connect(emitted.append)
            child = (
                "from pathlib import Path; import sys; "
                "Path(sys.argv[1]).write_bytes(b'new-root-output')"
            )

            tab.process.start(
                sys.executable, ["-c", child, str(root_output)]
            )
            self.wait_for(
                lambda: tab.process.state() == QProcess.ProcessState.NotRunning
                and tab.btn_run.isEnabled()
            )

            self.assertEqual(
                tab.db.finalizations[0][1]["status"],
                "production_completed",
            )
            self.assertEqual(emitted, [str(root_output.resolve())])
            self.assertIn(
                "validated output truth takes precedence",
                tab.log_console.toPlainText(),
            )
            tab.deleteLater()

    @unittest.skipUnless(os.name == "posix", "signal test is POSIX-only")
    def test_production_unresponsive_child_exposes_operator_force_stop(self):
        child = """
import signal
import time

signal.signal(signal.SIGTERM, signal.SIG_IGN)
print('child-ready', flush=True)
while True:
    time.sleep(0.02)
"""
        with tempfile.TemporaryDirectory() as directory:
            tab = self.make_production_tab(directory)
            tab.STOP_GRACE_MSEC = 50
            tab._run_active = True
            tab._active_debug_mode = True
            tab.btn_run.setEnabled(False)
            tab.btn_stop.setEnabled(True)
            tab.process.start(sys.executable, ["-u", "-c", child])
            self.wait_for(
                lambda: "child-ready" in tab.log_console.toPlainText()
            )

            started = time.monotonic()
            tab.stop_all()
            self.assertLess(time.monotonic() - started, 0.1)
            self.wait_for(lambda: tab._force_stop_offered)
            self.assertEqual(tab.btn_stop.text(), "Force Stop Conversion")
            self.assertTrue(tab.btn_stop.isEnabled())
            self.assertNotEqual(
                tab.process.state(), QProcess.ProcessState.NotRunning
            )

            tab.stop_all()
            self.wait_for(
                lambda: tab.process.state() == QProcess.ProcessState.NotRunning
            )
            self.assertIn(
                "interrupted/unknown", tab.log_console.toPlainText()
            )
            tab.deleteLater()

    def test_daq_process_spawn_failure_records_launch_failed(self):
        with tempfile.TemporaryDirectory() as directory:
            tab = self.make_daq_tab(directory)
            tab.db = RecordingDaqDatabase()
            tab.current_run_id = 8
            tab.current_run_uuid = str(uuid.uuid4())
            tab.current_run_context = {
                "raw_file": str(Path(directory) / "missing.dat"),
                "metadata_path": str(Path(directory) / "missing.run.json"),
                "run_number": 8,
            }
            tab.current_batch = 1
            tab.total_batches = 1
            tab.daq_process = SimpleNamespace(
                process_started=False,
                failure_message="frontend executable not found",
            )
            tab.on_batch_finished(-1)

            self.assertEqual(len(tab.db.launch_failures), 1)
            self.assertIn(
                "frontend executable not found",
                tab.db.launch_failures[0][1],
            )
            self.assertTrue(tab.btn_start.isEnabled())
            tab.disk_timer.stop()
            tab.deleteLater()

    @unittest.skipUnless(os.name == "posix", "executable fixture is POSIX-only")
    def test_production_uuid_mismatch_never_falls_back_to_path_update(self):
        from widgets import ProductionTab as production_module

        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            (project / "bin").mkdir()
            (project / "data").mkdir()
            executable = project / "bin" / "production_dt5730"
            executable.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            executable.chmod(0o755)
            raw = project / "data" / "input.dat"
            config = project / "data" / "input.dat.config.conf"
            metadata = project / "data" / "input.dat.run.json"
            raw.write_bytes(b"raw")
            config.write_text("config\n", encoding="utf-8")
            metadata.write_text("{}\n", encoding="utf-8")
            tab = self.make_production_tab(project)
            database = MismatchedContextDatabase()
            tab.db = database
            tab.set_run_context({
                "raw_file": str(raw),
                "config_path": str(config),
                "metadata_path": str(metadata),
                "run_number": 11,
                "db_run_id": 5,
                "run_uuid": str(uuid.uuid4()),
                "metadata_exists": True,
            })
            tab.chk_debug_mode.setChecked(True)

            with (
                mock.patch.object(production_module, "verify_deployed_gui"),
                mock.patch.object(
                    production_module,
                    "verify_binary_fresh",
                    return_value=production_module.file_identity(executable),
                ),
                mock.patch.object(production_module, "verify_expected_hashes"),
            ):
                tab.run_conversion()
            self.wait_for(
                lambda: tab.process.state() == QProcess.ProcessState.NotRunning
                and tab.btn_run.isEnabled()
            )

            self.assertEqual(database.resolve_calls, 1)
            self.assertEqual(database.find_calls, 0)
            self.assertEqual(database.begin_calls, 0)
            self.assertIn("UUID", tab.log_console.toPlainText())
            tab.deleteLater()

    @unittest.skipUnless(os.name == "posix", "signal test is POSIX-only")
    def test_mainwindow_close_waits_for_graceful_child_finalization(self):
        from widgets import DaqTab as daq_module
        from widgets import ProductionTab as production_module
        from windows.MainWindow import MainWindow

        child = """
import signal
import sys
import time

def terminate(_signal, _frame):
    time.sleep(0.15)
    print('child-finalized', flush=True)
    sys.exit(0)

signal.signal(signal.SIGTERM, terminate)
print('child-ready', flush=True)
while True:
    time.sleep(0.02)
"""
        with (
            mock.patch.object(
                daq_module, "DatabaseManager", DummyDatabaseManager
            ),
            mock.patch.object(
                production_module, "DatabaseManager", DummyDatabaseManager
            ),
        ):
            window = MainWindow()
        window.show()
        window.production_tab._run_active = True
        window.production_tab.process.start(
            sys.executable, ["-u", "-c", child]
        )
        self.wait_for(
            lambda: "child-ready" in window.production_tab.log_console.toPlainText()
        )

        close_event = QCloseEvent()
        started = time.monotonic()
        window.closeEvent(close_event)
        self.assertLess(time.monotonic() - started, 0.1)
        self.assertFalse(close_event.isAccepted())
        self.assertTrue(window._close_pending)
        self.assertFalse(window._shutdown_ready)
        self.assertFalse(window.tabs.isEnabled())

        self.wait_for(lambda: window._shutdown_ready, timeout=4.0)
        self.assertEqual(
            window.production_tab.process.state(),
            QProcess.ProcessState.NotRunning,
        )
        self.assertIn(
            "child-finalized", window.production_tab.log_console.toPlainText()
        )
        self.assertTrue(window._monitor_cleaned)
        window.daq_tab.disk_timer.stop()
        window.deleteLater()

    @unittest.skipUnless(os.name == "posix", "signal test is POSIX-only")
    def test_mainwindow_close_auto_escalates_unresponsive_production_child(self):
        from widgets import DaqTab as daq_module
        from widgets import ProductionTab as production_module
        from windows.MainWindow import MainWindow

        child = """
import signal
import time

signal.signal(signal.SIGTERM, signal.SIG_IGN)
print('child-ready', flush=True)
while True:
    time.sleep(0.02)
"""
        with (
            mock.patch.object(
                daq_module, "DatabaseManager", DummyDatabaseManager
            ),
            mock.patch.object(
                production_module, "DatabaseManager", DummyDatabaseManager
            ),
        ):
            window = MainWindow()
        window.show()
        window.production_tab.STOP_GRACE_MSEC = 50
        window.production_tab._run_active = True
        window.production_tab._active_debug_mode = True
        window.production_tab.process.start(
            sys.executable, ["-u", "-c", child]
        )
        self.wait_for(
            lambda: "child-ready"
            in window.production_tab.log_console.toPlainText()
        )

        close_event = QCloseEvent()
        window.closeEvent(close_event)
        self.assertFalse(close_event.isAccepted())
        self.wait_for(lambda: window._shutdown_ready, timeout=3.0)

        self.assertEqual(
            window.production_tab.process.state(),
            QProcess.ProcessState.NotRunning,
        )
        self.assertIn(
            "automatically force-stopping",
            window.production_tab.log_console.toPlainText(),
        )
        self.assertTrue(window._monitor_cleaned)
        window.daq_tab.disk_timer.stop()
        window.deleteLater()


if __name__ == "__main__":
    unittest.main()
