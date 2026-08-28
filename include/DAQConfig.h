#ifndef DAQ_CONFIG_H
#define DAQ_CONFIG_H

#include "ConfigParser.h"
#include "EventHeader.h"

#include <array>
#include <cstdint>

struct DAQChannelSettings {
  uint32_t dc_offset = 0;
  uint32_t trigger_threshold = 0;
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
  int trigger_polarity = 0;
  int ext_trigger_mode = 0;
  int self_trigger_mode = 0;
  bool explicit_trigger_routing = false;
  DAQPairLogic pair_logic = DAQPairLogic::kOr;
  std::array<DAQChannelSettings, MAX_CH> channels{};
};

// CAEN 장비를 열기 전에 호출할 수 있도록 표준 C++에만 의존합니다.
DAQHardwareSettings LoadDAQHardwareSettings(const ConfigParser& config);

#endif  // DAQ_CONFIG_H
