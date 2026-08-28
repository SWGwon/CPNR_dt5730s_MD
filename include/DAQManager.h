#ifndef DAQ_MANAGER_H
#define DAQ_MANAGER_H

#include "CaenDigitizer.h"
#include "DAQConfig.h"
#include "ConfigParser.h"
#include "EventHeader.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

class DAQManager {
public:
  DAQManager(const std::string &config_file, const std::string &output_file,
             int max_events, int run_time_sec, int run_number,
             const std::string &metadata_file = "",
             const std::string &executable_path = "",
             const std::string &git_commit = "unknown",
             const std::string &build_timestamp = "unknown",
             const std::atomic<bool> *cancellation_flag = nullptr);
  ~DAQManager();
  
  // 외부에서 제어 가능한 atomic 플래그를 받도록 수정
  void Start(std::atomic<bool>& is_running);
  void Stop();

private:
  struct ChannelRuntimeCalibration {
    bool enabled = false;
    bool participates_in_trigger = false;
    bool threshold_programmed = false;
    bool baseline_measured = false;
    bool measured_threshold = false;
    uint32_t requested_dc_offset = 0;
    uint32_t readback_dc_offset = 0;
    uint32_t input_range_register = 0;
    uint32_t input_range_readback = 0;
    int polarity_readback = -1;
    double measured_baseline_adc = 0.0;
    double requested_threshold_mv = 0.0;
    uint32_t delta_adc = 0;
    uint32_t written_threshold_adc = 0;
    uint32_t readback_threshold_adc = 0;
    double effective_threshold_mv = 0.0;
  };

  void SetupHardware();
  void ConfigureInputRangeAndOffsets(int handle);
  std::array<double, MAX_CH> MeasureBaselineBatch(
      int handle, std::chrono::steady_clock::time_point deadline);
  std::array<double, MAX_CH> WaitForStableBaselines(int handle);
  void ProgramAndVerifyThresholds(
      int handle, const std::array<double, MAX_CH> &baselines);
  void ConfigureAndVerifyTriggerRouting(int handle);
  void WriteRuntimeArtifacts();
  void AcquisitionLoop(std::atomic<bool>& is_running);
  void CheckSetupCancellation() const;

  std::string config_file_;
  std::string config_contents_;
  ConfigParser config_;
  // Parsed before digitizer_ construction so invalid settings cannot open hardware.
  DAQHardwareSettings hardware_settings_;
  std::string output_file_;
  std::string metadata_file_;
  std::string config_snapshot_file_;
  std::string executable_path_;
  std::string executable_sha256_;
  std::string git_commit_;
  std::string build_timestamp_;
  std::string board_model_ = "unknown";
  std::string board_roc_firmware_ = "unknown";
  std::string board_amc_firmware_ = "unknown";
  uint32_t board_serial_number_ = 0;
  uint32_t board_adc_bits_ = 0;
  std::string acquisition_status_ = "hardware_configuration_pending";
  std::string failure_reason_;
  bool config_snapshot_written_ = false;
  uint64_t output_device_ = 0;
  uint64_t output_inode_ = 0;
  uint64_t raw_output_size_bytes_ = 0;
  std::string raw_output_sha256_;
  bool raw_output_finalized_ = false;
  int run_number_;
  int max_events_;
  int run_time_sec_;
  std::atomic<bool> running_;
  const std::atomic<bool>* cancellation_flag_ = nullptr;
  CaenDigitizer digitizer_;
  std::FILE* output_stream_ = nullptr;
  std::vector<char> output_write_buffer_;
  std::array<ChannelRuntimeCalibration, MAX_CH> runtime_channels_{};
  std::array<uint32_t, MAX_CH / 2> pair_logic_readback_{};
  uint32_t global_trigger_mask_readback_ = 0;
  uint32_t channel_mask_readback_ = 0;
  uint32_t clock_source_readback_ = 0;
  int run_sync_mode_readback_ = -1;
  
  // ZMQ (ZeroMQ for real-time monitoring)
  void *zmq_ctx_;
  void *zmq_pub_;
  
  // Zero-Copy Memory Pool
  std::vector<char> raw_buffer_pool_;
};

#endif // DAQ_MANAGER_H
