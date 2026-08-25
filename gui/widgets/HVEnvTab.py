from PyQt6.QtWidgets import (QWidget, QVBoxLayout, QHBoxLayout, QPushButton, 
                             QLineEdit, QLabel, QTableWidget, QTableWidgetItem, 
                             QHeaderView, QGroupBox, QCheckBox, QMessageBox, QTabWidget, 
                             QDoubleSpinBox)
from PyQt6.QtCore import Qt, pyqtSlot
from core.HVManager import HVManager

class HVEnvTab(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.hv_manager = HVManager()
        self.hv_manager.connected_sig.connect(self.on_connected)
        self.hv_manager.discovery_sig.connect(self.build_dynamic_tabs)
        self.hv_manager.update_sig.connect(self.update_table_data)
        
        self.slot_tables = {} 
        self.live_hv_snapshot = {} # DAQ 연동을 위한 실시간 VMon 캐싱 버퍼
        self.setup_ui()

    def setup_ui(self):
        layout = QVBoxLayout(self)

        # 1. SY5527 서버 접속 제어부
        conn_group = QGroupBox("CAEN SY5527 Mainframe Connection")
        conn_layout = QHBoxLayout()
        
        conn_layout.addWidget(QLabel("IP Address:"))
        self.ip_input = QLineEdit("192.168.0.10")
        conn_layout.addWidget(self.ip_input)
        
        self.btn_connect = QPushButton("Connect & Discover")
        self.btn_connect.setStyleSheet("background-color: #0dcaf0; color: black; font-weight: bold;")
        self.btn_connect.clicked.connect(self.toggle_connection)
        conn_layout.addWidget(self.btn_connect)
        
        self.lbl_status = QLabel("Disconnected")
        self.lbl_status.setStyleSheet("color: #dc3545; font-weight: bold;")
        conn_layout.addWidget(self.lbl_status)
        
        conn_layout.addSpacing(30)
        
        # [핵심] DAQ 시스템 Run DB 연동 제어 체크박스
        self.chk_sync_daq = QCheckBox("🔗 Sync Live HV VMon to DAQ Run DB")
        self.chk_sync_daq.setChecked(True)
        self.chk_sync_daq.setToolTip("DAQ 시작 시, 현재 인가되어 있는 실제 전압(VMon) 값을 읽어 DB에 영구 기록합니다.")
        self.chk_sync_daq.setStyleSheet("color: #0d6efd; font-weight: bold;")
        conn_layout.addWidget(self.chk_sync_daq)
        conn_layout.addStretch()
        
        conn_group.setLayout(conn_layout)
        layout.addWidget(conn_group)

        # 2. 하드웨어 스캔 기반 동적 대시보드 렌더링 영역
        self.board_group = QGroupBox("HV Boards Dashboard (Dynamic Provisioning)")
        board_layout = QVBoxLayout()
        self.tabs_boards = QTabWidget()
        board_layout.addWidget(self.tabs_boards)
        self.board_group.setLayout(board_layout)
        layout.addWidget(self.board_group, stretch=1)

    def toggle_connection(self):
        if not self.hv_manager.is_connected:
            self.lbl_status.setText("Connecting...")
            self.btn_connect.setEnabled(False)
            self.hv_manager.connect_system(self.ip_input.text().strip())
        else:
            self.hv_manager.disconnect_system()

    @pyqtSlot(bool, str)
    def on_connected(self, success, msg):
        self.btn_connect.setEnabled(True)
        if success:
            self.lbl_status.setText("Connected")
            self.lbl_status.setStyleSheet("color: #198754; font-weight: bold;")
            self.btn_connect.setText("Disconnect")
            self.btn_connect.setStyleSheet("background-color: #dc3545; color: white; font-weight: bold;")
            if "Mock" in msg:
                QMessageBox.warning(self, "HV Connected (Mock)", msg)
        else:
            self.lbl_status.setText("Failed")
            self.lbl_status.setStyleSheet("color: #dc3545; font-weight: bold;")
            self.btn_connect.setText("Connect & Discover")
            self.btn_connect.setStyleSheet("background-color: #0dcaf0; color: black; font-weight: bold;")
            self.tabs_boards.clear()
            self.slot_tables.clear()
            if msg != "Disconnected": 
                QMessageBox.critical(self, "HV Error", msg)

    @pyqtSlot(dict)
    def build_dynamic_tabs(self, topology):
        """서버가 응답한 Crate Map을 바탕으로 다채널 테이블을 생성합니다."""
        self.tabs_boards.clear()
        self.slot_tables.clear()

        for slot, info in topology.items():
            model = info.get('BoardName', f'Slot_{slot}')
            num_ch = info.get('Channels', 0)
            
            tab = QWidget()
            t_layout = QVBoxLayout(tab)
            
            table = QTableWidget(num_ch, 6)
            table.setHorizontalHeaderLabels(["CH", "Status", "VMon (V)", "IMon (uA)", "V0Set (V)", "Power"])
            table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
            
            for ch in range(num_ch):
                # 읽기 전용 셀 초기화
                for col in range(4):
                    item = QTableWidgetItem()
                    item.setFlags(item.flags() & ~Qt.ItemFlag.ItemIsEditable)
                    if col == 0: item.setText(f"CH {ch:02d}")
                    item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
                    table.setItem(ch, col, item)

                # 전압 제어 스핀박스 (음극성/양극성 및 3000V/4000V 고려)
                vset_spin = QDoubleSpinBox()
                vset_spin.setRange(-4000, 4000) 
                vset_spin.setSuffix(" V")
                vset_spin.setAlignment(Qt.AlignmentFlag.AlignCenter)
                # Lambda 함수의 closure 바인딩 이슈 방지를 위해 인자 기본값 전달 기법 사용
                vset_spin.valueChanged.connect(lambda val, s=slot, c=ch: self.hv_manager.set_param(s, c, "V0Set", val))
                table.setCellWidget(ch, 4, vset_spin)
                
                # 전원 버튼
                btn_pw = QPushButton("Turn ON")
                btn_pw.clicked.connect(lambda checked, s=slot, c=ch, btn=btn_pw: self.toggle_channel_power(s, c, btn))
                table.setCellWidget(ch, 5, btn_pw)
                
            t_layout.addWidget(table)
            self.tabs_boards.addTab(tab, f"Slot {slot} [{model}]")
            self.slot_tables[slot] = table

    def toggle_channel_power(self, slot, ch, btn):
        if btn.text() == "Turn ON":
            self.hv_manager.set_param(slot, ch, "Pw", 1)
        else:
            self.hv_manager.set_param(slot, ch, "Pw", 0)

    @pyqtSlot(dict)
    def update_table_data(self, update_data):
        """1초마다 VMon, IMon, Status 값을 테이블에 렌더링"""
        for slot, ch_list in update_data.items():
            if slot not in self.slot_tables: continue
            table = self.slot_tables[slot]
            
            for data in ch_list:
                ch = data['ch']
                vmon = data['VMon']
                status_val = data['Status']
                pw = data['Pw']
                
                status_str = "ON" if pw else "OFF"
                
                # 향후 Status 비트마스킹을 통해 세분화 가능
                if status_val & (1 << 9) or status_val & (1 << 12): 
                    status_str = "TRIP"
                elif status_val & (1 << 1):
                    status_str = "RUP"
                elif status_val & (1 << 2):
                    status_str = "RDW"
                
                item_st = table.item(ch, 1)
                if item_st:
                    item_st.setText(status_str)
                    if status_str == "ON": 
                        item_st.setBackground(Qt.GlobalColor.green)
                        item_st.setForeground(Qt.GlobalColor.black)
                    elif status_str == "TRIP": 
                        item_st.setBackground(Qt.GlobalColor.red)
                        item_st.setForeground(Qt.GlobalColor.white)
                    elif status_str in ["RUP", "RDW"]:
                        item_st.setBackground(Qt.GlobalColor.yellow)
                        item_st.setForeground(Qt.GlobalColor.black)
                    else: 
                        item_st.setBackground(Qt.GlobalColor.lightGray)
                        item_st.setForeground(Qt.GlobalColor.black)
                
                table.item(ch, 2).setText(f"{vmon:.2f}")
                table.item(ch, 3).setText(f"{data['IMon']:.3f}")
                
                pw_btn = table.cellWidget(ch, 5)
                if pw_btn:
                    if pw == 1:
                        pw_btn.setText("Turn OFF")
                        pw_btn.setStyleSheet("background-color: #dc3545; color: white; font-weight: bold;")
                    else:
                        pw_btn.setText("Turn ON")
                        pw_btn.setStyleSheet("font-weight: bold;")
                
                # DAQ 연동을 위해 메모리에 스냅샷 유지
                self.live_hv_snapshot[f"Slot{slot}_CH{ch:02d}"] = vmon

    def get_env_data(self):
        """DaqTab.py에서 DAQ 시작 시 DB 기록을 위해 호출 (Single Source of Truth)"""
        env_data = {}
        if self.chk_sync_daq.isChecked() and self.hv_manager.is_connected:
            # 5V 이상 인가된 활성 채널의 전압만 필터링하여 기록
            for key, vmon in self.live_hv_snapshot.items():
                if abs(vmon) > 5.0:  
                    env_data[f"HV_{key}"] = f"{vmon:.1f} V"
        return env_data

    def closeEvent(self, event):
        self.hv_manager.stop()
        super().closeEvent(event)