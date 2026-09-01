#include "DAQConfig.h"
#include "DT5730Constraints.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using AllowedKeys = std::set<std::string>;
using ConfigSchema = std::map<std::string, AllowedKeys>;

const ConfigSchema& DAQConfigSchema() {
  static const ConfigSchema schema = [] {
    ConfigSchema result{
        {"Connection",
         {"Type", "Link", "Node", "BaseAddress", "ExpectedModel",
          "ExpectedSerial"}},
        {"Digitizer",
         {"RecordLength", "ChannelMask", "SelfTriggerMask", "PostTrigger",
          "InputRangeMv", "ADCBits", "TriggerPolarity", "ExtTriggerMode",
          "SelfTriggerMode", "SoftwareRandomTriggerMode",
          "SoftwareRandomTriggerRateHz"}},
        {"HardwareCoincidence", {"PairLogic"}},
        {"Synchronization", {"ClockSource", "RunSyncMode"}},
        {"TriggerCalibration",
         {"SettlingTimeMs", "SettlingTimeoutMs", "MeasurementEvents",
          "StabilityToleranceAdc", "StableMeasurements"}},
        {"Storage", {"MinimumFreeMiB", "StopFreeMiB"}},
        {"DataQuality", {"MaxLostEvents", "MaxLostFraction"}},
        {"SoftwareDSP",
         {"CoincidenceWindow", "BaselineSamples", "ShortGate", "LongGate",
          "PulseStartThresholdAdc"}},
    };
    for (int channel = 0; channel < MAX_CH; ++channel) {
      result.emplace("Channel_" + std::to_string(channel),
                     AllowedKeys{"DCOffset", "TriggerThreshold",
                                 "TriggerThresholdMv"});
    }
    return result;
  }();
  return schema;
}

std::size_t EditDistance(const std::string& lhs, const std::string& rhs) {
  std::vector<std::size_t> previous(rhs.size() + 1U);
  std::vector<std::size_t> current(rhs.size() + 1U);
  for (std::size_t column = 0; column <= rhs.size(); ++column) {
    previous[column] = column;
  }
  for (std::size_t row = 1; row <= lhs.size(); ++row) {
    current[0] = row;
    for (std::size_t column = 1; column <= rhs.size(); ++column) {
      const std::size_t substitution =
          previous[column - 1U] +
          (lhs[row - 1U] == rhs[column - 1U] ? 0U : 1U);
      current[column] = std::min(
          {previous[column] + 1U, current[column - 1U] + 1U,
           substitution});
    }
    previous.swap(current);
  }
  return previous[rhs.size()];
}

template <typename Candidates>
std::string SimilarName(const std::string& unknown,
                        const Candidates& candidates) {
  std::string closest;
  std::size_t closest_distance = std::numeric_limits<std::size_t>::max();
  for (const auto& candidate_entry : candidates) {
    const std::string& candidate = [&]() -> const std::string& {
      if constexpr (std::is_same_v<typename Candidates::value_type,
                                   std::string>) {
        return candidate_entry;
      } else {
        return candidate_entry.first;
      }
    }();
    const std::size_t distance = EditDistance(unknown, candidate);
    if (distance < closest_distance) {
      closest_distance = distance;
      closest = candidate;
    }
  }
  const std::size_t suggestion_limit =
      std::max<std::size_t>(2U, unknown.size() / 3U);
  return closest_distance <= suggestion_limit ? closest : std::string{};
}

void ValidateDAQConfigSchema(const ConfigParser& config) {
  const ConfigSchema& schema = DAQConfigSchema();
  for (const auto& [section, key_values] : config.GetSections()) {
    if (section.rfind("Channel_", 0U) == 0U) {
      const std::string suffix = section.substr(std::string("Channel_").size());
      if (suffix.size() != 1U || suffix[0] < '0' || suffix[0] > '9' ||
          static_cast<int>(suffix[0] - '0') >= MAX_CH) {
        throw std::runtime_error(
            "Invalid DAQ channel section [" + section +
            "] (expected Channel_0..Channel_" +
            std::to_string(MAX_CH - 1) + ")");
      }
    }

    const auto section_it = schema.find(section);
    if (section_it == schema.end()) {
      std::string message = "Unknown DAQ config section [" + section + "]";
      const std::string suggestion = SimilarName(section, schema);
      if (!suggestion.empty()) {
        message += "; did you mean [" + suggestion + "]?";
      }
      throw std::runtime_error(message);
    }

    for (const auto& [key, unused_value] : key_values) {
      (void)unused_value;
      if (section_it->second.count(key) != 0U) continue;
      std::string message =
          "Unknown DAQ config key [" + section + "] " + key;
      const std::string suggestion = SimilarName(key, section_it->second);
      if (!suggestion.empty()) {
        message += "; did you mean " + suggestion + "?";
      }
      throw std::runtime_error(message);
    }
  }
}

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

