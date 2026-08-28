import hashlib
import json
import os
import stat
import sys
import tempfile
import textwrap
import time
import unittest
from pathlib import Path
from unittest import mock


PROJECT_ROOT = Path(__file__).resolve().parents[1]
GUI_DIR = PROJECT_ROOT / "gui"
if str(GUI_DIR) not in sys.path:
    sys.path.insert(0, str(GUI_DIR))

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

try:
    from PyQt6.QtCore import QProcess, QSettings
    from PyQt6.QtWidgets import QApplication
except ImportError:  # pragma: no cover - optional outside the GUI environment
    QProcess = None
    QSettings = None
    QApplication = None


FAKE_VALIDATOR = textwrap.dedent(
    """\
    #!/usr/bin/env python3
    import argparse
    import hashlib
    import json
    import signal
    import sys
    import time
    from pathlib import Path

    parser = argparse.ArgumentParser()
    parser.add_argument("-i", required=True)
    parser.add_argument("--max-events", type=int, default=None)
    parser.add_argument("--raw-fidelity", action="store_true")
    args = parser.parse_args()
    root_path = Path(args.i).resolve()
    mode = root_path.read_text(encoding="utf-8").strip()

    def identity(path):
        value = path.stat()
        return {
            "device": int(value.st_dev),
            "inode": int(value.st_ino),
            "mode": int(value.st_mode),
            "size_bytes": int(value.st_size),
            "mtime_seconds": value.st_mtime_ns // 1_000_000_000,
            "mtime_nanoseconds": value.st_mtime_ns % 1_000_000_000,
            "ctime_seconds": value.st_ctime_ns // 1_000_000_000,
            "ctime_nanoseconds": value.st_ctime_ns % 1_000_000_000,
        }

    def make_report(status="PASS", completed=True, mismatch=False):
        executable = Path(sys.argv[0]).resolve()
        root_sha256 = hashlib.sha256(root_path.read_bytes()).hexdigest()
        if mode == "no-sha":
            root_sha256 = None
        requested_limit = args.max_events
        if mismatch:
            requested_limit = 987654321
        check_status = {
            "PASS": "PASS",
            "WARN": "WARN",
            "FAIL": "FAIL",
            "CANCELLED": "WARN",
        }[status]
        return {
            "schema_version": 1,
            "overall_status": status,
            "legacy": False,
            "input": {
                "path": str(root_path),
                "max_events": requested_limit,
                "raw_fidelity_requested": args.raw_fidelity,
                "identity_start": identity(root_path),
                "identity_end": identity(root_path),
                "sha256": root_sha256,
                "sha256_end": root_sha256,
            },
            "validator": {
                "executable_path": str(executable),
                "executable_sha256": hashlib.sha256(
                    executable.read_bytes()
                ).hexdigest(),
            },
            "analysis": {
                "completed": completed,
                "cancelled": status == "CANCELLED",
                "events_total": 2,
                "events_scanned": 1 if not completed else 2,
                "sampled": not completed,
            },
            "summary": {"entries": 2, "run_number": 42},
            "domain_status": {
                "data_integrity": check_status,
                "provenance": "SKIP",
                "trigger_and_quality": check_status,
            },
            "counts": {
                "pass": 1 if check_status == "PASS" else 0,
                "warn": 1 if check_status == "WARN" else 0,
                "fail": 1 if check_status == "FAIL" else 0,
                "skip": 0,
            },
            "checks": [{
                "status": check_status,
                "category": "operation",
                "name": "fake_process_lifecycle",
                "observed": status,
                "expected": "authenticated envelope",
                "detail": "hardware-free Qt lifecycle fixture",
            }],
            "channels": [{
                "channel": 0,
                "active": True,
                "trigger_enabled": True,
                "threshold": {"requested_mv": 1.0, "delta_adc": 8},
                "metrics": {"events": 2},
            }],
        }

    def emit(status="PASS", completed=True, mismatch=False):
        print(
            json.dumps(
                make_report(status, completed, mismatch),
                sort_keys=True,
                allow_nan=False,
            ),
            flush=True,
        )

    if mode == "success":
        print("[ValidationProgress] 25% | schema", file=sys.stderr, flush=True)
        print("[ValidationProgress] 80% | events", file=sys.stderr, flush=True)
        emit()
    elif mode == "warn":
        emit("WARN")
        raise SystemExit(1)
    elif mode == "fail":
        emit("FAIL")
        raise SystemExit(2)
    elif mode == "no-sha":
        emit()
    elif mode == "exit-mismatch":
        emit("FAIL")
        raise SystemExit(1)
    elif mode == "fatal-with-json":
        emit()
        raise SystemExit(64)
    elif mode == "crash-after-report":
        import os
        emit()
        os.kill(os.getpid(), signal.SIGKILL)
    elif mode == "mismatch":
        emit(mismatch=True)
    elif mode == "graceful-cancel":
        def terminate(_signum, _frame):
            emit("CANCELLED", False)
            raise SystemExit(3)

        signal.signal(signal.SIGTERM, terminate)
        print("[fake] graceful-ready", file=sys.stderr, flush=True)
        while True:
            time.sleep(0.02)
    elif mode == "stale-pass-on-cancel":
        def terminate_with_pass(_signum, _frame):
            emit("PASS", True)
            raise SystemExit(0)

        signal.signal(signal.SIGTERM, terminate_with_pass)
        print("[fake] stale-pass-ready", file=sys.stderr, flush=True)
        while True:
            time.sleep(0.02)
    elif mode == "ignore-cancel":
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        print("[fake] ignore-ready", file=sys.stderr, flush=True)
        while True:
            time.sleep(0.02)
    else:
        print("unknown fixture mode", file=sys.stderr, flush=True)
        raise SystemExit(4)
    """
)


