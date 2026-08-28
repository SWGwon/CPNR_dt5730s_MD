#ifndef DAQ_CONFIG_H
#define DAQ_CONFIG_H

#include "ConfigParser.h"
#include "EventHeader.h"

#include <array>
#include <cstdint>

struct DAQChannelSettings {
  uint32_t dc_offset = 0;
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

enum class DAQPairLogic : uint32_t {
  kAnd = 0,
  kOr = 3,
};

struct DAQHardwareSettings {
  uint32_t record_length = 0;
  uint32_t channel_mask = 0;
  uint32_t self_trigger_mask = 0;
  uint32_t post_trigger = 0;
  uint32_t input_range_mv = 2000;
  uint32_t adc_bits = 14;
  int trigger_polarity = 0;
  int ext_trigger_mode = 0;
  int self_trigger_mode = 0;
  int clock_source = 0;
  int run_sync_mode = 0;
  bool explicit_trigger_routing = false;
  DAQPairLogic pair_logic = DAQPairLogic::kOr;
  DAQTriggerCalibrationSettings trigger_calibration{};
  std::array<DAQChannelSettings, MAX_CH> channels{};
};

// CAEN 장비를 열기 전에 호출할 수 있도록 표준 C++에만 의존합니다.
DAQHardwareSettings LoadDAQHardwareSettings(const ConfigParser& config);

#endif  // DAQ_CONFIG_H