DAQHardwareSettings LoadDAQHardwareSettings(
    const ConfigParser& config,
    DAQRecordLengthContract record_length_contract,
    DAQWaveformDspContract waveform_dsp_contract) {
  ValidateDAQConfigSchema(config);
  DAQHardwareSettings settings;
  settings.connection.type = config.GetString("Connection", "Type", "USB");
  if (settings.connection.type != "USB") {
    throw std::runtime_error(
        "[Connection] Type currently supports only USB");
  }
  settings.connection.link =
      config.GetInt("Connection", "Link", 0);
  settings.connection.node =
      config.GetInt("Connection", "Node", 0);
  settings.connection.base_address = static_cast<uint32_t>(
      config.GetInt("Connection", "BaseAddress", 0));
  if (settings.connection.link < 0 || settings.connection.link > 127) {
    throw std::runtime_error("[Connection] Link must be in range 0..127");
  }
  if (settings.connection.node != 0 ||
      settings.connection.base_address != 0U) {
    throw std::runtime_error(
        "[Connection] USB requires Node=0 and BaseAddress=0");
  }
  settings.connection.expected_model =
      config.GetString("Connection", "ExpectedModel", "DT5730");
  if (settings.connection.expected_model.empty()) {
    throw std::runtime_error(
        "[Connection] ExpectedModel must not be empty");
  }
  if (HasValue(config, "Connection", "ExpectedSerial")) {
    settings.connection.has_expected_serial = true;
    settings.connection.expected_serial = static_cast<uint32_t>(
        config.GetRequiredInt("Connection", "ExpectedSerial", 1,
                              std::numeric_limits<int>::max()));
  }
  settings.record_length = static_cast<uint32_t>(
      config.GetRequiredInt(
          "Digitizer", "RecordLength",
          static_cast<int>(
              dt5730_constraints::kMinimumRecordLengthSamples),
          static_cast<int>(
              dt5730_constraints::kMaximumRecordLengthSamples)));
  settings.channel_mask = static_cast<uint32_t>(
      config.GetRequiredInt("Digitizer", "ChannelMask", 1, (1 << MAX_CH) - 1));
  settings.post_trigger = static_cast<uint32_t>(
      config.GetRequiredInt("Digitizer", "PostTrigger", 0, 100));
  const bool current_record_length =
      dt5730_constraints::IsSupportedRecordLength(settings.record_length);
  const bool legacy_record_length =
      settings.record_length >=
          dt5730_constraints::kMinimumRecordLengthSamples &&
      settings.record_length <=
          dt5730_constraints::kMaximumRecordLengthSamples &&
      settings.record_length % 8U == 0U;
  const bool accepted_record_length =
      record_length_contract == DAQRecordLengthContract::kCurrentX730
          ? current_record_length
          : record_length_contract ==
                    DAQRecordLengthContract::kLegacyMultipleOf8
              ? legacy_record_length
              : current_record_length || legacy_record_length;
  if (!accepted_record_length) {
    const std::string required_granularity =
        record_length_contract == DAQRecordLengthContract::kCurrentX730
            ? "a multiple of 10"
            : record_length_contract ==
                      DAQRecordLengthContract::kLegacyMultipleOf8
                  ? "a legacy multiple of 8"
                  : "a multiple of 10 or a legacy multiple of 8";
    throw std::runtime_error(
        "[Digitizer] RecordLength must be in range " +
        std::to_string(dt5730_constraints::kMinimumRecordLengthSamples) +
        ".." +
        std::to_string(dt5730_constraints::kMaximumRecordLengthSamples) +
        " and " + required_granularity);
  }
  struct TimingRegion {
    uint32_t pre_trigger_samples;
    uint32_t post_trigger_samples;
  };
  std::vector<TimingRegion> timing_regions;
  if (record_length_contract !=
          DAQRecordLengthContract::kLegacyMultipleOf8 &&
      current_record_length) {
    const auto post_trigger_layout =
        dt5730_constraints::PredictPostTriggerLayout(
            settings.record_length, settings.post_trigger);
    timing_regions.push_back({post_trigger_layout.pre_trigger_samples,
                              post_trigger_layout.post_trigger_samples});
  }
  if (record_length_contract != DAQRecordLengthContract::kCurrentX730 &&
      legacy_record_length) {
    // Preserve the exact offline parsing semantics used by artifacts written
    // before the hardware-native 10-sample contract was introduced.
    const uint32_t legacy_pre_trigger_samples = static_cast<uint32_t>(
        static_cast<uint64_t>(settings.record_length) *
        (100U - settings.post_trigger) / 100U);
    timing_regions.push_back(
        {legacy_pre_trigger_samples,
         settings.record_length - legacy_pre_trigger_samples});
  }
  const auto usable_timing_region = std::find_if(
      timing_regions.begin(), timing_regions.end(), [](const TimingRegion& r) {
        return r.pre_trigger_samples >=
               dt5730_constraints::kMinimumPreTriggerSamples;
      });
  if (usable_timing_region == timing_regions.end()) {
    throw std::runtime_error(
        "[Digitizer] RecordLength/PostTrigger leave less than 160 ns "
        "pre-trigger time under the selected record-length contract");
  }
  // Current timing is preferred for defaults when an unauthenticated recovery
  // config lies on both grids. Explicit DSP values are validated against each
  // complete candidate below, so a legacy pre region is never mixed with a
  // current post region (or vice versa).
  const uint32_t pre_trigger_samples =
      usable_timing_region->pre_trigger_samples;
  const uint32_t post_trigger_samples =
      usable_timing_region->post_trigger_samples;
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
  settings.software_random_trigger_mode =
      config.GetInt("Digitizer", "SoftwareRandomTriggerMode", 0);
  if (settings.software_random_trigger_mode < 0 ||
      settings.software_random_trigger_mode > 1) {
    throw std::runtime_error(
        "[Digitizer] SoftwareRandomTriggerMode must be 0 or 1");
  }
  const bool has_software_random_rate =
      HasValue(config, "Digitizer", "SoftwareRandomTriggerRateHz");
  if (settings.software_random_trigger_mode != 0) {
    if (!has_software_random_rate) {
      throw std::runtime_error(
          "[Digitizer] SoftwareRandomTriggerRateHz is required when "
          "SoftwareRandomTriggerMode is enabled");
    }
    settings.software_random_trigger_rate_hz =
        config.GetDouble("Digitizer", "SoftwareRandomTriggerRateHz", 0.0);
    if (!std::isfinite(settings.software_random_trigger_rate_hz) ||
        settings.software_random_trigger_rate_hz <
            kMinimumSoftwareRandomTriggerRateHz ||
        settings.software_random_trigger_rate_hz >
            kMaximumSoftwareRandomTriggerRateHz) {
      throw std::runtime_error(
          "[Digitizer] SoftwareRandomTriggerRateHz must be finite and in "
          "range " +
          std::to_string(kMinimumSoftwareRandomTriggerRateHz) + ".." +
          std::to_string(kMaximumSoftwareRandomTriggerRateHz) + " Hz");
    }
  } else if (has_software_random_rate) {
    settings.software_random_trigger_rate_hz =
        config.GetDouble("Digitizer", "SoftwareRandomTriggerRateHz", 0.0);
    if (!std::isfinite(settings.software_random_trigger_rate_hz) ||
        settings.software_random_trigger_rate_hz != 0.0) {
      throw std::runtime_error(
          "[Digitizer] SoftwareRandomTriggerRateHz must be 0 when "
          "SoftwareRandomTriggerMode is disabled");
    }
  }
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

  settings.lost_event_policy.max_lost_events = OptionalUnsigned(
      config, "DataQuality", "MaxLostEvents", 0, 0,
      std::numeric_limits<uint32_t>::max());
  settings.lost_event_policy.max_lost_fraction =
      config.GetDouble("DataQuality", "MaxLostFraction", 0.0);
  if (!std::isfinite(settings.lost_event_policy.max_lost_fraction) ||
      settings.lost_event_policy.max_lost_fraction < 0.0 ||
      settings.lost_event_policy.max_lost_fraction > 1.0) {
    throw std::runtime_error(
        "[DataQuality] MaxLostFraction must be finite and in range 0..1");
  }

  const uint32_t default_baseline_samples = static_cast<uint32_t>(
      std::min<uint32_t>(150U, pre_trigger_samples));
  const uint32_t default_long_gate_samples = static_cast<uint32_t>(
      std::min<uint32_t>(200U, post_trigger_samples));
  const uint32_t default_short_gate_samples =
      std::min<uint32_t>(40U, default_long_gate_samples);
  const bool legacy_threshold_gates =
      waveform_dsp_contract ==
      DAQWaveformDspContract::kLegacyThresholdGates;

  settings.software_dsp.coincidence_window_ns = OptionalUnsigned(
      config, "SoftwareDSP", "CoincidenceWindow", 20, 1, 1000000);
  auto& waveform_dsp = settings.software_dsp.waveform;
  waveform_dsp.baseline_samples = OptionalUnsigned(
      config, "SoftwareDSP", "BaselineSamples", default_baseline_samples, 1,
      settings.record_length);
  waveform_dsp.short_gate_samples = OptionalUnsigned(
      config, "SoftwareDSP", "ShortGate", default_short_gate_samples, 1,
      legacy_threshold_gates
          ? settings.record_length
          : static_cast<uint32_t>(std::numeric_limits<int>::max()));
  waveform_dsp.long_gate_samples = OptionalUnsigned(
      config, "SoftwareDSP", "LongGate", default_long_gate_samples, 1,
      legacy_threshold_gates
          ? settings.record_length
          : static_cast<uint32_t>(std::numeric_limits<int>::max()));
  if (legacy_threshold_gates) {
    waveform_dsp.pulse_start_threshold_adc = OptionalFiniteDouble(
        config, "SoftwareDSP", "PulseStartThresholdAdc", 30.0, 0.0,
        static_cast<double>((1U << settings.adc_bits) - 1U));
  } else {
    waveform_dsp.pulse_start_threshold_adc =
        config.GetDouble("SoftwareDSP", "PulseStartThresholdAdc", 30.0);
    const double maximum_threshold_adc =
        static_cast<double>((1U << settings.adc_bits) - 1U);
    if (!std::isfinite(waveform_dsp.pulse_start_threshold_adc) ||
        waveform_dsp.pulse_start_threshold_adc < 0.0 ||
        waveform_dsp.pulse_start_threshold_adc > maximum_threshold_adc) {
      throw std::runtime_error(
          "Config value out of range [SoftwareDSP] PulseStartThresholdAdc");
    }
  }

  if (legacy_threshold_gates &&
      waveform_dsp.short_gate_samples > waveform_dsp.long_gate_samples) {
    throw std::runtime_error(
        "[SoftwareDSP] ShortGate must not exceed LongGate");
  }
  const auto timing_region_accepts_dsp =
      [&](const TimingRegion& region) {
        return region.pre_trigger_samples >=
                   dt5730_constraints::kMinimumPreTriggerSamples &&
               waveform_dsp.baseline_samples <= region.pre_trigger_samples &&
               (!legacy_threshold_gates ||
                waveform_dsp.long_gate_samples <=
                    region.post_trigger_samples);
      };
  if (std::none_of(timing_regions.begin(), timing_regions.end(),
                   timing_region_accepts_dsp)) {
    const auto eligible_region = [](const TimingRegion& region) {
      return region.pre_trigger_samples >=
             dt5730_constraints::kMinimumPreTriggerSamples;
    };
    const bool baseline_exceeds_every_region = std::none_of(
        timing_regions.begin(), timing_regions.end(),
        [&](const TimingRegion& region) {
          return eligible_region(region) &&
                 waveform_dsp.baseline_samples <= region.pre_trigger_samples;
        });
    if (baseline_exceeds_every_region) {
      throw std::runtime_error(
          "[SoftwareDSP] BaselineSamples exceeds the configured pre-trigger "
          "region");
    }
    if (legacy_threshold_gates) {
      const bool long_gate_exceeds_every_region = std::none_of(
          timing_regions.begin(), timing_regions.end(),
          [&](const TimingRegion& region) {
            return eligible_region(region) &&
                   waveform_dsp.long_gate_samples <=
                       region.post_trigger_samples;
          });
      if (long_gate_exceeds_every_region) {
        throw std::runtime_error(
            "[SoftwareDSP] LongGate exceeds the configured post-trigger "
            "region");
      }
      throw std::runtime_error(
          "[SoftwareDSP] BaselineSamples and LongGate do not fit the same "
          "pre/post-trigger layout under the selected record-length "
          "contract");
    }
  }

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

  if (settings.software_random_trigger_mode != 0 &&
      (settings.ext_trigger_mode != 0 || settings.self_trigger_mode != 0)) {
    throw std::runtime_error(
        "[Digitizer] SoftwareRandomTriggerMode is mutually exclusive with "
        "ExtTriggerMode and SelfTriggerMode");
  }
  if (settings.ext_trigger_mode == 0 && settings.self_trigger_mode == 0 &&
      settings.software_random_trigger_mode == 0) {
    throw std::runtime_error(
        "[Digitizer] ExtTriggerMode, SelfTriggerMode, and "
        "SoftwareRandomTriggerMode cannot all be disabled");
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
