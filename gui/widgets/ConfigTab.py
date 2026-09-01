import os
import configparser
import io
import math
import pyqtgraph as pg
from PyQt6.QtWidgets import (QWidget, QVBoxLayout, QHBoxLayout, QGridLayout,
                             QPushButton, QLabel, QTableWidget, QTableWidgetItem,
                             QGroupBox, QSpinBox, QDoubleSpinBox, QHeaderView, 
                             QFileDialog, QCheckBox, QMessageBox, QComboBox)
from PyQt6.QtCore import (
    Qt, QIODevice, QSaveFile, QSettings, pyqtSignal, pyqtSlot,
)

from core.trigger_settings import (
    calculate_threshold_preview,
    millivolts_to_adc_delta,
)
from core.dt5730_constraints import (
    ADC_SAMPLE_PERIOD_NS,
    MAX_RECORD_LENGTH,
    MAX_SOFTWARE_RANDOM_TRIGGER_RATE_HZ,
    MIN_REQUESTED_RECORD_LENGTH,
    MIN_SOFTWARE_RANDOM_TRIGGER_RATE_HZ,
    RECORD_LENGTH_GRANULARITY,
    TRIGGER_LATENCY_NS,
    derive_time_dsp_plan,
    normalize_record_length,
    predict_post_trigger,
)

