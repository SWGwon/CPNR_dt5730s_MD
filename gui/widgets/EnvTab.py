import json
from PyQt5.QtWidgets import (QWidget, QVBoxLayout, QHBoxLayout, QPushButton,
                             QTableWidget, QTableWidgetItem, QHeaderView, QLabel)

class EnvTab(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setup_ui()

    def setup_ui(self):
        layout = QVBoxLayout(self)
        lbl = QLabel("Extra Environment Variables (Serialized to JSON)")
        lbl.setStyleSheet("font-weight: bold; color: #212529;")
        layout.addWidget(lbl)

        btn_layout = QHBoxLayout()
        self.btn_add = QPushButton("Add Parameter")
        self.btn_remove = QPushButton("Remove Selected")
        self.btn_add.clicked.connect(lambda: self.add_row("", ""))
        self.btn_remove.clicked.connect(self.remove_row)
        btn_layout.addWidget(self.btn_add)
        btn_layout.addWidget(self.btn_remove)
        btn_layout.addStretch()
        layout.addLayout(btn_layout)

        self.table = QTableWidget(0, 2)
        self.table.setHorizontalHeaderLabels(["Parameter (Key)", "Value"])
        self.table.horizontalHeader().setSectionResizeMode(0, QHeaderView.Stretch)
        self.table.horizontalHeader().setSectionResizeMode(1, QHeaderView.Stretch)
        layout.addWidget(self.table)

        defaults = {
            "Humidity (%)": "40.0",
            "Trigger Cable Length (m)": "2.0"
        }
        for k, v in defaults.items():
            self.add_row(k, v)

    def add_row(self, key, val):
        r = self.table.rowCount()
        self.table.insertRow(r)
        self.table.setItem(r, 0, QTableWidgetItem(key))
        self.table.setItem(r, 1, QTableWidgetItem(val))

    def remove_row(self):
        for item in self.table.selectedItems():
            self.table.removeRow(item.row())

    def get_env_data(self):
        data = {}
        for r in range(self.table.rowCount()):
            k_item = self.table.item(r, 0)
            v_item = self.table.item(r, 1)
            if k_item and v_item and k_item.text().strip():
                data[k_item.text().strip()] = v_item.text().strip()
        return data
