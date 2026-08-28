import os
import shutil
import configparser
import re
import math
from PyQt6.QtWidgets import (QWidget, QVBoxLayout, QHBoxLayout, QGridLayout, 
                             QPushButton, QLineEdit, QLabel, QTextEdit, 
                             QGroupBox, QSpinBox, QComboBox, QFileDialog, QMessageBox)
from PyQt6.QtGui import QFont, QTextCursor, QPainter, QColor, QPen, QBrush, QLinearGradient
from PyQt6.QtCore import QTimer, QSettings, pyqtSignal, pyqtSlot, Qt

from core.ProcessManager import ProcessManager
from core.DatabaseManager import DatabaseManager
from core.process_output import parse_drop_count
from core.trigger_settings import millivolts_to_adc_delta
from core.runtime_paths import (
    RuntimeValidationError,
    build_frontend_command,
    create_run_config_snapshot,
    file_identity,
    find_project_root,
    frontend_sources,
    identity_summary,
    metadata_status_paths,
    resolve_path,
    sidecar_paths,
    verify_binary_fresh,
    verify_deployed_gui,
)

DEFAULT_CONFIG_PATH = "config/dt5730s_inorganic.conf"
LEGACY_DEFAULT_CONFIG_PATH = "config/dt5730s_inorganic_master.conf"

