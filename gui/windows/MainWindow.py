from PyQt6.QtWidgets import QMainWindow, QTabWidget, QStatusBar, QLabel, QWidget, QHBoxLayout
from PyQt6.QtCore import QProcess, QTimer, pyqtSlot
from widgets.DaqTab import DaqTab
from widgets.ConfigTab import ConfigTab
from widgets.MonitorTab import MonitorTab
from widgets.ProductionTab import ProductionTab
from widgets.RootValidationTab import RootValidationTab
from widgets.DatabaseTab import DatabaseTab
from widgets.EnvTab import EnvTab
from core.monitor_stream import RuntimeConfigReference

class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self._close_pending = False
        self._shutdown_ready = False
        self._close_check_scheduled = False
        self._monitor_cleaned = False
        self.setWindowTitle("HEP 3-Tier DAQ Control Center (DT5730S 14-bit) - PyQt6")
        self.resize(1200, 900)

        self.tabs = QTabWidget()
        self.setCentralWidget(self.tabs)

        self.env_tab = EnvTab()
        self.daq_tab = DaqTab(env_data_provider=self.env_tab.get_env_data)
        self.config_tab = ConfigTab()
        self.monitor_tab = MonitorTab(
            config_path_provider=self._monitor_runtime_config_path
        )
        self.production_tab = ProductionTab()
        self.root_validation_tab = RootValidationTab()
        self.database_tab = DatabaseTab()

        self.tabs.addTab(self.daq_tab, "🚀 DAQ Control")
        self.tabs.addTab(self.env_tab, "🌡️ Environment & Meta")
        self.tabs.addTab(self.config_tab, "⚙️ Hardware Config")
        self.tabs.addTab(self.monitor_tab, "📈 Live Monitor")
        self.tabs.addTab(self.production_tab, "🔬 Offline Production")
        self.tabs.addTab(self.root_validation_tab, "✅ ROOT Validation")
        self.tabs.addTab(self.database_tab, "🗄️ Run DB History")

        self.init_statusbar()
        
        self.daq_tab.hardware_led_signal.connect(self.update_led_dashboard)
        self.daq_tab.hardware_temp_signal.connect(self.monitor_tab.update_temperature)
        self.daq_tab.daq_finished_signal.connect(self.reset_led_dashboard)
        self.daq_tab.runContextReady.connect(self.production_tab.set_run_context)
        self.production_tab.rootOutputReady.connect(
            self.root_validation_tab.set_root_file
        )
        self.daq_tab.daq_finished_signal.connect(self._maybe_finish_close)
        self.production_tab.process.finished.connect(self._maybe_finish_close)
        self.production_tab.process.errorOccurred.connect(
            self._maybe_finish_close
        )
        self.root_validation_tab.process.finished.connect(
            self._maybe_finish_close
        )
        self.root_validation_tab.process.errorOccurred.connect(
            self._maybe_finish_close
        )
        self.config_tab.configPathChanged.connect(self.daq_tab.set_config_path)
        self.config_tab.configDirtyChanged.connect(
            self.daq_tab.set_config_dirty
        )
        if self.config_tab.current_config_path:
            self.daq_tab.set_config_path(self.config_tab.current_config_path)
        self.daq_tab.set_config_dirty(
            self.config_tab.current_config_path,
            self.config_tab.is_dirty(),
        )

        # =========================================================================
        # [신규 추가] DAQ Control 탭과 Hardware Config 탭의 스캔 범위 시각화 파이프라인
        # =========================================================================
        self.daq_tab.scanRangeChanged.connect(self.config_tab.update_scan_region)
        self.daq_tab.scanModeToggled.connect(self.config_tab.toggle_scan_region_visibility)
        
        # 초기화 시 현재 스핀박스 값으로 1회 동기화 수행
        self.daq_tab.emit_scan_range()
        # =========================================================================

    def _monitor_runtime_config_path(self):
        """Return the exact run snapshot while DAQ is active, if available."""

        process = getattr(self.daq_tab, "daq_process", None)
        process_active = bool(
            process
            and (
                process.isRunning()
                or (
                    hasattr(process, "has_pending_work")
                    and process.has_pending_work()
                )
            )
        )
        if process_active:
            context = getattr(self.daq_tab, "current_run_context", None) or {}
            # Do not silently fall back to a mutable UI selection during an
            # active acquisition. The monitor will pause spectrum DSP if the
            # immutable run snapshot cannot be identified.
            config_path = context.get("config_path")
            config_sha256 = context.get("config_sha256")
            if not config_path or not config_sha256:
                return None
            return RuntimeConfigReference(config_path, config_sha256)
        return self.config_tab.current_config_path or None

    def init_statusbar(self):
        self.statusBar = QStatusBar()
        self.setStatusBar(self.statusBar)

        self.led_widgets = {}
        led_names = [
            'PLL LOCK', 'CLK EXT', 'BOARD READY', 'RUN', 'TRG', 'DRDY', 'FULL'
        ]
        
        container = QWidget()
        layout = QHBoxLayout(container)
        layout.setContentsMargins(0, 0, 10, 0)

        for name in led_names:
            lbl_title = QLabel(f"<b>{name}</b>")
            lbl_led = QLabel("●")
            lbl_led.setStyleSheet("color: #555555; font-size: 18px; margin-right: 15px;")
            
            layout.addWidget(lbl_title)
            layout.addWidget(lbl_led)
            self.led_widgets[name] = lbl_led

        self.statusBar.addPermanentWidget(container)

    @pyqtSlot(dict)
    def update_led_dashboard(self, status: dict):
        color_map = {
            'PLL LOCK': "#198754" if status.get('PLL LOCK', 0) else "#555555",
            'CLK EXT':  "#ffc107" if status.get('CLK EXT', 0) else "#555555",
            'BOARD READY': "#198754" if status.get('BOARD READY', 0) else "#dc3545",
            'RUN':      "#198754" if status.get('RUN', 0)      else "#555555",
            'TRG':      "#198754" if status.get('TRG', 0)      else "#555555",
            'DRDY':     "#0dcaf0" if status.get('DRDY', 0)     else "#555555",
            'FULL':     "#dc3545" if status.get('FULL', 0)     else "#555555"
        }

        for key, color in color_map.items():
            if key in self.led_widgets:
                self.led_widgets[key].setStyleSheet(
                    f"color: {color}; font-size: 18px; margin-right: 15px;"
                )

    @pyqtSlot(int)
    def reset_led_dashboard(self, returncode):
        for key in self.led_widgets:
            self.led_widgets[key].setStyleSheet("color: #555555; font-size: 18px; margin-right: 15px;")

    def _workers_active(self):
        daq_process = self.daq_tab.daq_process
        daq_active = bool(
            daq_process
            and (
                daq_process.isRunning()
                or (
                    hasattr(daq_process, "has_pending_work")
                    and daq_process.has_pending_work()
                )
            )
        )
        production_active = (
            self.production_tab.process.state()
            != QProcess.ProcessState.NotRunning
        )
        validation_active = self.root_validation_tab.has_pending_work()
        return daq_active or production_active or validation_active

    def _cleanup_monitor_once(self):
        if not self._monitor_cleaned:
            self._monitor_cleaned = True
            self.monitor_tab.cleanup()

    def _schedule_close_check(self):
        if self._close_check_scheduled:
            return
        self._close_check_scheduled = True
        QTimer.singleShot(50, self._maybe_finish_close)

    @pyqtSlot()
    def _maybe_finish_close(self, *_args):
        self._close_check_scheduled = False
        if not self._close_pending:
            return
        if self._workers_active():
            self._schedule_close_check()
            return
        self._shutdown_ready = True
        self.close()

    def closeEvent(self, event):
        if self._shutdown_ready or not self._workers_active():
            self._cleanup_monitor_once()
            event.accept()
            return

        event.ignore()
        if self._close_pending:
            self._schedule_close_check()
            return

        self._close_pending = True
        self.tabs.setEnabled(False)
        self.statusBar.showMessage(
            "Stopping active jobs; unresponsive children will be force-stopped "
            "after their bounded provenance-finalization grace period…"
        )
        self.daq_tab.stop_all(auto_force=True)
        self.production_tab.stop_all(auto_force=True)
        self.root_validation_tab.stop_all(wait=False)
        self._schedule_close_check()
