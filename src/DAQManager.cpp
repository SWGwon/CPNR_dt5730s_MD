#include "DAQManager.h"
#include "EventHeader.h"
#include "CAENComm.h" 
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <ctime>
#include <zmq.h>

DAQManager::DAQManager(const std::string &config_file,
                       const std::string &output_file, int max_events,
                       int run_time_sec)
    : config_(config_file), output_file_(output_file), max_events_(max_events),
      run_time_sec_(run_time_sec), running_(false),
      digitizer_(CAEN_DGTZ_USB, 0, 0, 0) {
        
  // [ZMQ Pub Socket 초기화]
  zmq_ctx_ = zmq_ctx_new();
  zmq_pub_ = zmq_socket(zmq_ctx_, ZMQ_PUB);
  int hwm = 5000;
  zmq_setsockopt(zmq_pub_, ZMQ_SNDHWM, &hwm, sizeof(hwm));
  
  // ZMQ 포트 TIME_WAIT 충돌 방지
  int linger = 0;
  zmq_setsockopt(zmq_pub_, ZMQ_LINGER, &linger, sizeof(linger));
  zmq_bind(zmq_pub_, "tcp://127.0.0.1:5555");

  if (!output_file_.empty()) {
    static std::vector<char> write_buffer(4 * 1024 * 1024);
    out_stream_.rdbuf()->pubsetbuf(write_buffer.data(), write_buffer.size());
    out_stream_.open(output_file_, std::ios::binary);
    if (!out_stream_.is_open()) {
      throw std::runtime_error("Cannot open output file: " + output_file_);
    }
  }
  
  SetupHardware();
}

DAQManager::~DAQManager() {
  Stop();
  if (out_stream_.is_open()) out_stream_.close();
  zmq_close(zmq_pub_);
  zmq_ctx_destroy(zmq_ctx_);
}

