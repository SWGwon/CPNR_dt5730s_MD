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
    log_signal = pyqtSignal(str)
    stat_signal = pyqtSignal(dict) 
    finished_signal = pyqtSignal(int)
    
    temp_signal = pyqtSignal(float)
    led_signal = pyqtSignal(dict)
    fatal_signal = pyqtSignal(str) # Soft-kill 이벤트 감지 시그널

    def __init__(self, cmd, cwd=None, expected_hashes=None,
                 expected_absent_paths=None):
        super().__init__()
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
        self._finished_lock = threading.Lock()
        self._finished_emitted = False
        self._start_requested = False
        
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
                self.log_signal.emit(
                    "[System] DAQ launch cancelled before runtime validation."
                )
                returncode = -signal.SIGINT
                return

            verify_expected_hashes(self.expected_hashes)
            verify_paths_absent(self.expected_absent_paths)

            if self._stop_requested.is_set():
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
                universal_newlines=True
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
            self.is_running = False
            self._emit_finished_once(returncode)

    def _parse_and_emit_stats(self, line):
        stats = parse_live_daq_stats(line)
        if stats:
            self.stat_signal.emit(stats)

    def stop(self):
        """Request graceful shutdown without blocking the GUI thread.

        The request is latched even when process creation has not happened yet.
        DAQ finalization is allowed to complete without an arbitrary SIGKILL
        deadline so the raw file and its provenance metadata can be flushed.
        """

        self._stop_requested.set()
        self.is_running = False
        self._send_graceful_stop_if_requested()

    def force_stop(self):
        """Force process exit immediately; intended only for explicit recovery."""

        self._stop_requested.set()
        self.is_running = False
        with self._process_lock:
            process = self.process
        if process is not None and process.poll() is None:
            self.log_signal.emit(
                "[System] Explicit force stop requested; killing the process."
            )
            try:
                process.kill()
            except (OSError, ProcessLookupError) as exc:
                self.log_signal.emit(
                    f"[Warning] Could not force-stop process: {exc}"
                )

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
            process.send_signal(signal.SIGINT)
        except (OSError, ProcessLookupError) as exc:
            self.log_signal.emit(
                f"[Warning] Could not deliver graceful stop signal: {exc}"
            )

    def _emit_finished_once(self, returncode):
        with self._finished_lock:
            if self._finished_emitted:
                return
            self._finished_emitted = True
        self.finished_signal.emit(returncode)