class ConfigTab(QWidget):
    configPathChanged = pyqtSignal(str)
    configDirtyChanged = pyqtSignal(str, bool)

    CONTROLLED_TABLE_KEYS = frozenset({
        ("Digitizer", "ChannelMask"),
        ("Digitizer", "SelfTriggerMask"),
        ("Digitizer", "ExtTriggerMode"),
        ("Digitizer", "SelfTriggerMode"),
        ("Digitizer", "SoftwareRandomTriggerMode"),
        ("Digitizer", "SoftwareRandomTriggerRateHz"),
        ("HardwareCoincidence", "PairLogic"),
    })

    TRIGGER_SOURCE_SELF = "Self Trigger"
    TRIGGER_SOURCE_EXTERNAL = "External Trigger"
    TRIGGER_SOURCE_RANDOM = "Software Random"

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
        self.trigger_controls_load_error = None
        self._config_dirty = False
        self.setup_ui()
        self.load_settings()
        self.update_mask_calc()
        self.sync_threshold_controls_from_config()

    def is_dirty(self):
        return self._config_dirty

    def _set_config_dirty(self, dirty):
        dirty = bool(dirty)
        changed = dirty != self._config_dirty
        self._config_dirty = dirty
        if self.current_config_path:
            marker = " * UNSAVED" if dirty else ""
            self.lbl_current_file.setText(
                f"Current File: {os.path.basename(self.current_config_path)}{marker}"
            )
            self.lbl_current_file.setStyleSheet(
                "color: #dc3545; font-weight: bold;"
                if dirty else "color: #6c757d; font-weight: bold;"
            )
        if changed:
            self.configDirtyChanged.emit(self.current_config_path, dirty)

    def sync_threshold_controls_from_config(self):
        """Reflect the loaded runtime-threshold schema in the calculator."""

        input_range = self.config.get(
            "Digitizer", "InputRangeMv", fallback="2000"
        ).strip()
        if input_range in {"500", "2000"}:
            self.combo_input_range.blockSignals(True)
            self.combo_input_range.setCurrentText(input_range)
            self.combo_input_range.blockSignals(False)
            self.on_input_range_changed(input_range)

        try:
            self_trigger_mode = self.config.getint(
                "Digitizer", "SelfTriggerMode", fallback=0
            )
            channel_mask = self.config.getint(
                "Digitizer", "ChannelMask", fallback=0
            )
            trigger_mask = self.config.getint(
                "Digitizer", "SelfTriggerMask",
                fallback=channel_mask if self_trigger_mode else 0,
            )
        except (ValueError, configparser.Error):
            return

        requested_values = []
        for ch in range(8):
            if not ((trigger_mask >> ch) & 1):
                continue
            raw_value = self.config.get(
                f"Channel_{ch}", "TriggerThresholdMv", fallback=""
            ).strip()
            try:
                requested_values.append(float(raw_value))
            except ValueError:
                continue
        if requested_values and all(
            math.isclose(value, requested_values[0], rel_tol=0.0, abs_tol=1e-9)
            for value in requested_values[1:]
        ):
            self.spin_trg_mv.setValue(requested_values[0])
        self.update_trigger_mask_calc()
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
        self.table.cellChanged.connect(self.on_table_cell_changed)
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
        mask_group = QGroupBox("Readout & Trigger Configuration (DT5730S 8-Ch)")
        mask_vbox = QVBoxLayout()

        mask_vbox.addWidget(QLabel("Readout channels (stored on every accepted global trigger):"))
        readout_chk_layout = QGridLayout()
        self.ch_checks = []
        for i in range(8):
            chk = QCheckBox(f"CH{i}")
            if i == 0: chk.setChecked(True)
            chk.stateChanged.connect(self.on_readout_control_changed)
            readout_chk_layout.addWidget(chk, i//4, i%4)
            self.ch_checks.append(chk)
        mask_vbox.addLayout(readout_chk_layout)
        res_mask_layout = QHBoxLayout()
        res_mask_layout.addWidget(QLabel("Readout mask (decimal):"))
        self.lbl_mask_res = QLabel("1")
        self.btn_apply_mask = QPushButton("Apply Readout Mask")
        self.btn_apply_mask.clicked.connect(self.apply_mask_to_table)
        res_mask_layout.addWidget(self.lbl_mask_res); res_mask_layout.addWidget(self.btn_apply_mask)
        mask_vbox.addLayout(res_mask_layout)

        mask_vbox.addWidget(QLabel("Self-trigger channels (do not select channels used only for readout):"))
        trigger_chk_layout = QGridLayout()
        self.trigger_ch_checks = []
        for i in range(8):
            chk = QCheckBox(f"CH{i}")
            if i == 0: chk.setChecked(True)
            chk.stateChanged.connect(self.on_trigger_control_changed)
            trigger_chk_layout.addWidget(chk, i//4, i%4)
            self.trigger_ch_checks.append(chk)
        mask_vbox.addLayout(trigger_chk_layout)

        trigger_mask_layout = QHBoxLayout()
        trigger_mask_layout.addWidget(QLabel("Self-trigger mask (decimal):"))
        self.lbl_trigger_mask_res = QLabel("1")
        trigger_mask_layout.addWidget(self.lbl_trigger_mask_res)
        mask_vbox.addLayout(trigger_mask_layout)

        trigger_options = QGridLayout()
        trigger_options.addWidget(QLabel("Trigger source:"), 0, 0)
        self.combo_trigger_source = QComboBox()
        self.combo_trigger_source.addItems([
            self.TRIGGER_SOURCE_SELF,
            self.TRIGGER_SOURCE_EXTERNAL,
            self.TRIGGER_SOURCE_RANDOM,
        ])
        self.combo_trigger_source.currentTextChanged.connect(
            self.on_trigger_source_changed
        )
        trigger_options.addWidget(self.combo_trigger_source, 0, 1)
        trigger_options.addWidget(QLabel("Random mean rate (Hz):"), 1, 0)
        self.spin_random_rate = QDoubleSpinBox()
        self.spin_random_rate.setDecimals(6)
        self.spin_random_rate.setRange(
            MIN_SOFTWARE_RANDOM_TRIGGER_RATE_HZ,
            MAX_SOFTWARE_RANDOM_TRIGGER_RATE_HZ,
        )
        self.spin_random_rate.setSingleStep(1.0)
        self.spin_random_rate.setValue(1.0)
        self.spin_random_rate.setToolTip(
            "Software Random 선택 시 평균 트리거율입니다. 실제 간격은 Poisson 분포를 따릅니다."
        )
        self.spin_random_rate.valueChanged.connect(self.on_random_rate_changed)
        trigger_options.addWidget(self.spin_random_rate, 1, 1)
        trigger_options.addWidget(QLabel("Adjacent-pair logic:"), 2, 0)
        self.combo_pair_logic = QComboBox()
        self.combo_pair_logic.addItems(["OR", "AND"])
        self.combo_pair_logic.currentTextChanged.connect(self.on_trigger_control_changed)
        trigger_options.addWidget(self.combo_pair_logic, 2, 1)
        mask_vbox.addLayout(trigger_options)

        self.lbl_trigger_hint = QLabel()
        self.lbl_trigger_hint.setWordWrap(True)
        mask_vbox.addWidget(self.lbl_trigger_hint)

        self.btn_apply_trigger = QPushButton("Apply Trigger Settings")
        self.btn_apply_trigger.clicked.connect(self.apply_trigger_to_table)
        mask_vbox.addWidget(self.btn_apply_trigger)

        mask_group.setLayout(mask_vbox)
        right_layout.addWidget(mask_group)

        time_group = QGroupBox("Time & DSP Calculator (500 MS/s = 2 ns/Sample)")
        time_vbox = QVBoxLayout()
        time_grid = QGridLayout()
        time_grid.addWidget(QLabel("RecordLength (Samples):"), 0, 0)
        self.spin_record = QSpinBox()
        self.spin_record.setRange(
            MIN_REQUESTED_RECORD_LENGTH, MAX_RECORD_LENGTH
        )
        self.spin_record.setSingleStep(RECORD_LENGTH_GRANULARITY)
        self.spin_record.setValue(2000)
        self.spin_record.valueChanged.connect(self.update_time_simulator)
        time_grid.addWidget(self.spin_record, 0, 1)
        time_grid.addWidget(QLabel("Target T0 Position (ns):"), 1, 0)
        self.spin_target_t0 = QSpinBox(); self.spin_target_t0.setRange(100, 10000); self.spin_target_t0.setValue(800)
        self.spin_target_t0.valueChanged.connect(self.update_time_simulator)
        time_grid.addWidget(self.spin_target_t0, 1, 1)
        time_vbox.addLayout(time_grid)
        self.lbl_res_record = QLabel()
        self.lbl_res_post = QLabel()
        self.lbl_res_window = QLabel()
        self.lbl_res_pedestal = QLabel()
        self.lbl_res_gates = QLabel()
        time_vbox.addWidget(QLabel("Hardware RecordLength:"))
        time_vbox.addWidget(self.lbl_res_record)
        time_vbox.addWidget(QLabel("Required PostTrigger (%):")); time_vbox.addWidget(self.lbl_res_post)
        time_vbox.addWidget(QLabel("Actual Window / Achieved T0:"))
        time_vbox.addWidget(self.lbl_res_window)
        time_vbox.addWidget(QLabel("Recommended BaselineSamples:")); time_vbox.addWidget(self.lbl_res_pedestal)
        legacy_gate_label = QLabel("Legacy gates (schema 1/2 only):")
        legacy_gate_hint = (
            "현재 schema 3 ROOT charge는 polarity-corrected peak 기준 "
            "[-20 ns, +40 ns) 고정 window를 사용합니다. ShortGate/LongGate와 "
            "PulseStartThresholdAdc는 schema 1/2 호환 및 provenance용입니다."
        )
        legacy_gate_label.setToolTip(legacy_gate_hint)
        self.lbl_res_gates.setToolTip(legacy_gate_hint)
        time_vbox.addWidget(legacy_gate_label)
        time_vbox.addWidget(self.lbl_res_gates)
        self.btn_apply_time = QPushButton("Apply Time Configs")
        self.btn_apply_time.clicked.connect(self.apply_time_to_table)
        time_vbox.addWidget(self.btn_apply_time)
        time_group.setLayout(time_vbox)
        right_layout.addWidget(time_group)

        sim_group = QGroupBox("Runtime Trigger Calibration (14-bit, per-channel baseline)")
        sim_vbox = QVBoxLayout()
        input_grid = QGridLayout()
        input_grid.addWidget(QLabel("Plot baseline preview only (%):"), 0, 0)
        self.spin_base_pct = QSpinBox(); self.spin_base_pct.setRange(10, 95); self.spin_base_pct.setValue(90)
        self.spin_base_pct.valueChanged.connect(self.update_adc_simulator)
        input_grid.addWidget(self.spin_base_pct, 0, 1)
        input_grid.addWidget(QLabel("Hardware threshold (mV):"), 1, 0)
        self.spin_trg_mv = QDoubleSpinBox(); self.spin_trg_mv.setDecimals(3)
        self.spin_trg_mv.setRange(0.001, 2000.0); self.spin_trg_mv.setValue(15.0)
        self.spin_trg_mv.valueChanged.connect(self.update_adc_simulator)
        input_grid.addWidget(self.spin_trg_mv, 1, 1)
        input_grid.addWidget(QLabel("Input range (mVpp):"), 2, 0)
        self.combo_input_range = QComboBox()
        self.combo_input_range.addItems(["2000", "500"])
        self.combo_input_range.currentTextChanged.connect(self.on_input_range_changed)
        input_grid.addWidget(self.combo_input_range, 2, 1)
        sim_vbox.addLayout(input_grid)
        self.lbl_res_offset = QLabel(); self.lbl_res_trg = QLabel()
        sim_vbox.addWidget(QLabel("DCOffset policy:")); sim_vbox.addWidget(self.lbl_res_offset)
        sim_vbox.addWidget(QLabel("Runtime threshold request:")); sim_vbox.addWidget(self.lbl_res_trg)
        self.btn_apply_adc = QPushButton("Store mV Threshold for Self-Trigger Channels")
        self.btn_apply_adc.setToolTip(
            "절대 ADC threshold를 계산하지 않습니다. 채널별 mV 요청값을 저장하며 "
            "DAQ가 실제 baseline을 측정한 뒤 각 채널의 absolute threshold를 정합니다."
        )
        self.btn_apply_adc.clicked.connect(self.apply_adc_to_table)
        sim_vbox.addWidget(self.btn_apply_adc)

        pg.setConfigOptions(antialias=True, background='#f8f9fa', foreground='#212529')
        self.plot_sim = pg.PlotWidget(title="14-bit Dynamic Range Visualizer")
        self.plot_sim.setYRange(0, 16383, padding=0)
        self.plot_sim.setXRange(0, 1, padding=0); self.plot_sim.hideAxis('bottom')
        self.plot_sim.setLabel('left', "ADC Bins (14-bit)")
        
        self.line_base = pg.InfiniteLine(angle=0, pen=pg.mkPen('#198754', width=2, style=Qt.PenStyle.DashLine))
        self.line_trg = pg.InfiniteLine(angle=0, pen=pg.mkPen('#dc3545', width=2))
        self.plot_sim.addItem(self.line_base)
        self.plot_sim.addItem(self.line_trg)

        # ====================================================================
        # [신규 추가] 스캔 범위를 표시할 수평 방향 반투명 면적 시각화
        # ====================================================================
        self.scan_region = pg.LinearRegionItem(orientation='horizontal', brush=pg.mkBrush(0, 100, 255, 50), movable=False)
        self.scan_region.setRegion([14000, 14500])
        self.scan_region.hide() 
        self.plot_sim.addItem(self.scan_region)
        # ====================================================================

        sim_vbox.addWidget(self.plot_sim)
        sim_group.setLayout(sim_vbox)
        right_layout.addWidget(sim_group, stretch=1)
        layout.addLayout(right_layout, stretch=3)

    # ====================================================================
    # [신규 추가] DaqTab 스캔 관련 시그널 수신 슬롯
    # ====================================================================
    @pyqtSlot(int, int)
    def update_scan_region(self, start_val, end_val):
        self.scan_region.setRegion([start_val, end_val])
        
        current_baseline = self.line_base.value()
        if start_val > (current_baseline - 15) or end_val > (current_baseline - 15):
            self.scan_region.setBrush(pg.mkBrush(255, 0, 0, 70))  # 위험
        else:
            self.scan_region.setBrush(pg.mkBrush(0, 100, 255, 50)) # 안전

    @pyqtSlot(bool)
    def toggle_scan_region_visibility(self, is_visible):
        self.scan_region.setVisible(is_visible)
    # ====================================================================

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
        if not os.path.exists(full_path):
            return

        loaded_config = configparser.ConfigParser()
        loaded_config.optionxform = str
        try:
            with open(full_path, "r", encoding="utf-8") as config_file:
                loaded_config.read_file(config_file)
            loaded_rows = [
                (section, key, value)
                for section in loaded_config.sections()
                for key, value in loaded_config.items(section)
            ]
        except (OSError, UnicodeError, configparser.Error) as exc:
            QMessageBox.critical(
                self, "Invalid Config File",
                f"설정 파일을 열지 않았습니다.\n\n{exc}"
            )
            return

        self.current_config_path = full_path
        self.settings.setValue("last_loaded_config", full_path)
        self.lbl_current_file.setText(f"Current File: {os.path.basename(full_path)}")
        self.configPathChanged.emit(full_path)
        self.config = loaded_config
        self.table.blockSignals(True)
        try:
            self.table.setRowCount(0)
            for section, key, value in loaded_rows:
                row = self.table.rowCount()
                self.table.insertRow(row)
                self.table.setItem(row, 0, QTableWidgetItem(section))
                self.table.setItem(row, 1, QTableWidgetItem(key))
                self.table.setItem(row, 2, QTableWidgetItem(value))
                self.protect_controlled_row(row)
        finally:
            self.table.blockSignals(False)

        try:
            mask_val = int(self.config.get("Digitizer", "ChannelMask"), 10)
            self_trigger_mode = int(
                self.config.get("Digitizer", "SelfTriggerMode"), 10
            )
            ext_trigger_mode = int(
                self.config.get("Digitizer", "ExtTriggerMode"), 10
            )
            random_trigger_mode = int(
                self.config.get(
                    "Digitizer", "SoftwareRandomTriggerMode", fallback="0"
                ), 10
            )
            random_trigger_rate = float(
                self.config.get(
                    "Digitizer", "SoftwareRandomTriggerRateHz", fallback="0"
                )
            )

            trigger_keys = (
                ("Digitizer", "SelfTriggerMask"),
                ("HardwareCoincidence", "PairLogic"),
            )
            trigger_key_count = sum(
                self.config.has_option(section, key)
                for section, key in trigger_keys
            )
            if trigger_key_count not in (0, len(trigger_keys)):
                raise ValueError(
                    "SelfTriggerMask와 PairLogic은 모두 설정하거나 모두 "
                    "생략해야 합니다."
                )

            if trigger_key_count == 0:
                trigger_mask = mask_val if self_trigger_mode else 0
                pair_logic = "OR"
            else:
                trigger_mask = int(
                    self.config.get("Digitizer", "SelfTriggerMask"), 10
                )
                pair_logic = self.config.get(
                    "HardwareCoincidence", "PairLogic"
                ).strip()

            self.validate_trigger_values(
                mask_val, trigger_mask, pair_logic, ext_trigger_mode,
                self_trigger_mode, random_trigger_mode, random_trigger_rate
            )

            self.set_mask_checks(self.ch_checks, mask_val)
            self.set_mask_checks(self.trigger_ch_checks, trigger_mask)
            self.combo_pair_logic.blockSignals(True)
            self.combo_pair_logic.setCurrentText(pair_logic)
            self.combo_pair_logic.blockSignals(False)
            self.sync_trigger_source_controls(
                ext_trigger_mode, self_trigger_mode,
                random_trigger_mode, random_trigger_rate
            )
            self.trigger_controls_load_error = None
        except (TypeError, ValueError, OverflowError, configparser.Error) as exc:
            # Never leave values from the previously loaded file in these
            # controls. The table remains untouched so Save/DAQ validation can
            # still report the original malformed setting.
            self.set_mask_checks(self.ch_checks, 1)
            self.set_mask_checks(self.trigger_ch_checks, 1)
            self.combo_pair_logic.blockSignals(True)
            self.combo_pair_logic.setCurrentText("OR")
            self.combo_pair_logic.blockSignals(False)
            self.sync_trigger_source_controls(1, 0, 0, 0.0)
            self.trigger_controls_load_error = (
                f"로드한 설정 오류: {exc} 값을 바꾼 뒤 전용 적용 버튼을 누르세요."
            )

        self.update_mask_calc()
        self.sync_time_controls_from_config()
        self.sync_threshold_controls_from_config()
        self._set_config_dirty(False)

    def protect_controlled_row(self, row):
        section_item = self.table.item(row, 0)
        parameter_item = self.table.item(row, 1)
        if not section_item or not parameter_item:
            return

        structure_tooltip = "Section과 Parameter 이름은 설정 스키마이므로 변경할 수 없습니다."
        for column in (0, 1):
            item = self.table.item(row, column)
            if item:
                item.setFlags(item.flags() & ~Qt.ItemFlag.ItemIsEditable)
                item.setToolTip(structure_tooltip)

        key = (section_item.text(), parameter_item.text())
        if key not in self.CONTROLLED_TABLE_KEYS:
            return

        tooltip = "오른쪽의 Readout & Trigger 전용 컨트롤에서 변경하세요."
        value_item = self.table.item(row, 2)
        if value_item:
            value_item.setFlags(
                value_item.flags() & ~Qt.ItemFlag.ItemIsEditable
            )
            value_item.setToolTip(tooltip)

    @staticmethod
    def set_mask_checks(checks, mask):
        for i, chk in enumerate(checks):
            chk.blockSignals(True)
            chk.setChecked(bool((mask >> i) & 1))
            chk.blockSignals(False)

    @staticmethod
    def trigger_source_from_values(ext_trigger, self_trigger, random_mode):
        if random_mode:
            return ConfigTab.TRIGGER_SOURCE_RANDOM
        if self_trigger and not ext_trigger:
            return ConfigTab.TRIGGER_SOURCE_SELF
        if ext_trigger and not self_trigger:
            return ConfigTab.TRIGGER_SOURCE_EXTERNAL
        return None

    def sync_trigger_source_controls(
        self, ext_trigger, self_trigger, random_mode, random_rate
    ):
        """Reflect source/rate values from the loaded table in the controls."""

        source = self.trigger_source_from_values(
            ext_trigger, self_trigger, random_mode
        )
        if source is not None:
            self.combo_trigger_source.blockSignals(True)
            self.combo_trigger_source.setCurrentText(source)
            self.combo_trigger_source.blockSignals(False)
        self.spin_random_rate.blockSignals(True)
        try:
            if math.isfinite(random_rate) and random_rate > 0:
                self.spin_random_rate.setValue(random_rate)
            else:
                self.spin_random_rate.setValue(1.0)
        finally:
            self.spin_random_rate.blockSignals(False)
        self.spin_random_rate.setEnabled(source == self.TRIGGER_SOURCE_RANDOM)

    def current_trigger_source_values(self):
        source = self.combo_trigger_source.currentText()
        if source == self.TRIGGER_SOURCE_RANDOM:
            return 0, 0, 1, self.spin_random_rate.value()
        if source == self.TRIGGER_SOURCE_EXTERNAL:
            return 1, 0, 0, 0.0
        return 0, 1, 0, 0.0

    def on_trigger_source_changed(self, source):
        self.spin_random_rate.setEnabled(source == self.TRIGGER_SOURCE_RANDOM)
        if self.table.rowCount() == 0:
            return
        ext_trigger, self_trigger, random_mode, random_rate = (
            self.current_trigger_source_values()
        )
        self.set_table_value("Digitizer", "ExtTriggerMode", str(ext_trigger))
        self.set_table_value("Digitizer", "SelfTriggerMode", str(self_trigger))
        self.set_table_value(
            "Digitizer", "SoftwareRandomTriggerMode", str(random_mode)
        )
        self.set_table_value(
            "Digitizer", "SoftwareRandomTriggerRateHz",
            self.format_random_rate(random_rate),
        )
        if self_trigger:
            selected_mask = sum(
                (1 << i) for i, chk in enumerate(self.trigger_ch_checks)
                if chk.isChecked()
            )
            if selected_mask == 0:
                readout_mask = sum(
                    (1 << i) for i, chk in enumerate(self.ch_checks)
                    if chk.isChecked()
                ) or 1
                self.set_mask_checks(self.trigger_ch_checks, readout_mask)
                self.set_table_value(
                    "Digitizer", "SelfTriggerMask", str(readout_mask)
                )
        else:
            self.set_mask_checks(self.trigger_ch_checks, 0)
            self.set_table_value("Digitizer", "SelfTriggerMask", "0")
        self.trigger_controls_load_error = None
        self.update_trigger_mask_calc()

    def on_random_rate_changed(self, value):
        if self.combo_trigger_source.currentText() != self.TRIGGER_SOURCE_RANDOM:
            return
        if self.table.rowCount() > 0:
            self.set_table_value(
                "Digitizer", "SoftwareRandomTriggerRateHz",
                self.format_random_rate(value),
            )
            self.trigger_controls_load_error = None
            self.update_trigger_mask_calc()

    @staticmethod
    def format_random_rate(value):
        return f"{float(value):.6f}".rstrip("0").rstrip(".")

    def update_mask_calc(self):
        mask = sum((1 << i) for i, chk in enumerate(self.ch_checks) if chk.isChecked())
        self.lbl_mask_res.setText(str(mask))
        if hasattr(self, "trigger_ch_checks"):
            self.update_trigger_mask_calc()

    def on_readout_control_changed(self, *_):
        self.trigger_controls_load_error = None
        self.update_mask_calc()

    def on_trigger_control_changed(self, *_):
        self.trigger_controls_load_error = None
        self.update_trigger_mask_calc()

    def on_table_cell_changed(self, row, _column):
        self._set_config_dirty(True)
        section_item = self.table.item(row, 0)
        parameter_item = self.table.item(row, 1)
        if not section_item or not parameter_item:
            return
        if (section_item.text(), parameter_item.text()) in {
            ("Digitizer", "ExtTriggerMode"),
            ("Digitizer", "SelfTriggerMode"),
            ("Digitizer", "SoftwareRandomTriggerMode"),
            ("Digitizer", "SoftwareRandomTriggerRateHz"),
        }:
            self.trigger_controls_load_error = None
            try:
                self.sync_trigger_source_controls(
                    self.table_int_value("Digitizer", "ExtTriggerMode"),
                    self.table_int_value("Digitizer", "SelfTriggerMode"),
                    self.optional_table_int(
                        "Digitizer", "SoftwareRandomTriggerMode"
                    ) or 0,
                    float(self.optional_table_value(
                        "Digitizer", "SoftwareRandomTriggerRateHz"
                    ) or 0.0),
                )
            except (ValueError, TypeError):
                pass
            self.update_trigger_mask_calc()

    def update_trigger_mask_calc(self):
        if not hasattr(self, "trigger_ch_checks"):
            return

        readout_mask = sum(
            (1 << i) for i, chk in enumerate(self.ch_checks) if chk.isChecked()
        )
        trigger_mask = sum(
            (1 << i) for i, chk in enumerate(self.trigger_ch_checks) if chk.isChecked()
        )
        self.lbl_trigger_mask_res.setText(str(trigger_mask))

        logic = self.combo_pair_logic.currentText().upper()
        validation_error = None
        if self.table.rowCount() > 0:
            try:
                table_channel_mask = self.table_int_value(
                    "Digitizer", "ChannelMask"
                )
                if readout_mask != table_channel_mask:
                    raise ValueError(
                        "먼저 Apply Readout Mask를 눌러 readout 채널 변경을 "
                        "적용하세요."
                    )
                ext_trigger = self.table_int_value("Digitizer", "ExtTriggerMode")
                self_trigger = self.table_int_value("Digitizer", "SelfTriggerMode")
                random_mode = self.optional_table_int(
                    "Digitizer", "SoftwareRandomTriggerMode"
                ) or 0
                random_rate = float(self.optional_table_value(
                    "Digitizer", "SoftwareRandomTriggerRateHz"
                ) or 0.0)
                self.validate_trigger_values(
                    readout_mask, trigger_mask, logic, ext_trigger,
                    self_trigger, random_mode, random_rate
                )
            except ValueError as exc:
                validation_error = str(exc)

        display_error = self.trigger_controls_load_error or validation_error
        if display_error:
            hint = f"오류: {display_error}"
            color = "#dc3545"
        elif self.combo_trigger_source.currentText() == self.TRIGGER_SOURCE_RANDOM:
            hint = (
                "Software Random: 하드웨어 self/external trigger는 끄고, "
                "입력한 Hz를 평균값으로 하는 Poisson 간격으로 소프트웨어 트리거를 생성합니다."
            )
            color = "#0d6efd"
        elif trigger_mask == 0:
            hint = (
                "외부 트리거 전용: SelfTriggerMode=0, ExtTriggerMode=1을 "
                "사용하세요."
            )
            color = "#6c757d"
        elif logic == "AND":
            hint = (
                "AND는 각 인접 pair의 threshold comparator 출력이 실제로 "
                "겹칠 때 성립합니다. 여러 pair의 결과는 서로 OR로 결합됩니다."
            )
            color = "#0d6efd"
        else:
            hint = "OR에서는 선택한 self-trigger 채널 중 하나만 임계값을 넘어도 트리거됩니다."
            color = "#0d6efd"

        self.lbl_trigger_hint.setText(hint)
        self.lbl_trigger_hint.setStyleSheet(f"color: {color};")
        self.btn_apply_trigger.setEnabled(
            self.table.rowCount() > 0 and validation_error is None
        )

    def apply_mask_to_table(self):
        if self.table.rowCount() == 0: return
        if self.lbl_mask_res.text() == "0":
            QMessageBox.warning(self, "Invalid Readout Mask", "Readout mask는 0일 수 없습니다.")
            return
        self.set_table_value("Digitizer", "ChannelMask", self.lbl_mask_res.text())
        self.update_trigger_mask_calc()

    def apply_trigger_to_table(self):
        if self.table.rowCount() == 0:
            return

        try:
            selected_channel_mask = int(self.lbl_mask_res.text())
            channel_mask = self.table_int_value("Digitizer", "ChannelMask")
            if selected_channel_mask != channel_mask:
                raise ValueError(
                    "먼저 Apply Readout Mask를 눌러 readout 채널 변경을 적용하세요."
                )
            trigger_mask = int(self.lbl_trigger_mask_res.text())
            pair_logic = self.combo_pair_logic.currentText().upper()
            ext_trigger, self_trigger, random_mode, random_rate = (
                self.current_trigger_source_values()
            )
            self.validate_trigger_values(
                channel_mask, trigger_mask, pair_logic, ext_trigger,
                self_trigger, random_mode, random_rate
            )
        except ValueError as exc:
            QMessageBox.warning(self, "Invalid Trigger Configuration", str(exc))
            return

        self.set_table_value("Digitizer", "SelfTriggerMask", str(trigger_mask))
        self.set_table_value("HardwareCoincidence", "PairLogic", pair_logic)
        self.set_table_value("Digitizer", "ExtTriggerMode", str(ext_trigger))
        self.set_table_value("Digitizer", "SelfTriggerMode", str(self_trigger))
        self.set_table_value(
            "Digitizer", "SoftwareRandomTriggerMode", str(random_mode)
        )
        self.set_table_value(
            "Digitizer", "SoftwareRandomTriggerRateHz",
            self.format_random_rate(random_rate),
        )
        self.trigger_controls_load_error = None
        self.update_trigger_mask_calc()

    def sync_time_controls_from_config(self):
        """Load the saved R/P timing into the calculator without editing it."""

        if self.table.rowCount() == 0:
            self.update_time_simulator()
            return
        try:
            record_length = self.table_int_value(
                "Digitizer", "RecordLength"
            )
            post_trigger = self.table_int_value(
                "Digitizer", "PostTrigger"
            )
            truth = predict_post_trigger(
                normalize_record_length(record_length), post_trigger
            )
            inferred_t0 = int(round(
                truth.actual_pre_samples * ADC_SAMPLE_PERIOD_NS
                - TRIGGER_LATENCY_NS
            ))
        except ValueError:
            self.update_time_simulator()
            return

        self.spin_record.blockSignals(True)
        self.spin_target_t0.blockSignals(True)
        try:
            self.spin_record.setValue(record_length)
            self.spin_target_t0.setValue(max(
                self.spin_target_t0.minimum(),
                min(self.spin_target_t0.maximum(), inferred_t0),
            ))
        finally:
            self.spin_target_t0.blockSignals(False)
            self.spin_record.blockSignals(False)
        self.update_time_simulator()

    def optional_table_int(self, section, parameter):
        value = self.optional_table_value(section, parameter)
        if value is None:
            return None
        try:
            return int(value, 10)
        except ValueError:
            return None

    def update_time_simulator(self, *_):
        try:
            plan = derive_time_dsp_plan(
                self.spin_record.value(),
                self.spin_target_t0.value(),
                current_short_gate=self.optional_table_int(
                    "SoftwareDSP", "ShortGate"
                ),
                current_long_gate=self.optional_table_int(
                    "SoftwareDSP", "LongGate"
                ),
            )
        except ValueError as exc:
            self.time_dsp_plan = None
            self.lbl_res_record.setText(f"Invalid: {exc}")
            self.lbl_res_post.clear()
            self.lbl_res_window.clear()
            self.lbl_res_pedestal.clear()
            self.lbl_res_gates.clear()
            self.btn_apply_time.setEnabled(False)
            return

        self.time_dsp_plan = plan
        if plan.record_length_adjusted:
            self.lbl_res_record.setText(
                f"{plan.requested_record_length} requested → "
                f"{plan.record_length} effective (10-sample grid)"
            )
        else:
            self.lbl_res_record.setText(
                f"{plan.record_length} Samples (hardware-native)"
            )
        readback_note = (
            "exact"
            if plan.post_trigger_exact_readback
            else f"readback {plan.post_trigger_readback_percent}%"
        )
        self.lbl_res_post.setText(
            f"{plan.post_trigger_percent}% ({readback_note}, "
            f"register K={plan.post_trigger_register_locations})"
        )
        self.lbl_res_window.setText(
            f"pre={plan.actual_pre_samples} Samples / "
            f"post={plan.actual_post_samples} Samples; "
            f"achieved T0≈{plan.achieved_t0_ns:g} ns"
        )
        self.lbl_res_pedestal.setText(
            f"{plan.baseline_samples} Samples"
        )
        gate_note = "preserved" if plan.preserved_gate_settings else "safe defaults"
        self.lbl_res_gates.setText(
            f"ShortGate={plan.short_gate_samples}, "
            f"LongGate={plan.long_gate_samples} "
            f"(legacy provenance; {gate_note})"
        )
        self.btn_apply_time.setEnabled(self.table.rowCount() > 0)

    def apply_time_to_table(self):
        if self.table.rowCount() == 0:
            return
        self.update_time_simulator()
        plan = getattr(self, "time_dsp_plan", None)
        if plan is None:
            return

        self.spin_record.blockSignals(True)
        try:
            self.spin_record.setValue(plan.record_length)
        finally:
            self.spin_record.blockSignals(False)
        for section, parameter, value in (
            ("Digitizer", "RecordLength", plan.record_length),
            ("Digitizer", "PostTrigger", plan.post_trigger_percent),
            ("SoftwareDSP", "BaselineSamples", plan.baseline_samples),
            ("SoftwareDSP", "ShortGate", plan.short_gate_samples),
            ("SoftwareDSP", "LongGate", plan.long_gate_samples),
        ):
            self.set_table_value(section, parameter, str(value))
        self.update_time_simulator()

    def on_input_range_changed(self, value):
        try:
            input_range_mv = int(value)
        except (TypeError, ValueError):
            input_range_mv = 2000
        minimum_mv = math.ceil(
            ((input_range_mv / (1 << 14)) / 2.0) * 1000.0
        ) / 1000.0
        self.spin_trg_mv.setMinimum(minimum_mv)
        self.spin_trg_mv.setMaximum(float(input_range_mv) - 0.001)
        self.update_adc_simulator()

    def update_adc_simulator(self):
        base_pct = self.spin_base_pct.value() / 100.0
        requested_mv = self.spin_trg_mv.value()
        input_range_mv = int(self.combo_input_range.currentText())
        adc_bits = 14
        adc_codes = 1 << adc_bits

        # The green line is only a plot aid. The DAC has analogue
        # tolerances/over-range, so this preview must never be reused as a
        # measured baseline for an absolute discriminator threshold.
        adc_baseline_preview = int(round(base_pct * (adc_codes - 1)))
        polarity = 1
        raw_polarity = self.optional_table_value("Digitizer", "TriggerPolarity")
        if raw_polarity in {"0", "1"}:
            polarity = int(raw_polarity)
        direction = "falling: measured baseline - delta" if polarity else \
            "rising: measured baseline + delta"
        try:
            preview = calculate_threshold_preview(
                adc_baseline_preview, requested_mv, input_range_mv,
                adc_bits, polarity
            )
        except ValueError as exc:
            self.lbl_res_offset.setText(
                "unchanged; runtime measures every channel after settling"
            )
            self.lbl_res_trg.setText(f"invalid request: {exc}")
            self.line_base.setValue(adc_baseline_preview)
            self.line_trg.setValue(adc_baseline_preview)
            return

        self.lbl_res_offset.setText(
            "unchanged; runtime measures every channel after settling"
        )
        self.lbl_res_trg.setText(
            f"request={requested_mv:.3f} mV, LSB={preview.lsb_mv:.6f} mV, "
            f"delta={preview.delta_adc} ADC; runtime: {direction}"
        )
        self.line_base.setValue(adc_baseline_preview)
        self.line_trg.setValue(preview.absolute_threshold_adc)
        
        # 베이스라인이 바뀔 때 스캔 영역의 경고 여부도 재평가
        if hasattr(self, 'scan_region') and self.scan_region.isVisible():
            r = self.scan_region.getRegion()
            self.update_scan_region(int(r[0]), int(r[1]))

    def apply_adc_to_table(self):
        if self.table.rowCount() == 0:
            return
        try:
            channel_mask = self.table_int_value("Digitizer", "ChannelMask")
            self_trigger_mode = self.table_int_value(
                "Digitizer", "SelfTriggerMode"
            )
            trigger_mask_raw = self.optional_table_value(
                "Digitizer", "SelfTriggerMask"
            )
            trigger_mask = (
                int(trigger_mask_raw, 10)
                if trigger_mask_raw is not None
                else channel_mask if self_trigger_mode else 0
            )
        except ValueError as exc:
            QMessageBox.critical(self, "Invalid Trigger Mask", str(exc))
            return
        if self_trigger_mode == 0 or trigger_mask == 0:
            QMessageBox.warning(
                self, "No Self-Trigger Channels",
                "Self-trigger 채널이 없어 mV threshold를 적용하지 않았습니다."
            )
            return

        requested_mv = self.spin_trg_mv.value()
        input_range_mv = int(self.combo_input_range.currentText())
        try:
            millivolts_to_adc_delta(requested_mv, input_range_mv, 14)
        except ValueError as exc:
            QMessageBox.critical(self, "Invalid mV Threshold", str(exc))
            return
        self.set_table_value("Digitizer", "InputRangeMv", str(input_range_mv))
        self.set_table_value("Digitizer", "ADCBits", "14")
        calibration_defaults = {
            "SettlingTimeMs": "3000",
            "SettlingTimeoutMs": "15000",
            "MeasurementEvents": "32",
            "StabilityToleranceAdc": "2.0",
            "StableMeasurements": "3",
        }
        for key, default_value in calibration_defaults.items():
            if self.optional_table_value("TriggerCalibration", key) is None:
                self.set_table_value("TriggerCalibration", key, default_value)

        value_text = f"{requested_mv:.6f}".rstrip("0").rstrip(".")
        for ch in range(8):
            if (trigger_mask >> ch) & 1:
                self.replace_channel_threshold_with_mv(ch, value_text)

        channels = ", ".join(
            f"CH{ch}" for ch in range(8) if (trigger_mask >> ch) & 1
        )
        QMessageBox.information(
            self, "Runtime Threshold Stored",
            f"{channels}에 TriggerThresholdMv={value_text}를 저장할 준비가 됐습니다.\n"
            "절대 ADC threshold는 DAQ가 채널별 안정 baseline을 측정한 뒤 계산합니다.\n"
            "설정 파일에 반영하려면 Save .conf를 누르세요."
        )

    def replace_channel_threshold_with_mv(self, channel, value):
        section = f"Channel_{channel}"
        matching_rows = []
        preferred_row = None
        for row in range(self.table.rowCount()):
            section_item = self.table.item(row, 0)
            parameter_item = self.table.item(row, 1)
            if not section_item or not parameter_item:
                continue
            if section_item.text() != section:
                continue
            if parameter_item.text() in {"TriggerThreshold", "TriggerThresholdMv"}:
                matching_rows.append(row)
                if parameter_item.text() == "TriggerThresholdMv":
                    preferred_row = row

        if preferred_row is None and matching_rows:
            preferred_row = matching_rows[0]
            parameter_item = QTableWidgetItem("TriggerThresholdMv")
            self.table.setItem(preferred_row, 1, parameter_item)
        if preferred_row is None:
            self.set_table_value(section, "TriggerThresholdMv", value)
            return

        self.table.setItem(preferred_row, 2, QTableWidgetItem(value))
        self.table.item(preferred_row, 2).setBackground(Qt.GlobalColor.yellow)
        self.protect_controlled_row(preferred_row)
        for row in sorted(
            (row for row in matching_rows if row != preferred_row), reverse=True
        ):
            self.table.removeRow(row)

    def set_table_value(self, target_section, target_param, value):
        for row in range(self.table.rowCount()):
            if self.table.item(row, 0).text() == target_section and self.table.item(row, 1).text() == target_param:
                self.table.setItem(row, 2, QTableWidgetItem(value)); self.table.item(row, 2).setBackground(Qt.GlobalColor.yellow)
                self.protect_controlled_row(row)
                return
        row = self.table.rowCount(); self.table.insertRow(row)
        self.table.setItem(row, 0, QTableWidgetItem(target_section)); self.table.setItem(row, 1, QTableWidgetItem(target_param))
        self.table.setItem(row, 2, QTableWidgetItem(value)); self.table.item(row, 2).setBackground(Qt.GlobalColor.yellow)
        self.protect_controlled_row(row)

    def table_value(self, target_section, target_param):
        value = self.optional_table_value(target_section, target_param)
        if value is not None:
            return value
        raise ValueError(f"필수 설정이 없습니다: [{target_section}] {target_param}")

    def optional_table_value(self, target_section, target_param):
        for row in range(self.table.rowCount()):
            section_item = self.table.item(row, 0)
            parameter_item = self.table.item(row, 1)
            value_item = self.table.item(row, 2)
            if not section_item or not parameter_item or not value_item:
                continue
            if (
                section_item.text() == target_section
                and parameter_item.text() == target_param
            ):
                return value_item.text().strip()
        return None

    def table_int_value(self, target_section, target_param):
        raw_value = self.table_value(target_section, target_param)
        try:
            return int(raw_value, 10)
        except ValueError as exc:
            raise ValueError(
                f"정수가 아닌 설정값입니다: [{target_section}] "
                f"{target_param}={raw_value}"
            ) from exc

    @staticmethod
    def validate_trigger_values(
        channel_mask, trigger_mask, pair_logic, ext_trigger, self_trigger,
        random_mode=0, random_rate=0.0
    ):
        if not 1 <= channel_mask <= 0xFF:
            raise ValueError("[Digitizer] ChannelMask는 1..255여야 합니다.")
        if not 0 <= trigger_mask <= 0xFF:
            raise ValueError("[Digitizer] SelfTriggerMask는 0..255여야 합니다.")
        if ext_trigger not in (0, 1) or self_trigger not in (0, 1):
            raise ValueError("ExtTriggerMode와 SelfTriggerMode는 0 또는 1이어야 합니다.")
        if random_mode not in (0, 1):
            raise ValueError(
                "SoftwareRandomTriggerMode는 0 또는 1이어야 합니다."
            )
        if not isinstance(random_rate, (int, float)) or not math.isfinite(random_rate):
            raise ValueError(
                "SoftwareRandomTriggerRateHz는 유한한 실수여야 합니다."
            )
        if random_mode:
            if ext_trigger or self_trigger:
                raise ValueError(
                    "Software Random은 External/Self trigger와 함께 사용할 수 없습니다."
                )
            if trigger_mask != 0:
                raise ValueError(
                    "Software Random에서는 SelfTriggerMask를 0으로 설정해야 합니다."
                )
            if not (
                MIN_SOFTWARE_RANDOM_TRIGGER_RATE_HZ
                <= random_rate
                <= MAX_SOFTWARE_RANDOM_TRIGGER_RATE_HZ
            ):
                raise ValueError(
                    "SoftwareRandomTriggerRateHz는 "
                    f"{MIN_SOFTWARE_RANDOM_TRIGGER_RATE_HZ:g}.."
                    f"{MAX_SOFTWARE_RANDOM_TRIGGER_RATE_HZ:g} Hz 범위여야 합니다."
                )
        else:
            if ext_trigger == 0 and self_trigger == 0:
                raise ValueError(
                    "외부 트리거와 자체 트리거를 동시에 끌 수 없습니다."
                )
            if random_rate != 0:
                raise ValueError(
                    "SoftwareRandomTriggerMode=0이면 SoftwareRandomTriggerRateHz는 0이어야 합니다."
                )
        if trigger_mask & ~channel_mask:
            raise ValueError("SelfTriggerMask는 ChannelMask의 부분집합이어야 합니다.")
        if pair_logic not in ("AND", "OR"):
            raise ValueError("[HardwareCoincidence] PairLogic은 AND 또는 OR여야 합니다.")

        if self_trigger:
            if trigger_mask == 0:
                raise ValueError(
                    "SelfTriggerMode=1이면 SelfTriggerMask에 채널을 하나 이상 선택해야 합니다."
                )
        else:
            if trigger_mask != 0:
                raise ValueError("SelfTriggerMode=0이면 SelfTriggerMask는 0이어야 합니다.")

        if pair_logic == "AND":
            incomplete_pairs = [
                f"CH{pair_start}/{pair_start + 1}"
                for pair_start in range(0, 8, 2)
                if ((trigger_mask >> pair_start) & 0x3) not in (0, 0x3)
            ]
            if incomplete_pairs:
                raise ValueError(
                    "AND는 완전한 인접 pair만 선택할 수 있습니다: "
                    + ", ".join(incomplete_pairs)
                )

    def validate_trigger_table(self):
        seen_keys = set()
        for row in range(self.table.rowCount()):
            items = [self.table.item(row, column) for column in range(3)]
            if any(item is None or not item.text().strip() for item in items):
                raise ValueError(f"비어 있는 설정 항목이 있습니다 (row {row + 1}).")
            key = (items[0].text().strip(), items[1].text().strip())
            if key in seen_keys:
                raise ValueError(
                    f"중복 설정 항목입니다: [{key[0]}] {key[1]}"
                )
            seen_keys.add(key)

        trigger_keys = (
            ("Digitizer", "SelfTriggerMask"),
            ("HardwareCoincidence", "PairLogic"),
        )
        present_values = [
            self.optional_table_value(section, parameter)
            for section, parameter in trigger_keys
        ]
        trigger_key_count = sum(value is not None for value in present_values)
        if trigger_key_count not in (0, len(trigger_keys)):
            raise ValueError(
                "[Digitizer] SelfTriggerMask와 [HardwareCoincidence] "
                "PairLogic은 두 항목을 모두 설정하거나 모두 생략해야 합니다."
            )

        channel_mask = self.table_int_value("Digitizer", "ChannelMask")
        ext_trigger = self.table_int_value("Digitizer", "ExtTriggerMode")
        self_trigger = self.table_int_value("Digitizer", "SelfTriggerMode")
        random_mode_raw = self.optional_table_value(
            "Digitizer", "SoftwareRandomTriggerMode"
        )
        random_mode = int(random_mode_raw, 10) if random_mode_raw is not None else 0
        random_rate_raw = self.optional_table_value(
            "Digitizer", "SoftwareRandomTriggerRateHz"
        )
        try:
            random_rate = float(random_rate_raw) if random_rate_raw is not None else 0.0
        except ValueError as exc:
            raise ValueError(
                "실수가 아닌 설정값입니다: [Digitizer] SoftwareRandomTriggerRateHz="
                f"{random_rate_raw}"
            ) from exc
        if trigger_key_count == 0:
            trigger_mask = channel_mask if self_trigger else 0
            pair_logic = "OR"
        else:
            trigger_mask = self.table_int_value("Digitizer", "SelfTriggerMask")
            pair_logic = self.table_value(
                "HardwareCoincidence", "PairLogic"
            )

        self.validate_trigger_values(
            channel_mask, trigger_mask, pair_logic, ext_trigger,
            self_trigger, random_mode, random_rate
        )

        uses_mv_threshold = False
        for ch in range(8):
            if not ((channel_mask >> ch) & 1):
                continue
            section = f"Channel_{ch}"
            raw_offset = self.table_value(section, "DCOffset")
            try:
                offset = int(raw_offset, 10)
            except ValueError as exc:
                raise ValueError(
                    f"정수가 아닌 설정값입니다: [{section}] DCOffset={raw_offset}"
                ) from exc
            if not 0 <= offset <= 65535:
                raise ValueError(
                    f"설정값 범위 오류: [{section}] DCOffset={offset} (허용 0..65535)"
                )

            raw_absolute = self.optional_table_value(
                section, "TriggerThreshold"
            )
            raw_mv = self.optional_table_value(section, "TriggerThresholdMv")
            participates_in_trigger = bool(
                self_trigger and ((trigger_mask >> ch) & 1)
            )
            if raw_absolute is not None and raw_mv is not None:
                raise ValueError(
                    f"[{section}] TriggerThreshold(legacy)와 TriggerThresholdMv 중 "
                    "하나만 설정해야 합니다."
                )
            if participates_in_trigger and raw_absolute is None and raw_mv is None:
                raise ValueError(
                    f"[{section}] self-trigger 채널에는 TriggerThreshold 또는 "
                    "TriggerThresholdMv가 필요합니다."
                )
            if raw_absolute is None and raw_mv is None:
                continue
            if raw_mv is not None:
                uses_mv_threshold = (
                    uses_mv_threshold or participates_in_trigger
                )
                try:
                    requested_mv = float(raw_mv)
                except ValueError as exc:
                    raise ValueError(
                        f"실수가 아닌 설정값입니다: [{section}] "
                        f"TriggerThresholdMv={raw_mv}"
                    ) from exc
                if not math.isfinite(requested_mv) or requested_mv <= 0:
                    raise ValueError(
                        f"[{section}] TriggerThresholdMv는 유한한 양수여야 합니다."
                    )
            else:
                try:
                    absolute = int(raw_absolute, 10)
                except ValueError as exc:
                    raise ValueError(
                        f"정수가 아닌 설정값입니다: [{section}] "
                        f"TriggerThreshold={raw_absolute}"
                    ) from exc
                if not 0 <= absolute <= 16383:
                    raise ValueError(
                        f"[{section}] TriggerThreshold={absolute} (허용 0..16383)"
                    )

        if uses_mv_threshold:
            input_range_raw = self.table_value("Digitizer", "InputRangeMv")
            adc_bits_raw = self.table_value("Digitizer", "ADCBits")
            try:
                input_range_mv = int(input_range_raw, 10)
                adc_bits = int(adc_bits_raw, 10)
            except ValueError as exc:
                raise ValueError("InputRangeMv와 ADCBits는 정수여야 합니다.") from exc
            if input_range_mv not in (500, 2000):
                raise ValueError("[Digitizer] InputRangeMv는 500 또는 2000이어야 합니다.")
            if adc_bits != 14:
                raise ValueError("[Digitizer] ADCBits는 DT5730S의 14여야 합니다.")

            for ch in range(8):
                raw_mv = self.optional_table_value(
                    f"Channel_{ch}", "TriggerThresholdMv"
                )
                if raw_mv is not None:
                    try:
                        millivolts_to_adc_delta(
                            float(raw_mv), input_range_mv, adc_bits
                        )
                    except ValueError as exc:
                        raise ValueError(
                            f"[Channel_{ch}] TriggerThresholdMv={raw_mv}: {exc}"
                        ) from exc

            calibration_values = {
                key: self.table_value("TriggerCalibration", key)
                for key in (
                    "SettlingTimeMs", "SettlingTimeoutMs", "MeasurementEvents",
                    "StabilityToleranceAdc", "StableMeasurements",
                )
            }
            try:
                settling_ms = int(calibration_values["SettlingTimeMs"], 10)
                timeout_ms = int(calibration_values["SettlingTimeoutMs"], 10)
                measurement_events = int(
                    calibration_values["MeasurementEvents"], 10
                )
                tolerance_adc = float(
                    calibration_values["StabilityToleranceAdc"]
                )
                stable_measurements = int(
                    calibration_values["StableMeasurements"], 10
                )
            except ValueError as exc:
                raise ValueError("TriggerCalibration 설정 형식이 잘못됐습니다.") from exc
            if settling_ms < 0 or timeout_ms <= settling_ms:
                raise ValueError(
                    "SettlingTimeoutMs는 SettlingTimeMs보다 커야 합니다."
                )
            if not 1 <= measurement_events <= 10000:
                raise ValueError(
                    "MeasurementEvents는 1..10000이어야 합니다."
                )
            if not 2 <= stable_measurements <= 100:
                raise ValueError("StableMeasurements는 2..100이어야 합니다.")
            if not math.isfinite(tolerance_adc) or tolerance_adc <= 0:
                raise ValueError("StabilityToleranceAdc는 유한한 양수여야 합니다.")

    def save_config(self):
        if not self.current_config_path: return
        try:
            self.validate_trigger_table()
        except ValueError as exc:
            QMessageBox.critical(
                self, "Invalid Configuration",
                f"설정 파일을 저장하지 않았습니다.\n\n{exc}"
            )
            return

        self.config.clear()
        for row in range(self.table.rowCount()):
            sec = self.table.item(row, 0).text(); key = self.table.item(row, 1).text(); val = self.table.item(row, 2).text()
            if not self.config.has_section(sec): self.config.add_section(sec)
            self.config.set(sec, key, val)

        serialized = io.StringIO()
        self.config.write(serialized)
        payload = serialized.getvalue().encode("utf-8")
        save_file = QSaveFile(self.current_config_path)
        # Never permit QSaveFile to fall back to truncating the destination in
        # place.  A failed write/commit must leave the last valid config intact.
        save_file.setDirectWriteFallback(False)
        try:
            if not save_file.open(QIODevice.OpenModeFlag.WriteOnly):
                raise OSError(
                    save_file.errorString() or "cannot open atomic save file"
                )
            written = save_file.write(payload)
            if written != len(payload):
                raise OSError(
                    save_file.errorString()
                    or f"short write ({written}/{len(payload)} bytes)"
                )
            if not save_file.commit():
                raise OSError(
                    save_file.errorString() or "atomic commit failed"
                )
        except (OSError, RuntimeError) as exc:
            if save_file.isOpen():
                save_file.cancelWriting()
            QMessageBox.critical(
                self, "Save Failed",
                "설정 파일을 저장하지 못했습니다. 변경사항은 저장되지 않은 "
                f"상태로 유지됩니다.\n\n{exc}"
            )
            self._set_config_dirty(True)
            return
        for row in range(self.table.rowCount()):
            self.table.item(row, 2).setBackground(Qt.GlobalColor.white)
        self._set_config_dirty(False)
        self.configPathChanged.emit(os.path.abspath(self.current_config_path))
