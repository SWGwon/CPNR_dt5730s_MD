import time
import queue
from PyQt6.QtCore import QThread, pyqtSignal

# CAEN 공식 Python Wrapper 로드 시도
try:
    from caen_libs.caenhvwrapper import CAENHVWrapper
    from caen_libs.caenhvwrapperflags import CAENHV_SYSTEM_TYPE, CAENHV_LINK_TYPE
    HV_AVAILABLE = True
except ImportError:
    HV_AVAILABLE = False

class HVManager(QThread):
    connected_sig = pyqtSignal(bool, str)       # 연결 상태 (성공 여부, 메시지)
    discovery_sig = pyqtSignal(dict)            # 크레이트 맵 스캔 결과 (슬롯별 보드 정보)
    update_sig = pyqtSignal(dict)               # 1초 주기 채널 파라미터 업데이트 데이터
    trip_sig = pyqtSignal(int, int)             # (Slot, Channel) 하드웨어 트립/에러 감지 알림

    def __init__(self):
        super().__init__()
        self.is_running = False
        self.is_connected = False
        self.wrapper = None
        self.handle = -1
        self.cmd_queue = queue.Queue()
        self.topology = {}

    def connect_system(self, ip_address, user="admin", password=""):
        """UI 스레드에서 접속을 지시할 때 호출"""
        self.cmd_queue.put(('CONNECT', (ip_address, user, password)))
        if not self.isRunning():
            self.start()

    def disconnect_system(self):
        self.cmd_queue.put(('DISCONNECT', None))

    def set_param(self, slot, ch, param, value):
        self.cmd_queue.put(('SET_PARAM', (slot, ch, param, value)))

    def run(self):
        self.is_running = True
        
        while self.is_running:
            # 1. 큐에 적재된 외부 명령 처리 (GUI에서 보낸 명령 실행)
            while not self.cmd_queue.empty():
                cmd, args = self.cmd_queue.get()
                
                if cmd == 'CONNECT':
                    ip, user, pwd = args
                    if not HV_AVAILABLE:
                        self.connected_sig.emit(True, "[WARNING] py-caen-libs 모듈을 찾을 수 없어 Mock(시뮬레이션) 모드로 진입합니다.")
                        self.is_connected = True
                        # A7435SN(24채널) 테스트용 더미 토폴로지 전송
                        self.topology = {0: {'BoardName': 'A7435SN (Mock)', 'Channels': 24}}
                        self.discovery_sig.emit(self.topology)
                        continue
                    
                    try:
                        self.wrapper = CAENHVWrapper()
                        # SY5527 연결 수립
                        self.handle = self.wrapper.init_system(
                            CAENHV_SYSTEM_TYPE.SY5527, 
                            CAENHV_LINK_TYPE.TCPIP, 
                            ip, user, pwd
                        )
                        self.is_connected = True
                        self.connected_sig.emit(True, f"SY5527 ({ip}) 접속 성공")
                        
                        # [동적 프로비저닝] Crate 장착 보드 목록 스캔
                        num_slots, crate_map = self.wrapper.get_crate_map(self.handle)
                        self.topology = {}
                        for slot, board in enumerate(crate_map):
                            if board and board.get('BoardName') and board['BoardName'] != "Empty":
                                self.topology[slot] = board
                        self.discovery_sig.emit(self.topology)
                    except Exception as e:
                        self.connected_sig.emit(False, f"접속 실패: {str(e)}")
                
                elif cmd == 'DISCONNECT':
                    if self.is_connected and HV_AVAILABLE:
                        try: self.wrapper.deinit_system(self.handle)
                        except: pass
                    self.is_connected = False
                    self.topology = {}
                    self.connected_sig.emit(False, "Disconnected")

                elif cmd == 'SET_PARAM':
                    slot, ch, param, value = args
                    if self.is_connected and HV_AVAILABLE:
                        try:
                            # 라이브러리 스펙에 맞춰 리스트 형태로 전달
                            self.wrapper.set_ch_param(self.handle, slot, param, [ch], [value])
                        except Exception as e:
                            print(f"[HVManager] Set Parameter Error: {e}")

            # 2. 1Hz 실시간 상태 폴링
            if self.is_connected:
                update_data = {}
                for slot, board in self.topology.items():
                    num_ch = board['Channels']
                    ch_list = list(range(num_ch))
                    try:
                        if HV_AVAILABLE:
                            # 24개 채널을 for문으로 개별 요청 시 엄청난 통신 랙이 발생함.
                            # 리스트로 한 번의 트랜잭션으로 읽어와 네트워크 I/O 오버헤드 최소화
                            vmon = self.wrapper.get_ch_param(self.handle, slot, "VMon", ch_list)
                            imon = self.wrapper.get_ch_param(self.handle, slot, "IMon", ch_list)
                            pw = self.wrapper.get_ch_param(self.handle, slot, "Pw", ch_list)
                            status = self.wrapper.get_ch_param(self.handle, slot, "Status", ch_list)
                            
                            ch_data = []
                            for i, ch in enumerate(ch_list):
                                st = status[i]
                                # CAEN Status Register: OverCurrent, OverVoltage, Trip 등 감지 시그널 발송
                                # (CAEN 매뉴얼 기준 보통 Bit 9가 OverCurrent, Bit 12가 Trip 등임)
                                if st & (1 << 9) or st & (1 << 12):
                                    self.trip_sig.emit(slot, ch)
                                    
                                ch_data.append({
                                    'ch': ch, 'VMon': vmon[i], 'IMon': imon[i], 
                                    'Pw': pw[i], 'Status': st
                                })
                            update_data[slot] = ch_data
                        else:
                            # Mock Data (UI 테스트용)
                            import random
                            ch_data = [{'ch': c, 'VMon': random.uniform(-100, -2900), 'IMon': random.uniform(0, 10), 'Pw': 1, 'Status': 0} for c in ch_list]
                            update_data[slot] = ch_data
                    except Exception as e:
                        pass
                
                if update_data:
                    self.update_sig.emit(update_data)
            
            time.sleep(1.0) # 백그라운드 스레드 부하 방지용 1Hz 폴링 딜레이
            
    def stop(self):
        self.is_running = False
        self.wait()
        if self.is_connected and HV_AVAILABLE:
            try: self.wrapper.deinit_system(self.handle)
            except: pass