import subprocess
import re
import shlex
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
        self.is_running = False
        
        self.ansi_escape = re.compile(r'\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])|\r')

    def run(self):
        self.is_running = True
        try:
            verify_expected_hashes(self.expected_hashes)
            verify_paths_absent(self.expected_absent_paths)
            self.log_signal.emit(f"[Launch] {shlex.join(self.cmd)}")
            self.process = subprocess.Popen(
                self.cmd,
                cwd=self.cwd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
                universal_newlines=True
            )
            
            for line in iter(self.process.stdout.readline, ''):
                if not self.is_running:
                    break
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
            
            self.process.wait()
            self.finished_signal.emit(self.process.returncode)
        except Exception as e:
            self.log_signal.emit(f"[Error] Process execution failed: {e}")
            self.finished_signal.emit(-1)
        finally:
            self.is_running = False

    def _parse_and_emit_stats(self, line):
        stats = parse_live_daq_stats(line)
        if stats:
            self.stat_signal.emit(stats)

    def stop(self):
        self.is_running = False
        if self.process and self.process.poll() is None:
            self.log_signal.emit("[System] Sending SIGINT to gracefully stop the process...")
            self.process.send_signal(2)
            try:
                self.process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.log_signal.emit(
                    "[System] Graceful stop exceeded 15 s; forcing process exit."
                )
                self.process.kill()
