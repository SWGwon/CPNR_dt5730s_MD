#ifndef DAQ_MANAGER_H
#define DAQ_MANAGER_H

#include "CaenDigitizer.h"
#include "DAQConfig.h"
#include "ConfigParser.h"
#include "EventHeader.h"
#include <atomic>
#include <fstream>
#include <string>
#include <vector>

class DAQManager {
public:
  DAQManager(const std::string &config_file, const std::string &output_file,
             int max_events, int run_time_sec);
  ~DAQManager();
  
  // 외부에서 제어 가능한 atomic 플래그를 받도록 수정
  void Start(std::atomic<bool>& is_running);
  void Stop();

private:
  void SetupHardware();
  void AcquisitionLoop(std::atomic<bool>& is_running);

  ConfigParser config_;
  // config_ 다음, digitizer_ 이전에 초기화되어 잘못된 설정으로 장비를 여는 것을 차단합니다.
  DAQHardwareSettings hardware_settings_;
  std::string output_file_;
  int max_events_;
  int run_time_sec_;
  std::atomic<bool> running_;
  CaenDigitizer digitizer_;
  std::ofstream out_stream_;
  
  // ZMQ (ZeroMQ for real-time monitoring)
  void *zmq_ctx_;
  void *zmq_pub_;
  
  // Zero-Copy Memory Pool
  std::vector<char> raw_buffer_pool_;
};

#endif // DAQ_MANAGER_H
