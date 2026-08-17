import os
import sqlite3
import json
from PyQt6.QtWidgets import (QWidget, QVBoxLayout, QHBoxLayout, 
                             QPushButton, QTableWidget, QTableWidgetItem, 
                             QHeaderView, QMessageBox)

class DatabaseTab(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        curr = os.path.abspath(os.path.dirname(__file__))
        while curr != '/' and not os.path.exists(os.path.join(curr, 'CMakeLists.txt')):
            curr = os.path.dirname(curr)
        self.proj_dir = curr if curr != '/' else os.getcwd()
        
        self.db_path = os.path.join(self.proj_dir, "data", "run_history.db")
        self.setup_ui()

    def setup_ui(self):
        layout = QVBoxLayout(self)
        btn_layout = QHBoxLayout()
        self.btn_refresh = QPushButton("Refresh Database")
        self.btn_refresh.setStyleSheet("font-weight: bold; padding: 8px;")
        self.btn_refresh.clicked.connect(self.load_data)
        btn_layout.addWidget(self.btn_refresh); btn_layout.addStretch()
        layout.addLayout(btn_layout)

        self.table = QTableWidget(0, 5)
        self.table.setHorizontalHeaderLabels(["Run ID", "Start Time", "Operator", "Output File", "Merged Env & Summary"])
        self.table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.ResizeToContents)
        self.table.horizontalHeader().setSectionResizeMode(1, QHeaderView.ResizeMode.ResizeToContents)
        self.table.horizontalHeader().setSectionResizeMode(2, QHeaderView.ResizeMode.ResizeToContents)
        self.table.horizontalHeader().setSectionResizeMode(3, QHeaderView.ResizeMode.Stretch)
        self.table.horizontalHeader().setSectionResizeMode(4, QHeaderView.ResizeMode.Stretch)
        layout.addWidget(self.table)

    def load_data(self):
        if not os.path.exists(self.db_path): return
        try:
            conn = sqlite3.connect(self.db_path)
            conn.row_factory = sqlite3.Row
            cursor = conn.cursor()
            cursor.execute("SELECT * FROM run_history ORDER BY id DESC")
            rows = cursor.fetchall()
            conn.close()

            self.table.setRowCount(0)
            for row_idx, row_data in enumerate(rows):
                self.table.insertRow(row_idx)
                self.table.setItem(row_idx, 0, QTableWidgetItem(str(row_data['id'])))
                self.table.setItem(row_idx, 1, QTableWidgetItem(str(row_data['start_time'])))
                
                keys = row_data.keys()
                env_raw = row_data['env_metadata'] if 'env_metadata' in keys else None
                daq_raw = row_data['daq_summary'] if 'daq_summary' in keys else None
                prod_raw = row_data['production_summary'] if 'production_summary' in keys else None
                legacy_hv = row_data['hv'] if 'hv' in keys else None
                
                operator_name = "Unknown"
                merged_info = []

                if env_raw:
                    try:
                        env_dict = json.loads(env_raw)
                        operator_name = env_dict.pop("Operator", "Unknown")
                        if env_dict: merged_info.append("[ENV] " + ", ".join([f"{k}: {v}" for k, v in env_dict.items()]))
                    except: pass
                elif legacy_hv: 
                     merged_info.append(f"[ENV] Legacy HV: {legacy_hv}")
                
                if daq_raw:
                    try:
                        daq_dict = json.loads(daq_raw)
                        merged_info.append(f"[DAQ] Evts: {daq_dict.get('events', '0')}, Spd: {daq_dict.get('speed', '0.00')}MB/s")
                    except: pass

                if prod_raw:
                    try:
                        prod_dict = json.loads(prod_raw)
                        merged_info.append(f"[ROOT] Evts: {prod_dict.get('events', '0')}, Spd: {prod_dict.get('avg_speed', '0')}MB/s")
                    except: pass

                self.table.setItem(row_idx, 2, QTableWidgetItem(operator_name))
                self.table.setItem(row_idx, 3, QTableWidgetItem(str(row_data['output_file'])))
                self.table.setItem(row_idx, 4, QTableWidgetItem(" | ".join(merged_info)))

        except Exception as e: QMessageBox.critical(self, "DB Error", f"Failed to load database:\n{e}")

    def showEvent(self, event):
        self.load_data()
        super().showEvent(event)