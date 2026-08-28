import io
import os
import signal
import sys
import threading
import time
import unittest
from pathlib import Path
from unittest import mock


PROJECT_ROOT = Path(__file__).resolve().parents[1]
GUI_DIR = PROJECT_ROOT / "gui"
if str(GUI_DIR) not in sys.path:
    sys.path.insert(0, str(GUI_DIR))

try:
    from PyQt6.QtCore import QCoreApplication
except ImportError:  # pragma: no cover - portable skip outside the GUI venv
    QCoreApplication = None


@unittest.skipIf(QCoreApplication is None, "PyQt6 is not installed")
class ProcessManagerLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QCoreApplication.instance() or QCoreApplication([])

    def setUp(self):
        from core import ProcessManager as process_manager_module

        self.module = process_manager_module
        self.ProcessManager = process_manager_module.ProcessManager

    def wait_for_thread(self, manager, timeout_ms=3000):
        deadline = time.monotonic() + timeout_ms / 1000.0
        while manager.isRunning() and time.monotonic() < deadline:
            self.app.processEvents()
            manager.wait(10)
        self.app.processEvents()
        self.assertFalse(manager.isRunning(), "ProcessManager thread did not finish")

    def wait_for(self, predicate, timeout=3.0):
        deadline = time.monotonic() + timeout
        while not predicate() and time.monotonic() < deadline:
            self.app.processEvents()
            time.sleep(0.005)
        self.app.processEvents()
        self.assertTrue(predicate(), "condition was not reached before timeout")

    def test_stop_during_popen_is_forwarded_and_drains_stdout(self):
        popen_entered = threading.Event()
        release_popen = threading.Event()
        fake_process = FakeProcess(["first line\n", "final line\n"])
        logs = []
        finished = []

        def delayed_popen(*_args, **_kwargs):
            # Model stop() arriving after the final pre-launch check but before
            # subprocess.Popen has returned a handle to ProcessManager.
            popen_entered.set()
            self.assertTrue(release_popen.wait(2.0))
            return fake_process

        manager = self.ProcessManager(["fake-frontend"])
        manager.log_signal.connect(logs.append)
        manager.finished_signal.connect(finished.append)

        with (
            mock.patch.object(self.module, "verify_expected_hashes"),
            mock.patch.object(self.module, "verify_paths_absent"),
            mock.patch.object(
                self.module.subprocess, "Popen", side_effect=delayed_popen
            ) as popen,
        ):
            manager.start()
            self.assertTrue(popen_entered.wait(1.0))
            started = time.monotonic()
            manager.stop()
            self.assertLess(time.monotonic() - started, 0.1)
            release_popen.set()
            self.wait_for_thread(manager)

        popen.assert_called_once()
        self.assertEqual(fake_process.signals, [signal.SIGINT])
        self.assertEqual(logs.count("first line"), 1)
        self.assertEqual(logs.count("final line"), 1)
        self.assertEqual(finished, [-signal.SIGINT])

    def test_stop_before_thread_start_prevents_popen_and_emits_finished_once(self):
        finished = []
        manager = self.ProcessManager(["must-not-launch"])
        manager.finished_signal.connect(finished.append)
        manager.stop()

        with mock.patch.object(self.module.subprocess, "Popen") as popen:
            manager.start()
            self.wait_for_thread(manager)

        popen.assert_not_called()
        self.assertEqual(finished, [-signal.SIGINT])
        self.assertTrue(manager.launch_cancelled_before_process)

    def test_stop_during_validation_prevents_process_creation(self):
        validation_entered = threading.Event()
        release_validation = threading.Event()
        finished = []

        def delayed_validation(_expected):
            validation_entered.set()
            self.assertTrue(release_validation.wait(2.0))

        manager = self.ProcessManager(["must-not-launch"])
        manager.finished_signal.connect(finished.append)
        with (
            mock.patch.object(
                self.module, "verify_expected_hashes", delayed_validation
            ),
            mock.patch.object(self.module, "verify_paths_absent"),
            mock.patch.object(self.module.subprocess, "Popen") as popen,
        ):
            manager.start()
            self.assertTrue(validation_entered.wait(1.0))
            manager.stop()
            release_validation.set()
            self.wait_for_thread(manager)

        popen.assert_not_called()
        self.assertEqual(finished, [-signal.SIGINT])

    def test_popen_failure_is_exposed_for_durable_launch_status(self):
        finished = []
        manager = self.ProcessManager(["missing-frontend"])
        manager.finished_signal.connect(finished.append)
        with (
            mock.patch.object(self.module, "verify_expected_hashes"),
            mock.patch.object(self.module, "verify_paths_absent"),
            mock.patch.object(
                self.module.subprocess,
                "Popen",
                side_effect=FileNotFoundError("frontend missing"),
            ),
        ):
            manager.start()
            self.wait_for_thread(manager)

        self.assertFalse(manager.process_started)
        self.assertIn("frontend missing", manager.failure_message)
        self.assertEqual(finished, [-1])

    def test_repeated_stop_is_nonblocking_and_sends_sigint_once(self):
        manager = self.ProcessManager(["unused"])
        fake_process = FakeProcess([])
        with manager._process_lock:
            manager.process = fake_process

        started = time.monotonic()
        manager.stop()
        manager.stop()
        self.assertLess(time.monotonic() - started, 0.1)
        self.assertEqual(fake_process.signals, [signal.SIGINT])
        self.assertEqual(fake_process.wait_calls, 0)

    def test_force_stop_is_explicit_and_nonblocking(self):
        manager = self.ProcessManager(["unused"])
        fake_process = FakeProcess([])
        with manager._process_lock:
            manager.process = fake_process

        manager.force_stop()
        self.assertEqual(fake_process.kill_calls, 1)
        self.assertEqual(fake_process.wait_calls, 0)

    def test_grace_timeout_offers_operator_force_stop_without_blocking(self):
        manager = self.ProcessManager(
            ["unused"], graceful_stop_timeout_sec=0.05
        )
        fake_process = IgnoringFakeProcess([])
        offered = []
        logs = []
        manager.force_stop_available_signal.connect(lambda: offered.append(True))
        manager.log_signal.connect(logs.append)
        with manager._process_lock:
            manager.process = fake_process

        started = time.monotonic()
        manager.stop()
        self.assertLess(time.monotonic() - started, 0.1)
        self.wait_for(manager.force_stop_is_available)
        self.assertEqual(fake_process.signals, [signal.SIGINT])
        self.assertTrue(offered)
        self.assertEqual(fake_process.kill_calls, 0)
        self.assertIn("operator force-stop", "\n".join(logs))

        manager.force_stop()
        self.assertEqual(fake_process.kill_calls, 1)
        self.assertTrue(manager.force_stop_was_escalated())

    def test_grace_timeout_auto_escalates_during_shutdown(self):
        manager = self.ProcessManager(
            ["unused"], graceful_stop_timeout_sec=0.05
        )
        fake_process = IgnoringFakeProcess([])
        escalated = []
        manager.auto_force_escalated_signal.connect(
            lambda: escalated.append(True)
        )
        with manager._process_lock:
            manager.process = fake_process

        manager.stop(auto_force=True)
        self.wait_for(lambda: fake_process.kill_calls == 1)
        self.assertTrue(manager.force_stop_was_escalated())
        self.assertTrue(escalated)

    @unittest.skipUnless(os.name == "posix", "SIGINT integration is POSIX-only")
    def test_real_child_gracefully_finalizes_without_blocking_stop(self):
        child = """
import signal
import sys
import time

def stop(_signal, _frame):
    print("child-finalized", flush=True)
    sys.exit(0)

signal.signal(signal.SIGINT, stop)
print("child-ready", flush=True)
while True:
    time.sleep(0.05)
"""
        logs = []
        finished = []
        manager = self.ProcessManager([sys.executable, "-u", "-c", child])
        manager.log_signal.connect(logs.append)
        manager.finished_signal.connect(finished.append)
        manager.start()
        self.wait_for(lambda: "child-ready" in logs)

        started = time.monotonic()
        manager.stop()
        self.assertLess(time.monotonic() - started, 0.1)
        self.wait_for_thread(manager)

        self.assertIn("child-finalized", logs)
        self.assertEqual(finished, [0])

    @unittest.skipUnless(os.name == "posix", "signal integration is POSIX-only")
    def test_real_sigint_ignoring_child_is_auto_killed_after_bound(self):
        child = """
import signal
import time

signal.signal(signal.SIGINT, signal.SIG_IGN)
print("child-ready", flush=True)
while True:
    time.sleep(0.05)
"""
        logs = []
        finished = []
        manager = self.ProcessManager(
            [sys.executable, "-u", "-c", child],
            graceful_stop_timeout_sec=0.1,
        )
        manager.log_signal.connect(logs.append)
        manager.finished_signal.connect(finished.append)
        manager.start()
        self.wait_for(lambda: "child-ready" in logs)

        manager.stop(auto_force=True)
        self.wait_for_thread(manager)

        self.assertTrue(manager.force_stop_was_escalated())
        self.assertEqual(finished, [-signal.SIGKILL])
        self.assertIn("automatically force-stopping", "\n".join(logs))


class FakeProcess:
    def __init__(self, lines):
        self.stdout = io.StringIO("".join(lines))
        self.returncode = None
        self.signals = []
        self.wait_calls = 0
        self.kill_calls = 0

    def poll(self):
        return self.returncode

    def send_signal(self, requested_signal):
        self.signals.append(requested_signal)
        self.returncode = -int(requested_signal)

    def wait(self, timeout=None):
        self.wait_calls += 1
        if self.returncode is None:
            self.returncode = 0
        return self.returncode

    def kill(self):
        self.kill_calls += 1
        self.returncode = -signal.SIGKILL


class IgnoringFakeProcess(FakeProcess):
    def send_signal(self, requested_signal):
        self.signals.append(requested_signal)


if __name__ == "__main__":
    unittest.main()
