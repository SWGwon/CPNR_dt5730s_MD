#include "ConfigParser.h"
#include "DAQConfig.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

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
        "RecordLength=1024\n"
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
    Check(settings.record_length == 1024, "DAQ schema record length");
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

    const auto relative_threshold_path = test_dir / "relative_threshold.conf";
    WriteFile(relative_threshold_path,
              "[Digitizer]\n"
              "RecordLength=1024\n"
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
              "[Digitizer]\nRecordLength=1024\nChannelMask=15\n"
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
              "RecordLength=1024\n"
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
              "RecordLength=1024\n"
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
              "RecordLength=1024\n"
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
              "RecordLength=1024\n"
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
              "RecordLength=1024\n"
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
    non_multiple_config.replace(non_multiple_config.find("RecordLength=1024"),
                                std::string("RecordLength=1024").size(),
                                "RecordLength=1025");
    WriteFile(non_multiple_path, non_multiple_config);
    ConfigParser non_multiple(non_multiple_path.string());
    CheckThrows([&]() { LoadDAQHardwareSettings(non_multiple); },
                "multiple of 8", "unaligned record length fails closed");

    const auto short_pretrigger_path = test_dir / "short_pretrigger.conf";
    std::string short_pretrigger_config = valid_daq_config;
    short_pretrigger_config.replace(short_pretrigger_config.find("RecordLength=1024"),
                                    std::string("RecordLength=1024").size(),
                                    "RecordLength=128");
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
                "cannot both be disabled", "missing trigger source fails closed");

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
