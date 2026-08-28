#include "DAQConfig.h"

#include <cmath>
#include <limits>
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

uint32_t OptionalUnsigned(const ConfigParser& config,
                          const std::string& section,
                          const std::string& key, uint32_t default_value,
                          uint32_t min_value, uint32_t max_value) {
  const int value = config.GetInt(section, key, static_cast<int>(default_value));
  if (value < 0 || static_cast<uint32_t>(value) < min_value ||
      static_cast<uint32_t>(value) > max_value) {
    throw std::runtime_error(
        "Config value out of range [" + section + "] " + key + "=" +
        std::to_string(value) + " (expected " + std::to_string(min_value) +
        ".." + std::to_string(max_value) + ")");
  }
  return static_cast<uint32_t>(value);
}

double OptionalFiniteDouble(const ConfigParser& config,
                            const std::string& section,
                            const std::string& key, double default_value,
                            double min_exclusive, double max_inclusive) {
  const double value = config.GetDouble(section, key, default_value);
  if (!std::isfinite(value) || value <= min_exclusive ||
      value > max_inclusive) {
    throw std::runtime_error(
        "Config value out of range [" + section + "] " + key);
  }
  return value;
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
  settings.input_range_mv = OptionalUnsigned(
      config, "Digitizer", "InputRangeMv", 2000, 500, 2000);
  if (settings.input_range_mv != 500 && settings.input_range_mv != 2000) {
    throw std::runtime_error(
        "[Digitizer] InputRangeMv must be exactly 500 or 2000");
  }
  settings.adc_bits = OptionalUnsigned(config, "Digitizer", "ADCBits", 14,
                                       14, 14);
  settings.trigger_polarity =
      config.GetRequiredInt("Digitizer", "TriggerPolarity", 0, 1);
  settings.ext_trigger_mode =
      config.GetRequiredInt("Digitizer", "ExtTriggerMode", 0, 1);
  settings.self_trigger_mode =
      config.GetRequiredInt("Digitizer", "SelfTriggerMode", 0, 1);
  settings.clock_source =
      config.GetInt("Synchronization", "ClockSource", 0);
  if (settings.clock_source < 0 || settings.clock_source > 1) {
    throw std::runtime_error(
        "[Synchronization] ClockSource must be 0 (internal) or 1 (external)");
  }
  settings.run_sync_mode =
      config.GetInt("Synchronization", "RunSyncMode", 0);
  if (settings.run_sync_mode < 0 || settings.run_sync_mode > 4) {
    throw std::runtime_error(
        "[Synchronization] RunSyncMode must be in the CAEN range 0..4");
  }

  auto& calibration = settings.trigger_calibration;
  calibration.settling_time_ms = OptionalUnsigned(
      config, "TriggerCalibration", "SettlingTimeMs", 3000, 0, 600000);
  calibration.settling_timeout_ms = OptionalUnsigned(
      config, "TriggerCalibration", "SettlingTimeoutMs", 15000, 1,
      600000);
  calibration.measurement_events = OptionalUnsigned(
      config, "TriggerCalibration", "MeasurementEvents", 32, 1, 10000);
  calibration.stability_tolerance_adc = OptionalFiniteDouble(
      config, "TriggerCalibration", "StabilityToleranceAdc", 2.0, 0.0,
      16383.0);
  calibration.stable_measurements = OptionalUnsigned(
      config, "TriggerCalibration", "StableMeasurements", 3, 2, 100);
  if (calibration.settling_timeout_ms <= calibration.settling_time_ms) {
    throw std::runtime_error(
        "[TriggerCalibration] SettlingTimeoutMs must be greater than "
        "SettlingTimeMs");
  }

  const uint32_t minimum_free_mib = OptionalUnsigned(
      config, "Storage", "MinimumFreeMiB", 1024, 64, 1048576);
  const uint32_t stop_free_mib = OptionalUnsigned(
      config, "Storage", "StopFreeMiB", 512, 32, 1048575);
  if (stop_free_mib >= minimum_free_mib) {
    throw std::runtime_error(
        "[Storage] StopFreeMiB must be smaller than MinimumFreeMiB");
  }
  settings.storage.minimum_free_bytes =
      static_cast<uint64_t>(minimum_free_mib) * 1024U * 1024U;
  settings.storage.stop_free_bytes =
      static_cast<uint64_t>(stop_free_mib) * 1024U * 1024U;

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
      auto& channel = settings.channels[ch];
      channel.dc_offset = static_cast<uint32_t>(
          config.GetRequiredInt(section, "DCOffset", 0, 65535));

      const bool has_absolute = HasValue(config, section, "TriggerThreshold");
      const bool has_relative =
          HasValue(config, section, "TriggerThresholdMv");
      const bool participates_in_self_trigger =
          settings.self_trigger_mode != 0 &&
          ((settings.self_trigger_mask >> ch) & 1U) != 0U;
      if (has_absolute && has_relative) {
        throw std::runtime_error(
            "[" + section +
            "] TriggerThresholdMv and TriggerThreshold are mutually exclusive");
      }
      if (participates_in_self_trigger && !has_absolute && !has_relative) {
        throw std::runtime_error(
            "[" + section +
            "] a self-trigger channel requires TriggerThresholdMv or "
            "TriggerThreshold");
      }

      if (has_relative) {
        channel.has_trigger_threshold = true;
        channel.threshold_is_relative_mv = true;
        channel.trigger_threshold_mv = OptionalFiniteDouble(
            config, section, "TriggerThresholdMv", 0.0, 0.0,
            static_cast<double>(settings.input_range_mv));
        if (channel.trigger_threshold_mv >=
            static_cast<double>(settings.input_range_mv)) {
          throw std::runtime_error(
              "[" + section +
              "] TriggerThresholdMv must be smaller than InputRangeMv");
        }
        const double adc_delta =
            channel.trigger_threshold_mv *
            static_cast<double>(1U << settings.adc_bits) /
            static_cast<double>(settings.input_range_mv);
        const long long rounded_delta = std::llround(adc_delta);
        if (rounded_delta <= 0 ||
            rounded_delta >= static_cast<long long>(1U << settings.adc_bits)) {
          throw std::runtime_error(
              "[" + section +
              "] TriggerThresholdMv rounds outside the representable ADC "
              "delta range");
        }
      } else {
        channel.has_trigger_threshold = has_absolute;
        if (!has_absolute) continue;
        channel.trigger_threshold = static_cast<uint32_t>(
            config.GetRequiredInt(section, "TriggerThreshold", 0, 16383));
      }
    }
  }

  return settings;
}
