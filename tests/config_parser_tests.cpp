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
        "PostTrigger=70\n"
        "TriggerPolarity=1\n"
        "ExtTriggerMode=0\n"
        "SelfTriggerMode=1\n"
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
    Check(settings.channels[1].trigger_threshold == 14615,
          "DAQ schema active-channel settings");

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
        LoadDAQHardwareSettings(shipped_parser);
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
