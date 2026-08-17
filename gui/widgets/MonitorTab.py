import struct
import numpy as np
import pyqtgraph as pg
import zmq
from PyQt6.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout, QComboBox, QLabel, QPushButton, QSpinBox, QProgressBar, QMessageBox
from PyQt6.QtCore import QTimer, pyqtSlot
from collections import deque

HEADER_FORMAT = "=QIIHHI"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)

class MonitorTab(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.current_mask = -1  
        self.curves_wave = {}   
        self.curves_qlong = {}  
        self.q_long_hists = {}  
        
        self.colors = [
            '#0d6efd', '#198754', '#dc3545', '#fd7e14', 
            '#6f42c1', '#0dcaf0', '#d63384', '#6c757d'
        ]
        
        self.warning_latched = False 
        
        self.setup_zmq()
        self.setup_ui()
        self.timer = QTimer()
        self.timer.timeout.connect(self.poll_zmq)
        self.timer.start(33) 

    def setup_zmq(self):
        self.ctx = zmq.Context()
        self.sock = self.ctx.socket(zmq.SUB)
        self.sock.setsockopt(zmq.RCVHWM, 2000) 
        self.sock.connect("tcp://127.0.0.1:5555")
        self.sock.setsockopt_string(zmq.SUBSCRIBE, "")

    def setup_ui(self):
        layout = QVBoxLayout(self)
        
        ctrl_layout = QHBoxLayout()
        ctrl_layout.addWidget(QLabel("<b>Display Engine:</b>"))
        
        self.cb_monitor = QComboBox()
        self.cb_monitor.addItems(["🟢 Live Monitor: ON (Auto Multi-Channel)", "🔴 Live Monitor: OFF (Save CPU)"])
        self.cb_monitor.currentIndexChanged.connect(self.toggle_monitor)
        ctrl_layout.addWidget(self.cb_monitor)

        ctrl_layout.addWidget(QLabel("  |  <b>Spectrum History:</b>"))
        self.spin_history = QSpinBox()
        self.spin_history.setRange(100, 100000)
        self.spin_history.setSingleStep(500)
        self.spin_history.setValue(2000)
        self.spin_history.setSuffix(" Evts")
        self.spin_history.valueChanged.connect(self.update_history_size)
        ctrl_layout.addWidget(self.spin_history)

        self.btn_clear = QPushButton("🗑️ Clear All Histograms")
        self.btn_clear.setStyleSheet("font-weight: bold; padding: 4px 15px; margin-left: 10px;")
        self.btn_clear.clicked.connect(self.clear_data)
        ctrl_layout.addWidget(self.btn_clear)
        
        ctrl_layout.addWidget(QLabel("  |  <b>ADC Temp:</b>"))
        self.temp_bar = QProgressBar()
        self.temp_bar.setRange(0, 100)
        self.temp_bar.setFormat("%v °C")
        self.temp_bar.setFixedWidth(100)
        self.temp_bar.setStyleSheet("QProgressBar { text-align: center; } QProgressBar::chunk { background-color: #198754; }")
        ctrl_layout.addWidget(self.temp_bar)
        
        ctrl_layout.addStretch() 
        layout.addLayout(ctrl_layout)

        pg.setConfigOptions(antialias=True, background='#f8f9fa', foreground='#212529')
        self.glw = pg.GraphicsLayoutWidget()
        layout.addWidget(self.glw)

        self.plot_wave = self.glw.addPlot(title="Live Waveform (Auto Overlay)")
        self.plot_wave.setLabel('bottom', "Samples (2ns)")
        self.plot_wave.setLabel('left', "ADC Value (14-bit)")
        self.plot_wave.addLegend(offset=(10, 10))
        self.glw.nextRow()

        self.plot_qlong = self.glw.addPlot(title="Real-time Computed Charge Spectrum")
        self.plot_qlong.setLogMode(y=True)
        self.plot_qlong.setLabel('bottom', "Integrated Charge (ADC Bins)")
        self.plot_qlong.setLabel('left', "Counts (Log)")
        self.plot_qlong.addLegend(offset=(10, 10))

    @pyqtSlot(float)
    def update_temperature(self, temp: float):
        self.temp_bar.setValue(int(temp))
        
        if temp >= 80.0:
            self.temp_bar.setStyleSheet("QProgressBar { text-align: center; } QProgressBar::chunk { background-color: #dc3545; }")
            if not self.warning_latched:
                self.warning_latched = True
                QMessageBox.warning(
                    self, 
                    "Over-Temperature Warning", 
                    "ADC 내부 온도가 80°C를 초과했습니다.\n85°C 도달 시 하드웨어 보호를 위해 ADC가 강제 종료됩니다."
                )
        else:
            self.temp_bar.setStyleSheet("QProgressBar { text-align: center; } QProgressBar::chunk { background-color: #198754; }")
            if temp < 75.0:
                self.warning_latched = False

    def update_history_size(self):
        new_size = self.spin_history.value()
        for ch in self.q_long_hists:
            current_data = list(self.q_long_hists[ch])
            self.q_long_hists[ch] = deque(current_data[-new_size:], maxlen=new_size)

    def rebuild_plots(self, mask):
        self.plot_wave.clear()
        self.plot_qlong.clear()
        if self.plot_wave.legend: self.plot_wave.legend.clear()
        if self.plot_qlong.legend: self.plot_qlong.legend.clear()
        
        self.curves_wave.clear()
        self.curves_qlong.clear()
        self.q_long_hists.clear()
        
        active_channels = [i for i in range(8) if (mask >> i) & 1]
        
        for ch in active_channels:
            color = self.colors[ch % len(self.colors)]
            
            pen = pg.mkPen(color, width=1.5)
            self.curves_wave[ch] = self.plot_wave.plot(name=f"CH {ch}", pen=pen)
            
            brush = pg.mkColor(color)
            brush.setAlpha(100)
            self.curves_qlong[ch] = self.plot_qlong.plot(name=f"CH {ch}", stepMode="center", fillLevel=0, brush=brush, pen=color)
            
            self.q_long_hists[ch] = deque(maxlen=self.spin_history.value())

    def toggle_monitor(self, idx):
        if idx == 0:
            self.timer.start(33)
        else:
            self.timer.stop()
            while True:
                try: self.sock.recv(flags=zmq.NOBLOCK)
                except zmq.Again: break

    def clear_data(self):
        for ch in self.q_long_hists:
            self.q_long_hists[ch].clear()
            self.curves_wave[ch].setData(np.array([], dtype=np.uint16))
            self.curves_qlong[ch].setData(np.array([0, 1]), np.array([0.1]))

    def poll_zmq(self):
        latest_msg = None
        while True:
            try:
                msg = self.sock.recv(flags=zmq.NOBLOCK)
                latest_msg = msg
                
                header = struct.unpack(HEADER_FORMAT, msg[:HEADER_SIZE])
                record_len = int(header[2])
                mask = int(header[3])
                
                if mask != self.current_mask:
                    self.current_mask = mask
                    self.rebuild_plots(mask)
                    
                active_channels = [i for i in range(8) if (mask >> i) & 1]
                
                for idx, ch in enumerate(active_channels):
                    offset = HEADER_SIZE + (idx * record_len * 2)
                    wave_bytes = msg[offset : offset + (record_len * 2)]
                    
                    if wave_bytes:
                        wave_arr = np.frombuffer(wave_bytes, dtype=np.uint16)
                        if len(wave_arr) > 200:
                            baseline_end = record_len // 4 
                            baseline = np.mean(wave_arr[:baseline_end])
                            pulse_area = np.sum(baseline - wave_arr[baseline_end:]) 
                            if pulse_area > 0: 
                                self.q_long_hists[ch].append(pulse_area)
            except zmq.Again: 
                break

        if latest_msg:
            header = struct.unpack(HEADER_FORMAT, latest_msg[:HEADER_SIZE])
            record_len = int(header[2])
            mask = int(header[3])
            active_channels = [i for i in range(8) if (mask >> i) & 1]
            
            for idx, ch in enumerate(active_channels):
                if ch in self.curves_wave:
                    offset = HEADER_SIZE + (idx * record_len * 2)
                    wave_bytes = latest_msg[offset : offset + (record_len * 2)]
                    if wave_bytes:
                        wave_arr = np.frombuffer(wave_bytes, dtype=np.uint16)
                        self.curves_wave[ch].setData(wave_arr)
                        
            for ch in self.curves_qlong:
                if len(self.q_long_hists[ch]) > 2:
                    y, x = np.histogram(np.array(self.q_long_hists[ch]), bins=100)
                    y = np.where(y == 0, 0.1, y)
                    self.curves_qlong[ch].setData(x, y)

    def cleanup(self):
        if self.timer.isActive(): self.timer.stop()
        self.sock.close()
        self.ctx.term()