void DAQManager::SetupHardware() {
  std::cout << "\033[1;36m[DAQManager]\033[0m Configuring Hardware from Config...\n";
  int handle = digitizer_.GetHandle();
  
  // 링잉 거부 카운트 오류 해결 (진성 Dead Time 모드)
  uint32_t acq_ctrl = 0;
  CAEN_CHECK(CAEN_DGTZ_ReadRegister(handle, 0x8100, &acq_ctrl));
  CAEN_CHECK(CAEN_DGTZ_WriteRegister(handle, 0x8100, acq_ctrl & ~(1 << 3)));

  uint32_t record_length = config_.GetInt("Digitizer", "RecordLength", 4096);
  uint32_t channel_mask = config_.GetInt("Digitizer", "ChannelMask", 0xFF);
  uint32_t post_trigger = config_.GetInt("Digitizer", "PostTrigger", 80);

  // =========================================================================
  // [디버그 패치 1] 하드웨어 고유 지연 방어 및 파형 블록 메모리 동적 정렬
  // =========================================================================
  // DT5730의 RecordLength는 8 단위 블록 정렬이 필수적임
  record_length = ((record_length + 7) / 8) * 8;

  // 하드웨어 Intrinsic Delay(약 150ns) + Baseline 연산 마진(10ns) = 절대 최소 160ns 확보
  uint32_t pre_trigger_ns = record_length * (100 - post_trigger) * 2 / 100;
  
  if (pre_trigger_ns < 160) { 
      std::cerr << "\n\033[1;33m[Warning] Pre-trigger window (" << pre_trigger_ns 
                << " ns) is too close to the DT5730 hardware latency limit (~150 ns).\033[0m\n";
      
      // 160ns를 확보할 수 있는 절대적인 Pre-trigger 비율(%)을 동적 역산 (소수점 올림 처리)
      uint32_t required_pre_pct = (160 * 100 + (record_length * 2 - 1)) / (record_length * 2);
      
      if (required_pre_pct >= 100) {
          // RecordLength 자체가 너무 짧아 160ns를 물리적으로 확보할 수 없는 치명적 상태
          std::cerr << "\033[1;31m[Error] RecordLength (" << record_length << " Samples) is fundamentally too short.\033[0m\n";
          record_length = 512;
          post_trigger = 80;
          std::cerr << "\033[1;33m[Warning] Auto-adjusted RecordLength to 512 and PostTrigger to 80%.\033[0m\n";
      } else {
          // 하드코딩 70%가 아닌, 현재 RecordLength에 맞춘 안전한 PostTrigger 동적 도출
          post_trigger = 100 - required_pre_pct;
          std::cerr << "\033[1;33m[Warning] Auto-adjusting PostTrigger dynamically to " << post_trigger << "% to secure baseline.\033[0m\n";
      }
  }
  // =========================================================================

  CAEN_CHECK(CAEN_DGTZ_SetRecordLength(handle, record_length));
  CAEN_CHECK(CAEN_DGTZ_SetChannelEnableMask(handle, channel_mask));
  CAEN_CHECK(CAEN_DGTZ_SetPostTriggerSize(handle, post_trigger));

  int pol_val = config_.GetInt("Digitizer", "TriggerPolarity", 1);

  for (int ch = 0; ch < MAX_CH; ++ch) {
      if ((channel_mask >> ch) & 1) {
          std::string ch_sec = "Channel_" + std::to_string(ch);
          uint32_t offset = config_.GetInt(ch_sec, "DCOffset", 7050);
          uint32_t thr = config_.GetInt(ch_sec, "TriggerThreshold", 15000);
          
          CAEN_CHECK(CAEN_DGTZ_SetChannelDCOffset(handle, ch, offset));
          CAEN_CHECK(CAEN_DGTZ_SetTriggerPolarity(handle, ch, (pol_val == 0) ? CAEN_DGTZ_TriggerOnRisingEdge : CAEN_DGTZ_TriggerOnFallingEdge));
          CAEN_CHECK(CAEN_DGTZ_SetChannelTriggerThreshold(handle, ch, thr));
      }
  }

  CAEN_DGTZ_TriggerMode_t trg_mode = CAEN_DGTZ_TRGMODE_ACQ_ONLY;
  int ext_trg = config_.GetInt("Digitizer", "ExtTriggerMode", 1);
  if (ext_trg > 0) CAEN_CHECK(CAEN_DGTZ_SetExtTriggerInputMode(handle, trg_mode));
  else CAEN_CHECK(CAEN_DGTZ_SetExtTriggerInputMode(handle, CAEN_DGTZ_TRGMODE_DISABLED));
  
  int self_trg = config_.GetInt("Digitizer", "SelfTriggerMode", 1);
  if (self_trg > 0) CAEN_CHECK(CAEN_DGTZ_SetChannelSelfTrigger(handle, trg_mode, channel_mask));
  else CAEN_CHECK(CAEN_DGTZ_SetChannelSelfTrigger(handle, CAEN_DGTZ_TRGMODE_DISABLED, 0xFF));

  CAEN_CHECK(CAEN_DGTZ_SetSWTriggerMode(handle, trg_mode));
  CAEN_CHECK(CAEN_DGTZ_SetAcquisitionMode(handle, CAEN_DGTZ_SW_CONTROLLED));

  std::cout << "\033[1;36m[FPGA Trigger]\033[0m Standard OR Logic (Software Coincidence Ready).\n";

  digitizer_.AllocateBuffers();
  
  size_t max_safe_size = sizeof(EventHeader) + (record_length + 1024) * sizeof(uint16_t) * MAX_CH;
  raw_buffer_pool_.resize(max_safe_size);
}

void DAQManager::Start(std::atomic<bool>& is_running) {
  std::cout << "\033[1;32m[DAQManager]\033[0m Starting Acquisition...\n";
  std::cout << " - Stop Condition : ";
  if (max_events_ > 0) std::cout << max_events_ << " Events\n";
  else if (run_time_sec_ > 0) std::cout << run_time_sec_ << " Seconds\n";
  else std::cout << "Unlimited (Manual Stop)\n";

  CAEN_CHECK(CAEN_DGTZ_SWStartAcquisition(digitizer_.GetHandle()));
  AcquisitionLoop(is_running);
}

void DAQManager::Stop() {
  running_ = false;
}

