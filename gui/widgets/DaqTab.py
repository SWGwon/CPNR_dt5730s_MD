import os
import shutil
import configparser
import re
from PyQt6.QtWidgets import (QWidget, QVBoxLayout, QHBoxLayout, QGridLayout, 
                             QPushButton, QLineEdit, QLabel, QTextEdit, 
                             QGroupBox, QSpinBox, QComboBox, QFileDialog)
from PyQt6.QtGui import QFont, QTextCursor
from PyQt6.QtCore import QTimer, QSettings, pyqtSignal
from core.ProcessManager import ProcessManager
from core.DatabaseManager import DatabaseManager

class DaqTab(QWidget):
    # MainWindow와 영구적으로 통신할 브릿지 시그널 선언
    hardware_led_signal = pyqtSignal(dict)
    hardware_temp_signal = pyqtSignal(float)
    daq_finished_signal = pyqtSignal(int)

    def __init__(self, parent=None, env_data_provider=None):
        super().__init__(parent)
        self.env_data_provider = env_data_provider
        self.daq_process = None
        
        curr = os.path.abspath(os.path.dirname(__file__))
        while curr != '/' and not os.path.exists(os.path.join(curr, 'CMakeLists.txt')):
            curr = os.path.dirname(curr)
        self.proj_dir = curr if curr != '/' else os.getcwd()
        
        self.bin_dir = os.path.join(self.proj_dir, "bin")
        self.data_dir = os.path.join(self.proj_dir, "data")
        self.config_dir = os.path.join(self.proj_dir, "config")
        
        os.makedirs(self.data_dir, exist_ok=True)
        self.settings = QSettings("CPNR", "DT5730S_DAQTab")
        self.db = DatabaseManager(os.path.join(self.data_dir, "run_history.db"))
        
        self.current_batch = 0; self.total_batches = 1
        self.base_output_path = ""; self.scan_values = [] 
        self.last_stats = {}; self.current_run_id = -1
        self.current_run_no = 1
        
        self.setup_ui()
        self.load_settings()

        self.disk_timer = QTimer(self)
        self.disk_timer.timeout.connect(self.update_disk_space)
        self.disk_timer.start(1000)
        self.update_disk_space()

    def setup_ui(self):
        layout = QVBoxLayout(self)

        file_group = QGroupBox("File & Configuration Environment")
        file_layout = QGridLayout()
        file_layout.addWidget(QLabel("Config (.conf):"), 0, 0)
        self.config_input = QLineEdit("config/dt5730s_inorganic_master.conf")
        file_layout.addWidget(self.config_input, 0, 1)
        self.btn_browse_config = QPushButton("Browse")
        self.btn_browse_config.clicked.connect(self.browse_config)
        file_layout.addWidget(self.btn_browse_config, 0, 2)

        file_layout.addWidget(QLabel("Base Output (.dat):"), 1, 0)
        
        # Output 경로 옆에 Run No. 스핀박스 배치 (자동 증가)
        out_layout = QHBoxLayout()
        self.output_input = QLineEdit("data/data_run.dat")
        out_layout.addWidget(self.output_input)
        self.spin_run_no = QSpinBox()
        self.spin_run_no.setPrefix("Run No: ")
        self.spin_run_no.setRange(1, 99999)
        self.spin_run_no.setToolTip("데이터 덮어쓰기 방지: 매 시작마다 파일명 끝에 _runNNN 이 붙고 자동 증가합니다.")
        out_layout.addWidget(self.spin_run_no)
        file_layout.addLayout(out_layout, 1, 1)
        
        self.btn_browse_output = QPushButton("Browse")
        self.btn_browse_output.clicked.connect(self.browse_output)
        file_layout.addWidget(self.btn_browse_output, 1, 2)

        file_layout.addWidget(QLabel("Run Metadata:"), 2, 0)
        env_layout = QHBoxLayout()
        self.operator_input = QLineEdit("Unknown")
        self.hv_input = QLineEdit("0V")
        self.temp_input = QLineEdit("20.0")
        
        env_layout.addWidget(QLabel("Operator:"))
        env_layout.addWidget(self.operator_input)
        env_layout.addWidget(QLabel("  |  Applied HV:"))
        env_layout.addWidget(self.hv_input)
        env_layout.addWidget(QLabel("  |  Temp (°C):"))
        env_layout.addWidget(self.temp_input)
        
        file_layout.addLayout(env_layout, 2, 1, 1, 2)
        file_group.setLayout(file_layout)
        layout.addWidget(file_group)

        cond_group = QGroupBox("Run Conditions & Mode")
        cond_main_layout = QVBoxLayout()
        cond_layout1 = QHBoxLayout()
        
        # 획득 제한 조건 콤보박스 (상호 배타적 제어 UI)
        cond_layout1.addWidget(QLabel("Stop Cond:"))
        self.combo_stop_cond = QComboBox()
        self.combo_stop_cond.addItems(["Unlimited", "Max Events", "Max Time"])
        self.combo_stop_cond.currentIndexChanged.connect(self.toggle_stop_cond)
        cond_layout1.addWidget(self.combo_stop_cond)
        
        self.spin_events = QSpinBox(); self.spin_events.setRange(0, 2000000000); self.spin_events.setPrefix("Evts: ")
        cond_layout1.addWidget(self.spin_events)
        
        self.spin_time = QSpinBox(); self.spin_time.setRange(0, 86400); self.spin_time.setPrefix("Sec: ")
        cond_layout1.addWidget(self.spin_time)
        
        cond_layout1.addWidget(QLabel("  |  Run Mode:"))
        self.combo_mode = QComboBox()
        self.combo_mode.addItems(["Single Continuous", "Split/Batch Mode", "Auto Threshold Scan"])
        self.combo_mode.currentIndexChanged.connect(self.toggle_batch_mode)
        cond_layout1.addWidget(self.combo_mode)
        
        self.lbl_batch = QLabel("Batches:")
        self.spin_batch = QSpinBox(); self.spin_batch.setRange(2, 999); self.spin_batch.setEnabled(False)
        cond_layout1.addWidget(self.lbl_batch)
        cond_layout1.addWidget(self.spin_batch)
        cond_main_layout.addLayout(cond_layout1)
        
        self.scan_layout = QHBoxLayout()
        self.scan_layout.addWidget(QLabel("Scan Range (14-bit ADC):"))
        self.scan_layout.addWidget(QLabel("Start:"))
        self.spin_scan_start = QSpinBox(); self.spin_scan_start.setRange(0, 16383); self.spin_scan_start.setValue(14000)
        self.scan_layout.addWidget(self.spin_scan_start)
        self.scan_layout.addWidget(QLabel("End:"))
        self.spin_scan_end = QSpinBox(); self.spin_scan_end.setRange(0, 16383); self.spin_scan_end.setValue(14500)
        self.scan_layout.addWidget(self.spin_scan_end)
        self.scan_layout.addWidget(QLabel("Step:"))
        self.spin_scan_step = QSpinBox(); self.spin_scan_step.setRange(1, 1000); self.spin_scan_step.setValue(20)
        self.scan_layout.addWidget(self.spin_scan_step)
        self.set_scan_enabled(False)
        cond_main_layout.addLayout(self.scan_layout)
        cond_group.setLayout(cond_main_layout)
        layout.addWidget(cond_group)

        dash_group = QGroupBox("Real-time Status Dashboard")
        dash_layout = QGridLayout()
        lbl_style = "font-weight: bold; color: #495057; font-size: 13px;"
        self.val_style = "font-weight: bold; font-size: 14px; background-color: #e9ecef; color: #0d6efd; padding: 4px; border: 1px solid #ced4da; border-radius: 4px;"
        self.val_style_warn = "font-weight: bold; font-size: 14px; background-color: #f8d7da; color: #dc3545; padding: 4px; border: 1px solid #f5c2c7; border-radius: 4px;"
        
        dash_layout.addWidget(QLabel("Storage:", styleSheet=lbl_style), 0, 0); self.val_disk = QLabel("Checking...", styleSheet=self.val_style); dash_layout.addWidget(self.val_disk, 0, 1)
        dash_layout.addWidget(QLabel("Batch/Scan:", styleSheet=lbl_style), 0, 2); self.val_batch = QLabel("1/1", styleSheet=self.val_style); dash_layout.addWidget(self.val_batch, 0, 3)
        dash_layout.addWidget(QLabel("Time:", styleSheet=lbl_style), 0, 4); self.val_time = QLabel("00:00", styleSheet=self.val_style); dash_layout.addWidget(self.val_time, 0, 5)
        dash_layout.addWidget(QLabel("Events:", styleSheet=lbl_style), 0, 6); self.val_events = QLabel("0", styleSheet=self.val_style); dash_layout.addWidget(self.val_events, 0, 7)
        dash_layout.addWidget(QLabel("Trg Rate:", styleSheet=lbl_style), 1, 0); self.val_rate = QLabel("0.0 Hz", styleSheet=self.val_style); dash_layout.addWidget(self.val_rate, 1, 1)
        dash_layout.addWidget(QLabel("Data Speed:", styleSheet=lbl_style), 1, 2); self.val_speed = QLabel("0.00 MB/s", styleSheet=self.val_style); dash_layout.addWidget(self.val_speed, 1, 3)
        dash_layout.addWidget(QLabel("ZMQ Drops:", styleSheet=lbl_style), 1, 4); self.val_drops = QLabel("0", styleSheet=self.val_style); dash_layout.addWidget(self.val_drops, 1, 5)
        dash_group.setLayout(dash_layout)
        layout.addWidget(dash_group)

        btn_layout = QHBoxLayout()
        self.btn_start = QPushButton("Start DAQ")
        self.btn_start.setStyleSheet("background-color: #0d6efd; color: white; font-weight: bold; padding: 10px; font-size: 14px;")
        self.btn_start.clicked.connect(self.start_daq_sequence)
        self.btn_stop = QPushButton("Stop DAQ")
        self.btn_stop.setStyleSheet("background-color: #dc3545; color: white; font-weight: bold; padding: 10px; font-size: 14px;")
        self.btn_stop.setEnabled(False)
        self.btn_stop.clicked.connect(self.stop_all)
        btn_layout.addWidget(self.btn_start); btn_layout.addWidget(self.btn_stop)
        layout.addLayout(btn_layout)

        self.terminal = QTextEdit(); self.terminal.setReadOnly(True); self.terminal.setFont(QFont("Monospace", 10))
        self.terminal.setStyleSheet("background-color: #ffffff; color: #212529; border: 1px solid #ced4da;")
        layout.addWidget(self.terminal)

    def toggle_stop_cond(self, idx):
        """콤보박스 선택에 따라 설정 불가능한 스핀박스를 비활성화하여 오입력 차단"""
        self.spin_events.setEnabled(idx == 1)
        self.spin_time.setEnabled(idx == 2)

    def load_settings(self):
        saved_config = self.settings.value("last_config", "config/dt5730s_inorganic_master.conf")
        self.config_input.setText(saved_config)
        self.output_input.setText(self.settings.value("last_output", "data/data_run.dat"))
        self.spin_run_no.setValue(int(self.settings.value("last_run_no", 1)))
        self.spin_events.setValue(int(self.settings.value("last_events", 0)))
        self.spin_time.setValue(int(self.settings.value("last_time", 3600)))
        self.combo_stop_cond.setCurrentIndex(int(self.settings.value("last_stop_cond", 0)))
        self.toggle_stop_cond(self.combo_stop_cond.currentIndex())
        if saved_config: self.parse_env_from_config(saved_config)

    def save_settings(self):
        self.settings.setValue("last_config", self.config_input.text())
        self.settings.setValue("last_output", self.output_input.text())
        self.settings.setValue("last_run_no", self.spin_run_no.value())
        self.settings.setValue("last_events", self.spin_events.value())
        self.settings.setValue("last_time", self.spin_time.value())
        self.settings.setValue("last_stop_cond", self.combo_stop_cond.currentIndex())

    def parse_env_from_config(self, filepath):
        if not os.path.isabs(filepath): full_path = os.path.abspath(os.path.join(self.proj_dir, filepath))
        else: full_path = filepath
        if not os.path.exists(full_path): return

        cfg = configparser.ConfigParser()
        cfg.optionxform = str
        cfg.read(full_path)
        if cfg.has_section("Environment"):
            self.operator_input.setText(cfg.get("Environment", "Operator", fallback="Unknown"))
            self.hv_input.setText(cfg.get("Environment", "AppliedHV", fallback="0V"))
            self.temp_input.setText(cfg.get("Environment", "Temperature", fallback="24.5"))

    def set_scan_enabled(self, enabled):
        self.spin_scan_start.setEnabled(enabled); self.spin_scan_end.setEnabled(enabled); self.spin_scan_step.setEnabled(enabled)

    def toggle_batch_mode(self, idx):
        self.spin_batch.setEnabled(idx == 1); self.set_scan_enabled(idx == 2)

    def browse_config(self):
        path, _ = QFileDialog.getOpenFileName(self, "Select Config File", self.config_dir, "Config Files (*.conf *.ini);;All Files (*)")
        if path: 
            self.config_input.setText(os.path.relpath(path, self.proj_dir))
            self.parse_env_from_config(path)
            self.save_settings()

    def browse_output(self):
        path, _ = QFileDialog.getSaveFileName(self, "Select Base Output File", self.data_dir, "Data Files (*.dat);;All Files (*)")
        if path: 
            self.output_input.setText(os.path.relpath(path, self.proj_dir))
            self.save_settings()

    def update_disk_space(self):
        os.makedirs(self.data_dir, exist_ok=True)
        total, used, free = shutil.disk_usage(self.data_dir)
        free_gb = free / (2**30)
        self.val_disk.setStyleSheet(self.val_style_warn if free_gb < 10.0 else self.val_style)
        self.val_disk.setText(f"{free_gb:.1f} GB")

    def append_log(self, text):
        safe_text = text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        color = "#212529"; bold = False
        if "[DAQ]" in safe_text: color = "#0d6efd"; bold = True
        elif "[Frontend]" in safe_text: color = "#198754"; bold = True
        elif "[DAQManager]" in safe_text: color = "#0dcaf0" 
        elif "[Warning]" in safe_text or "Error" in safe_text or "Failed" in safe_text: color = "#dc3545"; bold = True
        elif "===" in safe_text or "---" in safe_text: color = "#6c757d"; bold = True
        elif safe_text.strip().startswith("[") and "]" in safe_text: color = "#d63384" 
        b_open = "<b>" if bold else ""; b_close = "</b>" if bold else ""
        self.terminal.append(f'<span style="color: {color};">{b_open}{safe_text}{b_close}</span>')
        self.terminal.moveCursor(QTextCursor.MoveOperation.End)

    def update_dashboard(self, stats):
        self.last_stats = stats
        self.val_time.setText(stats.get('time', '00:00'))
        self.val_events.setText(stats.get('events', '0'))
        self.val_rate.setText(stats.get('rate', '0.0 Hz'))
        self.val_speed.setText(stats.get('speed', '0.00 MB/s')) 
        drops = int(stats.get('drops', '0'))
        self.val_drops.setStyleSheet(self.val_style_warn if drops > 0 else self.val_style)
        self.val_drops.setText(str(drops))

    def start_daq_sequence(self):
        self.current_run_no = self.spin_run_no.value()
        
        self.save_settings()
        self.base_output_path = self.output_input.text()
        self.current_batch = 1
        mode = self.combo_mode.currentIndex()
        if mode == 0: self.total_batches = 1
        elif mode == 1: self.total_batches = self.spin_batch.value()
        elif mode == 2:
            start = self.spin_scan_start.value(); end = self.spin_scan_end.value(); step = self.spin_scan_step.value()
            self.scan_values = list(range(start, end + 1, step)) if start <= end else list(range(start, end - 1, -step))
            self.total_batches = len(self.scan_values)

        self.btn_start.setEnabled(False); self.btn_stop.setEnabled(True)
        self.combo_mode.setEnabled(False); self.combo_stop_cond.setEnabled(False)
        self.spin_run_no.setEnabled(False)
        
        self.run_single_batch()

    def run_single_batch(self):
        self.last_stats = {}
        self.val_batch.setText(f"{self.current_batch} / {self.total_batches}")
        
        # 파일명에 자동 증가하는 _runNNN 부착
        name, ext = os.path.splitext(self.base_output_path)
        name_with_run = f"{name}_run{self.current_run_no:03d}"
        
        mode = self.combo_mode.currentIndex()
        if mode == 0: 
            output_file = f"{name_with_run}{ext}"
        elif mode == 1: 
            output_file = f"{name_with_run}_part{self.current_batch:02d}{ext}"
        elif mode == 2:
            current_th = self.scan_values[self.current_batch - 1]
            output_file = f"{name_with_run}_th{current_th}{ext}"

        out_file_full = os.path.abspath(os.path.join(self.proj_dir, output_file))
        os.makedirs(os.path.dirname(out_file_full), exist_ok=True)

        config_path_str = self.config_input.text()
        config_full = os.path.abspath(os.path.join(self.proj_dir, config_path_str))

        # [버그 픽스] configparser의 write()는 원본의 주석을 날려버립니다. 
        # 스캔 모드(Threshold 자동 갱신)가 아닐 경우, 원본 config 파일을 파괴하지 않도록 처리합니다.
        run_config_path_str = config_path_str
        if mode == 2:
            with open(config_full, 'r') as f: content = f.read()
            content = re.sub(r'TriggerThreshold\s*=\s*\d+', f'TriggerThreshold={current_th}', content)
            temp_scan_path = os.path.join(self.proj_dir, "config", f"temp_scan_th{current_th}.conf")
            with open(temp_scan_path, 'w') as f: f.write(content)
            run_config_path_str = os.path.relpath(temp_scan_path, self.proj_dir)
            self.append_log(f"\n[SCAN AUTOMATION] Target Threshold updated to {current_th} ADC.")

        current_env_data = {
            "Operator": self.operator_input.text().strip(),
            "Applied HV": self.hv_input.text().strip(),
            "Temperature (C)": self.temp_input.text().strip()
        }
        if self.env_data_provider: current_env_data.update(self.env_data_provider())

        self.current_run_id = self.db.record_run_start(output_file, current_env_data, config_full)
        self.append_log(f"\n========== [ Batch/Scan {self.current_batch}/{self.total_batches} Started ] ==========")
        self.append_log(f"--- Output: {output_file} | DB ID: {self.current_run_id} ---")
        
        exe_path = os.path.join(self.bin_dir, "frontend_dt5730")
        cmd = [exe_path, "-c", run_config_path_str, "-o", output_file]
        
        # 상호 배타적 획득 제한 인자 전달
        stop_idx = self.combo_stop_cond.currentIndex()
        if stop_idx == 1 and self.spin_events.value() > 0:
            cmd.extend(["-n", str(self.spin_events.value())])
        elif stop_idx == 2 and self.spin_time.value() > 0:
            cmd.extend(["-t", str(self.spin_time.value())])

        self.daq_process = ProcessManager(cmd, cwd=self.proj_dir)
        self.daq_process.log_signal.connect(self.append_log)
        self.daq_process.stat_signal.connect(self.update_dashboard)
        
        # ProcessManager의 하드웨어 시그널을 DaqTab의 영구 시그널로 포워딩
        self.daq_process.led_signal.connect(self.hardware_led_signal.emit)
        self.daq_process.temp_signal.connect(self.hardware_temp_signal.emit)
        self.daq_process.finished_signal.connect(self.daq_finished_signal.emit)
        self.daq_process.finished_signal.connect(self.on_batch_finished)

        self.daq_process.start()

    def on_batch_finished(self, returncode):
        self.append_log(f">>> Process Exited (Code: {returncode})")
        if self.current_run_id > 0 and self.last_stats:
            self.db.update_daq_summary(self.current_run_id, self.last_stats)
            self.append_log("[DB] DAQ Summary successfully pushed to database.")

        if self.current_batch < self.total_batches and returncode == 0:
            self.current_batch += 1
            self.run_single_batch()
        else:
            self.append_log("\n========== [ All DAQ Sequences Completed ] ==========")
            self.btn_start.setEnabled(True); self.btn_stop.setEnabled(False)
            self.combo_mode.setEnabled(True); self.combo_stop_cond.setEnabled(True)
            self.spin_run_no.setEnabled(True)
            
            # [자동 번호 증가] 완료(혹은 정지) 시 무조건 Run No 를 +1 증가시킵니다.
            self.spin_run_no.setValue(self.current_run_no + 1)
            self.save_settings()

    def stop_all(self):
        self.total_batches = 0 
        if self.daq_process and self.daq_process.isRunning(): self.daq_process.stop()