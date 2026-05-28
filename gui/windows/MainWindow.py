from PyQt5.QtWidgets import QMainWindow, QTabWidget
from widgets.DaqTab import DaqTab
from widgets.ConfigTab import ConfigTab
from widgets.MonitorTab import MonitorTab
from widgets.ProductionTab import ProductionTab
from widgets.DatabaseTab import DatabaseTab
from widgets.EnvTab import EnvTab  # 신규 추가

class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("HEP 3-Tier DAQ Control Center (DT5730S 14-bit)")
        self.resize(1200, 900)

        self.tabs = QTabWidget()
        self.setCentralWidget(self.tabs)

        self.env_tab = EnvTab()
        # DaqTab 구동 시점에 동적 메타데이터를 획득할 수 있도록 콜백 주입
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

    def closeEvent(self, event):
        self.daq_tab.stop_all()
        self.monitor_tab.cleanup()
        self.production_tab.stop_all()
        event.accept()
