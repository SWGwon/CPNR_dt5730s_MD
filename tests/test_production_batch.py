"""Batch queue/lifecycle tests; all inputs, outputs and settings are temporary."""

import hashlib
import os
from pathlib import Path
import struct
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest import mock

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / "gui"))
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

try:
    from PyQt6.QtCore import QProcess, QSettings
    from PyQt6.QtWidgets import QApplication
    from widgets.ProductionBatch import ProductionBatch
    from widgets import ProductionTab as production_module
except ImportError:
    QApplication = None


class RecordingDatabase:
    def __init__(self, *_args, **_kwargs):
        self.paths = {}
        self.begins = []
        self.ends = []

    def find_run_id_by_output(self, path):
        return self.paths.get(str(path))

    def get_run_uuid(self, run_id):
        return f"uuid-{run_id}"

    def begin_production(self, run_id, **kwargs):
        self.begins.append((run_id, kwargs))

    def finalize_production_run(self, run_id, **kwargs):
        self.ends.append((run_id, kwargs))


@unittest.skipIf(QApplication is None, "PyQt6 is not installed")
class ProductionBatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.settings_dir = tempfile.TemporaryDirectory()
        for settings_format in (QSettings.Format.NativeFormat, QSettings.Format.IniFormat):
            QSettings.setPath(settings_format, QSettings.Scope.UserScope, cls.settings_dir.name)
        cls.app = QApplication.instance() or QApplication([])

    @classmethod
    def tearDownClass(cls):
        cls.settings_dir.cleanup()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="cpnr_batch_test_")
        self.directory = Path(self.temporary.name)
        self.widgets = []
        QSettings("CPNR", "DT5730S_ProductionTab").clear()

    def tearDown(self):
        for widget in self.widgets:
            if hasattr(widget, "process"):
                widget.stop_all()
                if widget.process.state() != QProcess.ProcessState.NotRunning:
                    widget.process.kill()
                    widget.process.waitForFinished(2000)
            elif hasattr(widget, "stop"):
                widget.stop()
            widget.deleteLater()
        self.app.processEvents()
        self.temporary.cleanup()

    def wait_for(self, condition, timeout=5):
        deadline = time.monotonic() + timeout
        while not condition() and time.monotonic() < deadline:
            self.app.processEvents()
            time.sleep(0.002)
        self.assertTrue(condition(), "condition timed out")

    def make_tab(self, project=None):
        with (
            mock.patch.object(production_module, "find_project_root", return_value=project or self.directory),
            mock.patch.object(production_module, "DatabaseManager", RecordingDatabase),
        ):
            tab = production_module.ProductionTab()
        tab.chk_verify_metadata.setChecked(False)
        tab.chk_debug_mode.setChecked(False)
        self.widgets.append(tab)
        return tab

    def make_queue(self):
        queue = ProductionBatch(self.directory)
        self.widgets.append(queue)
        return queue

    def make_raw(self, name):
        path = self.directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("wb") as stream:
            for event in range(2):
                stream.write(struct.pack("<QIIHHI", event * 1000, event, 520, 3, 0, event + 100))
                for baseline in (14000, 14500):
                    trace = [baseline] * 520
                    trace[200:208] = [baseline - 64] * 8
                    stream.write(struct.pack("<520H", *trace))
        return path

    def launch_patches(self, executable):
        return (
            mock.patch.object(production_module, "verify_deployed_gui"),
            mock.patch.object(production_module, "verify_binary_fresh",
                              return_value=production_module.file_identity(executable)),
        )

    def fake_converter(self, script):
        binary = self.directory / "bin/production_dt5730"
        binary.parent.mkdir(exist_ok=True)
        binary.write_text(f"#!{sys.executable}\n" + script, encoding="utf-8")
        binary.chmod(0o755)
        return binary

    def test_multiselect_folder_dedup_and_remove_never_touch_files(self):
        queue = self.make_queue()
        first = self.make_raw("a file_run001.dat")
        second = self.make_raw("b.DAT")
        nested = self.make_raw("subdir/c.dat")
        partial = self.directory / "unfinished.dat.partial"
        partial.write_bytes(b"partial")
        alias = self.directory / "alias.dat"
        alias.symlink_to(first)
        (self.directory / "directory.dat").mkdir()
        queue.add_paths([first, first, alias, partial])
        queue.add_folder(self.directory)
        self.assertEqual([job["raw"] for job in queue.jobs], [str(first), str(second)])
        queue.table.selectRow(0)
        queue.remove_selected()
        self.assertEqual(len(queue.jobs), 1)
        queue.clear()
        self.assertEqual(queue.table.rowCount(), 0)
        self.assertFalse(queue.btn_start.isEnabled())
        self.assertTrue(all(path.exists() for path in (first, second, nested, partial)))

    def test_queue_is_serial_and_skips_existing_output_without_trusting_it(self):
        queue = self.make_queue()
        paths = [self.make_raw(f"{name}.dat") for name in ("a", "b", "c")]
        existing = self.directory / "b_prod.root"
        existing.write_bytes(b"not necessarily a valid ROOT")
        queue.add_paths(paths)
        started = []
        queue.jobRequested.connect(started.append)
        queue.start({"polarity": "falling"})
        self.wait_for(lambda: len(started) == 1)
        self.app.processEvents()
        self.assertEqual(len(started), 1)
        self.assertTrue(queue.active)
        self.assertFalse(queue.btn_add.isEnabled())
        queue.complete_current("Succeeded")
        self.wait_for(lambda: len(started) == 2)
        self.assertEqual(started[1]["raw"], str(paths[2]))
        queue.complete_current("Succeeded")
        self.wait_for(lambda: not queue.active)
        self.assertEqual([job["status"] for job in queue.jobs], ["Succeeded", "Skipped", "Succeeded"])
        self.assertIn("NOT validated", queue.jobs[1]["detail"])
        self.assertEqual(existing.read_bytes(), b"not necessarily a valid ROOT")
        self.assertEqual(queue.progress.value(), 3)

    def test_stop_between_jobs_and_restart_cancels_old_timer(self):
        queue = self.make_queue()
        queue.add_paths([self.make_raw("a.dat"), self.make_raw("b.dat")])
        started = []
        queue.jobRequested.connect(started.append)
        queue.start({})
        queue.stop()  # No child has been launched yet.
        self.app.processEvents()
        self.assertEqual(started, [])
        self.assertEqual([job["status"] for job in queue.jobs], ["Cancelled", "Cancelled"])
        queue.start({})
        self.wait_for(lambda: len(started) == 1)
        queue.complete_current("Succeeded")
        queue.stop()  # Cancel the scheduled transition to the second child.
        self.app.processEvents()
        self.assertEqual(len(started), 1)
        self.assertEqual([job["status"] for job in queue.jobs], ["Succeeded", "Cancelled"])

    def test_large_queue_and_dangling_output_skip_without_recursion(self):
        queue = self.make_queue()
        paths = []
        for number in range(500):
            raw = self.directory / f"sample_run{number + 1:04d}.dat"
            raw.touch()
            output = raw.with_name(raw.stem + "_prod.root")
            if number == 0:
                output.symlink_to(self.directory / "missing-target")
            else:
                output.touch()
            paths.append(raw)
        queue.add_paths(paths)
        launched = []
        queue.jobRequested.connect(launched.append)
        queue.start({})
        self.wait_for(lambda: not queue.active)
        self.assertEqual(launched, [])
        self.assertEqual(queue.progress.value(), 500)
        self.assertTrue(all(job["status"] == "Skipped" for job in queue.jobs))
        self.assertTrue(Path(queue.jobs[0]["output"]).is_symlink())

    def test_stop_on_error_is_selectable(self):
        for continue_after_failure in (False, True):
            queue = self.make_queue()
            queue.add_paths([self.make_raw("a.dat"), self.make_raw("b.dat")])
            queue.chk_continue.setChecked(continue_after_failure)
            requested = []

            def convert(job):
                requested.append(job)
                queue.complete_current("Failed", "test error")

            queue.jobRequested.connect(convert)
            queue.start({})
            self.wait_for(lambda: not queue.active)
            self.assertEqual(len(requested), 2 if continue_after_failure else 1)
            self.assertEqual(queue.jobs[1]["status"], "Failed" if continue_after_failure else "Cancelled")

    def test_batch_snapshots_options_and_uses_per_file_provenance(self):
        tab = self.make_tab()
        paths = [self.make_raw("a_run2_run021.dat"), self.make_raw("b_run022.dat")]
        tab.batch.add_paths(paths)
        tab.spin_run_number.setValue(999)
        tab.config_edit.setText("wrong-common.conf")
        tab.metadata_edit.setText("wrong-common.json")
        tab.output_edit.setText("wrong-common.root")
        tab.chk_verify_metadata.setChecked(True)
        tab.chk_save_waveforms.setChecked(True)
        calls = []

        def launch(**kwargs):
            calls.append(kwargs)
            tab._last_launch_error = "deliberate preflight error"
            return False

        with mock.patch.object(tab, "_launch_conversion", side_effect=launch):
            tab.start_batch()
            self.assertFalse(tab.io_group.isEnabled())
            tab.run_conversion()  # Must not interleave a single-file job.
            tab.chk_verify_metadata.setChecked(False)  # Programmatic mutation cannot change the snapshot.
            tab.set_run_context({"raw_file": "later.dat", "run_number": 33})
            self.assertEqual(tab.input_edit.text(), "")
            self.wait_for(lambda: not tab.has_pending_work())
        self.assertEqual(len(calls), 2)
        for path, call in zip(paths, calls):
            self.assertEqual(call["config_value"], str(path) + ".config.conf")
            self.assertEqual(call["metadata_value"], str(path) + ".run.json")
            self.assertEqual(call["run_number"], 0)
            self.assertEqual(call["output_value"], "")
            self.assertEqual(call["context"], {})
            self.assertTrue(call["verified"])
            self.assertTrue(call["save_waveforms"])
        self.assertEqual(tab.input_edit.text(), "later.dat")
        self.assertEqual(tab.spin_run_number.value(), 33)

    def test_failed_to_start_completes_each_row_and_database_identity(self):
        binary = self.fake_converter("pass\n")
        binary.write_bytes(b"not an executable format")
        tab = self.make_tab()
        paths = [self.make_raw("a.dat"), self.make_raw("b.dat")]
        tab.batch.add_paths(paths)
        tab.db.paths = {str(path): index + 1 for index, path in enumerate(paths)}
        first, second = self.launch_patches(binary)
        with first, second:
            tab.start_batch()
            self.wait_for(lambda: not tab.has_pending_work())
        self.assertEqual([job["status"] for job in tab.batch.jobs], ["Failed", "Failed"])
        self.assertEqual([entry[0] for entry in tab.db.ends], [1, 2])
        self.assertTrue(all(entry[1]["status"] == "production_launch_failed" for entry in tab.db.ends))
        self.assertTrue(tab.btn_run.isEnabled())

    def test_current_child_stop_cancels_pending_and_keeps_no_output(self):
        binary = self.fake_converter("import time\nprint('ready', flush=True)\ntime.sleep(30)\n")
        tab = self.make_tab()
        tab.batch.add_paths([self.make_raw("a.dat"), self.make_raw("b.dat")])
        first, second = self.launch_patches(binary)
        with first, second:
            tab.start_batch()
            self.wait_for(lambda: "ready" in tab.log_console.toPlainText())
            tab.stop_all(auto_force=True)
            self.wait_for(lambda: not tab.has_pending_work())
        self.assertEqual([job["status"] for job in tab.batch.jobs], ["Cancelled", "Cancelled"])
        self.assertFalse(any(self.directory.glob("*_prod.root")))

    def test_crashed_child_continues_and_carriage_return_progress_is_read(self):
        binary = self.fake_converter(
            "import os, pathlib, signal, sys\n"
            "raw = sys.argv[sys.argv.index('-i') + 1]\n"
            "if pathlib.Path(raw).name == 'a.dat': os.kill(os.getpid(), signal.SIGKILL)\n"
            "sys.stdout.write('[Progress] 25.0% | Events: 2 | Speed: 1.0 MB/s | ETA: 3\\r')\n"
            "sys.stdout.flush()\n"
            "path = pathlib.Path(raw)\n"
            "path.with_name(path.stem + '_prod.root').write_bytes(b'test output')\n"
        )
        tab = self.make_tab()
        tab.batch.add_paths([self.make_raw("a.dat"), self.make_raw("b.dat")])
        ready = []
        tab.rootOutputReady.connect(ready.append)
        first, second = self.launch_patches(binary)
        with first, second:
            tab.start_batch()
            self.wait_for(lambda: not tab.has_pending_work())
        self.assertEqual([job["status"] for job in tab.batch.jobs], ["Failed", "Succeeded"])
        self.assertEqual(ready, [str(self.directory / "b_prod.root")])
        self.assertEqual(tab.last_stats["events"], "2")

    def test_debug_batch_is_blocked_and_shutdown_counts_timer_gap(self):
        from windows.MainWindow import MainWindow
        tab = self.make_tab()
        tab.batch.add_paths([self.make_raw("a.dat")])
        tab.chk_debug_mode.setChecked(True)
        tab.start_batch()
        self.assertFalse(tab.batch.active)
        tab.chk_debug_mode.setChecked(False)
        tab.start_batch()
        self.assertEqual(tab.process.state(), QProcess.ProcessState.NotRunning)
        window = SimpleNamespace(
            _daq_process_active=lambda: False, production_tab=tab,
            root_validation_tab=SimpleNamespace(has_pending_work=lambda: False),
        )
        self.assertTrue(MainWindow._workers_active(window))
        tab.stop_all(auto_force=True)
        self.assertFalse(MainWindow._workers_active(window))

    @unittest.skipUnless((PROJECT / "bin/production_dt5730").is_file(), "production binary not built")
    def test_real_gui_batch_converts_dat_continues_failure_and_preserves_inputs(self):
        tab = self.make_tab(PROJECT)
        paths = [self.make_raw(name) for name in (
            "a_run2_run021_part17.dat", "b_bad_run022.dat", "c run023.dat", "d_existing.dat",
        )]
        paths[1].write_bytes(paths[1].read_bytes()[:-1])
        before = {path: hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}
        existing = self.directory / "d_existing_prod.root"
        existing.write_bytes(b"preserve existing output")
        tab.batch.add_paths(paths + paths)
        tab.chk_save_waveforms.setChecked(True)
        ready = []
        tab.rootOutputReady.connect(ready.append)
        tab.start_batch()
        self.wait_for(lambda: not tab.has_pending_work(), timeout=20)
        self.assertEqual([job["status"] for job in tab.batch.jobs],
                         ["Succeeded", "Failed", "Succeeded", "Skipped"], tab.log_console.toPlainText())
        self.assertIn("Truncated waveform", tab.batch.jobs[1]["detail"])
        self.assertEqual(tab.process.program(), str(PROJECT / "bin/production_dt5730"))
        self.assertEqual(len(ready), 2)
        self.assertTrue(all(Path(path).read_bytes().startswith(b"root") for path in ready))
        self.assertEqual(existing.read_bytes(), b"preserve existing output")
        for path in paths:
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), before[path])
        # Re-running retries only the failed file; successful/existing outputs
        # are skipped without being labelled as validated.
        tab.start_batch()
        self.wait_for(lambda: not tab.has_pending_work(), timeout=20)
        self.assertEqual([job["status"] for job in tab.batch.jobs],
                         ["Skipped", "Failed", "Skipped", "Skipped"])


if __name__ == "__main__":
    unittest.main()
