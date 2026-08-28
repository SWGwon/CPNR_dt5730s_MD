import subprocess
import re
import shlex
from PyQt6.QtCore import QThread, pyqtSignal

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
        self.re_temp = re.compile(r"\[STATUS\] TEMP:\s*([\d\.]+)")
        self.re_led = re.compile(r"\[STATUS\] LED:\s*LOCK=(\d),\s*BYPS=(\d),\s*RUN=(\d),\s*TRG=(\d),\s*DRDY=(\d),\s*BUSY=(\d)")

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
                    elif "[LIVE DAQ]" in clean_line:
                        self._parse_and_emit_stats(clean_line)
                    elif "[STATUS] TEMP:" in clean_line:
                        m = self.re_temp.search(clean_line)
                        if m: self.temp_signal.emit(float(m.group(1)))
                    elif "[STATUS] LED:" in clean_line:
                        m = self.re_led.search(clean_line)
                        if m:
                            self.led_signal.emit({
                                'PLL LOCK': int(m.group(1)),
                                'PLL BYPS': int(m.group(2)),
                                'RUN': int(m.group(3)),
                                'TRG': int(m.group(4)),
                                'DRDY': int(m.group(5)),
                                'BUSY': int(m.group(6))
                            })
                    else:
                        self.log_signal.emit(clean_line)
            
            self.process.wait()
            self.finished_signal.emit(self.process.returncode)
        except Exception as e:
            self.log_signal.emit(f"[Error] Process execution failed: {e}")
            self.finished_signal.emit(-1)
        finally:
            self.is_running = False

    def _parse_and_emit_stats(self, line):
        try:
            stats = {}
            parts = line.split("|")
            for part in parts:
                if "Live:" in part:
                    stats['live_time'] = part.split("Live:")[1].strip()
                elif "DT:" in part:
                    stats['dead_time'] = part.split("DT:")[1].strip()
                elif "Events:" in part:
                    stats['events'] = part.split("Events:")[1].strip()
                elif "Speed:" in part:
                    stats['speed'] = part.split("Speed:")[1].strip()
            self.stat_signal.emit(stats)
        except Exception:
            pass

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