class DummyDatabaseManager:
    def __init__(self, *_args, **_kwargs):
        pass

    def update_production_summary(self, *_args, **_kwargs):
        pass

    def find_run_id_by_output(self, *_args, **_kwargs):
        return None


@unittest.skipIf(QApplication is None, "PyQt6 is not installed")
class RootValidationTabLifecycleTests(unittest.TestCase):
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

    def setUp(self):
        from widgets import RootValidationTab as root_validation_module

        self.module = root_validation_module
        self.temp_dir = tempfile.TemporaryDirectory()
        self.project = Path(self.temp_dir.name)
        (self.project / "gui").mkdir()
        self.validator = self.project / "fake_root_validator"
        self.validator.write_text(FAKE_VALIDATOR, encoding="utf-8")
        self.validator.chmod(
            self.validator.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP
        )
        self.settings = QSettings("CPNR", "DT5730S_RootValidationTab")
        self.settings.clear()
        self.settings.sync()
        self.tabs = []
        self.windows = []

    def tearDown(self):
        for window in self.windows:
            if window.isVisible():
                window.close()
        for tab in self.tabs:
            if tab.has_pending_work():
                tab.stop_all(wait=True)
            tab.deleteLater()
        self.app.processEvents()
        self.settings.clear()
        self.settings.sync()
        self.temp_dir.cleanup()

    def make_input(self, name, mode):
        path = self.project / name
        path.write_text(mode, encoding="utf-8")
        return path

    def make_tab(self, root_path):
        tab = self.module.RootValidationTab(
            project_root=self.project,
            runtime_gui_dir=self.project / "gui",
            validator_path=self.validator,
            validator_sources=[],
        )
        tab.set_root_file(root_path)
        self.tabs.append(tab)
        return tab

    def wait_for(self, predicate, timeout=4.0):
        deadline = time.monotonic() + timeout
        while not predicate() and time.monotonic() < deadline:
            self.app.processEvents()
            time.sleep(0.005)
        self.app.processEvents()
        self.assertTrue(predicate(), "condition was not reached before timeout")

    def wait_until_idle(self, tab, timeout=4.0):
        self.wait_for(
            lambda: (
                not tab._run_active
                and tab.process.state() == QProcess.ProcessState.NotRunning
                and not tab.has_pending_work()
            ),
            timeout=timeout,
        )

    def assert_controls_recovered(self, tab):
        self.assertTrue(tab.btn_validate.isEnabled())
        self.assertTrue(tab.btn_browse.isEnabled())
        self.assertTrue(tab.input_edit.isEnabled())
        self.assertEqual(
            tab.spin_max_events.isEnabled(),
            not tab.check_raw_fidelity.isChecked(),
        )
        self.assertTrue(tab.check_raw_fidelity.isEnabled())
        self.assertFalse(tab.btn_cancel.isEnabled())
        self.assertFalse(tab._cancel_kill_timer.isActive())
        self.assertIsNone(tab._expected_validator_identity)
        self.assertIsNone(tab._expected_input_identity)
        self.assertIsNone(tab._expected_max_events)
        self.assertIsNone(tab._expected_raw_fidelity)
        self.assertIsNone(tab._observed_input_end_identity)
        self.assertIsNone(tab._pinned_validator_fd)
        self.assertIsNone(tab._pinned_validator_launch_path)
        self.assertFalse(tab._fingerprint_threads)
        self.assertIsNone(tab._pending_root_request)

    def run_success(self, tab):
        tab.start_validation()
        self.wait_until_idle(tab)
        self.assertIsNotNone(tab._last_report)
        self.assertEqual(tab._last_report["overall_status"], "PASS")

    def test_real_qprocess_success_renders_authenticated_report(self):
        root_path = self.make_input("success.root", "success")
        original = root_path.read_bytes()
        tab = self.make_tab(root_path)
        tab.spin_max_events.setValue(17)

        self.run_success(tab)

        self.assertEqual(root_path.read_bytes(), original)
        self.assertEqual(tab.lbl_overall.text(), "PASS")
        self.assertEqual(tab.lbl_entries.text(), "2")
        self.assertEqual(tab.lbl_run.text(), "42")
        self.assertEqual(tab.progress_bar.value(), 100)
        self.assertIn("Validation complete", tab.progress_bar.text())
        self.assertEqual(tab.checks_table.rowCount(), 1)
        self.assertGreaterEqual(tab.channels_table.rowCount(), 2)
        self.assertTrue(tab.btn_export.isEnabled())
        self.assertTrue(tab._result_identity_timer.isActive())
        self.assertIn("Report received", tab.log_console.toPlainText())
        self.assertRegex(
            tab.process.program(),
            rf"^/proc/{os.getpid()}/fd/[0-9]+$",
        )
        self.assertEqual(
            tab._last_report["validator"]["executable_path"],
            str(self.validator.resolve()),
        )
        self.assert_controls_recovered(tab)

    def test_nonzero_warn_and_fail_exit_codes_render_exportable_reports(self):
        for mode, expected_status in (("warn", "WARN"), ("fail", "FAIL")):
            with self.subTest(status=expected_status):
                root_path = self.make_input(f"{mode}.root", mode)
                tab = self.make_tab(root_path)

                tab.start_validation()
                self.wait_until_idle(tab)

                self.assertIsNotNone(tab._last_report)
                self.assertEqual(
                    tab._last_report["overall_status"], expected_status
                )
                self.assertEqual(tab.lbl_overall.text(), expected_status)
                self.assertTrue(tab.btn_export.isEnabled())
                self.assertIn(
                    f"status={expected_status}", tab.log_console.toPlainText()
                )
                destination = self.project / f"{mode}-report.json"
                with mock.patch.object(
                    self.module.QFileDialog,
                    "getSaveFileName",
                    return_value=(str(destination), "JSON Files (*.json)"),
                ):
                    tab.export_json()
                self.assertEqual(
                    json.loads(destination.read_text(encoding="utf-8"))[
                        "overall_status"
                    ],
                    expected_status,
                )
                self.assert_controls_recovered(tab)

    def test_exit_contract_mismatch_and_crash_reject_emitted_json(self):
        cases = (
            ("exit-mismatch", "exit code does not match"),
            ("fatal-with-json", "exit code does not match"),
            ("crash-after-report", "crashed after emitting JSON"),
        )
        for mode, expected_log in cases:
            with self.subTest(mode=mode):
                root_path = self.make_input(f"{mode}.root", mode)
                tab = self.make_tab(root_path)

                tab.start_validation()
                self.wait_until_idle(tab)

                self.assertIsNone(tab._last_report)
                self.assertEqual(tab.lbl_overall.text(), "FAIL")
                self.assertFalse(tab.btn_export.isEnabled())
                self.assertIn(expected_log, tab.log_console.toPlainText())
                self.assert_controls_recovered(tab)

    def test_missing_report_hash_is_computed_off_thread_before_export(self):
        root_path = self.make_input("no-sha.root", "no-sha")
        tab = self.make_tab(root_path)

        tab.start_validation()
        self.wait_until_idle(tab)

        self.assertIsNotNone(tab._last_report)
        self.assertEqual(tab.lbl_overall.text(), "PASS")
        self.assertTrue(tab.btn_export.isEnabled())
        self.assertEqual(
            tab._validated_input_fingerprint["sha256"],
            hashlib.sha256(root_path.read_bytes()).hexdigest(),
        )
        self.assertIn("report ready", tab.progress_bar.text())
        self.assert_controls_recovered(tab)

    def test_identity_timer_marks_in_place_change_and_replacement_stale(self):
        for mutation in ("in-place", "replacement"):
            with self.subTest(mutation=mutation):
                root_path = self.make_input(
                    f"identity-{mutation}.root", "success"
                )
                tab = self.make_tab(root_path)
                self.run_success(tab)

                if mutation == "in-place":
                    root_path.write_bytes(b"SUCCESS")
                else:
                    replacement = self.project / f"{mutation}.tmp"
                    replacement.write_bytes(root_path.read_bytes())
                    os.replace(replacement, root_path)
                tab._handle_result_identity_timer()

                self.assertTrue(tab._result_stale)
                self.assertEqual(tab.lbl_overall.text(), "STALE — REVALIDATE")
                self.assertFalse(tab.btn_export.isEnabled())
                self.assertFalse(tab._result_identity_timer.isActive())
                self.assertIn("is STALE", tab.log_console.toPlainText())

    def test_shutdown_stops_result_identity_timer(self):
        root_path = self.make_input("timer-shutdown.root", "success")
        tab = self.make_tab(root_path)
        self.run_success(tab)
        self.assertTrue(tab._result_identity_timer.isActive())

        tab.stop_all(wait=True)

        self.assertFalse(tab._result_identity_timer.isActive())

    def test_export_sha_recheck_blocks_same_inode_content_change(self):
        root_path = self.make_input("sha-stale.root", "success")
        tab = self.make_tab(root_path)
        self.run_success(tab)
        expected_sha256 = tab._validated_input_fingerprint["sha256"]

        root_path.write_bytes(b"SUCCESS")
        current_identity = tab._identity_from_stat(
            os.stat(root_path, follow_symlinks=False)
        )
        # Isolate the SHA guard from the cheap identity timer: even if every
        # stat field were accepted, changed bytes must still block export.
        tab._validated_input_identity = dict(current_identity)
        tab._validated_input_fingerprint = {
            **current_identity,
            "sha256": expected_sha256,
        }
        destination = self.project / "must-not-export.json"
        with (
            mock.patch.object(
                self.module.QFileDialog,
                "getSaveFileName",
                return_value=(str(destination), "JSON Files (*.json)"),
            ),
            mock.patch.object(self.module.QMessageBox, "warning") as warning,
        ):
            tab.export_json()

        warning.assert_called_once()
        self.assertFalse(destination.exists())
        self.assertTrue(tab._result_stale)
        self.assertEqual(tab.lbl_overall.text(), "STALE — REVALIDATE")
        self.assertFalse(tab.btn_export.isEnabled())

    def test_root_handoff_during_run_is_applied_and_invalidates_result(self):
        active = self.make_input("active.root", "graceful-cancel")
        queued = self.make_input("queued.root", "success")
        tab = self.make_tab(active)
        tab.start_validation()
        self.wait_for(
            lambda: "graceful-ready" in tab.log_console.toPlainText()
        )

        tab.set_root_file(queued)
        self.assertEqual(tab.input_edit.text(), str(active.resolve()))
        self.assertEqual(
            tab._pending_root_request, (str(queued.resolve()), False)
        )
        tab.cancel_validation()
        self.wait_until_idle(tab)

        self.assertEqual(tab.input_edit.text(), str(queued.resolve()))
        self.assertIsNone(tab._pending_root_request)
        self.assertIsNone(tab._last_report)
        self.assertEqual(tab.lbl_overall.text(), "NOT RUN")
        self.assertFalse(tab.btn_export.isEnabled())
        self.assertFalse(tab._result_identity_timer.isActive())
        self.assertIn("Applying queued ROOT input", tab.log_console.toPlainText())
        self.assert_controls_recovered(tab)

    @unittest.skipUnless(
        sys.platform.startswith("linux"), "procfd executable pinning is Linux-only"
    )
    def test_pinned_validator_descriptor_survives_started_until_finish(self):
        root_path = self.make_input("pinned-running.root", "ignore-cancel")
        tab = self.make_tab(root_path)
        tab.CANCEL_GRACE_MSEC = 50
        tab.start_validation()
        self.wait_for(lambda: "ignore-ready" in tab.log_console.toPlainText())

        descriptor = tab._pinned_validator_fd
        self.assertIsNotNone(descriptor)
        os.fstat(descriptor)
        self.assertRegex(
            tab.process.program(),
            rf"^/proc/{os.getpid()}/fd/{descriptor}$",
        )

        tab.cancel_validation()
        self.wait_until_idle(tab)
        with self.assertRaises(OSError):
            os.fstat(descriptor)
        self.assert_controls_recovered(tab)

    @unittest.skipUnless(
        sys.platform.startswith("linux"), "procfd executable pinning is Linux-only"
    )
    def test_failed_to_start_releases_pinned_validator_descriptor(self):
        root_path = self.make_input("failed-start.root", "success")
        tab = self.make_tab(root_path)
        real_pin = self.module.pin_verified_executable
        opened_descriptors = []

        def return_missing_procfd(*args, **kwargs):
            identity, descriptor, _launch_path = real_pin(*args, **kwargs)
            opened_descriptors.append(descriptor)
            return (
                identity,
                descriptor,
                f"/proc/{os.getpid()}/fd/{descriptor}-missing",
            )

        with mock.patch.object(
            self.module,
            "pin_verified_executable",
            side_effect=return_missing_procfd,
        ):
            tab.start_validation()
            self.wait_until_idle(tab)

        self.assertEqual(len(opened_descriptors), 1)
        with self.assertRaises(OSError):
            os.fstat(opened_descriptors[0])
        self.assertIsNone(tab._last_report)
        self.assertEqual(tab.lbl_overall.text(), "FAIL")
        self.assertIn("Validator process error", tab.log_console.toPlainText())
        self.assert_controls_recovered(tab)

    def test_rejected_new_attempt_immediately_invalidates_prior_pass(self):
        root_path = self.make_input("stale-view.root", "success")
        tab = self.make_tab(root_path)
        self.run_success(tab)
        self.assertEqual(tab.lbl_overall.text(), "PASS")
        self.assertTrue(tab.btn_export.isEnabled())

        self.validator.chmod(stat.S_IRUSR | stat.S_IWUSR)
        tab.start_validation()

        self.assertFalse(tab._run_active)
        self.assertIsNone(tab._last_report)
        self.assertFalse(tab.btn_export.isEnabled())
        self.assertFalse(tab._result_identity_timer.isActive())
        self.assertEqual(tab.lbl_overall.text(), "NOT RUN")
        self.assertEqual(tab.checks_table.rowCount(), 0)
        self.assertEqual(tab.channels_table.rowCount(), 0)
        self.assertEqual(tab.progress_bar.text(), "Idle")
        self.assertIn("launch blocked", tab.log_console.toPlainText())
        self.assert_controls_recovered(tab)

    def test_raw_fidelity_forces_full_scan_and_is_bound_to_report(self):
        root_path = self.make_input("fidelity.root", "success")
        tab = self.make_tab(root_path)
        tab.spin_max_events.setValue(19)
        tab.check_raw_fidelity.setChecked(True)

        self.assertEqual(tab.spin_max_events.value(), 0)
        self.assertFalse(tab.spin_max_events.isEnabled())
        self.run_success(tab)

        self.assertTrue(
            tab._last_report["input"]["raw_fidelity_requested"]
        )
        self.assertFalse(tab.spin_max_events.isEnabled())

    def test_graceful_cancel_accepts_only_cancelled_partial_report(self):
        success = self.make_input("first.root", "success")
        cancelled = self.make_input("cancel.root", "graceful-cancel")
        tab = self.make_tab(success)
        self.run_success(tab)
        prior_report = tab._last_report
        self.assertTrue(tab.btn_export.isEnabled())

        tab.set_root_file(cancelled)
        self.assertIsNone(tab._last_report)
        tab.start_validation()
        self.wait_for(
            lambda: "graceful-ready" in tab.log_console.toPlainText()
        )
        tab.cancel_validation()
        self.wait_until_idle(tab)

        self.assertIsNotNone(tab._last_report)
        self.assertIsNot(tab._last_report, prior_report)
        self.assertEqual(tab._last_report["overall_status"], "CANCELLED")
        self.assertEqual(tab.lbl_overall.text(), "CANCELLED")
        self.assertFalse(tab.btn_export.isEnabled())
        self.assertIn("Cancelled", tab.progress_bar.text())
        self.assert_controls_recovered(tab)

    def test_cancel_discards_completed_pass_emitted_after_request(self):
        root_path = self.make_input("stale.root", "stale-pass-on-cancel")
        tab = self.make_tab(root_path)
        tab.start_validation()
        self.wait_for(
            lambda: "stale-pass-ready" in tab.log_console.toPlainText()
        )

        tab.cancel_validation()
        self.wait_until_idle(tab)

        self.assertIsNone(tab._last_report)
        self.assertEqual(tab.lbl_overall.text(), "CANCELLED")
        self.assertFalse(tab.btn_export.isEnabled())
        self.assertNotIn("Validation complete", tab.progress_bar.text())
        self.assert_controls_recovered(tab)

    def test_cancel_force_kills_unresponsive_validator_without_report(self):
        root_path = self.make_input("unresponsive.root", "ignore-cancel")
        tab = self.make_tab(root_path)
        tab.CANCEL_GRACE_MSEC = 50
        tab.start_validation()
        self.wait_for(lambda: "ignore-ready" in tab.log_console.toPlainText())

        tab.cancel_validation()
        self.wait_until_idle(tab)

        self.assertIsNone(tab._last_report)
        self.assertEqual(tab.lbl_overall.text(), "CANCELLED")
        self.assertFalse(tab.btn_export.isEnabled())
        self.assertIn("killing validator", tab.log_console.toPlainText())
        self.assert_controls_recovered(tab)

    def test_envelope_mismatch_is_rejected_and_state_is_recovered(self):
        root_path = self.make_input("mismatch.root", "mismatch")
        tab = self.make_tab(root_path)
        tab.spin_max_events.setValue(11)

        tab.start_validation()
        self.wait_until_idle(tab)

        self.assertIsNone(tab._last_report)
        self.assertEqual(tab.lbl_overall.text(), "FAIL")
        self.assertFalse(tab.btn_export.isEnabled())
        self.assertIn(
            "max_events does not match", tab.log_console.toPlainText()
        )
        self.assert_controls_recovered(tab)

    def test_export_is_strict_no_clobber_and_blocks_same_input(self):
        root_path = self.make_input("export.root", "success")
        original = root_path.read_bytes()
        tab = self.make_tab(root_path)
        self.run_success(tab)

        exported = self.project / "report.json"
        with mock.patch.object(
            self.module.QFileDialog,
            "getSaveFileName",
            return_value=(str(exported), "JSON Files (*.json)"),
        ):
            tab.export_json()
        self.assertEqual(
            json.loads(exported.read_text(encoding="utf-8")),
            tab._last_report,
        )

        existing = self.project / "existing.json"
        existing.write_bytes(b"existing sentinel\n")
        with (
            mock.patch.object(
                self.module.QFileDialog,
                "getSaveFileName",
                return_value=(str(existing), "JSON Files (*.json)"),
            ),
            mock.patch.object(self.module.QMessageBox, "warning") as warning,
        ):
            tab.export_json()
        warning.assert_called_once()
        self.assertEqual(existing.read_bytes(), b"existing sentinel\n")

        late = self.project / "late.json"
        real_atomic_write = self.module.atomic_write_bytes_no_clobber

        def create_late_collision(destination, payload):
            Path(destination).write_bytes(b"late sentinel\n")
            return real_atomic_write(destination, payload)

        with (
            mock.patch.object(
                self.module.QFileDialog,
                "getSaveFileName",
                return_value=(str(late), "JSON Files (*.json)"),
            ),
            mock.patch.object(
                self.module,
                "atomic_write_bytes_no_clobber",
                side_effect=create_late_collision,
            ),
            mock.patch.object(self.module.QMessageBox, "warning") as warning,
        ):
            tab.export_json()
        warning.assert_called_once()
        self.assertEqual(late.read_bytes(), b"late sentinel\n")

        hard_link = self.project / "input-hard-link.json"
        os.link(root_path, hard_link)
        with (
            mock.patch.object(
                self.module.QFileDialog,
                "getSaveFileName",
                return_value=(str(hard_link), "JSON Files (*.json)"),
            ),
            mock.patch.object(self.module.QMessageBox, "critical") as critical,
        ):
            tab.export_json()
        critical.assert_called_once()
        self.assertEqual(root_path.read_bytes(), original)
        self.assertEqual(hard_link.read_bytes(), original)

    @unittest.skipUnless(os.name == "posix", "signal test is POSIX-only")
    def test_mainwindow_production_output_handoff_survives_active_validation(self):
        from widgets import DaqTab as daq_module
        from widgets import ProductionTab as production_module
        from windows.MainWindow import MainWindow

        active = self.make_input("window-active.root", "ignore-cancel")
        produced = self.make_input("window-produced.root", "success")
        with (
            mock.patch.object(
                daq_module, "DatabaseManager", DummyDatabaseManager
            ),
            mock.patch.object(
                production_module, "DatabaseManager", DummyDatabaseManager
            ),
        ):
            window = MainWindow()
        self.windows.append(window)
        tab = window.root_validation_tab
        tab.proj_dir = str(self.project)
        tab.runtime_gui_dir = str(self.project / "gui")
        tab.validator_path = str(self.validator)
        tab.validator_sources = []
        tab.CANCEL_GRACE_MSEC = 50
        tab.set_root_file(active)

        tab.start_validation()
        self.wait_for(lambda: "ignore-ready" in tab.log_console.toPlainText())
        window.production_tab.rootOutputReady.emit(str(produced))

        self.assertEqual(tab.input_edit.text(), str(active.resolve()))
        self.assertEqual(
            tab._pending_root_request, (str(produced.resolve()), False)
        )
        tab.cancel_validation()
        self.wait_until_idle(tab)

        self.assertEqual(tab.input_edit.text(), str(produced.resolve()))
        self.assertIsNone(tab._last_report)
        self.assertFalse(tab.btn_export.isEnabled())
        window.daq_tab.disk_timer.stop()
        window.close()
        window.deleteLater()

    @unittest.skipUnless(os.name == "posix", "signal test is POSIX-only")
    def test_closing_mainwindow_stops_root_validation_worker(self):
        from widgets import DaqTab as daq_module
        from widgets import ProductionTab as production_module
        from windows.MainWindow import MainWindow

        root_path = self.make_input("window-close.root", "ignore-cancel")
        with (
            mock.patch.object(
                daq_module, "DatabaseManager", DummyDatabaseManager
            ),
            mock.patch.object(
                production_module, "DatabaseManager", DummyDatabaseManager
            ),
        ):
            window = MainWindow()
        self.windows.append(window)
        tab = window.root_validation_tab
        tab.proj_dir = str(self.project)
        tab.runtime_gui_dir = str(self.project / "gui")
        tab.validator_path = str(self.validator)
        tab.validator_sources = []
        tab.CANCEL_GRACE_MSEC = 50
        tab.set_root_file(root_path)
        window.show()

        tab.start_validation()
        self.wait_for(lambda: "ignore-ready" in tab.log_console.toPlainText())
        self.assertTrue(window.isVisible())

        started = time.monotonic()
        window.close()
        self.assertLess(time.monotonic() - started, 0.1)
        self.assertTrue(window._close_pending)
        self.assertFalse(window.tabs.isEnabled())
        self.wait_for(lambda: window._shutdown_ready, timeout=4.0)

        self.assertEqual(
            tab.process.state(), QProcess.ProcessState.NotRunning
        )
        self.assertFalse(tab._run_active)
        self.assertIsNone(tab._last_report)
        self.assertTrue(window._monitor_cleaned)
        window.daq_tab.disk_timer.stop()


if __name__ == "__main__":
    unittest.main()
