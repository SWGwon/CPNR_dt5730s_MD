#include "ConfigParser.h"
#include "DAQConfig.h"
#include "DT5730Constraints.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "[FAIL] " << message << '\n';
    ++failures;
  }
}

void CheckThrows(const std::function<void()>& action,
                 const std::string& expected_message_part,
                 const std::string& description) {
  try {
    action();
    Check(false, description + ": expected std::runtime_error");
  } catch (const std::runtime_error& error) {
    Check(std::string(error.what()).find(expected_message_part) != std::string::npos,
          description + ": unexpected message: " + error.what());
  } catch (...) {
    Check(false, description + ": unexpected exception type");
  }
}

void WriteFile(const std::filesystem::path& path, const std::string& content) {
  std::ofstream output(path);
  if (!output) throw std::runtime_error("Cannot create test fixture: " + path.string());
  output << content;
}

}  // namespace

int main() {
  const auto unique_suffix = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const auto test_dir =
      std::filesystem::temp_directory_path() / ("cpnr_config_tests_" + unique_suffix);
  std::filesystem::create_directories(test_dir);

  try {
    const auto valid_path = test_dir / "valid.conf";
    WriteFile(valid_path,
              "[Digitizer]\n"
              "RecordLength = 1024\n"
              "ChannelMask = 3\n"
              "[Meta]\n"
              "Name = NaI\n"
              "Gain = 1.25\n");

    ConfigParser valid(valid_path.string());
    Check(valid.GetRequiredInt("Digitizer", "RecordLength", 128, 102400) == 1024,
          "valid required integer");
    Check(valid.GetInt("Digitizer", "ChannelMask", -1) == 3,
          "valid optional integer");
    Check(valid.GetString("Meta", "Name", "") == "NaI", "valid string");
    const double gain = valid.GetDouble("Meta", "Gain", -1.0);
    Check(gain > 1.249999999999 && gain < 1.250000000001, "valid double");
    Check(valid.GetInt("Digitizer", "Missing", 77) == 77, "missing optional fallback");

    const auto missing_path = test_dir / "missing.conf";
    CheckThrows([&]() { ConfigParser missing(missing_path.string()); },
                missing_path.string(), "missing config fails closed");

    const auto empty_path = test_dir / "empty.conf";
    WriteFile(empty_path, "# comments are not a configuration\n");
    CheckThrows([&]() { ConfigParser empty(empty_path.string()); },
                "no settings", "empty config fails closed");

    const auto empty_section_path = test_dir / "empty_section.conf";
    WriteFile(empty_section_path, "[Digitizer]\n");
    CheckThrows([&]() { ConfigParser empty(empty_section_path.string()); },
                "no settings", "a section declaration alone is not a setting");

    CheckThrows(
        [&]() { valid.GetRequiredInt("Digitizer", "PostTrigger", 0, 100); },
        "PostTrigger", "missing required key fails closed");
    CheckThrows(
        [&]() { valid.GetRequiredInt("Digitizer", "ChannelMask", 1, 2); },
        "out of range", "out-of-range required value fails closed");

    const auto invalid_integer_path = test_dir / "invalid_integer.conf";
    WriteFile(invalid_integer_path, "[Digitizer]\nRecordLength=1024junk\n");
    ConfigParser invalid_integer(invalid_integer_path.string());
    CheckThrows(
        [&]() { invalid_integer.GetRequiredInt("Digitizer", "RecordLength"); },
        "Invalid integer", "partially numeric value fails closed");

    const auto malformed_path = test_dir / "malformed.conf";
    WriteFile(malformed_path, "[Digitizer]\nRecordLength 1024\n");
    CheckThrows([&]() { ConfigParser malformed(malformed_path.string()); },
                "line 2", "malformed entry fails closed");

    const auto duplicate_path = test_dir / "duplicate.conf";
    WriteFile(duplicate_path,
              "[Digitizer]\nRecordLength=1024\nRecordLength=2048\n");
    CheckThrows([&]() { ConfigParser duplicate(duplicate_path.string()); },
                "Duplicate config key", "duplicate key fails closed");

    const auto duplicate_section_path = test_dir / "duplicate_section.conf";
    WriteFile(duplicate_section_path,
              "[Digitizer]\nRecordLength=1024\n[Digitizer]\nChannelMask=1\n");
    CheckThrows([&]() { ConfigParser duplicate_section(duplicate_section_path.string()); },
                "Duplicate config section", "duplicate section fails closed");

    const std::string valid_daq_config =
        "[Digitizer]\n"
        "RecordLength=1030\n"
        "ChannelMask=3\n"
        "SelfTriggerMask=3\n"
        "PostTrigger=70\n"
        "TriggerPolarity=1\n"
        "ExtTriggerMode=0\n"
        "SelfTriggerMode=1\n"
        "[HardwareCoincidence]\n"
        "PairLogic=AND\n"
        "[Channel_0]\n"
        "DCOffset=32768\n"
        "TriggerThreshold=8050\n"
        "[Channel_1]\n"
        "DCOffset=58981\n"
        "TriggerThreshold=14615\n";
    const auto valid_daq_path = test_dir / "valid_daq.conf";
    WriteFile(valid_daq_path, valid_daq_config);
    ConfigParser valid_daq_parser(valid_daq_path.string());
    const DAQHardwareSettings settings = LoadDAQHardwareSettings(valid_daq_parser);
    Check(settings.record_length == 1030, "DAQ schema record length");
    Check(settings.channel_mask == 3, "DAQ schema channel mask");
    Check(settings.self_trigger_mask == 3, "DAQ schema self-trigger mask");
    Check(settings.explicit_trigger_routing,
          "split-trigger schema enables explicit hardware routing");
    Check(settings.pair_logic == DAQPairLogic::kAnd,
          "DAQ schema adjacent-pair logic");
    Check(settings.input_range_mv == 2000,
          "legacy DAQ schema defaults to the reset 2 Vpp range");
    Check(settings.adc_bits == 14, "DT5730 ADC resolution is fixed at 14 bits");
    Check(settings.trigger_calibration.settling_time_ms == 3000,
          "trigger calibration settling default");
    Check(settings.trigger_calibration.settling_timeout_ms == 15000,
          "trigger calibration timeout default");
    Check(settings.storage.minimum_free_bytes ==
                  uint64_t{1024} * 1024U * 1024U &&
              settings.storage.stop_free_bytes ==
                  uint64_t{512} * 1024U * 1024U,
          "storage safety defaults reserve 1 GiB and stop at 512 MiB");
    Check(settings.channels[1].trigger_threshold == 14615,
          "DAQ schema active-channel settings");
    Check(!settings.channels[1].threshold_is_relative_mv,
          "legacy absolute threshold mode");

    const auto quantized_layout =
        dt5730_constraints::PredictPostTriggerLayout(260U, 27U);
    Check(quantized_layout.register_value == 9U &&
              quantized_layout.pre_trigger_samples == 188U &&
              quantized_layout.post_trigger_samples == 72U &&
              quantized_layout.predicted_readback_percent == 27U,
          "x730 post-trigger quantization predicts K and actual sample "
          "regions");

    std::string quantized_dsp_config = valid_daq_config;
    quantized_dsp_config.replace(
        quantized_dsp_config.find("RecordLength=1030"),
        std::string("RecordLength=1030").size(), "RecordLength=260");
    quantized_dsp_config.replace(
        quantized_dsp_config.find("PostTrigger=70"),
        std::string("PostTrigger=70").size(), "PostTrigger=27");
    const auto quantized_dsp_settings = LoadDAQHardwareSettings(
        ConfigParser::FromText(quantized_dsp_config,
                               "quantized-dsp-defaults-test"));
    Check(quantized_dsp_settings.software_dsp.waveform.baseline_samples ==
                  150U &&
              quantized_dsp_settings.software_dsp.waveform
                      .short_gate_samples == 40U &&
              quantized_dsp_settings.software_dsp.waveform
                      .long_gate_samples == 72U,
          "DSP defaults use the hardware-quantized pre/post sample regions");

    std::string quantized_dsp_bounds_config = quantized_dsp_config;
    quantized_dsp_bounds_config.insert(
        quantized_dsp_bounds_config.find("[Channel_0]"),
        "[SoftwareDSP]\nBaselineSamples=188\nShortGate=40\nLongGate=72\n");
    (void)LoadDAQHardwareSettings(ConfigParser::FromText(
        quantized_dsp_bounds_config, "quantized-dsp-bounds-test"));
    quantized_dsp_bounds_config.replace(
        quantized_dsp_bounds_config.find("BaselineSamples=188"),
        std::string("BaselineSamples=188").size(), "BaselineSamples=189");
    CheckThrows(
        [&]() {
          (void)LoadDAQHardwareSettings(ConfigParser::FromText(
              quantized_dsp_bounds_config,
              "quantized-dsp-pre-bound-test"));
        },
        "BaselineSamples exceeds",
        "DSP baseline bound uses actual quantized pre-trigger samples");

    quantized_dsp_bounds_config.replace(
        quantized_dsp_bounds_config.find("BaselineSamples=189"),
        std::string("BaselineSamples=189").size(), "BaselineSamples=188");
    quantized_dsp_bounds_config.replace(
        quantized_dsp_bounds_config.find("LongGate=72"),
        std::string("LongGate=72").size(), "LongGate=73");
    const auto peak_centered_dsp_settings = LoadDAQHardwareSettings(
        ConfigParser::FromText(quantized_dsp_bounds_config,
                               "peak-centered-dsp-post-bound-test"));
    Check(peak_centered_dsp_settings.software_dsp.waveform
                  .long_gate_samples == 73U,
          "schema-3 parser retains a legacy LongGate beyond the post-trigger "
          "region as non-blocking provenance");
    CheckThrows(
        [&]() {
          (void)LoadDAQHardwareSettings(ConfigParser::FromText(
              quantized_dsp_bounds_config,
              "legacy-dsp-post-bound-test"),
              DAQRecordLengthContract::kCurrentX730,
              DAQWaveformDspContract::kLegacyThresholdGates);
        },
        "LongGate exceeds",
        "legacy DSP long-gate bound uses actual quantized post-trigger "
        "samples");

    std::string peak_provenance_config = quantized_dsp_config;
    peak_provenance_config.insert(
        peak_provenance_config.find("[Channel_0]"),
        "[SoftwareDSP]\nBaselineSamples=188\nShortGate=2000\n"
        "LongGate=1500\nPulseStartThresholdAdc=0\n");
    const auto peak_provenance_settings = LoadDAQHardwareSettings(
        ConfigParser::FromText(peak_provenance_config,
                               "peak-centered-legacy-provenance-test"));
    Check(peak_provenance_settings.software_dsp.waveform.short_gate_samples ==
                  2000U &&
              peak_provenance_settings.software_dsp.waveform
                      .long_gate_samples == 1500U &&
              peak_provenance_settings.software_dsp.waveform
                      .pulse_start_threshold_adc == 0.0,
          "schema-3 parser preserves out-of-window legacy gates and a "
          "zero-valued legacy threshold without blocking "
          "acquisition");
    CheckThrows(
        [&]() {
          (void)LoadDAQHardwareSettings(
              ConfigParser::FromText(peak_provenance_config,
                                     "legacy-dsp-gate-range-test"),
              DAQRecordLengthContract::kCurrentX730,
              DAQWaveformDspContract::kLegacyThresholdGates);
        },
        "ShortGate",
        "legacy DSP contract retains the gate-to-record bound");

    std::string zero_legacy_threshold_config = quantized_dsp_config;
    zero_legacy_threshold_config.insert(
        zero_legacy_threshold_config.find("[Channel_0]"),
        "[SoftwareDSP]\nBaselineSamples=188\nShortGate=40\nLongGate=72\n"
        "PulseStartThresholdAdc=0\n");
    CheckThrows(
        [&]() {
          (void)LoadDAQHardwareSettings(
              ConfigParser::FromText(zero_legacy_threshold_config,
                                     "legacy-dsp-threshold-range-test"),
              DAQRecordLengthContract::kCurrentX730,
              DAQWaveformDspContract::kLegacyThresholdGates);
        },
        "PulseStartThresholdAdc",
        "legacy DSP contract retains the positive ADC threshold bound");

    std::string complete_schema_config = valid_daq_config;
    complete_schema_config.insert(
        complete_schema_config.find("[Channel_0]"),
        "[Connection]\nType=USB\nLink=0\nNode=0\nBaseAddress=0\n"
        "ExpectedModel=DT5730\nExpectedSerial=5730\n"
        "[Synchronization]\nClockSource=0\nRunSyncMode=0\n"
        "[TriggerCalibration]\nSettlingTimeMs=100\n"
        "SettlingTimeoutMs=200\nMeasurementEvents=8\n"
        "StabilityToleranceAdc=2.5\nStableMeasurements=3\n"
        "[Storage]\nMinimumFreeMiB=1024\nStopFreeMiB=512\n"
        "[DataQuality]\nMaxLostEvents=4\nMaxLostFraction=0.01\n"
        "[SoftwareDSP]\nCoincidenceWindow=24\nBaselineSamples=200\n"
        "ShortGate=50\nLongGate=700\nPulseStartThresholdAdc=12.5\n");
    const auto complete_schema_settings = LoadDAQHardwareSettings(
        ConfigParser::FromText(complete_schema_config,
                               "complete-schema-test"));
    Check(complete_schema_settings.connection.has_expected_serial &&
              complete_schema_settings.connection.expected_serial == 5730U &&
              complete_schema_settings.lost_event_policy.max_lost_events ==
                  4U &&
              complete_schema_settings.software_dsp.waveform
                      .pulse_start_threshold_adc == 12.5,
          "all official Connection/DataQuality/SoftwareDSP keys are accepted");

    std::string typo_section_config = valid_daq_config;
    typo_section_config.append("[Digtizer]\nMystery=1\n");
    CheckThrows(
        [&]() {
          (void)LoadDAQHardwareSettings(ConfigParser::FromText(
              typo_section_config, "typo-section-test"));
        },
        "did you mean [Digitizer]?",
        "a misspelled DAQ section is rejected with a useful suggestion");

    std::string unknown_section_config = valid_daq_config;
    unknown_section_config.append("[Diagnostics]\n");
    CheckThrows(
        [&]() {
          (void)LoadDAQHardwareSettings(ConfigParser::FromText(
              unknown_section_config, "unknown-section-test"));
        },
        "Unknown DAQ config section [Diagnostics]",
        "an unsupported DAQ section fails closed");

    std::string typo_key_config = valid_daq_config;
    typo_key_config.insert(typo_key_config.find("PostTrigger=70"),
                           "PostTriger=70\n");
    CheckThrows(
        [&]() {
          (void)LoadDAQHardwareSettings(ConfigParser::FromText(
              typo_key_config, "typo-key-test"));
        },
        "did you mean PostTrigger?",
        "a misspelled DAQ key is rejected with a scoped suggestion");

    std::string unknown_key_config = valid_daq_config;
    unknown_key_config.insert(unknown_key_config.find("PostTrigger=70"),
                              "MysteryKnob=1\n");
    CheckThrows(
        [&]() {
          (void)LoadDAQHardwareSettings(ConfigParser::FromText(
              unknown_key_config, "unknown-key-test"));
        },
        "Unknown DAQ config key [Digitizer] MysteryKnob",
        "an unsupported key cannot be silently ignored");

    std::string bad_channel_section_config = valid_daq_config;
    bad_channel_section_config.append("[Channel_8]\nDCOffset=32768\n");
    CheckThrows(
        [&]() {
          (void)LoadDAQHardwareSettings(ConfigParser::FromText(
              bad_channel_section_config, "bad-channel-section-test"));
        },
        "expected Channel_0..Channel_7",
        "an out-of-range channel section fails closed");

    std::string inactive_channel_typo_config = valid_daq_config;
    inactive_channel_typo_config.append("[Channel_7]\nDCOfset=32768\n");
    CheckThrows(
        [&]() {
          (void)LoadDAQHardwareSettings(ConfigParser::FromText(
              inactive_channel_typo_config, "inactive-channel-key-test"));
        },
        "did you mean DCOffset?",
        "unknown keys in inactive channel sections are still rejected");

    const auto dsp_path = test_dir / "dsp.conf";
    std::string dsp_config = valid_daq_config;
    dsp_config.insert(
        dsp_config.find("[Channel_0]"),
        "[SoftwareDSP]\nCoincidenceWindow=24\nBaselineSamples=200\n"
        "ShortGate=50\nLongGate=700\nPulseStartThresholdAdc=12.5\n");
    WriteFile(dsp_path, dsp_config);
    const auto dsp_settings =
        LoadDAQHardwareSettings(ConfigParser(dsp_path.string()));
    Check(dsp_settings.software_dsp.coincidence_window_ns == 24U &&
              dsp_settings.software_dsp.waveform.baseline_samples == 200U &&
              dsp_settings.software_dsp.waveform.short_gate_samples == 50U &&
              dsp_settings.software_dsp.waveform.long_gate_samples == 700U &&
              dsp_settings.software_dsp.waveform.pulse_start_threshold_adc ==
                  12.5,
          "all SoftwareDSP settings are parsed into the production contract");

    const auto bad_dsp_path = test_dir / "bad_dsp.conf";
    std::string bad_dsp_config = valid_daq_config;
    bad_dsp_config.insert(
        bad_dsp_config.find("[Channel_0]"),
        "[SoftwareDSP]\nBaselineSamples=311\nShortGate=40\n"
        "LongGate=200\n");
    WriteFile(bad_dsp_path, bad_dsp_config);
    CheckThrows(
        [&]() {
          (void)LoadDAQHardwareSettings(ConfigParser(bad_dsp_path.string()));
        },
        "BaselineSamples exceeds", "DSP baseline cannot leave pre-trigger bounds");

    const auto relative_threshold_path = test_dir / "relative_threshold.conf";
    WriteFile(relative_threshold_path,
              "[Digitizer]\n"
              "RecordLength=1030\n"
              "ChannelMask=3\n"
              "SelfTriggerMask=3\n"
              "PostTrigger=70\n"
              "InputRangeMv=2000\n"
              "ADCBits=14\n"
              "TriggerPolarity=1\n"
              "ExtTriggerMode=0\n"
              "SelfTriggerMode=1\n"
              "[HardwareCoincidence]\n"
              "PairLogic=AND\n"
              "[TriggerCalibration]\n"
              "SettlingTimeMs=3000\n"
              "SettlingTimeoutMs=15000\n"
              "MeasurementEvents=32\n"
              "StabilityToleranceAdc=2.0\n"
              "StableMeasurements=3\n"
              "[Channel_0]\n"
              "DCOffset=3276\n"
              "TriggerThresholdMv=1.0\n"
              "[Channel_1]\n"
              "DCOffset=3276\n"
              "TriggerThresholdMv=1.0\n");
    ConfigParser relative_threshold_parser(relative_threshold_path.string());
    const DAQHardwareSettings relative_settings =
        LoadDAQHardwareSettings(relative_threshold_parser);
    Check(relative_settings.channels[0].threshold_is_relative_mv,
          "millivolt threshold selects measured-baseline mode");
    Check(relative_settings.channels[0].trigger_threshold_mv == 1.0,
          "millivolt threshold value is preserved");
    Check(relative_settings.trigger_calibration.measurement_events == 32,
          "baseline measurement event count");
    Check(relative_settings.trigger_calibration.stability_tolerance_adc == 2.0,
          "baseline stability tolerance");

    const auto record_only_path = test_dir / "record_only_channels.conf";
    WriteFile(record_only_path,
              "[Digitizer]\nRecordLength=1030\nChannelMask=15\n"
              "SelfTriggerMask=3\nPostTrigger=70\nInputRangeMv=2000\n"
              "ADCBits=14\nTriggerPolarity=1\nExtTriggerMode=0\n"
              "SelfTriggerMode=1\n[HardwareCoincidence]\nPairLogic=AND\n"
              "[Channel_0]\nDCOffset=3276\nTriggerThresholdMv=1.0\n"
              "[Channel_1]\nDCOffset=3276\nTriggerThresholdMv=1.0\n"
              "[Channel_2]\nDCOffset=3276\n"
              "[Channel_3]\nDCOffset=3276\n");
    ConfigParser record_only_parser(record_only_path.string());
    const auto record_only_settings =
        LoadDAQHardwareSettings(record_only_parser);
    Check(!record_only_settings.channels[2].has_trigger_threshold &&
              !record_only_settings.channels[3].has_trigger_threshold,
          "record-only channels do not require discriminator thresholds");

    const std::string target_baseline_config =
        "[Digitizer]\nRecordLength=1030\nChannelMask=15\n"
        "SelfTriggerMask=3\nPostTrigger=70\nInputRangeMv=2000\n"
        "ADCBits=14\nTriggerPolarity=1\nExtTriggerMode=0\n"
        "SelfTriggerMode=1\n[HardwareCoincidence]\nPairLogic=AND\n"
        "[DCOffsetCalibration]\nTargetTolerancePercent=0.5\n"
        "MaxAdjustmentIterations=8\nDacBusyTimeoutMs=1000\n"
        "StepSettlingTimeMs=200\n"
        "[Channel_0]\nDCOffsetMode=TargetBaseline\n"
        "BaselineTargetPercent=90\nTriggerThresholdMv=1.0\n"
        "[Channel_1]\nDCOffsetMode=TargetBaseline\n"
        "BaselineTargetPercent=85\nTriggerThresholdMv=1.0\n"
        "[Channel_2]\nDCOffsetMode=TargetBaseline\n"
        "BaselineTargetPercent=90\n"
        "[Channel_3]\nDCOffsetMode=TargetBaseline\n"
        "BaselineTargetPercent=90\n";
    const auto target_baseline_settings = LoadDAQHardwareSettings(
        ConfigParser::FromText(target_baseline_config,
                               "target-baseline-config-test"));
    Check(target_baseline_settings.channels[0].dc_offset_mode ==
                  DAQDCOffsetMode::kTargetBaseline &&
              target_baseline_settings.channels[0].target_baseline_adc ==
                  14745U &&
              target_baseline_settings.channels[0].dc_offset == 6554U,
          "90-percent target produces a 14-bit target and nominal 16-bit DAC seed");
    Check(target_baseline_settings.channels[1].target_baseline_adc ==
                  BaselinePercentToAdc(85.0, 14) &&
              target_baseline_settings.channels[1].dc_offset ==
                  BaselinePercentToInitialDac(85.0),
          "baseline targets remain independent per channel");
    Check(target_baseline_settings.dc_offset_calibration
                      .target_tolerance_percent == 0.5 &&
              target_baseline_settings.dc_offset_calibration
                      .max_adjustment_iterations == 8U &&
              target_baseline_settings.dc_offset_calibration
                      .dac_busy_timeout_ms == 1000U &&
              target_baseline_settings.dc_offset_calibration
                      .step_settling_time_ms == 200U,
          "DC-offset closed-loop calibration controls are parsed");

    std::string target_with_raw = target_baseline_config;
    target_with_raw.replace(
        target_with_raw.find("BaselineTargetPercent=90"),
        std::string("BaselineTargetPercent=90").size(),
        "BaselineTargetPercent=90\nDCOffset=6554");
    CheckThrows(
        [&]() {
          (void)LoadDAQHardwareSettings(ConfigParser::FromText(
              target_with_raw, "target-with-raw-test"));
        },
        "forbids DCOffset",
        "target baseline and a raw DAC code are mutually exclusive");

    std::string target_without_percent = target_baseline_config;
    target_without_percent.erase(
        target_without_percent.find("BaselineTargetPercent=90\n"),
        std::string("BaselineTargetPercent=90\n").size());
    CheckThrows(
        [&]() {
          (void)LoadDAQHardwareSettings(ConfigParser::FromText(
              target_without_percent, "target-without-percent-test"));
        },
        "requires BaselineTargetPercent",
        "target mode requires an explicit percentage");

    std::string unsafe_target = target_baseline_config;
    unsafe_target.replace(unsafe_target.find("BaselineTargetPercent=90"),
                          std::string("BaselineTargetPercent=90").size(),
                          "BaselineTargetPercent=4.9");
    CheckThrows(
        [&]() {
          (void)LoadDAQHardwareSettings(ConfigParser::FromText(
              unsafe_target, "unsafe-target-percent-test"));
        },
        "range 5..95", "target baseline rejects unsafe rail proximity");

    std::string target_absolute_threshold = target_baseline_config;
    target_absolute_threshold.replace(
        target_absolute_threshold.find("TriggerThresholdMv=1.0"),
        std::string("TriggerThresholdMv=1.0").size(),
        "TriggerThreshold=14700");
    CheckThrows(
        [&]() {
          (void)LoadDAQHardwareSettings(ConfigParser::FromText(
              target_absolute_threshold,
              "target-absolute-threshold-test"));
        },
        "requires TriggerThresholdMv",
        "target-mode self trigger rejects a baseline-dependent absolute threshold");

    std::string zero_target_tolerance = target_baseline_config;
    zero_target_tolerance.replace(
        zero_target_tolerance.find("TargetTolerancePercent=0.5"),
        std::string("TargetTolerancePercent=0.5").size(),
        "TargetTolerancePercent=0");
    CheckThrows(
        [&]() {
          (void)LoadDAQHardwareSettings(ConfigParser::FromText(
              zero_target_tolerance, "zero-target-tolerance-test"));
        },
        "TargetTolerancePercent",
        "zero placement tolerance is rejected before hardware is opened");

    const auto sub_lsb_threshold_path = test_dir / "sub_lsb_threshold.conf";
    std::string sub_lsb_threshold = valid_daq_config;
    sub_lsb_threshold.replace(
        sub_lsb_threshold.find("TriggerThreshold=8050"),
        std::string("TriggerThreshold=8050").size(),
        "TriggerThresholdMv=0.001");
    WriteFile(sub_lsb_threshold_path, sub_lsb_threshold);
    ConfigParser sub_lsb_threshold_parser(sub_lsb_threshold_path.string());
    CheckThrows(
        [&]() { LoadDAQHardwareSettings(sub_lsb_threshold_parser); },
        "representable ADC delta",
        "sub-LSB millivolt threshold fails before hardware is opened");

    const auto duplicate_threshold_mode_path =
        test_dir / "duplicate_threshold_mode.conf";
    std::string duplicate_threshold_mode = valid_daq_config;
    duplicate_threshold_mode.replace(
        duplicate_threshold_mode.find("TriggerThreshold=8050"),
        std::string("TriggerThreshold=8050").size(),
        "TriggerThreshold=8050\nTriggerThresholdMv=1.0");
    WriteFile(duplicate_threshold_mode_path, duplicate_threshold_mode);
    ConfigParser duplicate_threshold_mode_parser(
        duplicate_threshold_mode_path.string());
    CheckThrows(
        [&]() { LoadDAQHardwareSettings(duplicate_threshold_mode_parser); },
        "mutually exclusive", "absolute and millivolt thresholds are mutually exclusive");

    const auto missing_threshold_mode_path =
        test_dir / "missing_threshold_mode.conf";
    std::string missing_threshold_mode = valid_daq_config;
    missing_threshold_mode.replace(
        missing_threshold_mode.find("TriggerThreshold=8050\n"),
        std::string("TriggerThreshold=8050\n").size(), "");
    WriteFile(missing_threshold_mode_path, missing_threshold_mode);
    ConfigParser missing_threshold_mode_parser(
        missing_threshold_mode_path.string());
    CheckThrows(
        [&]() { LoadDAQHardwareSettings(missing_threshold_mode_parser); },
        "requires", "each self-trigger channel requires one threshold mode");

    const auto invalid_input_range_path = test_dir / "invalid_input_range.conf";
    std::string invalid_input_range = valid_daq_config;
    invalid_input_range.replace(
        invalid_input_range.find("PostTrigger=70"),
        std::string("PostTrigger=70").size(),
        "PostTrigger=70\nInputRangeMv=750");
    WriteFile(invalid_input_range_path, invalid_input_range);
    ConfigParser invalid_input_range_parser(invalid_input_range_path.string());
    CheckThrows([&]() { LoadDAQHardwareSettings(invalid_input_range_parser); },
                "exactly 500 or 2000", "unsupported input range fails closed");

    const auto invalid_adc_bits_path = test_dir / "invalid_adc_bits.conf";
    std::string invalid_adc_bits = valid_daq_config;
    invalid_adc_bits.replace(
        invalid_adc_bits.find("PostTrigger=70"),
        std::string("PostTrigger=70").size(),
        "PostTrigger=70\nADCBits=12");
    WriteFile(invalid_adc_bits_path, invalid_adc_bits);
    ConfigParser invalid_adc_bits_parser(invalid_adc_bits_path.string());
    CheckThrows([&]() { LoadDAQHardwareSettings(invalid_adc_bits_parser); },
                "out of range", "non-14-bit ADC configuration fails closed");

    const auto invalid_settling_path = test_dir / "invalid_settling.conf";
    std::string invalid_settling = valid_daq_config;
    invalid_settling.append(
        "[TriggerCalibration]\nSettlingTimeMs=3000\n"
        "SettlingTimeoutMs=3000\n");
    WriteFile(invalid_settling_path, invalid_settling);
    ConfigParser invalid_settling_parser(invalid_settling_path.string());
    CheckThrows([&]() { LoadDAQHardwareSettings(invalid_settling_parser); },
                "must be greater", "settling timeout must exceed initial delay");

    const auto invalid_storage_path = test_dir / "invalid_storage.conf";
    std::string invalid_storage = valid_daq_config;
    invalid_storage.append(
        "[Storage]\nMinimumFreeMiB=512\nStopFreeMiB=512\n");
    WriteFile(invalid_storage_path, invalid_storage);
    ConfigParser invalid_storage_parser(invalid_storage_path.string());
    CheckThrows([&]() { LoadDAQHardwareSettings(invalid_storage_parser); },
                "must be smaller",
                "runtime disk stop watermark must preserve a metadata reserve");

    const auto legacy_daq_path = test_dir / "legacy_daq.conf";
    WriteFile(legacy_daq_path,
              "[Digitizer]\n"
              "RecordLength=1030\n"
              "ChannelMask=1\n"
              "PostTrigger=70\n"
              "TriggerPolarity=1\n"
              "ExtTriggerMode=0\n"
              "SelfTriggerMode=1\n"
              "[Channel_0]\n"
              "DCOffset=32768\n"
              "TriggerThreshold=8050\n");
    ConfigParser legacy_daq_parser(legacy_daq_path.string());
    const DAQHardwareSettings legacy_settings =
        LoadDAQHardwareSettings(legacy_daq_parser);
    Check(legacy_settings.self_trigger_mask == 1,
          "legacy DAQ config uses readout mask for self-trigger");
    Check(!legacy_settings.explicit_trigger_routing,
          "legacy DAQ config preserves firmware pair routing");
    Check(legacy_settings.pair_logic == DAQPairLogic::kOr,
          "legacy DAQ config preserves OR behavior");

    const auto valid_or_single_path = test_dir / "valid_or_single.conf";
    WriteFile(valid_or_single_path,
              "[Digitizer]\n"
              "RecordLength=1030\n"
              "ChannelMask=1\n"
              "SelfTriggerMask=1\n"
              "PostTrigger=70\n"
              "TriggerPolarity=1\n"
              "ExtTriggerMode=0\n"
              "SelfTriggerMode=1\n"
              "[HardwareCoincidence]\n"
              "PairLogic=OR\n"
              "[Channel_0]\n"
              "DCOffset=32768\n"
              "TriggerThreshold=8050\n");
    ConfigParser valid_or_single(valid_or_single_path.string());
    const DAQHardwareSettings single_or_settings =
        LoadDAQHardwareSettings(valid_or_single);
    Check(single_or_settings.self_trigger_mask == 1,
          "OR logic accepts a single channel from a pair");

    const auto valid_ext_only_path = test_dir / "valid_ext_only.conf";
    WriteFile(valid_ext_only_path,
              "[Digitizer]\n"
              "RecordLength=1030\n"
              "ChannelMask=1\n"
              "SelfTriggerMask=0\n"
              "PostTrigger=70\n"
              "TriggerPolarity=1\n"
              "ExtTriggerMode=1\n"
              "SelfTriggerMode=0\n"
              "[HardwareCoincidence]\n"
              "PairLogic=OR\n"
              "[Channel_0]\n"
              "DCOffset=32768\n"
              "TriggerThreshold=8050\n");
    ConfigParser valid_ext_only(valid_ext_only_path.string());
    const DAQHardwareSettings ext_only_settings =
        LoadDAQHardwareSettings(valid_ext_only);
    Check(ext_only_settings.self_trigger_mask == 0,
          "external-only trigger has an empty self-trigger mask");

    const auto partial_trigger_schema_path =
        test_dir / "partial_trigger_schema.conf";
    WriteFile(partial_trigger_schema_path,
              "[Digitizer]\n"
              "RecordLength=1030\n"
              "ChannelMask=1\n"
              "SelfTriggerMask=1\n"
              "PostTrigger=70\n"
              "TriggerPolarity=1\n"
              "ExtTriggerMode=0\n"
              "SelfTriggerMode=1\n"
              "[Channel_0]\n"
              "DCOffset=32768\n"
              "TriggerThreshold=8050\n");
    ConfigParser partial_trigger_schema(partial_trigger_schema_path.string());
    CheckThrows([&]() { LoadDAQHardwareSettings(partial_trigger_schema); },
                "must be specified together",
                "partial split-trigger schema fails closed");

    const auto trigger_not_subset_path = test_dir / "trigger_not_subset.conf";
    std::string trigger_not_subset_config = valid_daq_config;
    trigger_not_subset_config.replace(
        trigger_not_subset_config.find("SelfTriggerMask=3"),
        std::string("SelfTriggerMask=3").size(), "SelfTriggerMask=12");
    WriteFile(trigger_not_subset_path, trigger_not_subset_config);
    ConfigParser trigger_not_subset(trigger_not_subset_path.string());
    CheckThrows([&]() { LoadDAQHardwareSettings(trigger_not_subset); },
                "subset of ChannelMask",
                "self-trigger mask outside readout mask fails closed");

    const auto incomplete_and_pair_path = test_dir / "incomplete_and_pair.conf";
    std::string incomplete_and_pair_config = valid_daq_config;
    incomplete_and_pair_config.replace(
        incomplete_and_pair_config.find("SelfTriggerMask=3"),
        std::string("SelfTriggerMask=3").size(), "SelfTriggerMask=1");
    WriteFile(incomplete_and_pair_path, incomplete_and_pair_config);
    ConfigParser incomplete_and_pair(incomplete_and_pair_path.string());
    CheckThrows([&]() { LoadDAQHardwareSettings(incomplete_and_pair); },
                "complete adjacent channel pairs",
                "AND with an incomplete x730 pair fails closed");

    const auto invalid_pair_logic_path = test_dir / "invalid_pair_logic.conf";
    std::string invalid_pair_logic_config = valid_daq_config;
    invalid_pair_logic_config.replace(invalid_pair_logic_config.find("PairLogic=AND"),
                                      std::string("PairLogic=AND").size(),
                                      "PairLogic=XOR");
    WriteFile(invalid_pair_logic_path, invalid_pair_logic_config);
    ConfigParser invalid_pair_logic(invalid_pair_logic_path.string());
    CheckThrows([&]() { LoadDAQHardwareSettings(invalid_pair_logic); },
                "must be AND or OR", "unknown pair logic fails closed");

    const auto disabled_self_mask_path = test_dir / "disabled_self_mask.conf";
    std::string disabled_self_mask_config = valid_daq_config;
    disabled_self_mask_config.replace(
        disabled_self_mask_config.find("ExtTriggerMode=0"),
        std::string("ExtTriggerMode=0").size(), "ExtTriggerMode=1");
    disabled_self_mask_config.replace(
        disabled_self_mask_config.find("SelfTriggerMode=1"),
        std::string("SelfTriggerMode=1").size(), "SelfTriggerMode=0");
    WriteFile(disabled_self_mask_path, disabled_self_mask_config);
    ConfigParser disabled_self_mask(disabled_self_mask_path.string());
    CheckThrows([&]() { LoadDAQHardwareSettings(disabled_self_mask); },
                "SelfTriggerMask must be 0",
                "disabled self-trigger with a nonzero mask fails closed");

    const auto enabled_empty_mask_path = test_dir / "enabled_empty_mask.conf";
    std::string enabled_empty_mask_config = valid_daq_config;
    enabled_empty_mask_config.replace(
        enabled_empty_mask_config.find("SelfTriggerMask=3"),
        std::string("SelfTriggerMask=3").size(), "SelfTriggerMask=0");
    WriteFile(enabled_empty_mask_path, enabled_empty_mask_config);
    ConfigParser enabled_empty_mask(enabled_empty_mask_path.string());
    CheckThrows([&]() { LoadDAQHardwareSettings(enabled_empty_mask); },
                "must enable at least one channel",
                "enabled self-trigger with an empty mask fails closed");

    const auto missing_channel_path = test_dir / "missing_channel.conf";
    WriteFile(missing_channel_path,
              "[Digitizer]\n"
              "RecordLength=1030\n"
              "ChannelMask=2\n"
              "PostTrigger=70\n"
              "TriggerPolarity=1\n"
              "ExtTriggerMode=0\n"
              "SelfTriggerMode=1\n"
              "[Channel_0]\n"
              "DCOffset=32768\n"
              "TriggerThreshold=8050\n");
    ConfigParser missing_channel(missing_channel_path.string());
    CheckThrows([&]() { LoadDAQHardwareSettings(missing_channel); },
                "Channel_1", "enabled channel settings are required");

    const auto non_multiple_path = test_dir / "non_multiple.conf";
    std::string non_multiple_config = valid_daq_config;
    non_multiple_config.replace(non_multiple_config.find("RecordLength=1030"),
                                std::string("RecordLength=1030").size(),
                                "RecordLength=256");
    WriteFile(non_multiple_path, non_multiple_config);
    ConfigParser non_multiple(non_multiple_path.string());
    CheckThrows([&]() { LoadDAQHardwareSettings(non_multiple); },
                "multiple of 10",
                "a record length CAEN would round upward fails closed");

    std::string legacy_record_length_config = valid_daq_config;
    legacy_record_length_config.replace(
        legacy_record_length_config.find("RecordLength=1030"),
        std::string("RecordLength=1030").size(), "RecordLength=512");
    const ConfigParser legacy_record_length_parser = ConfigParser::FromText(
        legacy_record_length_config, "legacy-record-length-contract-test");
    CheckThrows(
        [&]() {
          (void)LoadDAQHardwareSettings(legacy_record_length_parser);
        },
        "multiple of 10",
        "legacy multiple-of-8 config is rejected for hardware acquisition");
    const DAQHardwareSettings legacy_record_length_settings =
        LoadDAQHardwareSettings(
            legacy_record_length_parser,
            DAQRecordLengthContract::kLegacyMultipleOf8);
    Check(legacy_record_length_settings.record_length == 512U,
          "offline legacy profile accepts authenticated multiple-of-8 data");

    std::string legacy_timing_config = valid_daq_config;
    legacy_timing_config.replace(
        legacy_timing_config.find("RecordLength=1030"),
        std::string("RecordLength=1030").size(), "RecordLength=128");
    legacy_timing_config.replace(
        legacy_timing_config.find("PostTrigger=70"),
        std::string("PostTrigger=70").size(), "PostTrigger=10");
    const DAQHardwareSettings legacy_timing_settings =
        LoadDAQHardwareSettings(
            ConfigParser::FromText(legacy_timing_config,
                                   "legacy-timing-contract-test"),
            DAQRecordLengthContract::kLegacyMultipleOf8);
    Check(legacy_timing_settings.software_dsp.waveform.baseline_samples ==
                  115U &&
              legacy_timing_settings.software_dsp.waveform
                      .short_gate_samples == 13U &&
              legacy_timing_settings.software_dsp.waveform
                      .long_gate_samples == 13U,
          "offline legacy profile preserves nominal percentage timing and DSP "
          "defaults");

    std::string overlapping_legacy_timing_config = valid_daq_config;
    overlapping_legacy_timing_config.replace(
        overlapping_legacy_timing_config.find("RecordLength=1030"),
        std::string("RecordLength=1030").size(), "RecordLength=520");
    overlapping_legacy_timing_config.insert(
        overlapping_legacy_timing_config.find("[Channel_0]"),
        "[SoftwareDSP]\nBaselineSamples=156\nShortGate=40\nLongGate=364\n");
    const ConfigParser overlapping_legacy_timing_parser =
        ConfigParser::FromText(overlapping_legacy_timing_config,
                               "overlapping-legacy-timing-contract-test");
    CheckThrows(
        [&]() {
          (void)LoadDAQHardwareSettings(overlapping_legacy_timing_parser);
        },
        "BaselineSamples exceeds",
        "current timing rejects a DSP window that only fits legacy timing");
    const DAQHardwareSettings overlapping_legacy_timing_settings =
        LoadDAQHardwareSettings(
            overlapping_legacy_timing_parser,
            DAQRecordLengthContract::kLegacyOrCurrent,
            DAQWaveformDspContract::kLegacyThresholdGates);
    Check(overlapping_legacy_timing_settings.software_dsp.waveform
                      .baseline_samples == 156U &&
              overlapping_legacy_timing_settings.software_dsp.waveform
                      .long_gate_samples == 364U,
          "recovery union accepts a complete legacy layout on the 8/10-grid "
          "intersection");

    overlapping_legacy_timing_config.replace(
        overlapping_legacy_timing_config.find("LongGate=364"),
        std::string("LongGate=364").size(), "LongGate=368");
    CheckThrows(
        [&]() {
          (void)LoadDAQHardwareSettings(
              ConfigParser::FromText(overlapping_legacy_timing_config,
                                     "mixed-overlap-timing-contract-test"),
              DAQRecordLengthContract::kLegacyOrCurrent,
              DAQWaveformDspContract::kLegacyThresholdGates);
        },
        "same pre/post-trigger layout",
        "recovery union cannot mix the legacy pre region with the current "
        "post region");

    const auto short_pretrigger_path = test_dir / "short_pretrigger.conf";
    std::string short_pretrigger_config = valid_daq_config;
    short_pretrigger_config.replace(short_pretrigger_config.find("RecordLength=1030"),
                                    std::string("RecordLength=1030").size(),
                                    "RecordLength=130");
    short_pretrigger_config.replace(short_pretrigger_config.find("PostTrigger=70"),
                                    std::string("PostTrigger=70").size(),
                                    "PostTrigger=38");
    WriteFile(short_pretrigger_path, short_pretrigger_config);
    ConfigParser short_pretrigger(short_pretrigger_path.string());
    CheckThrows([&]() { LoadDAQHardwareSettings(short_pretrigger); },
                "less than 160 ns", "short pre-trigger window fails closed");

    const auto no_trigger_path = test_dir / "no_trigger.conf";
    std::string no_trigger_config = valid_daq_config;
    no_trigger_config.replace(no_trigger_config.find("SelfTriggerMode=1"),
                              std::string("SelfTriggerMode=1").size(),
                              "SelfTriggerMode=0");
    WriteFile(no_trigger_path, no_trigger_config);
    ConfigParser no_trigger(no_trigger_path.string());
    CheckThrows([&]() { LoadDAQHardwareSettings(no_trigger); },
                "cannot all be disabled", "missing trigger source fails closed");

    // Software-random triggering is an explicit, mutually-exclusive trigger
    // source.  Missing keys retain the legacy hardware-trigger behavior, but
    // enabling the mode requires a finite positive mean rate and no hardware
    // trigger source.
    const auto random_config = [&](const std::string& rate,
                                   const std::string& ext = "0",
                                   const std::string& self = "0",
                                   const std::string& mask = "0") {
      std::string config = valid_daq_config;
      config.replace(config.find("SelfTriggerMask=3"),
                     std::string("SelfTriggerMask=3").size(),
                     "SelfTriggerMask=" + mask);
      config.replace(config.find("ExtTriggerMode=0"),
                     std::string("ExtTriggerMode=0").size(),
                     "ExtTriggerMode=" + ext);
      config.replace(config.find("SelfTriggerMode=1"),
                     std::string("SelfTriggerMode=1").size(),
                     "SelfTriggerMode=" + self);
      const std::size_t insertion = config.find("PostTrigger=70");
      config.insert(insertion + std::string("PostTrigger=70\n").size(),
                    "SoftwareRandomTriggerMode=1\n"
                    "SoftwareRandomTriggerRateHz=" + rate + "\n");
      return config;
    };

    const auto random_valid_settings = LoadDAQHardwareSettings(
        ConfigParser::FromText(random_config("12.5"),
                               "random-trigger-valid-test"));
    Check(random_valid_settings.software_random_trigger_mode == 1,
          "software-random trigger mode parses as enabled");
    Check(random_valid_settings.software_random_trigger_rate_hz == 12.5,
          "software-random trigger rate parses as Hz");
    Check(random_valid_settings.ext_trigger_mode == 0 &&
              random_valid_settings.self_trigger_mode == 0 &&
              random_valid_settings.self_trigger_mask == 0U,
          "software-random mode disables all hardware trigger sources");

    const auto minimum_random_rate_settings = LoadDAQHardwareSettings(
        ConfigParser::FromText(random_config("0.001"),
                               "random-trigger-minimum-rate-test"));
    Check(minimum_random_rate_settings.software_random_trigger_rate_hz ==
              kMinimumSoftwareRandomTriggerRateHz,
          "software-random trigger accepts the inclusive 0.001 Hz minimum");

    // Omitting both new keys is deliberately backward compatible.
    Check(settings.software_random_trigger_mode == 0 &&
              settings.software_random_trigger_rate_hz == 0.0,
          "legacy config defaults software-random triggering to disabled");

    const std::array<std::pair<std::string, std::string>, 5>
        invalid_random_rates{{{"0", "positive"},
                              {"-1", "positive"},
                              {"0.0009", "below-minimum"},
                              {"nan", "finite"},
                              {"1e12", "range"}}};
    for (const auto& [rate, reason] : invalid_random_rates) {
      CheckThrows(
          [&]() {
            (void)LoadDAQHardwareSettings(
                ConfigParser::FromText(random_config(rate),
                                       "random-trigger-invalid-rate-test"));
          },
          "SoftwareRandomTriggerRateHz",
          "software-random rate rejects " + reason + " value");
    }
    CheckThrows(
        [&]() {
          (void)LoadDAQHardwareSettings(
              ConfigParser::FromText(random_config("12.5", "1"),
                                     "random-trigger-mixed-external-test"));
        },
        "SoftwareRandomTrigger",
        "software-random mode rejects external-trigger mixing");
    CheckThrows(
        [&]() {
          (void)LoadDAQHardwareSettings(
              ConfigParser::FromText(random_config("12.5", "0", "1", "3"),
                                     "random-trigger-mixed-self-test"));
        },
        "SoftwareRandomTrigger",
        "software-random mode rejects self-trigger mixing");
    CheckThrows(
        [&]() {
          (void)LoadDAQHardwareSettings(
              ConfigParser::FromText(random_config("12.5", "0", "0", "1"),
                                     "random-trigger-mask-test"));
        },
        "SelfTriggerMask",
        "software-random mode rejects an enabled self-trigger mask");

    const std::filesystem::path source_dir(CPNR_SOURCE_DIR);
    const std::string shipped_configs[] = {
        "dt5730s_ext_clock.conf", "dt5730s_inorganic.conf",
        "dt5730s_ls_coin.conf", "dt5730s_master.conf", "test.conf"};
    for (const auto& config_name : shipped_configs) {
      const auto config_path = source_dir / "config" / config_name;
      try {
        ConfigParser shipped_parser(config_path.string());
        const auto shipped_settings = LoadDAQHardwareSettings(shipped_parser);
        if (config_name == "dt5730s_ext_clock.conf") {
          Check(shipped_settings.clock_source == 1 &&
                    shipped_settings.run_sync_mode == 1,
                "external-clock config preserves clock and run-sync mode");
        }
      } catch (const std::exception& error) {
        Check(false, "shipped config rejected (" + config_name + "): " + error.what());
      }
    }
  } catch (const std::exception& error) {
    std::cerr << "[FATAL] Test setup failed: " << error.what() << '\n';
    ++failures;
  }

  std::error_code cleanup_error;
  std::filesystem::remove_all(test_dir, cleanup_error);
  if (cleanup_error) {
    std::cerr << "[FAIL] Could not remove test fixtures: " << cleanup_error.message() << '\n';
    ++failures;
  }

  if (failures != 0) {
    std::cerr << failures << " config parser test(s) failed.\n";
    return 1;
  }

  std::cout << "All config parser regression tests passed.\n";
  return 0;
}