# =========================================================================
# [물리적 방향성(Negative Pulse)이 적용된 1D 스캔 비주얼라이저]
# =========================================================================
class ADCScanVisualizer(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMinimumHeight(45)  
        self.setMaximumHeight(45)
        self.start_val = 14000
        self.end_val = 13000
        self.baseline = 14744      
        self.max_val = 16383       
        
    def update_range(self, start, end):
        self.start_val = start
        self.end_val = end
        self.update() 
        
    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        
        w = self.width()
        h = self.height()
        
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor("#e9ecef"))
        painter.drawRoundedRect(0, 0, w, h, 5, 5)
        
        def val_to_x(v): 
            return int((v / self.max_val) * w)
        
        x_start = val_to_x(self.start_val)
        x_end = val_to_x(self.end_val)
        x_base = val_to_x(self.baseline)
        
        left_x = min(x_start, x_end)
        right_x = max(x_start, x_end)
        rect_w = max(right_x - left_x, 4)
        
        is_danger = max(self.start_val, self.end_val) > (self.baseline - 15)
        
        gradient = QLinearGradient(x_start, 0, x_end, 0)
        if is_danger:
            gradient.setColorAt(0.0, QColor(255, 120, 120, 180)) 
            gradient.setColorAt(1.0, QColor(200, 0, 0, 220))     
        else:
            gradient.setColorAt(0.0, QColor(120, 180, 255, 160)) 
            gradient.setColorAt(1.0, QColor(13, 80, 253, 220))   
            
        painter.setBrush(gradient)
        painter.drawRoundedRect(left_x, 0, rect_w, h, 3, 3)
        
        pen_base = QPen(QColor("#198754"), 3)
        painter.setPen(pen_base)
        painter.drawLine(x_base, 0, x_base, h)
        
        font_small = QFont("Arial", 8, QFont.Weight.Bold)
        painter.setFont(font_small)
        painter.setPen(QColor("#6c757d"))
        painter.drawText(5, h - 5, "0")
        painter.drawText(w - 40, h - 5, "16383")
        
        painter.setPen(QColor("#0d6efd")) 
        painter.drawText(15, 15, "⟵ Deeper Voltage Drop (Smaller ADC)")
        
        painter.setPen(QColor("#198754"))
        painter.drawText(x_base - 55, 15, "Baseline")
        
        if rect_w > 50:
            painter.setPen(QColor(255, 255, 255))
            y_pos = h // 2 + 4
            if self.start_val >= self.end_val:
                painter.drawText(x_start - 35, y_pos, "Start")
                painter.drawText(x_end + 5, y_pos, "End ⟵")
            else:
                painter.drawText(x_start + 5, y_pos, "Start ⟶")
                painter.drawText(x_end - 30, y_pos, "End")
                
        if is_danger:
            painter.setPen(QColor(255, 255, 255))
            painter.drawText(left_x + (rect_w // 2) - 25, h // 2 + 4, "⚠ DANGER")

class DaqTab(QWidget):
    hardware_led_signal = pyqtSignal(dict)
    hardware_temp_signal = pyqtSignal(float)
    daq_finished_signal = pyqtSignal(int)
    
    scanRangeChanged = pyqtSignal(int, int)
    scanModeToggled = pyqtSignal(bool)
    runContextReady = pyqtSignal(dict)

    def __init__(self, parent=None, env_data_provider=None):
        super().__init__(parent)
        self.env_data_provider = env_data_provider
        self.daq_process = None
        
        self.proj_dir = str(find_project_root(__file__))
        self.runtime_gui_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        
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
        self.validated_config_full = ""
        self.validated_config_text = ""
        self.validated_config_identity = {}
        self.validated_frontend_identity = {}
        self.config_uses_mv_threshold = False
        self.current_run_context = None
        
        self.setup_ui()
        self.load_settings()
        self.refresh_runtime_identities()

        self.disk_timer = QTimer(self)
        self.disk_timer.timeout.connect(self.update_disk_space)
        self.disk_timer.start(1000)
        self.update_disk_space()

    def setup_ui(self):
        layout = QVBoxLayout(self)

        file_group = QGroupBox("File & Configuration Environment")
        file_layout = QGridLayout()
        file_layout.addWidget(QLabel("Config (.conf):"), 0, 0)
        self.config_input = QLineEdit(DEFAULT_CONFIG_PATH)
        self.config_input.textChanged.connect(self.refresh_runtime_identities)
        file_layout.addWidget(self.config_input, 0, 1)
        self.btn_browse_config = QPushButton("Browse")
        self.btn_browse_config.clicked.connect(self.browse_config)
        file_layout.addWidget(self.btn_browse_config, 0, 2)

        file_layout.addWidget(QLabel("Base Output (.dat):"), 1, 0)
        
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

        file_layout.addWidget(QLabel("Resolved frontend:"), 3, 0)
        self.lbl_frontend_identity = QLabel()
        self.lbl_frontend_identity.setWordWrap(True)
        self.lbl_frontend_identity.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextSelectableByMouse
        )
        file_layout.addWidget(self.lbl_frontend_identity, 3, 1, 1, 2)

        file_layout.addWidget(QLabel("Resolved config:"), 4, 0)
        self.lbl_config_identity = QLabel()
        self.lbl_config_identity.setWordWrap(True)
        self.lbl_config_identity.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextSelectableByMouse
        )
        file_layout.addWidget(self.lbl_config_identity, 4, 1, 1, 2)

        file_layout.addWidget(QLabel("Loaded GUI:"), 5, 0)
        self.lbl_gui_identity = QLabel(os.path.abspath(__file__))
        self.lbl_gui_identity.setWordWrap(True)
        self.lbl_gui_identity.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextSelectableByMouse
        )
        file_layout.addWidget(self.lbl_gui_identity, 5, 1, 1, 2)
        file_group.setLayout(file_layout)
        layout.addWidget(file_group)

        cond_group = QGroupBox("Run Conditions & Mode")
        cond_main_layout = QVBoxLayout()
        cond_layout1 = QHBoxLayout()
        
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
        self.spin_scan_end = QSpinBox(); self.spin_scan_end.setRange(0, 16383); self.spin_scan_end.setValue(13000)
        self.scan_layout.addWidget(self.spin_scan_end)
        self.scan_layout.addWidget(QLabel("Step:"))
        self.spin_scan_step = QSpinBox(); self.spin_scan_step.setRange(1, 1000); self.spin_scan_step.setValue(20)
        self.scan_layout.addWidget(self.spin_scan_step)
        
        self.spin_scan_start.valueChanged.connect(self.emit_scan_range)
        self.spin_scan_end.valueChanged.connect(self.emit_scan_range)
        
        cond_main_layout.addLayout(self.scan_layout)

        self.scan_visualizer = ADCScanVisualizer()
        self.scan_visualizer.setVisible(False)
        cond_main_layout.addWidget(self.scan_visualizer)

        self.set_scan_enabled(False)
        cond_group.setLayout(cond_main_layout)
        layout.addWidget(cond_group)

        dash_group = QGroupBox("Real-time Status Dashboard")
        dash_layout = QGridLayout()
        lbl_style = "font-weight: bold; color: #495057; font-size: 13px;"
        self.val_style = "font-weight: bold; font-size: 14px; background-color: #e9ecef; color: #0d6efd; padding: 4px; border: 1px solid #ced4da; border-radius: 4px;"
        self.val_style_warn = "font-weight: bold; font-size: 14px; background-color: #f8d7da; color: #dc3545; padding: 4px; border: 1px solid #f5c2c7; border-radius: 4px;"
        
        dash_layout.addWidget(QLabel("Storage:", styleSheet=lbl_style), 0, 0); self.val_disk = QLabel("Checking...", styleSheet=self.val_style); dash_layout.addWidget(self.val_disk, 0, 1)
        dash_layout.addWidget(QLabel("Batch/Scan:", styleSheet=lbl_style), 0, 2); self.val_batch = QLabel("1/1", styleSheet=self.val_style); dash_layout.addWidget(self.val_batch, 0, 3)
        
        dash_layout.addWidget(QLabel("Live Time:", styleSheet=lbl_style), 0, 4); self.val_live_time = QLabel("0.0 s", styleSheet=self.val_style); dash_layout.addWidget(self.val_live_time, 0, 5)
        dash_layout.addWidget(QLabel("Events:", styleSheet=lbl_style), 0, 6); self.val_events = QLabel("0", styleSheet=self.val_style); dash_layout.addWidget(self.val_events, 0, 7)
        
        dash_layout.addWidget(QLabel("Data Speed:", styleSheet=lbl_style), 1, 0); self.val_speed = QLabel("0.00 MB/s", styleSheet=self.val_style); dash_layout.addWidget(self.val_speed, 1, 1)
        dash_layout.addWidget(QLabel("ZMQ Drops:", styleSheet=lbl_style), 1, 2); self.val_drops = QLabel("0", styleSheet=self.val_style); dash_layout.addWidget(self.val_drops, 1, 3)
        dash_layout.addWidget(QLabel("Dead Time:", styleSheet=lbl_style), 1, 4); self.val_dead_time = QLabel("0.000 %", styleSheet=self.val_style); dash_layout.addWidget(self.val_dead_time, 1, 5)
        
        # =========================================================================
        # [신규 추가] 레이아웃 우측 하단에 Trig Rate 블록 추가
        # =========================================================================
        dash_layout.addWidget(QLabel("Trig Rate:", styleSheet=lbl_style), 1, 6)
        self.val_rate = QLabel("0.0 Hz", styleSheet=self.val_style)
        dash_layout.addWidget(self.val_rate, 1, 7)
        # =========================================================================
        
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

    @pyqtSlot()
    def emit_scan_range(self):
        s_val = self.spin_scan_start.value()
        e_val = self.spin_scan_end.value()
        self.scanRangeChanged.emit(s_val, e_val)
        self.scan_visualizer.update_range(s_val, e_val)

    def toggle_stop_cond(self, idx):
        self.spin_events.setEnabled(idx == 1)
        self.spin_time.setEnabled(idx == 2)

    def load_settings(self):
        saved_config = self.settings.value("last_config", DEFAULT_CONFIG_PATH)
        legacy_full_path = os.path.abspath(os.path.join(self.proj_dir, LEGACY_DEFAULT_CONFIG_PATH))
        if saved_config == LEGACY_DEFAULT_CONFIG_PATH and not os.path.isfile(legacy_full_path):
            saved_config = DEFAULT_CONFIG_PATH
            self.settings.setValue("last_config", saved_config)
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

    def refresh_runtime_identities(self, *_):
        if not hasattr(self, "lbl_frontend_identity"):
            return
        executable = os.path.join(self.bin_dir, "frontend_dt5730")
        try:
            identity = file_identity(executable)
            self.lbl_frontend_identity.setText(identity_summary(identity))
            self.lbl_frontend_identity.setStyleSheet("color: #495057;")
        except (OSError, RuntimeValidationError) as exc:
            self.lbl_frontend_identity.setText(f"UNAVAILABLE: {executable} ({exc})")
            self.lbl_frontend_identity.setStyleSheet("color: #dc3545; font-weight: bold;")

        config_value = self.config_input.text().strip()
        if not config_value:
            self.lbl_config_identity.setText("UNAVAILABLE: config path is empty")
            self.lbl_config_identity.setStyleSheet("color: #dc3545; font-weight: bold;")
            return
        try:
            config_path = resolve_path(self.proj_dir, config_value)
            identity = file_identity(config_path)
            self.lbl_config_identity.setText(identity_summary(identity))
            self.lbl_config_identity.setStyleSheet("color: #495057;")
        except (OSError, RuntimeValidationError) as exc:
            self.lbl_config_identity.setText(f"UNAVAILABLE: {config_value} ({exc})")
            self.lbl_config_identity.setStyleSheet("color: #dc3545; font-weight: bold;")

    def validate_runtime_artifacts(self, config_path):
        verify_deployed_gui(self.runtime_gui_dir, self.proj_dir)
        executable = os.path.join(self.bin_dir, "frontend_dt5730")
        frontend_identity = verify_binary_fresh(
            executable, frontend_sources(self.proj_dir)
        )
        config_identity = file_identity(config_path)
        self.validated_frontend_identity = frontend_identity
        self.validated_config_identity = config_identity
        self.lbl_frontend_identity.setText(identity_summary(frontend_identity))
        self.lbl_config_identity.setText(identity_summary(config_identity))
        return str(executable), frontend_identity, config_identity

    def parse_env_from_config(self, filepath):
        if not os.path.isabs(filepath): full_path = os.path.abspath(os.path.join(self.proj_dir, filepath))
        else: full_path = filepath
        if not os.path.exists(full_path): return

        cfg = configparser.ConfigParser(interpolation=None)
        cfg.optionxform = str
        try:
            with open(full_path, 'r', encoding='utf-8') as config_file:
                cfg.read_file(config_file)
        except (OSError, UnicodeError, configparser.Error) as exc:
            self.append_log(f"[Config Warning] 환경 메타데이터를 읽지 못했습니다: {exc}")
            return
        if cfg.has_section("Environment"):
            self.operator_input.setText(cfg.get("Environment", "Operator", fallback="Unknown"))
            self.hv_input.setText(cfg.get("Environment", "AppliedHV", fallback="0V"))
            self.temp_input.setText(cfg.get("Environment", "Temperature", fallback="24.5"))

    def set_scan_enabled(self, enabled):
        self.spin_scan_start.setEnabled(enabled); self.spin_scan_end.setEnabled(enabled); self.spin_scan_step.setEnabled(enabled)

    def toggle_batch_mode(self, idx):
        self.spin_batch.setEnabled(idx == 1)
        is_scan_mode = (idx == 2)
        self.set_scan_enabled(is_scan_mode)
        
        self.scan_visualizer.setVisible(is_scan_mode)
        self.scanModeToggled.emit(is_scan_mode)
        if is_scan_mode:
            self.emit_scan_range()

    def browse_config(self):
        path, _ = QFileDialog.getOpenFileName(self, "Select Config File", self.config_dir, "Config Files (*.conf *.ini);;All Files (*)")
        if path:
            self.set_config_path(path)

    @pyqtSlot(str)
    def set_config_path(self, path):
        resolved = resolve_path(self.proj_dir, path)
        if not resolved.is_file():
            self.append_log(
                f"[Config Warning] 동기화할 설정 파일이 없습니다: {resolved}"
            )
            return
        self.config_input.setText(str(resolved))
        self.parse_env_from_config(str(resolved))
        self.save_settings()
        self.refresh_runtime_identities()

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
        self.val_live_time.setText(stats.get('live_time', stats.get('Live', '0.0 s')))
        self.val_events.setText(stats.get('events', stats.get('Events', '0')))
        self.val_speed.setText(stats.get('speed', stats.get('Speed', '0.00 MB/s'))) 
        
        # =========================================================================
        # [신규 추가] Rate 업데이트 파싱 강화
        # 파이썬 파서가 넘겨줄 때 'Rate' 혹은 'rate' 키워드를 모두 추적하여 UI에 반영
        # =========================================================================
        rate_val = stats.get('rate', stats.get('Rate', '0.0 Hz'))
        if "Hz" not in rate_val and rate_val != "0.0": 
            rate_val += " Hz"
        self.val_rate.setText(rate_val)
        # =========================================================================
        
        dt_str = stats.get('dead_time', stats.get('DT', '0.000 %'))
        self.val_dead_time.setText(dt_str)
        try:
            dt_val = float(dt_str.replace('%', '').strip())
            self.val_dead_time.setStyleSheet(self.val_style_warn if dt_val > 5.0 else self.val_style)
        except ValueError:
            pass

        drops = parse_drop_count(stats.get('drops', stats.get('Drops', '0')))
        self.val_drops.setStyleSheet(self.val_style_warn if drops > 0 else self.val_style)
        self.val_drops.setText(str(drops))

    @pyqtSlot(str)
    def handle_fatal_error(self, err_type):
        if err_type == "OVER_TEMP_SOFT_KILL":
            QMessageBox.critical(
                self, 
                "Critical Hardware Error", 
                "ADC 내부 온도가 82°C에 도달하여 하드웨어 보호를 위해 DAQ 루프를 강제 종료(Soft-kill)했습니다.\n장비 쿨링 후 재시작하십시오."
            )
            self.stop_all()

    def validate_config_before_start(self):
        config_path = self.config_input.text().strip()
        if not config_path:
            raise ValueError("설정 파일 경로가 비어 있습니다.")

        config_full = config_path if os.path.isabs(config_path) else os.path.join(self.proj_dir, config_path)
        config_full = os.path.abspath(config_full)
        if not os.path.isfile(config_full):
            raise ValueError(f"설정 파일을 찾을 수 없습니다: {config_full}")

        try:
            with open(config_full, 'r', encoding='utf-8') as config_file:
                config_lines = config_file.readlines()
        except (OSError, UnicodeError) as exc:
            raise ValueError(f"설정 파일을 읽을 수 없습니다: {exc}") from exc

        config_data = {}
        current_section = None
        for line_number, raw_line in enumerate(config_lines, start=1):
            line = raw_line.replace('\u00a0', ' ').strip()
            if not line or line[0] in '#;':
                continue

            if line.startswith('['):
                if not line.endswith(']'):
                    raise ValueError(f"잘못된 섹션 문법입니다 (line {line_number}).")
                current_section = line[1:-1].strip()
                if not current_section:
                    raise ValueError(f"빈 섹션 이름입니다 (line {line_number}).")
                if current_section in config_data:
                    raise ValueError(f"중복 섹션입니다: [{current_section}] (line {line_number})")
                config_data[current_section] = {}
                continue

            if '=' not in line or current_section is None:
                raise ValueError(f"잘못된 설정 문법입니다 (line {line_number}).")
            key, raw_value = (part.strip() for part in line.split('=', 1))
            if not key or not raw_value:
                raise ValueError(f"빈 설정 키 또는 값입니다 (line {line_number}).")
            if key in config_data[current_section]:
                raise ValueError(
                    f"중복 설정 키입니다: [{current_section}] {key} (line {line_number})"
                )
            config_data[current_section][key] = raw_value

        if not any(config_data.values()):
            raise ValueError("설정 파일에 적용할 값이 없습니다.")

        def required_int(section, key, min_value, max_value):
            if section not in config_data or key not in config_data[section]:
                raise ValueError(f"필수 설정이 없습니다: [{section}] {key}")
            raw_value = config_data[section][key]
            if not re.fullmatch(r'[+-]?[0-9]+', raw_value):
                raise ValueError(f"정수가 아닌 설정값입니다: [{section}] {key}={raw_value}")
            value = int(raw_value, 10)
            if not min_value <= value <= max_value:
                raise ValueError(
                    f"설정값 범위 오류: [{section}] {key}={value} "
                    f"(허용 {min_value}..{max_value})"
                )
            return value

        def required_choice(section, key, choices):
            if section not in config_data or key not in config_data[section]:
                raise ValueError(f"필수 설정이 없습니다: [{section}] {key}")
            value = config_data[section][key]
            if value not in choices:
                allowed = ", ".join(sorted(choices))
                raise ValueError(
                    f"설정값 오류: [{section}] {key}={value} (허용 {allowed})"
                )
            return value

        def required_float(section, key, min_exclusive=None, max_exclusive=None):
            if section not in config_data or key not in config_data[section]:
                raise ValueError(f"필수 설정이 없습니다: [{section}] {key}")
            raw_value = config_data[section][key]
            try:
                value = float(raw_value)
            except ValueError as exc:
                raise ValueError(
                    f"실수가 아닌 설정값입니다: [{section}] {key}={raw_value}"
                ) from exc
            if not math.isfinite(value):
                raise ValueError(f"유한하지 않은 설정값입니다: [{section}] {key}")
            if min_exclusive is not None and value <= min_exclusive:
                raise ValueError(
                    f"설정값 범위 오류: [{section}] {key}={value} "
                    f"({min_exclusive}보다 커야 함)"
                )
            if max_exclusive is not None and value >= max_exclusive:
                raise ValueError(
                    f"설정값 범위 오류: [{section}] {key}={value} "
                    f"({max_exclusive}보다 작아야 함)"
                )
            return value

        record_length = required_int("Digitizer", "RecordLength", 128, 102400)
        channel_mask = required_int("Digitizer", "ChannelMask", 1, (1 << 8) - 1)
        post_trigger = required_int("Digitizer", "PostTrigger", 0, 100)
        required_int("Digitizer", "TriggerPolarity", 0, 1)
        ext_trigger = required_int("Digitizer", "ExtTriggerMode", 0, 1)
        self_trigger = required_int("Digitizer", "SelfTriggerMode", 0, 1)

        input_range_mv = required_int("Digitizer", "InputRangeMv", 500, 2000)
        if input_range_mv not in (500, 2000):
            raise ValueError("[Digitizer] InputRangeMv는 500 또는 2000이어야 합니다.")
        adc_bits = required_int("Digitizer", "ADCBits", 14, 14)
        if "Synchronization" in config_data:
            required_int("Synchronization", "ClockSource", 0, 1)
            required_int("Synchronization", "RunSyncMode", 0, 4)

        trigger_keys = (
            ("Digitizer", "SelfTriggerMask"),
            ("HardwareCoincidence", "PairLogic"),
        )
        trigger_key_count = sum(
            section in config_data and key in config_data[section]
            for section, key in trigger_keys
        )
        if trigger_key_count not in (0, len(trigger_keys)):
            raise ValueError(
                "[Digitizer] SelfTriggerMask와 [HardwareCoincidence] "
                "PairLogic은 두 항목을 모두 설정하거나 모두 생략해야 합니다."
            )

        if trigger_key_count == 0:
            self_trigger_mask = channel_mask if self_trigger else 0
            pair_logic = "OR"
        else:
            self_trigger_mask = required_int(
                "Digitizer", "SelfTriggerMask", 0, (1 << 8) - 1
            )
            pair_logic = required_choice(
                "HardwareCoincidence", "PairLogic", {"AND", "OR"}
            )

        if record_length % 8 != 0:
            raise ValueError("[Digitizer] RecordLength는 8의 배수여야 합니다.")
        if record_length * (100 - post_trigger) < 8000:
            raise ValueError("[Digitizer] 트리거 이전 구간이 최소 160 ns보다 짧습니다.")
        if ext_trigger == 0 and self_trigger == 0:
            raise ValueError("외부 트리거와 자체 트리거를 동시에 끌 수 없습니다.")
        if self_trigger_mask & ~channel_mask:
            raise ValueError(
                "[Digitizer] SelfTriggerMask는 ChannelMask의 부분집합이어야 합니다."
            )
        if self_trigger:
            if self_trigger_mask == 0:
                raise ValueError(
                    "SelfTriggerMode=1이면 SelfTriggerMask에 채널을 하나 이상 "
                    "선택해야 합니다."
                )
        else:
            if self_trigger_mask != 0:
                raise ValueError(
                    "SelfTriggerMode=0이면 [Digitizer] SelfTriggerMask는 0이어야 합니다."
                )

        if pair_logic == "AND":
            incomplete_pairs = [
                f"CH{pair_start}/{pair_start + 1}"
                for pair_start in range(0, 8, 2)
                if ((self_trigger_mask >> pair_start) & 0x3) not in (0, 0x3)
            ]
            if incomplete_pairs:
                raise ValueError(
                    "AND는 완전한 인접 pair만 선택할 수 있습니다: "
                    + ", ".join(incomplete_pairs)
                    + ". 여러 pair를 선택하면 각 pair의 AND 결과는 서로 OR로 결합됩니다."
                )
        uses_mv_threshold = False
        for ch in range(8):
            if (channel_mask >> ch) & 1:
                section = f"Channel_{ch}"
                required_int(section, "DCOffset", 0, 65535)
                has_absolute = (
                    section in config_data
                    and "TriggerThreshold" in config_data[section]
                )
                has_mv = (
                    section in config_data
                    and "TriggerThresholdMv" in config_data[section]
                )
                participates_in_trigger = bool(
                    self_trigger and ((self_trigger_mask >> ch) & 1)
                )
                if has_absolute and has_mv:
                    raise ValueError(
                        f"[{section}] TriggerThreshold(legacy)와 TriggerThresholdMv 중 "
                        "하나만 설정해야 합니다."
                    )
                if participates_in_trigger and not has_absolute and not has_mv:
                    raise ValueError(
                        f"[{section}] self-trigger 채널에는 TriggerThreshold 또는 "
                        "TriggerThresholdMv가 필요합니다."
                    )
                if not has_absolute and not has_mv:
                    continue
                if has_mv:
                    uses_mv_threshold = (
                        uses_mv_threshold or participates_in_trigger
                    )
                    requested_mv = required_float(
                        section, "TriggerThresholdMv", 0.0,
                        float(input_range_mv)
                    )
                    try:
                        millivolts_to_adc_delta(
                            requested_mv, input_range_mv, adc_bits
                        )
                    except ValueError as exc:
                        raise ValueError(
                            f"[{section}] TriggerThresholdMv={requested_mv}: {exc}"
                        ) from exc
                else:
                    required_int(section, "TriggerThreshold", 0, (1 << adc_bits) - 1)

        if uses_mv_threshold:
            settling_ms = required_int(
                "TriggerCalibration", "SettlingTimeMs", 0, 600000
            )
            settling_timeout_ms = required_int(
                "TriggerCalibration", "SettlingTimeoutMs", 1, 600000
            )
            if settling_timeout_ms <= settling_ms:
                raise ValueError(
                    "[TriggerCalibration] SettlingTimeoutMs는 "
                    "SettlingTimeMs보다 커야 합니다."
                )
            required_int(
                "TriggerCalibration", "MeasurementEvents", 1, 10000
            )
            required_float(
                "TriggerCalibration", "StabilityToleranceAdc", 0.0,
                float(1 << adc_bits)
            )
            required_int(
                "TriggerCalibration", "StableMeasurements", 2, 100
            )

        self.config_uses_mv_threshold = uses_mv_threshold
        self.validated_config_text = "".join(config_lines)

        return config_full

    def start_daq_sequence(self):
        try:
            self.validated_config_full = self.validate_config_before_start()
            if self.combo_mode.currentIndex() == 2 and self.config_uses_mv_threshold:
                raise ValueError(
                    "Auto Threshold Scan은 legacy absolute ADC threshold 전용입니다. "
                    "채널별 measured-baseline mV calibration 설정에서는 안전하지 않아 "
                    "실행을 차단했습니다."
                )
            executable, frontend_identity, config_identity = \
                self.validate_runtime_artifacts(self.validated_config_full)
            self.validated_frontend_path = executable
        except (ValueError, OSError) as exc:
            message = f"DAQ를 시작하지 않았습니다.\n\n{exc}"
            self.append_log(f"[Config Error] {exc}")
            QMessageBox.critical(self, "Invalid DAQ Configuration", message)
            return

        self.current_run_no = self.spin_run_no.value()

        self.append_log(
            f"[Runtime] Frontend: {identity_summary(frontend_identity)}"
        )
        self.append_log(
            f"[Runtime] Source config: {identity_summary(config_identity)}"
        )
        
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
        
        self.launch_current_batch()

    def restore_run_controls(self):
        self.btn_start.setEnabled(True)
        self.btn_stop.setEnabled(False)
        self.combo_mode.setEnabled(True)
        self.combo_stop_cond.setEnabled(True)
        self.spin_run_no.setEnabled(True)

    def launch_current_batch(self):
        """Launch one batch and recover the GUI on a fail-closed refusal."""

        try:
            self.run_single_batch()
            return True
        except (OSError, RuntimeError, ValueError) as exc:
            self.total_batches = 0
            self.current_run_context = None
            self.append_log(f"[Launch Error] DAQ를 시작하지 않았습니다: {exc}")
            QMessageBox.critical(
                self,
                "DAQ Launch Blocked",
                "DAQ를 시작하지 않았습니다. 기존 run 산출물은 변경하지 "
                f"않았습니다.\n\n{exc}",
            )
            self.restore_run_controls()
            return False

    def run_single_batch(self):
        self.last_stats = {}
        self.val_batch.setText(f"{self.current_batch} / {self.total_batches}")
        
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

        config_full = self.validated_config_full
        run_config_content = self.validated_config_text
        if mode == 2:
            run_config_content, replacement_count = re.subn(
                r'(?m)^(\s*TriggerThreshold\s*=\s*)[+-]?[0-9]+\s*$',
                rf'\g<1>{current_th}',
                run_config_content
            )
            if replacement_count == 0:
                raise RuntimeError("검증된 설정에서 TriggerThreshold를 갱신하지 못했습니다.")
            self.append_log(f"\n[SCAN AUTOMATION] Target Threshold updated to {current_th} ADC.")

        config_snapshot_path, metadata_path = sidecar_paths(out_file_full)
        snapshot_identity = create_run_config_snapshot(
            out_file_full, run_config_content
        )

        current_env_data = {
            "Operator": self.operator_input.text().strip(),
            "Applied HV": self.hv_input.text().strip(),
            "Temperature (C)": self.temp_input.text().strip()
        }
        if self.env_data_provider: current_env_data.update(self.env_data_provider())

        self.current_run_id = self.db.record_run_start(
            str(out_file_full), current_env_data, str(config_snapshot_path)
        )
        self.append_log(f"\n========== [ Batch/Scan {self.current_batch}/{self.total_batches} Started ] ==========")
        self.append_log(f"--- Output: {out_file_full} | DB ID: {self.current_run_id} ---")
        self.append_log(
            f"[Runtime] Config snapshot: {identity_summary(snapshot_identity)}"
        )
        self.append_log(f"[Runtime] Metadata sidecar: {metadata_path}")

        stop_idx = self.combo_stop_cond.currentIndex()
        max_events = 0
        run_time_sec = 0
        if stop_idx == 1 and self.spin_events.value() > 0:
            max_events = self.spin_events.value()
        elif stop_idx == 2 and self.spin_time.value() > 0:
            run_time_sec = self.spin_time.value()

        cmd = build_frontend_command(
            self.validated_frontend_path,
            config_snapshot_path,
            out_file_full,
            self.current_run_no,
            metadata_path,
            max_events=max_events,
            run_time_sec=run_time_sec,
        )

        self.current_run_context = {
            "raw_file": str(out_file_full),
            "config_path": str(config_snapshot_path),
            "metadata_path": str(metadata_path),
            "run_number": self.current_run_no,
            "source_config_path": str(config_full),
            "frontend_path": self.validated_frontend_identity["path"],
            "frontend_sha256": self.validated_frontend_identity["sha256"],
            "config_sha256": snapshot_identity["sha256"],
        }

        self.daq_process = ProcessManager(
            cmd,
            cwd=self.proj_dir,
            expected_hashes={
                self.validated_frontend_path:
                    self.validated_frontend_identity["sha256"],
                str(config_snapshot_path): snapshot_identity["sha256"],
            },
            expected_absent_paths=[
                out_file_full,
                metadata_path,
                *metadata_status_paths(metadata_path),
            ],
        )
        self.daq_process.log_signal.connect(self.append_log)
        self.daq_process.stat_signal.connect(self.update_dashboard)
        
        self.daq_process.led_signal.connect(self.hardware_led_signal.emit)
        self.daq_process.temp_signal.connect(self.hardware_temp_signal.emit)
        self.daq_process.fatal_signal.connect(self.handle_fatal_error)
        self.daq_process.finished_signal.connect(self.daq_finished_signal.emit)
        self.daq_process.finished_signal.connect(self.on_batch_finished)

        self.daq_process.start()

    def on_batch_finished(self, returncode):
        self.append_log(f">>> Process Exited (Code: {returncode})")
        if self.current_run_id > 0 and self.last_stats:
            self.db.update_daq_summary(self.current_run_id, self.last_stats)
            self.append_log("[DB] DAQ Summary successfully pushed to database.")

        if returncode == 0 and self.current_run_context:
            context = dict(self.current_run_context)
            metadata_path = context["metadata_path"]
            if os.path.isfile(metadata_path):
                metadata_identity = file_identity(metadata_path)
                context["metadata_sha256"] = metadata_identity["sha256"]
                context["metadata_exists"] = True
                self.append_log(
                    f"[Runtime] Completed metadata: "
                    f"{identity_summary(metadata_identity)}"
                )
            else:
                context["metadata_exists"] = False
                self.append_log(
                    "[Provenance Error] Frontend가 성공 종료했지만 metadata "
                    f"sidecar가 없습니다: {metadata_path}"
                )
            self.runContextReady.emit(context)

        if self.current_batch < self.total_batches and returncode == 0:
            self.current_batch += 1
            self.launch_current_batch()
        else:
            self.append_log("\n========== [ All DAQ Sequences Completed ] ==========")
            self.restore_run_controls()
            
            self.spin_run_no.setValue(self.current_run_no + 1)
            self.save_settings()

    def stop_all(self):
        self.total_batches = 0 
        if self.daq_process and self.daq_process.isRunning(): self.daq_process.stop()
