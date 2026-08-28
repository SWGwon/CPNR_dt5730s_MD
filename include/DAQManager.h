#ifndef DAQ_MANAGER_H
#define DAQ_MANAGER_H

#include "CaenDigitizer.h"
#include "DAQConfig.h"
#include "ConfigParser.h"
#include "EventHeader.h"
#include "Sha256.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

class DAQSetupCancelled : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

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
  void VerifyRuntimeConfiguration(int handle);
  void VerifyAcquisitionStartCapacity();
  void WaitForBoardReady(int handle);
  void CheckRuntimeHealthAndStorage(int handle, bool strict_readback,
                                    bool require_running);
  void WaitForAcquisitionRunning(int handle);
  void WriteRuntimeArtifacts();
  void FinalizeRawOutput(bool truncate_to_complete_prefix);
  void AcquisitionLoop(std::atomic<bool>& is_running);
  void CheckSetupCancellation() const;

  std::string config_file_;
  std::string config_contents_;
  ConfigParser config_;
  // Parsed before digitizer_ construction so invalid settings cannot open hardware.
  DAQHardwareSettings hardware_settings_;
  std::string output_file_;
  std::string working_output_file_;
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
  std::string raw_digest_method_ = "pending";
  bool raw_output_finalized_ = false;
  bool raw_output_published_ = false;
  bool raw_recovery_performed_ = false;
  bool lost_events_exact_ = true;
  uint64_t raw_events_before_recovery_ = 0;
  std::string raw_finalization_error_;
  uint64_t recorded_events_ = 0;
  uint64_t lost_events_ = 0;
  bool loss_policy_evaluation_observed_ = false;
  bool loss_policy_exceeded_ = false;
  bool loss_policy_rejected_post_gap_event_ = false;
  uint64_t loss_policy_observed_events_ = 0;
  uint64_t loss_policy_observed_lost_events_ = 0;
  double loss_policy_observed_lost_fraction_ = 0.0;
  bool trigger_time_tag_observed_ = false;
  uint64_t first_extended_ttt_ = 0;
  uint64_t last_extended_ttt_ = 0;
  uint64_t complete_raw_bytes_ = 0;
  uint64_t expected_raw_bytes_ = 0;
  uint64_t output_free_bytes_at_start_ = 0;
  uint64_t output_free_bytes_at_end_ = 0;
  bool output_free_bytes_at_end_known_ = false;
  std::string termination_reason_ = "not_started";
  std::time_t hardware_verified_unix_time_ = 0;
  std::time_t acquisition_start_unix_time_ = 0;
  std::time_t acquisition_end_unix_time_ = 0;
  uint64_t readout_error_count_ = 0;
  uint64_t health_check_count_ = 0;
  uint64_t health_read_error_count_ = 0;
  uint32_t consecutive_health_read_errors_ = 0;
  uint32_t latest_status_register_ = 0;
  uint32_t latest_board_failure_register_ = 0;
  float latest_max_temperature_c_ = 0.0F;
  bool health_readback_available_ = false;
  uint64_t runtime_configuration_checks_ = 0;
  // Counts only failures returned by zmq_send(DONTWAIT).  A PUB socket can
  // silently discard per-subscriber messages at HWM, so this is deliberately
  // not called a delivery/drop counter.
  uint64_t zmq_send_failure_count_ = 0;
  uint64_t zmq_send_error_count_ = 0;
  uint32_t zmq_send_hwm_messages_ = 0;
  uint64_t zmq_send_hwm_approx_bytes_ = 0;
  std::array<bool, MAX_CH> temperature_observed_{};
  std::array<uint32_t, MAX_CH> max_temperature_c_{};
  int run_number_;
  int max_events_;
  int run_time_sec_;
  std::atomic<bool> running_;
  const std::atomic<bool>* cancellation_flag_ = nullptr;
  std::unique_ptr<CaenDigitizer> digitizer_;
  std::FILE* output_stream_ = nullptr;
  Sha256Accumulator raw_output_digest_;
  std::vector<char> output_write_buffer_;
  std::array<ChannelRuntimeCalibration, MAX_CH> runtime_channels_{};
  std::array<uint32_t, MAX_CH / 2> pair_logic_readback_{};
  uint32_t global_trigger_mask_readback_ = 0;
  uint32_t channel_mask_readback_ = 0;
  uint32_t post_trigger_readback_ = 0;
  uint32_t clock_source_readback_ = 0;
  int run_sync_mode_readback_ = -1;
  
  // ZMQ (ZeroMQ for real-time monitoring)
  void *zmq_ctx_ = nullptr;
  void *zmq_pub_ = nullptr;
  
  // Zero-Copy Memory Pool
  std::vector<char> raw_buffer_pool_;
};

#endif // DAQ_MANAGER_H
