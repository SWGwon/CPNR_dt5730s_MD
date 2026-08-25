import struct
import numpy as np
import pyqtgraph as pg
import zmq
from PyQt6.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout, QComboBox, QLabel, QPushButton, QSpinBox, QProgressBar, QMessageBox
from PyQt6.QtCore import Qt, QTimer, pyqtSlot
from collections import deque

# CAEN Event Header: ExtTTT(Q), EvtID(I), RecLen(I), Mask(H), Pattern(H), BoardEventCounter(I)
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

        # [요구사항 반영] 스펙트럼 분석 모드 콤보박스 (적분 면적 vs 펄스 높이)
        ctrl_layout.addWidget(QLabel("  |  <b>Analysis Mode:</b>"))
        self.cb_spec_mode = QComboBox()
        self.cb_spec_mode.addItems(["📊 Pulse Charge (Integral Area)", "📈 Pulse Height (Amplitude)"])
        self.cb_spec_mode.currentIndexChanged.connect(self.toggle_spec_mode)
        ctrl_layout.addWidget(self.cb_spec_mode)

        ctrl_layout.addWidget(QLabel("  |  <b>Spectrum History:</b>"))
        self.spin_history = QSpinBox()
        self.spin_history.setRange(100, 100000)
        self.spin_history.setSingleStep(500)
        self.spin_history.setValue(2000)
        self.spin_history.setSuffix(" Evts")
        self.spin_history.valueChanged.connect(self.update_history_size)
        ctrl_layout.addWidget(self.spin_history)

        self.btn_clear = QPushButton("🗑️ Clear All")
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

    @pyqtSlot(int)
    def toggle_spec_mode(self, idx):
        """스펙트럼 모드 콤보박스 변경 시 타이틀 및 라벨 변경, 데이터 강제 초기화"""
        if idx == 0:
            self.plot_qlong.setTitle("Real-time Computed Charge Spectrum")
            self.plot_qlong.setLabel('bottom', "Integrated Charge (ADC Bins)")
        else:
            self.plot_qlong.setTitle("Real-time Pulse Height Spectrum")
            self.plot_qlong.setLabel('bottom', "Pulse Height Amplitude (ADC Bins)")
        self.clear_data()

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
                    "ADC 내부 온도가 80°C를 초과했습니다.\n82°C 도달 시 하드웨어 보호를 위해 ADC가 강제 종료됩니다."
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
            
            # stepMode=True 요구 조건 (len(x) == len(y) + 1)
            self.curves_qlong[ch] = self.plot_qlong.plot(name=f"CH {ch}", stepMode=True, fillLevel=0.1, brush=brush, pen=color)
            
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
            if ch in self.curves_wave:
                self.curves_wave[ch].setData(np.array([], dtype=np.uint16))
            if ch in self.curves_qlong:
                # [버그 픽스 1] 초기화 시 길이 규칙(x=2, y=1) 강제하여 차원 불일치 에러 방지
                self.curves_qlong[ch].setData(x=np.array([-0.5, 0.5]), y=np.array([0.1]))

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
                    self.clear_data()
                    
                active_channels = [i for i in range(8) if (mask >> i) & 1]
                spec_mode = self.cb_spec_mode.currentIndex()
                
                for idx, ch in enumerate(active_channels):
                    offset = HEADER_SIZE + (idx * record_len * 2)
                    wave_bytes = msg[offset : offset + (record_len * 2)]
                    
                    if wave_bytes:
                        wave_arr = np.frombuffer(wave_bytes, dtype=np.uint16)
                        if len(wave_arr) > 20: 
                            # [핵심] 스마트 베이스라인 알고리즘
                            # 파형에서 가장 깊게 파인 곳(최소값)의 인덱스를 스스로 찾음
                            min_idx = int(np.argmin(wave_arr))
                            
                            # 펄스가 발생하기 직전의 평탄한 영역만을 베이스라인으로 설정하여 왜곡 방지
                            if min_idx > 10:
                                baseline_end = min(record_len // 4, min_idx - 5)
                            else:
                                baseline_end = 10
                                
                            baseline_end = max(5, baseline_end) # 최소 5샘플 방어 로직
                            baseline = np.mean(wave_arr[:baseline_end])

                            val = 0.0
                            if spec_mode == 0:
                                # Mode 0: Pulse Charge (면적 적분) - 베이스라인보다 아래로 파인 음극성 면적만 모두 적분
                                pulse_region = wave_arr[wave_arr < baseline]
                                val = np.sum(baseline - pulse_region)
                            else:
                                # Mode 1: Pulse Height (진폭 높이) - 베이스라인과 최소값의 깊이 차이
                                val = baseline - wave_arr[min_idx]

                            if val > 0: 
                                self.q_long_hists[ch].append(val)
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
                        
            # [버그 픽스 2] 히스토그램 X,Y 배열 차원 불일치 에러 완벽 해결
            for ch in self.curves_qlong:
                hist_data = self.q_long_hists[ch]
                if len(hist_data) > 5:
                    data_min, data_max = min(hist_data), max(hist_data)
                    if data_min == data_max: 
                        continue 
                        
                    # np.histogram은 x_edges 길이를 y보다 1 크게 반환함 (151개, 150개)
                    # 변환 없이 그대로 setData에 넘겨주어 stepMode=True 조건을 완벽하게 만족시킴
                    y, x_edges = np.histogram(hist_data, bins=150)
                    y = np.where(y == 0, 0.1, y) # Log 스케일 에러 방지 (0점 바닥 띄우기)
                    
                    self.curves_qlong[ch].setData(x=x_edges, y=y)

    def cleanup(self):
        if self.timer.isActive(): self.timer.stop()
        self.sock.close()
        self.ctx.term()