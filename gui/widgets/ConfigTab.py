import os
import configparser
import pyqtgraph as pg
from PyQt6.QtWidgets import (QWidget, QVBoxLayout, QHBoxLayout, QGridLayout,
                             QPushButton, QLabel, QTableWidget, QTableWidgetItem,
                             QGroupBox, QSpinBox, QDoubleSpinBox, QHeaderView, 
                             QFileDialog, QCheckBox)
from PyQt6.QtCore import Qt, QSettings

class ConfigTab(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        
        curr = os.path.abspath(os.path.dirname(__file__))
        while curr != '/' and not os.path.exists(os.path.join(curr, 'CMakeLists.txt')):
            curr = os.path.dirname(curr)
        self.proj_dir = curr if curr != '/' else os.getcwd()
        self.config_dir = os.path.join(self.proj_dir, "config")
        
        self.settings = QSettings("CPNR", "DT5730S_ConfigTab")
        self.current_config_path = ""
        self.config = configparser.ConfigParser()
        self.config.optionxform = str 
        self.setup_ui()
        self.load_settings()
        self.update_mask_calc()
        self.update_adc_simulator()
        self.update_time_simulator()

    def setup_ui(self):
        layout = QHBoxLayout(self)

        left_layout = QVBoxLayout()
        btn_layout = QHBoxLayout()
        self.btn_load = QPushButton("Load .conf")
        self.btn_load.clicked.connect(self.load_config_dialog)
        self.btn_save = QPushButton("Save .conf")
        self.btn_save.clicked.connect(self.save_config)
        self.btn_save.setStyleSheet("background-color: #0d6efd; color: white; font-weight: bold;")
        
        btn_layout.addWidget(self.btn_load); btn_layout.addWidget(self.btn_save)
        left_layout.addLayout(btn_layout)

        self.lbl_current_file = QLabel("Current File: None")
        self.lbl_current_file.setStyleSheet("color: #6c757d; font-weight: bold;")
        left_layout.addWidget(self.lbl_current_file)

        self.table = QTableWidget(0, 3)
        self.table.setHorizontalHeaderLabels(["Section", "Parameter", "Value"])
        self.table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.ResizeToContents)
        self.table.horizontalHeader().setSectionResizeMode(1, QHeaderView.ResizeMode.ResizeToContents)
        self.table.horizontalHeader().setSectionResizeMode(2, QHeaderView.ResizeMode.Stretch)
        left_layout.addWidget(self.table)

        self.advanced_group = QGroupBox("Advanced Settings (DT5730S Auto-calibrated)")
        advanced_layout = QVBoxLayout()
        self.btn_calibrate = QPushButton("Manual ADC Calibration")
        self.btn_calibrate.setToolTip("DT5730S 보드는 전원 인가 시 자동 캘리브레이션되므로 일반적인 런에서는 필요하지 않습니다.")
        self.btn_calibrate.setStyleSheet("background-color: #6c757d; color: white;")
        advanced_layout.addWidget(self.btn_calibrate)
        self.advanced_group.setLayout(advanced_layout)
        left_layout.addWidget(self.advanced_group)

        layout.addLayout(left_layout, stretch=5)

        right_layout = QVBoxLayout()
        mask_group = QGroupBox("Channel Bitmask Calculator (DT5730S 8-Ch)")
        mask_vbox = QVBoxLayout()
        chk_layout = QGridLayout()
        self.ch_checks = []
        for i in range(8):
            chk = QCheckBox(f"CH{i}")
            if i == 0: chk.setChecked(True)
            chk.stateChanged.connect(self.update_mask_calc)
            chk_layout.addWidget(chk, i//4, i%4)
            self.ch_checks.append(chk)
        mask_vbox.addLayout(chk_layout)
        res_mask_layout = QHBoxLayout()
        res_mask_layout.addWidget(QLabel("Decimal Mask Value:"))
        self.lbl_mask_res = QLabel("1")
        self.btn_apply_mask = QPushButton("Apply Mask")
        self.btn_apply_mask.clicked.connect(self.apply_mask_to_table)
        res_mask_layout.addWidget(self.lbl_mask_res); res_mask_layout.addWidget(self.btn_apply_mask)
        mask_vbox.addLayout(res_mask_layout); mask_group.setLayout(mask_vbox)
        right_layout.addWidget(mask_group)

        time_group = QGroupBox("Time & DSP Calculator (500 MS/s = 2 ns/Sample)")
        time_vbox = QVBoxLayout()
        time_grid = QGridLayout()
        time_grid.addWidget(QLabel("RecordLength (Samples):"), 0, 0)
        self.spin_record = QSpinBox(); self.spin_record.setRange(128, 102400); self.spin_record.setValue(2000)
        self.spin_record.valueChanged.connect(self.update_time_simulator)
        time_grid.addWidget(self.spin_record, 0, 1)
        time_grid.addWidget(QLabel("Target T0 Position (ns):"), 1, 0)
        self.spin_target_t0 = QSpinBox(); self.spin_target_t0.setRange(100, 10000); self.spin_target_t0.setValue(800)
        self.spin_target_t0.valueChanged.connect(self.update_time_simulator)
        time_grid.addWidget(self.spin_target_t0, 1, 1)
        time_vbox.addLayout(time_grid)
        self.lbl_res_post = QLabel(); self.lbl_res_pedestal = QLabel()
        time_vbox.addWidget(QLabel("Required PostTrigger (%):")); time_vbox.addWidget(self.lbl_res_post)
        time_vbox.addWidget(QLabel("Recommended BaselineSamples:")); time_vbox.addWidget(self.lbl_res_pedestal)
        self.btn_apply_time = QPushButton("Apply Time Configs")
        self.btn_apply_time.clicked.connect(self.apply_time_to_table)
        time_vbox.addWidget(self.btn_apply_time)
        time_group.setLayout(time_vbox)
        right_layout.addWidget(time_group)

        sim_group = QGroupBox("ADC Parameter Simulator (14-bit, 2Vpp)")
        sim_vbox = QVBoxLayout()
        input_grid = QGridLayout()
        input_grid.addWidget(QLabel("Target Baseline (%):"), 0, 0)
        self.spin_base_pct = QSpinBox(); self.spin_base_pct.setRange(10, 95); self.spin_base_pct.setValue(90)
        self.spin_base_pct.valueChanged.connect(self.update_adc_simulator)
        input_grid.addWidget(self.spin_base_pct, 0, 1)
        input_grid.addWidget(QLabel("Trigger Depth (mV):"), 1, 0)
        self.spin_trg_mv = QDoubleSpinBox(); self.spin_trg_mv.setRange(1.0, 2000.0); self.spin_trg_mv.setValue(15.0)
        self.spin_trg_mv.valueChanged.connect(self.update_adc_simulator)
        input_grid.addWidget(self.spin_trg_mv, 1, 1)
        sim_vbox.addLayout(input_grid)
        self.lbl_res_offset = QLabel(); self.lbl_res_trg = QLabel()
        sim_vbox.addWidget(QLabel("Required DCOffset (16-bit DAC):")); sim_vbox.addWidget(self.lbl_res_offset)
        sim_vbox.addWidget(QLabel("Required TriggerThreshold (14-bit ADC):")); sim_vbox.addWidget(self.lbl_res_trg)
        self.btn_apply_adc = QPushButton("Apply ADC to Active Channels")
        self.btn_apply_adc.clicked.connect(self.apply_adc_to_table)
        sim_vbox.addWidget(self.btn_apply_adc)

        pg.setConfigOptions(antialias=True, background='#f8f9fa', foreground='#212529')
        self.plot_sim = pg.PlotWidget(title="14-bit Dynamic Range Visualizer")
        self.plot_sim.setYRange(0, 16383, padding=0)
        self.plot_sim.setXRange(0, 1, padding=0); self.plot_sim.hideAxis('bottom')
        self.plot_sim.setLabel('left', "ADC Bins (14-bit)")
        self.line_base = pg.InfiniteLine(angle=0, pen=pg.mkPen('#198754', width=2, style=Qt.PenStyle.DashLine))
        self.line_trg = pg.InfiniteLine(angle=0, pen=pg.mkPen('#dc3545', width=2))
        self.plot_sim.addItem(self.line_base); self.plot_sim.addItem(self.line_trg)
        sim_vbox.addWidget(self.plot_sim)
        sim_group.setLayout(sim_vbox)
        right_layout.addWidget(sim_group, stretch=1)
        layout.addLayout(right_layout, stretch=3)

    def load_settings(self):
        saved_path = self.settings.value("last_loaded_config", "")
        if saved_path and os.path.exists(saved_path): self.load_file(saved_path)

    def load_config_dialog(self):
        last_dir = os.path.dirname(self.settings.value("last_loaded_config", self.config_dir))
        path, _ = QFileDialog.getOpenFileName(self, "Select Config File", last_dir, "Config Files (*.conf *.ini);;All Files (*)")
        if path: 
            rel_path = os.path.relpath(path, self.proj_dir)
            self.load_file(rel_path)

    def load_file(self, rel_path):
        full_path = os.path.abspath(os.path.join(self.proj_dir, rel_path))
        if not os.path.exists(full_path): return
        self.current_config_path = full_path
        self.settings.setValue("last_loaded_config", full_path)
        self.lbl_current_file.setText(f"Current File: {os.path.basename(full_path)}")
        self.config.read(full_path)
        self.table.setRowCount(0)
        for section in self.config.sections():
            for key, val in self.config.items(section):
                row = self.table.rowCount()
                self.table.insertRow(row)
                self.table.setItem(row, 0, QTableWidgetItem(section)); self.table.setItem(row, 1, QTableWidgetItem(key))
                self.table.setItem(row, 2, QTableWidgetItem(val))
        try:
            mask_val = int(self.config.get("Digitizer", "ChannelMask", fallback="1"))
            for i, chk in enumerate(self.ch_checks): chk.setChecked(bool((mask_val >> i) & 1))
        except: pass

    def update_mask_calc(self):
        mask = sum((1 << i) for i, chk in enumerate(self.ch_checks) if chk.isChecked())
        self.lbl_mask_res.setText(str(mask))

    def apply_mask_to_table(self):
        if self.table.rowCount() == 0: return
        self.set_table_value("Digitizer", "ChannelMask", self.lbl_mask_res.text())

    def update_time_simulator(self):
        rec_len = self.spin_record.value()
        target_t0_ns = self.spin_target_t0.value()
        dt_ns = 2.0 
        total_time_ns = rec_len * dt_ns
        
        # [제1원리 보정] 하드웨어 트리거 래치 지연시간(120 ns) 선행 보상
        intrinsic_latency_ns = 120.0
        required_pre_ns = target_t0_ns + intrinsic_latency_ns

        if required_pre_ns >= total_time_ns: 
            required_pre_ns = total_time_ns - 16.0 # RecordLength 오버플로우 방어
            
        pre_pct = (required_pre_ns / total_time_ns) * 100.0
        post_pct = int(round(100.0 - pre_pct))
        
        if post_pct < 10: post_pct = 10
        if post_pct > 90: post_pct = 90
        
        target_t0_samples = int(target_t0_ns / dt_ns)
        recommended_pedestal = int(target_t0_samples * 0.8) 
        
        self.lbl_res_post.setText(f"{post_pct} %")
        self.lbl_res_pedestal.setText(f"{recommended_pedestal} Samples")
        self.calculated_post_pct = post_pct
        self.calculated_pedestal = recommended_pedestal

    def apply_time_to_table(self):
        if self.table.rowCount() == 0: return
        self.set_table_value("Digitizer", "RecordLength", str(self.spin_record.value()))
        if hasattr(self, 'calculated_post_pct'):
            self.set_table_value("Digitizer", "PostTrigger", str(self.calculated_post_pct))
            self.set_table_value("SoftwareDSP", "BaselineSamples", str(self.calculated_pedestal))

    def update_adc_simulator(self):
        base_pct = self.spin_base_pct.value() / 100.0
        trg_mv = self.spin_trg_mv.value()
        dac_offset = int((1.0 - base_pct) * 65535)
        adc_baseline = int(base_pct * 16383)
        adc_trg_drop = int(trg_mv / 0.12207) 
        adc_trigger = adc_baseline - adc_trg_drop
        
        self.lbl_res_offset.setText(f"{dac_offset}  (Target: {self.spin_base_pct.value()}%)")
        self.lbl_res_trg.setText(f"{adc_trigger}  (Baseline {adc_baseline} - Drop {adc_trg_drop})")
        self.line_base.setValue(adc_baseline)
        self.line_trg.setValue(adc_trigger)

    def apply_adc_to_table(self):
        if self.table.rowCount() == 0: return
        base_pct = self.spin_base_pct.value() / 100.0
        trg_mv = self.spin_trg_mv.value()
        calc_offset = str(int((1.0 - base_pct) * 65535))
        calc_trg = str(int((base_pct * 16383) - (trg_mv / 0.12207)))
        for row in range(self.table.rowCount()):
            section = self.table.item(row, 0).text()
            param = self.table.item(row, 1).text()
            if section.startswith("Channel_"):
                if param == "DCOffset" or param == "TriggerThreshold":
                    val = calc_offset if param == "DCOffset" else calc_trg
                    self.table.setItem(row, 2, QTableWidgetItem(val))
                    self.table.item(row, 2).setBackground(Qt.GlobalColor.yellow)

    def set_table_value(self, target_section, target_param, value):
        for row in range(self.table.rowCount()):
            if self.table.item(row, 0).text() == target_section and self.table.item(row, 1).text() == target_param:
                self.table.setItem(row, 2, QTableWidgetItem(value)); self.table.item(row, 2).setBackground(Qt.GlobalColor.yellow)
                return
        row = self.table.rowCount(); self.table.insertRow(row)
        self.table.setItem(row, 0, QTableWidgetItem(target_section)); self.table.setItem(row, 1, QTableWidgetItem(target_param))
        self.table.setItem(row, 2, QTableWidgetItem(value)); self.table.item(row, 2).setBackground(Qt.GlobalColor.yellow)

    def save_config(self):
        if not self.current_config_path: return
        self.config.clear()
        for row in range(self.table.rowCount()):
            sec = self.table.item(row, 0).text(); key = self.table.item(row, 1).text(); val = self.table.item(row, 2).text()
            if not self.config.has_section(sec): self.config.add_section(sec)
            self.config.set(sec, key, val)
            self.table.item(row, 2).setBackground(Qt.GlobalColor.white) 
        with open(self.current_config_path, 'w') as configfile: self.config.write(configfile)