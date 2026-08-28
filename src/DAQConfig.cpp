#include "DAQConfig.h"

#include <stdexcept>
#include <string>

namespace {

bool HasValue(const ConfigParser& config, const std::string& section,
              const std::string& key) {
  // ConfigParser rejects empty values, so an empty fallback unambiguously means
  // that the key is absent.
  return !config.GetString(section, key, "").empty();
}

DAQPairLogic ParsePairLogic(const ConfigParser& config) {
  const std::string value =
      config.GetString("HardwareCoincidence", "PairLogic", "");
  if (value == "AND") return DAQPairLogic::kAnd;
  if (value == "OR") return DAQPairLogic::kOr;
  throw std::runtime_error(
      "[HardwareCoincidence] PairLogic must be AND or OR");
}

bool ContainsPartialPair(uint32_t mask) {
  for (int even_ch = 0; even_ch < MAX_CH; even_ch += 2) {
    const uint32_t pair_bits = (mask >> even_ch) & 0x3U;
    if (pair_bits == 0x1U || pair_bits == 0x2U) return true;
  }
  return false;
}

}  // namespace

DAQHardwareSettings LoadDAQHardwareSettings(const ConfigParser& config) {
  DAQHardwareSettings settings;
  settings.record_length = static_cast<uint32_t>(
      config.GetRequiredInt("Digitizer", "RecordLength", 128, 102400));
  settings.channel_mask = static_cast<uint32_t>(
      config.GetRequiredInt("Digitizer", "ChannelMask", 1, (1 << MAX_CH) - 1));
  settings.post_trigger = static_cast<uint32_t>(
      config.GetRequiredInt("Digitizer", "PostTrigger", 0, 100));
  settings.trigger_polarity =
      config.GetRequiredInt("Digitizer", "TriggerPolarity", 0, 1);
  settings.ext_trigger_mode =
      config.GetRequiredInt("Digitizer", "ExtTriggerMode", 0, 1);
  settings.self_trigger_mode =
      config.GetRequiredInt("Digitizer", "SelfTriggerMode", 0, 1);

  const bool has_self_trigger_mask =
      HasValue(config, "Digitizer", "SelfTriggerMask");
  const bool has_pair_logic =
      HasValue(config, "HardwareCoincidence", "PairLogic");
  const int trigger_setting_count = static_cast<int>(has_self_trigger_mask) +
                                    static_cast<int>(has_pair_logic);

  if (trigger_setting_count != 0 && trigger_setting_count != 2) {
    throw std::runtime_error(
        "[Digitizer] SelfTriggerMask and [HardwareCoincidence] PairLogic "
        "must be specified together");
  }

  if (trigger_setting_count == 0) {
    // Backward-compatible behavior for configurations written before readout
    // and self-trigger masks were separated. The hardware setup leaves the
    // legacy pair pulse/logic registers untouched in this mode.
    settings.self_trigger_mask =
        settings.self_trigger_mode != 0 ? settings.channel_mask : 0U;
    settings.pair_logic = DAQPairLogic::kOr;
  } else {
    settings.explicit_trigger_routing = true;
    settings.self_trigger_mask = static_cast<uint32_t>(config.GetRequiredInt(
        "Digitizer", "SelfTriggerMask", 0, (1 << MAX_CH) - 1));
    settings.pair_logic = ParsePairLogic(config);
  }

  if (settings.record_length % 8 != 0) {
    throw std::runtime_error("[Digitizer] RecordLength must be a multiple of 8");
  }
  if (static_cast<uint64_t>(settings.record_length) *
          (100U - settings.post_trigger) <
      8000U) {
    throw std::runtime_error(
        "[Digitizer] RecordLength/PostTrigger leave less than 160 ns pre-trigger time");
  }
  if (settings.ext_trigger_mode == 0 && settings.self_trigger_mode == 0) {
    throw std::runtime_error(
        "[Digitizer] ExtTriggerMode and SelfTriggerMode cannot both be disabled");
  }
  if ((settings.self_trigger_mask & ~settings.channel_mask) != 0U) {
    throw std::runtime_error(
        "[Digitizer] SelfTriggerMask must be a subset of ChannelMask");
  }

  if (settings.self_trigger_mode == 0) {
    if (settings.self_trigger_mask != 0U) {
      throw std::runtime_error(
          "[Digitizer] SelfTriggerMask must be 0 when SelfTriggerMode is disabled");
    }
  } else {
    if (settings.self_trigger_mask == 0U) {
      throw std::runtime_error(
          "[Digitizer] SelfTriggerMask must enable at least one channel when "
          "SelfTriggerMode is enabled");
    }
    if (settings.pair_logic == DAQPairLogic::kAnd &&
        ContainsPartialPair(settings.self_trigger_mask)) {
      throw std::runtime_error(
          "[HardwareCoincidence] PairLogic=AND requires complete adjacent "
          "channel pairs (CH0/1, CH2/3, CH4/5, or CH6/7)");
    }
  }

  for (int ch = 0; ch < MAX_CH; ++ch) {
    if ((settings.channel_mask >> ch) & 1U) {
      const std::string section = "Channel_" + std::to_string(ch);
      settings.channels[ch].dc_offset = static_cast<uint32_t>(
          config.GetRequiredInt(section, "DCOffset", 0, 65535));
      settings.channels[ch].trigger_threshold = static_cast<uint32_t>(
          config.GetRequiredInt(section, "TriggerThreshold", 0, 16383));
    }
  }

  return settings;
}
