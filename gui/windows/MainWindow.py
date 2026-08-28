from PyQt6.QtWidgets import QMainWindow, QTabWidget, QStatusBar, QLabel, QWidget, QHBoxLayout
from PyQt6.QtCore import pyqtSlot
from widgets.DaqTab import DaqTab
from widgets.ConfigTab import ConfigTab
from widgets.MonitorTab import MonitorTab
from widgets.ProductionTab import ProductionTab
from widgets.DatabaseTab import DatabaseTab
from widgets.EnvTab import EnvTab

class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("HEP 3-Tier DAQ Control Center (DT5730S 14-bit) - PyQt6")
        self.resize(1200, 900)

        self.tabs = QTabWidget()
        self.setCentralWidget(self.tabs)

        self.env_tab = EnvTab()
        self.daq_tab = DaqTab(env_data_provider=self.env_tab.get_env_data)
        self.config_tab = ConfigTab()
        self.monitor_tab = MonitorTab()
        self.production_tab = ProductionTab()
        self.database_tab = DatabaseTab()

        self.tabs.addTab(self.daq_tab, "🚀 DAQ Control")
        self.tabs.addTab(self.env_tab, "🌡️ Environment & Meta")
        self.tabs.addTab(self.config_tab, "⚙️ Hardware Config")
        self.tabs.addTab(self.monitor_tab, "📈 Live Monitor")
        self.tabs.addTab(self.production_tab, "🔬 Offline Production")
        self.tabs.addTab(self.database_tab, "🗄️ Run DB History")

        self.init_statusbar()
        
        self.daq_tab.hardware_led_signal.connect(self.update_led_dashboard)
        self.daq_tab.hardware_temp_signal.connect(self.monitor_tab.update_temperature)
        self.daq_tab.daq_finished_signal.connect(self.reset_led_dashboard)
        self.daq_tab.runContextReady.connect(self.production_tab.set_run_context)
        self.config_tab.configPathChanged.connect(self.daq_tab.set_config_path)
        if self.config_tab.current_config_path:
            self.daq_tab.set_config_path(self.config_tab.current_config_path)

        # =========================================================================
        # [신규 추가] DAQ Control 탭과 Hardware Config 탭의 스캔 범위 시각화 파이프라인
        # =========================================================================
        self.daq_tab.scanRangeChanged.connect(self.config_tab.update_scan_region)
        self.daq_tab.scanModeToggled.connect(self.config_tab.toggle_scan_region_visibility)
        
        # 초기화 시 현재 스핀박스 값으로 1회 동기화 수행
        self.daq_tab.emit_scan_range()
        # =========================================================================

    def init_statusbar(self):
        self.statusBar = QStatusBar()
        self.setStatusBar(self.statusBar)

        self.led_widgets = {}
        led_names = ['PLL LOCK', 'PLL BYPS', 'RUN', 'TRG', 'DRDY', 'BUSY']
        
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
            'PLL BYPS': "#ffc107" if status.get('PLL BYPS', 0) else "#555555", 
            'RUN':      "#198754" if status.get('RUN', 0)      else "#555555",
            'TRG':      "#198754" if status.get('TRG', 0)      else "#555555",
            'DRDY':     "#0dcaf0" if status.get('DRDY', 0)     else "#555555",
            'BUSY':     "#dc3545" if status.get('BUSY', 0)     else "#555555"
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

    def closeEvent(self, event):
        self.daq_tab.stop_all()
        self.monitor_tab.cleanup()
        self.production_tab.stop_all()
        event.accept()
