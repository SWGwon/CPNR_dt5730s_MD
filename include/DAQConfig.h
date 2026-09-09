#ifndef DAQ_CONFIG_H
#define DAQ_CONFIG_H

#include "ConfigParser.h"
#include "DataQuality.h"
#include "EventHeader.h"
#include "WaveformDsp.h"

#include <array>
#include <cstdint>
#include <string>

inline constexpr double kMinimumSoftwareRandomTriggerRateHz = 0.001;
inline constexpr double kMaximumSoftwareRandomTriggerRateHz = 100000.0;

struct DAQConnectionSettings {
  std::string type = "USB";
  int link = 0;
  int node = 0;
  uint32_t base_address = 0;
  std::string expected_model = "DT5730";
  bool has_expected_serial = false;
  uint32_t expected_serial = 0;
};

enum class DAQDCOffsetMode {
  // Backward-compatible expert mode: write the configured 16-bit DAC code
  // exactly and verify its readback.
  kRawDac,
  // Operator mode: use the configured percentage as a measured 14-bit ADC
  // baseline target and tune the per-channel DAC before trigger setup.
  kTargetBaseline,
};

struct DAQChannelSettings {
  DAQDCOffsetMode dc_offset_mode = DAQDCOffsetMode::kRawDac;
  uint32_t dc_offset = 0;
  double baseline_target_percent = 0.0;
  uint32_t target_baseline_adc = 0;
  bool has_trigger_threshold = false;
  // Legacy configurations provide an absolute ADC discriminator code.  New
  // configurations keep the user's voltage request and let DAQManager derive
  // an absolute code from the measured, per-channel baseline.
  bool threshold_is_relative_mv = false;
  double trigger_threshold_mv = 0.0;
  uint32_t trigger_threshold = 0;
};

struct DAQTriggerCalibrationSettings {
  uint32_t settling_time_ms = 3000;
  uint32_t settling_timeout_ms = 15000;
  uint32_t measurement_events = 32;
  double stability_tolerance_adc = 2.0;
  uint32_t stable_measurements = 3;
};

struct DAQDCOffsetCalibrationSettings {
  // Placement tolerance is intentionally separate from the much tighter
  // baseline-stability tolerance used by trigger calibration.
  double target_tolerance_percent = 0.5;
  uint32_t max_adjustment_iterations = 8;
  uint32_t dac_busy_timeout_ms = 1000;
  uint32_t step_settling_time_ms = 3000;
};

struct DAQStorageSettings {
  // Keep enough space for runtime metadata and an orderly failed-run prefix.
  // The preflight reserve is added to the exact raw size for event-limited
  // runs.  The runtime stop watermark prevents ENOSPC from being the normal
  // mechanism that ends an unlimited/time-limited run.
  uint64_t minimum_free_bytes = uint64_t{1024} * 1024U * 1024U;
  uint64_t stop_free_bytes = uint64_t{512} * 1024U * 1024U;
};

struct DAQSoftwareDspSettings {
  uint32_t coincidence_window_ns = 20;
  cpnr::WaveformDspSettings waveform{};
};

enum class DAQPairLogic : uint32_t {
  kAnd = 0,
  kOr = 3,
};

struct DAQHardwareSettings {
  DAQConnectionSettings connection{};
  uint32_t record_length = 0;
  uint32_t channel_mask = 0;
  uint32_t self_trigger_mask = 0;
  uint32_t post_trigger = 0;
  uint32_t input_range_mv = 2000;
  uint32_t adc_bits = 14;
  int trigger_polarity = 0;
  int ext_trigger_mode = 0;
  int self_trigger_mode = 0;
  int software_random_trigger_mode = 0;
  double software_random_trigger_rate_hz = 0.0;
  int clock_source = 0;
  int run_sync_mode = 0;
  bool explicit_trigger_routing = false;
  DAQPairLogic pair_logic = DAQPairLogic::kOr;
  DAQTriggerCalibrationSettings trigger_calibration{};
  DAQDCOffsetCalibrationSettings dc_offset_calibration{};
  DAQStorageSettings storage{};
  DAQSoftwareDspSettings software_dsp{};
  cpnr::LostEventPolicy lost_event_policy{};
  std::array<DAQChannelSettings, MAX_CH> channels{};
};

// Pure conversion helpers shared by parsing, runtime calibration, and tests.
// The DAC result is only an initial seed; the physical baseline must be
// measured and corrected independently for every channel.
uint32_t BaselinePercentToAdc(double percent, uint32_t adc_bits);
uint32_t BaselinePercentToInitialDac(double percent);

enum class DAQRecordLengthContract {
  // Hardware acquisition contract for DT5730/x730 standard waveform firmware.
  kCurrentX730,
  // Authenticated artifacts written by older releases used an 8-sample
  // software contract. This mode is for offline replay/validation only.
  kLegacyMultipleOf8,
  // Recovery scans may receive either kind of frozen config and never touch
  // hardware, so they can safely accept the union of both contracts.
  kLegacyOrCurrent,
};

enum class DAQWaveformDspContract {
  // Current acquisition/ROOT schema 3: charge uses the fixed peak-centered
  // window. The configurable threshold/gates are retained only as provenance.
  kPeakCentered,
  // ROOT schemas 1/2: threshold crossing and configurable gates determine the
  // stored charge, so their historical bounds remain mandatory.
  kLegacyThresholdGates,
};

// CAEN 장비를 열기 전에 호출할 수 있도록 표준 C++에만 의존합니다.
DAQHardwareSettings LoadDAQHardwareSettings(
    const ConfigParser& config,
    DAQRecordLengthContract record_length_contract =
        DAQRecordLengthContract::kCurrentX730,
    DAQWaveformDspContract waveform_dsp_contract =
        DAQWaveformDspContract::kPeakCentered);

#endif  // DAQ_CONFIG_H
