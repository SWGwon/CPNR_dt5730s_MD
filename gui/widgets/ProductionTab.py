import re
import os
from PyQt6.QtWidgets import (QWidget, QVBoxLayout, QHBoxLayout, QGroupBox, 
                             QPushButton, QProgressBar, QLabel, QLineEdit, 
                             QTextEdit, QSpinBox, QFileDialog, QGridLayout, QCheckBox)
from PyQt6.QtCore import Qt, pyqtSlot, QSettings, QProcess
from core.DatabaseManager import DatabaseManager

class ProductionTab(QWidget):
    def __init__(self):
        super().__init__()
        
        curr = os.path.abspath(os.path.dirname(__file__))
        while curr != '/' and not os.path.exists(os.path.join(curr, 'CMakeLists.txt')):
            curr = os.path.dirname(curr)
        self.proj_dir = curr if curr != '/' else os.getcwd()
        
        self.bin_dir = os.path.join(self.proj_dir, "bin")
        self.data_dir = os.path.join(self.proj_dir, "data")
        os.makedirs(self.data_dir, exist_ok=True)
        
        self.settings = QSettings("CPNR", "DT5730S_ProductionTab")
        self.db = DatabaseManager(os.path.join(self.data_dir, "run_history.db"))
        
        self.process = QProcess()
        self.process.readyReadStandardOutput.connect(self.handle_stdout)
        self.process.readyReadStandardError.connect(self.handle_stderr)
        self.process.finished.connect(self.handle_finished)
        
        self.last_stats = {}; self.current_raw_file = ""
        self.init_ui()
        self.load_settings()
        self.log_pattern = re.compile(r"\[Progress\]\s+([0-9.]+)%\s+\|\s+Events:\s+(\d+)\s+\|\s+Speed:\s+([0-9.]+)\s+MB/s\s+\|\s+ETA:\s+(\d+)")

    def init_ui(self):
        layout = QVBoxLayout()
        io_group = QGroupBox("Input / Output Selection")
        io_group.setStyleSheet("QGroupBox { font-weight: bold; color: #17a2b8; }")
        io_layout = QGridLayout()
        self.input_edit = QLineEdit()
        self.btn_browse_in = QPushButton("Browse Raw")
        self.btn_browse_in.clicked.connect(self.browse_input)
        self.output_edit = QLineEdit()
        self.output_edit.setPlaceholderText("Auto-generated if empty (*_prod.root)")
        self.btn_browse_out = QPushButton("Browse ROOT")
        self.btn_browse_out.clicked.connect(self.browse_output)
        io_layout.addWidget(QLabel("Input Raw (.dat):"), 0, 0); io_layout.addWidget(self.input_edit, 0, 1); io_layout.addWidget(self.btn_browse_in, 0, 2)
        io_layout.addWidget(QLabel("Output ROOT (.root):"), 1, 0); io_layout.addWidget(self.output_edit, 1, 1); io_layout.addWidget(self.btn_browse_out, 1, 2)
        io_group.setLayout(io_layout); layout.addWidget(io_group)

        opt_group = QGroupBox("Conversion Options & Time-Machine Debugger")
        opt_layout = QHBoxLayout()
        self.chk_save_waveforms = QCheckBox("Save Waveforms (-w)")
        self.chk_save_waveforms.setStyleSheet("font-weight: bold;")
        self.chk_debug_mode = QCheckBox("Interactive Debug Mode (-d)")
        self.chk_debug_mode.setStyleSheet("font-weight: bold; color: #d9534f;")
        self.chk_debug_mode.stateChanged.connect(self.toggle_debug_ui)
        self.spin_debug_start = QSpinBox(); self.spin_debug_start.setRange(0, 9999999); self.spin_debug_start.setPrefix("Start Evt ID: "); self.spin_debug_start.setEnabled(False)

        self.btn_run = QPushButton("Run ROOT Conversion")
        self.btn_run.setStyleSheet("background-color: #5bc0de; color: white; font-weight: bold; padding: 8px;")
        self.btn_run.clicked.connect(self.run_conversion)
        self.btn_stop = QPushButton("Force Stop")
        self.btn_stop.setStyleSheet("background-color: #d9534f; color: white; font-weight: bold; padding: 8px;")
        self.btn_stop.clicked.connect(self.stop_all)
        
        self.btn_prev = QPushButton("Prev (p)"); self.btn_next = QPushButton("Next (n)"); self.btn_jump = QPushButton("Jump (j)")
        self.spin_jump = QSpinBox(); self.spin_jump.setRange(0, 9999999)
        self.btn_quit = QPushButton("Quit Debug (q)")

        self.btn_prev.clicked.connect(lambda: self.send_debug_command("p\n")); self.btn_next.clicked.connect(lambda: self.send_debug_command("n\n"))
        self.btn_jump.clicked.connect(lambda: self.send_debug_command(f"j {self.spin_jump.value()}\n")); self.btn_quit.clicked.connect(lambda: self.send_debug_command("q\n"))

        opt_layout.addWidget(self.chk_save_waveforms); opt_layout.addWidget(self.chk_debug_mode); opt_layout.addWidget(self.spin_debug_start)
        opt_layout.addSpacing(10); opt_layout.addWidget(self.btn_run); opt_layout.addWidget(self.btn_stop); opt_layout.addSpacing(20)
        opt_layout.addWidget(self.btn_prev); opt_layout.addWidget(self.btn_next); opt_layout.addWidget(self.spin_jump); opt_layout.addWidget(self.btn_jump); opt_layout.addWidget(self.btn_quit)
        opt_group.setLayout(opt_layout); layout.addWidget(opt_group)

        dash_group = QGroupBox("Conversion Status Dashboard")
        dash_layout = QVBoxLayout()
        self.progress_bar = QProgressBar(); self.progress_bar.setValue(0); self.progress_bar.setAlignment(Qt.AlignmentFlag.AlignCenter); self.progress_bar.setStyleSheet("QProgressBar::chunk { background-color: #5cb85c; }")
        stat_layout = QHBoxLayout()
        self.lbl_events = QLabel("Events: 0"); self.lbl_speed = QLabel("Speed: 0.0 MB/s"); self.lbl_eta = QLabel("ETA: 0 s")
        font = self.lbl_events.font(); font.setPointSize(11); font.setBold(True)
        for lbl in [self.lbl_events, self.lbl_speed, self.lbl_eta]: lbl.setFont(font); lbl.setAlignment(Qt.AlignmentFlag.AlignCenter); stat_layout.addWidget(lbl)
        dash_layout.addWidget(self.progress_bar); dash_layout.addLayout(stat_layout); dash_group.setLayout(dash_layout)
        layout.addWidget(dash_group)

        self.log_console = QTextEdit(); self.log_console.setReadOnly(True); self.log_console.setMaximumHeight(200)
        self.log_console.setStyleSheet("background-color: #f8f9fa; color: #333333; font-family: monospace; border: 1px solid #ced4da;")
        layout.addWidget(self.log_console)
        self.setLayout(layout)
        self.set_debug_controls_enabled(False)

    def toggle_debug_ui(self, state):
        self.spin_debug_start.setEnabled(self.chk_debug_mode.isChecked())

    def set_debug_controls_enabled(self, enabled):
        self.btn_prev.setEnabled(enabled); self.btn_next.setEnabled(enabled); self.spin_jump.setEnabled(enabled); self.btn_jump.setEnabled(enabled); self.btn_quit.setEnabled(enabled)

    def load_settings(self):
        self.input_edit.setText(self.settings.value("last_prod_input", ""))
        self.output_edit.setText(self.settings.value("last_prod_output", ""))
        self.chk_save_waveforms.setChecked(self.settings.value("last_save_wave", False, type=bool))

    def save_settings(self):
        self.settings.setValue("last_prod_input", self.input_edit.text())
        self.settings.setValue("last_prod_output", self.output_edit.text())
        self.settings.setValue("last_save_wave", self.chk_save_waveforms.isChecked())

    def browse_input(self):
        last_dir = os.path.dirname(os.path.join(self.proj_dir, self.input_edit.text())) if self.input_edit.text() else self.data_dir
        fname, _ = QFileDialog.getOpenFileName(self, "Open Raw Data", last_dir, "Data Files (*.dat)")
        if fname: self.input_edit.setText(os.path.relpath(fname, self.proj_dir)); self.save_settings()

    def browse_output(self):
        last_dir = os.path.dirname(os.path.join(self.proj_dir, self.output_edit.text())) if self.output_edit.text() else self.data_dir
        fname, _ = QFileDialog.getSaveFileName(self, "Save ROOT Data", last_dir, "ROOT Files (*.root)")
        if fname: self.output_edit.setText(os.path.relpath(fname, self.proj_dir)); self.save_settings()

    def run_conversion(self):
        self.save_settings()
        self.current_raw_file = self.input_edit.text().strip(); out_file = self.output_edit.text().strip()
        if not self.current_raw_file:
            self.log_console.append("<span style='color:red;'>[Error] Please select input file!</span>"); return
        if out_file:
            out_file_full = os.path.abspath(os.path.join(self.proj_dir, out_file))
            os.makedirs(os.path.dirname(out_file_full), exist_ok=True)
            
        args = ["-i", self.current_raw_file]
        if out_file: args.extend(["-o", out_file])
        if self.chk_save_waveforms.isChecked(): args.append("-w")
        is_debug_mode = self.chk_debug_mode.isChecked()
        if is_debug_mode: args.extend(["-d", str(self.spin_debug_start.value())])
            
        self.progress_bar.setValue(0); self.lbl_events.setText("Events: 0"); self.lbl_speed.setText("Speed: 0.0 MB/s"); self.lbl_eta.setText("ETA: 0 s")
        self.log_console.clear(); self.last_stats = {}
        
        exe_path = os.path.join(self.bin_dir, "production_dt5730")
        if not os.path.exists(exe_path):
            self.log_console.append(f"<span style='color:red;'>[Error] Executable not found at: {exe_path}. Did you run 'make'?</span>"); return

        self.btn_run.setEnabled(False); self.set_debug_controls_enabled(is_debug_mode)
        self.log_console.append(f"<b>[System] Starting:</b> {exe_path} {' '.join(args)}")
        self.process.setWorkingDirectory(self.proj_dir)
        self.process.start(exe_path, args)

    def stop_all(self):
        if self.process.state() == QProcess.ProcessState.Running:
            self.process.terminate()
            self.process.waitForFinished(1000)
            if self.process.state() == QProcess.ProcessState.Running: self.process.kill()
            self.log_console.append("<span style='color:red;'>[System] Conversion forcefully stopped.</span>")
            self.btn_run.setEnabled(True); self.set_debug_controls_enabled(False)

    def send_debug_command(self, cmd_str):
        if self.process.state() == QProcess.ProcessState.Running: self.process.write(cmd_str.encode('utf-8'))

    @pyqtSlot()
    def handle_stdout(self):
        while self.process.canReadLine():
            line = self.process.readLine().data().decode('utf-8', errors='ignore').strip()
            if not line: continue
            clean_line = re.compile(r'\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])').sub('', line)
            match = self.log_pattern.search(clean_line)
            if match:
                self.progress_bar.setValue(int(float(match.group(1)))); self.lbl_events.setText(f"Events: {int(match.group(2)):,}")
                self.lbl_speed.setText(f"Speed: {match.group(3)} MB/s"); self.lbl_eta.setText(f"ETA: {match.group(4)} s")
                self.last_stats = {"events": match.group(2), "avg_speed": match.group(3)}
            else:
                self.log_console.append(clean_line)
                self.log_console.verticalScrollBar().setValue(self.log_console.verticalScrollBar().maximum())

    @pyqtSlot()
    def handle_stderr(self):
        while self.process.canReadLine():
            line = self.process.readLine().data().decode('utf-8', errors='ignore').strip()
            if line: self.log_console.append(f"<span style='color:red;'>{line}</span>")

    @pyqtSlot(int, QProcess.ExitStatus)
    def handle_finished(self, exitCode, exitStatus):
        self.btn_run.setEnabled(True); self.set_debug_controls_enabled(False)
        if exitStatus == QProcess.ExitStatus.NormalExit and exitCode == 0:
            self.log_console.append(f"<span style='color:#5cb85c;'><b>[System] Conversion Successfully Finished!</b></span>")
            if self.current_raw_file and self.last_stats:
                self.db.update_production_summary(self.current_raw_file, self.last_stats)
                self.log_console.append("<span style='color:#6f42c1;'><b>[DB] Production Summary pushed to database.</b></span>")
        else:
            self.log_console.append(f"<span style='color:red;'><b>[System] Conversion Exited with Code: {exitCode}</b></span>")