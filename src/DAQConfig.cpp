#include "DAQConfig.h"

#include <stdexcept>
#include <string>

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
