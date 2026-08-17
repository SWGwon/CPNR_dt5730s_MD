#!/usr/bin/env python3
import sys
import os
from PyQt6.QtWidgets import QApplication

# gui 하위 모듈(windows, widgets) 경로 인식을 위해 sys.path에 현재 경로 추가
sys.path.append(os.path.dirname(os.path.abspath(__file__)))
from windows.MainWindow import MainWindow

def main():
    app = QApplication(sys.argv)
    app.setStyle("Fusion")  # 리눅스 환경 최적화 스타일

    window = MainWindow()
    window.show()

    sys.exit(app.exec()) 

if __name__ == "__main__":
    main()