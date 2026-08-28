import math
import os
import subprocess
import re
import shlex
import signal
import threading
from PyQt6.QtCore import QThread, pyqtSignal

from core.process_output import (
    parse_led_status,
    parse_live_daq_stats,
    parse_temperature,
)
from core.runtime_paths import verify_expected_hashes, verify_paths_absent

class ProcessManager(QThread):
    DEFAULT_GRACEFUL_STOP_TIMEOUT_SEC = 10.0

    log_signal = pyqtSignal(str)
    stat_signal = pyqtSignal(dict) 
    finished_signal = pyqtSignal(int)
    
    temp_signal = pyqtSignal(float)
    led_signal = pyqtSignal(dict)
    fatal_signal = pyqtSignal(str) # Soft-kill 이벤트 감지 시그널
    force_stop_available_signal = pyqtSignal()
    auto_force_escalated_signal = pyqtSignal()

    def __init__(self, cmd, cwd=None, expected_hashes=None,
                 expected_absent_paths=None, *,
                 graceful_stop_timeout_sec=DEFAULT_GRACEFUL_STOP_TIMEOUT_SEC):
        super().__init__()
        if (
            isinstance(graceful_stop_timeout_sec, bool)
            or not isinstance(graceful_stop_timeout_sec, (int, float))
            or not math.isfinite(float(graceful_stop_timeout_sec))
            or not 0.05 <= float(graceful_stop_timeout_sec) <= 300.0
        ):
            raise ValueError(
                "graceful_stop_timeout_sec must be finite and in 0.05..300"
            )
        self.cmd = list(cmd)
        self.cwd = cwd
        self.expected_hashes = dict(expected_hashes or {})
        self.expected_absent_paths = list(expected_absent_paths or [])
        self.process = None
        self.process_started = False
        self.failure_message = ""
        self.is_running = False
        self._stop_requested = threading.Event()
        self._process_lock = threading.Lock()
        self._graceful_signal_sent = False
        self._graceful_stop_timeout_sec = float(graceful_stop_timeout_sec)
        self._grace_timer = None
        self._grace_timer_lock = threading.Lock()
        self._auto_force_on_timeout = threading.Event()
        self._force_stop_available = threading.Event()
        self._force_stop_escalated = threading.Event()
        self._finished_lock = threading.Lock()
        self._finished_emitted = False
        self._start_requested = False
        self.launch_cancelled_before_process = False
        
        self.ansi_escape = re.compile(r'\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])|\r')

    def start(self, *args, **kwargs):
        # Record the request synchronously so application shutdown can wait for
        # a QThread that Qt has accepted but has not scheduled yet.
        self._start_requested = True
        return super().start(*args, **kwargs)

    def has_pending_work(self):
        with self._finished_lock:
            finished = self._finished_emitted
        return self._start_requested and not finished

    def run(self):
        returncode = -1
        self.is_running = not self._stop_requested.is_set()
        try:
            if self._stop_requested.is_set():
                self.launch_cancelled_before_process = True
                self.log_signal.emit(
                    "[System] DAQ launch cancelled before runtime validation."
                )
                returncode = -signal.SIGINT
                return

            verify_expected_hashes(self.expected_hashes)
            verify_paths_absent(self.expected_absent_paths)

            if self._stop_requested.is_set():
                self.launch_cancelled_before_process = True
                self.log_signal.emit(
                    "[System] DAQ launch cancelled before process creation."
                )
                returncode = -signal.SIGINT
                return

            self.log_signal.emit(f"[Launch] {shlex.join(self.cmd)}")
            process = subprocess.Popen(
                self.cmd,
                cwd=self.cwd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
                universal_newlines=True,
                start_new_session=(os.name == "posix"),
            )

            # stop() may run in the small window between the pre-launch check
            # above and Popen returning. Publish the process under a lock, then
            # immediately forward any latched request.
            with self._process_lock:
                self.process = process
                self.process_started = True
            self._send_graceful_stop_if_requested()

            # Always drain stdout to EOF, including during graceful shutdown.
            # Breaking early can hide finalization/provenance errors and can
            # deadlock a child that is still writing to a full pipe.
            try:
                for line in iter(process.stdout.readline, ''):
                    if line:
                        clean_line = self.ansi_escape.sub('', line).strip()
                        if not clean_line:
                            continue

                        if "[FATAL] OVER_TEMP_SOFT_KILL" in clean_line:
                            self.fatal_signal.emit("OVER_TEMP_SOFT_KILL")

                        temperature = parse_temperature(clean_line)
                        if temperature is not None:
                            self.temp_signal.emit(temperature)

                        led_status = parse_led_status(clean_line)
                        if led_status is not None:
                            self.led_signal.emit(led_status)

                        if "[LIVE DAQ]" in clean_line:
                            self._parse_and_emit_stats(clean_line)

                        if not any(tag in clean_line for tag in (
                            "[LIVE DAQ]", "[STATUS]", "[FATAL]"
                        )):
                            self.log_signal.emit(clean_line)
            finally:
                process.stdout.close()

            returncode = process.wait()
        except Exception as e:
            self.failure_message = str(e)
            self.log_signal.emit(f"[Error] Process execution failed: {e}")
        finally:
            self._cancel_grace_timer()
            self.is_running = False
            self._emit_finished_once(returncode)

    def _parse_and_emit_stats(self, line):
        stats = parse_live_daq_stats(line)
        if stats:
            self.stat_signal.emit(stats)

    def stop(self, *, auto_force=False):
        """Request graceful shutdown without blocking the GUI thread.

        The request is latched even when process creation has not happened yet.
        The child gets a bounded grace interval for raw/metadata finalization.
        At expiry the GUI is notified that an explicit force-stop is available,
        or the process is killed automatically when application shutdown asked
        for safe automatic escalation.
        """

        if auto_force:
            self._auto_force_on_timeout.set()
        self._stop_requested.set()
        self.is_running = False
        self._send_graceful_stop_if_requested()

    def force_stop_is_available(self):
        return self._force_stop_available.is_set()

    def force_stop_was_escalated(self):
        return self._force_stop_escalated.is_set()

    def force_stop(self):
        """Force process exit immediately; intended only for explicit recovery."""

        self._stop_requested.set()
        self.is_running = False
        self._cancel_grace_timer()
        with self._process_lock:
            process = self.process
        if process is not None and process.poll() is None:
            self._force_stop_escalated.set()
            self.log_signal.emit(
                "[System] Explicit force stop requested; killing the process."
            )
            self._kill_process(process)

    def _send_graceful_stop_if_requested(self):
        if not self._stop_requested.is_set():
            return

        with self._process_lock:
            process = self.process
            if (
                process is None
                or self._graceful_signal_sent
                or process.poll() is not None
            ):
                return
            self._graceful_signal_sent = True

        self.log_signal.emit(
            "[System] Sending SIGINT to gracefully stop the process..."
        )
        try:
            self._send_process_signal(process, signal.SIGINT)
        except (OSError, ProcessLookupError) as exc:
            self.log_signal.emit(
                f"[Warning] Could not deliver graceful stop signal: {exc}"
            )
        if process.poll() is None:
            self._start_grace_timer()

    def _send_process_signal(self, process, requested_signal):
        pid = getattr(process, "pid", None)
        if os.name == "posix" and isinstance(pid, int) and pid > 0:
            os.killpg(pid, requested_signal)
        else:
            process.send_signal(requested_signal)

    def _kill_process(self, process):
        pid = getattr(process, "pid", None)
        try:
            if os.name == "posix" and isinstance(pid, int) and pid > 0:
                os.killpg(pid, signal.SIGKILL)
            else:
                process.kill()
        except (OSError, ProcessLookupError) as exc:
            self.log_signal.emit(
                f"[Warning] Could not force-stop process: {exc}"
            )

    def _start_grace_timer(self):
        with self._grace_timer_lock:
            if self._grace_timer is not None:
                return
            timer = threading.Timer(
                self._graceful_stop_timeout_sec,
                self._handle_grace_timeout,
            )
            timer.daemon = True
            self._grace_timer = timer
            timer.start()

    def _cancel_grace_timer(self):
        with self._grace_timer_lock:
            timer = self._grace_timer
            self._grace_timer = None
        if timer is not None:
            timer.cancel()

    def _handle_grace_timeout(self):
        with self._grace_timer_lock:
            self._grace_timer = None
        with self._process_lock:
            process = self.process
        if process is None or process.poll() is not None:
            return

        self._force_stop_available.set()
        if self._auto_force_on_timeout.is_set():
            self._force_stop_escalated.set()
            self.log_signal.emit(
                "[System] Graceful stop deadline expired during application "
                "shutdown; automatically force-stopping the child."
            )
            self.auto_force_escalated_signal.emit()
            self._kill_process(process)
        else:
            self.log_signal.emit(
                "[Warning] Graceful stop deadline expired. The child is still "
                "running; operator force-stop is now available."
            )
            self.force_stop_available_signal.emit()

    def _emit_finished_once(self, returncode):
        with self._finished_lock:
            if self._finished_emitted:
                return
            self._finished_emitted = True
        self.finished_signal.emit(returncode)