void DAQManager::AcquisitionLoop(std::atomic<bool>& is_running) {
  EventHeader *header = reinterpret_cast<EventHeader *>(raw_buffer_pool_.data());
  uint16_t *wave_dest = reinterpret_cast<uint16_t *>(raw_buffer_pool_.data() + sizeof(EventHeader));
  
  int handle = digitizer_.GetHandle();
  char *caen_buffer = digitizer_.GetReadoutBuffer();
  CAEN_DGTZ_UINT16_EVENT_t *caen_event = digitizer_.GetDecodedEvent();
  
  uint32_t event_count = 0;
  const uint32_t TTT_MASK = 0x7FFFFFFF;
  
  bool is_first_event = true;
  uint32_t first_ttt = 0;
  uint32_t current_ttt = 0;
  uint32_t prev_ttt = 0;
  uint64_t ttt_rollovers = 0;
  
  uint32_t prev_event_counter = 0;
  uint64_t lost_events = 0;

  auto start_time = std::chrono::steady_clock::now();
  auto last_log_time = start_time;
  uint32_t log_events = 0;
  uint32_t zmq_drops = 0;
  size_t total_bytes_written = 0; 
  size_t last_bytes_written = 0; 
  
  uint32_t loop_counter = 0; 

  while (is_running) {
    if (max_events_ > 0 && (int)event_count >= max_events_) break;
    if (run_time_sec_ > 0) {
      auto now = std::chrono::steady_clock::now();
      if (std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count() >= run_time_sec_) break;
    }

    uint32_t bsize = 0; 

    try {
      CAEN_CHECK(CAEN_DGTZ_ReadData(handle, CAEN_DGTZ_SLAVE_TERMINATED_READOUT_MBLT, caen_buffer, &bsize));
      
      if (bsize > 0) {
        uint32_t num_events = 0;
        CAEN_CHECK(CAEN_DGTZ_GetNumEvents(handle, caen_buffer, bsize, &num_events));

        for (uint32_t i = 0; i < num_events; ++i) {
          CAEN_DGTZ_EventInfo_t evt_info;
          char *evt_ptr = nullptr;
          CAEN_CHECK(CAEN_DGTZ_GetEventInfo(handle, caen_buffer, bsize, i, &evt_info, &evt_ptr));
          CAEN_CHECK(CAEN_DGTZ_DecodeEvent(handle, evt_ptr, (void **)&caen_event));

          current_ttt = evt_info.TriggerTimeTag & TTT_MASK;
          uint32_t current_event_counter = ((uint32_t*)evt_ptr)[2] & 0xFFFFFF;

          if (is_first_event) {
              first_ttt = current_ttt;
              prev_ttt = current_ttt;
              prev_event_counter = current_event_counter;
              is_first_event = false;
          } else {
              if (current_ttt < prev_ttt) {
                  ttt_rollovers++;
              }
              uint32_t diff = (current_event_counter - prev_event_counter) & 0xFFFFFF;
              if (diff > 1) {
                  lost_events += (diff - 1); 
              }
          }
          prev_ttt = current_ttt;
          prev_event_counter = current_event_counter;

          uint32_t actual_trace_size = 0;
          for (int ch = 0; ch < MAX_CH; ++ch) {
              if ((evt_info.ChannelMask >> ch) & 1) {
                  actual_trace_size = caen_event->ChSize[ch];
                  break; 
              }
          }

          std::memset(header, 0, sizeof(EventHeader));
          header->ExtendedTTT = (ttt_rollovers << 31) | current_ttt;
          header->EventID = event_count++;
          header->RecordLength = actual_trace_size; 
          header->ChannelMask = evt_info.ChannelMask;
          header->Pattern = evt_info.Pattern;
          header->BoardEventCounter = current_event_counter;

          size_t payload_size = sizeof(EventHeader);
          for (int ch = 0; ch < MAX_CH; ++ch) {
            if ((header->ChannelMask >> ch) & 1) {
              uint16_t *wave_src = caen_event->DataChannel[ch];
              uint32_t trace_size = caen_event->ChSize[ch];
              if (trace_size == 0) continue;
              if (payload_size + trace_size * sizeof(uint16_t) > raw_buffer_pool_.size()) break; 

              std::memcpy(wave_dest + (payload_size - sizeof(EventHeader)) / sizeof(uint16_t),
                          wave_src, trace_size * sizeof(uint16_t));
              payload_size += trace_size * sizeof(uint16_t);
            }
          }

          if (out_stream_.is_open()) {
              out_stream_.write(raw_buffer_pool_.data(), payload_size);
              total_bytes_written += payload_size; 
          }
          
          if (zmq_send(zmq_pub_, raw_buffer_pool_.data(), payload_size, ZMQ_DONTWAIT) < 0) {
              if (zmq_errno() == EAGAIN) zmq_drops++;
          }
          log_events++;
        }
      }
    } catch (const std::exception& e) {
        std::cerr << "\n\033[1;33m[Warning] Readout Soft-Error: \033[0m" << e.what() << "\n";
    }

    if (bsize > 0 || ++loop_counter % 10000 == 0) {
        auto now = std::chrono::steady_clock::now();
        
        if (run_time_sec_ > 0) {
            if (std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count() >= run_time_sec_) break;
        }

        double elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log_time).count();
        if (elapsed_ms >= 1000.0) {
            auto total_sec = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
            int mins = total_sec / 60;
            int secs = total_sec % 60;

            double rate = (log_events / elapsed_ms) * 1000.0;
            double speed_mbps = ((total_bytes_written - last_bytes_written) / 1048576.0) / (elapsed_ms / 1000.0);
            last_bytes_written = total_bytes_written;

            uint32_t temp_reg = 0, status_reg = 0;
            if (CAEN_DGTZ_ReadRegister(handle, 0x10A8, &temp_reg) == CAEN_DGTZ_Success) {
                float temp_celsius = static_cast<float>(temp_reg & 0xFF);
                std::cout << "\n[STATUS] TEMP: " << temp_celsius << std::endl;
                if (temp_celsius >= 82.0) {
                    std::cout << "\n[FATAL] OVER_TEMP_SOFT_KILL" << std::endl;
                    is_running = false;
                    break;
                }
            }
            
            if (CAEN_DGTZ_ReadRegister(handle, 0x8104, &status_reg) == CAEN_DGTZ_Success) {
                int run      = (status_reg >> 0) & 0x1; 
                int drdy     = (status_reg >> 2) & 0x1; 
                int busy     = (status_reg >> 3) & 0x1; 
                int pll_lock = ((status_reg >> 5) & 0x1) == 0 ? 1 : 0; 
                int trg      = (rate > 0.0) ? 1 : 0; 
                int pll_byps = 0; 

                std::cout << "[STATUS] LED: LOCK=" << pll_lock << ", BYPS=" << pll_byps
                          << ", RUN=" << run << ", TRG=" << trg << ", DRDY=" << drdy
                          << ", BUSY=" << busy << std::endl;
            }

            uint32_t record_length = config_.GetInt("Digitizer", "RecordLength", 4096);
            uint64_t total_ticks = (ttt_rollovers << 31) + current_ttt - first_ttt;
            
            double hw_real_time_sec = total_ticks * 8e-9; 
            double dead_time_sec = event_count * (record_length * 2e-9); 
            double live_time_sec = hw_real_time_sec - dead_time_sec;
            if (live_time_sec < 0) live_time_sec = 0.0;
            
            double dead_time_pct = (hw_real_time_sec > 0) ? (dead_time_sec / hw_real_time_sec * 100.0) : 0.0;

            std::cout << "\r\033[K\033[1;36m[LIVE DAQ]\033[0m "
                      << "Time: \033[1m" << std::setfill('0') << std::setw(2) << mins << ":" << std::setw(2) << secs << "\033[0m | "
                      << "RealTime: \033[1m" << std::fixed << std::setprecision(2) << hw_real_time_sec << " s\033[0m | "
                      << "Live: \033[1m" << std::fixed << std::setprecision(2) << live_time_sec << " s\033[0m | " 
                      << "DT: \033[1;31m" << std::fixed << std::setprecision(4) << dead_time_pct << " %\033[0m | "
                      << "Events: \033[1;33m" << event_count << "\033[0m | "
                      << "Speed: \033[1;32m" << std::fixed << std::setprecision(2) << speed_mbps << " MB/s\033[0m | "
                      << "Drops: " << zmq_drops
                      << std::flush;
              
            log_events = 0;
            zmq_drops = 0;
            last_log_time = now;
        }
    }
  }

  CAEN_DGTZ_SWStopAcquisition(handle);
  std::cout << "\n\033[1;31m[DAQManager] Stopped Acquisition.\033[0m\n";

  auto t = std::time(nullptr);
  auto tm = *std::localtime(&t);
  
  uint32_t record_length = config_.GetInt("Digitizer", "RecordLength", 4096);
  uint64_t final_total_ticks = (ttt_rollovers << 31) + current_ttt - first_ttt;
  
  double final_real_time_sec = final_total_ticks * 8e-9;
  double final_dead_time_sec = event_count * (record_length * 2e-9);
  double final_live_time_sec = final_real_time_sec - final_dead_time_sec;
  if (final_live_time_sec < 0) final_live_time_sec = 0.0;
  
  double final_dead_time_pct = (final_real_time_sec > 0) ? (final_dead_time_sec / final_real_time_sec * 100.0) : 0.0;
  auto wall_clock_duration = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time).count();
  
  std::cout << "\n\033[1;36m========== [ DAQ Run Summary ] ==========\033[0m\n"
            << " - End Time        : " << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "\n"
            << " - Wall Clock Time : " << wall_clock_duration << " seconds\n"
            << " - HW Real Time    : " << std::fixed << std::setprecision(2) << final_real_time_sec << " seconds\n"
            << " - HW Live Time    : " << std::fixed << std::setprecision(2) << final_live_time_sec << " seconds\n"
            << " - True Dead Time  : " << std::fixed << std::setprecision(5) << final_dead_time_pct << " %\n"
            << " - Total Events    : " << event_count << " events\n"
            << " - Lost Events     : " << lost_events << " events (Buffer Full)\n"
            << " - Data Size Saved : " << std::fixed << std::setprecision(2) << (total_bytes_written / (1024.0 * 1024.0)) << " MB\n"
            << "\033[1;36m=========================================\033[0m\n\n";
}