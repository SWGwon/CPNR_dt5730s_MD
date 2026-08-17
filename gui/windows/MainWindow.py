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
        
        # [핵심 변경] 재생성되는 ProcessManager에 의존하지 않고, DaqTab 자체와 영구적으로 시그널 연동
        self.daq_tab.hardware_led_signal.connect(self.update_led_dashboard)
        self.daq_tab.hardware_temp_signal.connect(self.monitor_tab.update_temperature)
        self.daq_tab.daq_finished_signal.connect(self.reset_led_dashboard)

    def init_statusbar(self):
        self.statusBar = QStatusBar()
        self.setStatusBar(self.statusBar)

        self.led_widgets = {}
        # 6개 장비 전면 패널 상태 인디케이터
        led_names = ['PLL LOCK', 'PLL BYPS', 'RUN', 'TRG', 'DRDY', 'BUSY']
        
        container = QWidget()
        layout = QHBoxLayout(container)
        layout.setContentsMargins(0, 0, 10, 0)

        for name in led_names:
            lbl_title = QLabel(f"<b>{name}</b>")
            lbl_led = QLabel("●")
            # 기본 비활성화 색상 (회색)
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
        """DAQ 정지 시 오해를 방지하기 위해 모든 LED를 즉시 회색(Off)으로 초기화합니다."""
        for key in self.led_widgets:
            self.led_widgets[key].setStyleSheet("color: #555555; font-size: 18px; margin-right: 15px;")

    def closeEvent(self, event):
        self.daq_tab.stop_all()
        self.monitor_tab.cleanup()
        self.production_tab.stop_all()
        event.accept()