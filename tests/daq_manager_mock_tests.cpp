#include "DAQManager.h"
#include "Sha256.h"

#include <CAENDigitizer.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
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

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("Cannot read test artifact: " + path.string());
  std::ostringstream content;
  content << input.rdbuf();
  return content.str();
}

void WriteConfig(const std::filesystem::path& path, uint32_t timeout_ms) {
  std::ofstream output(path);
  if (!output) throw std::runtime_error("Cannot write mock config");
  output << "[Digitizer]\n"
         << "RecordLength=512\n"
         << "ChannelMask=15\n"
         << "SelfTriggerMask=3\n"
         << "PostTrigger=60\n"
         << "InputRangeMv=2000\n"
         << "ADCBits=14\n"
         << "TriggerPolarity=1\n"
         << "ExtTriggerMode=0\n"
         << "SelfTriggerMode=1\n"
         << "[HardwareCoincidence]\n"
         << "PairLogic=AND\n"
         << "[TriggerCalibration]\n"
         << "SettlingTimeMs=0\n"
         << "SettlingTimeoutMs=" << timeout_ms << "\n"
         << "MeasurementEvents=1\n"
         << "StabilityToleranceAdc=2.0\n"
         << "StableMeasurements=3\n"
         << "[Synchronization]\n"
         << "ClockSource=0\n"
         << "RunSyncMode=0\n"
         << "[Channel_0]\n"
         << "DCOffset=3276\n"
         << "TriggerThresholdMv=1.0\n"
         << "[Channel_1]\n"
         << "DCOffset=3276\n"
         << "TriggerThresholdMv=1.0\n"
         << "[Channel_2]\n"
         << "DCOffset=3276\n"
         << "[Channel_3]\n"
         << "DCOffset=3276\n";
}

void CheckThrowsWith(const std::function<void()>& action,
                     const std::string& expected_text,
                     const std::string& description) {
  try {
    action();
    Check(false, description + ": expected an exception");
  } catch (const std::exception& error) {
    Check(std::string(error.what()).find(expected_text) != std::string::npos,
          description + ": unexpected message: " + error.what());
  } catch (...) {
    Check(false, description + ": unexpected exception type");
  }
}

}  // namespace

int main() {
  const auto suffix = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const auto test_dir = std::filesystem::temp_directory_path() /
                        ("cpnr_daq_mock_" + suffix);
  std::filesystem::create_directories(test_dir);
  const std::string mock_executable =
      std::filesystem::canonical("/proc/self/exe").string();

  try {
    const auto config_path = test_dir / "stable.conf";
    const auto output_path = test_dir / "stable.dat";
    const auto metadata_path = test_dir / "stable.dat.run.json";
    WriteConfig(config_path, 250);
    caen_mock::SetUnstableBaseline(false);

    std::string metadata_before_start;
    {
      DAQManager manager(config_path.string(), output_path.string(), 0, 0, 42,
                         metadata_path.string(), mock_executable,
                         "mock-commit", "mock-build");
      metadata_before_start = ReadFile(
          metadata_path.string() +
          ".status.hardware_verified_not_started.json");
      std::atomic<bool> stop_immediately{false};
      manager.Start(stop_immediately);
    }

    Check(caen_mock::state.software_triggers_sent >= 4U,
          "software triggers collect stable baselines and final confirmation");
    Check(caen_mock::state.dc_offsets[0] == 3276U &&
              caen_mock::state.dc_offsets[1] == 3276U,
          "per-channel DC offsets are written and read back");
    Check(caen_mock::state.polarities[0] ==
                  CAEN_DGTZ_TriggerOnFallingEdge &&
              caen_mock::state.polarities[1] ==
                  CAEN_DGTZ_TriggerOnFallingEdge,
          "falling trigger polarity is programmed per channel");
    Check((caen_mock::state.registers[0x1028] & 0x1U) == 0U &&
              (caen_mock::state.registers[0x1128] & 0x1U) == 0U,
          "2 Vpp input range is explicitly written and read back");
    Check((caen_mock::state.registers[0x8100] & (1U << 6)) == 0U &&
              caen_mock::state.run_sync_mode ==
                  CAEN_DGTZ_RUN_SYNC_Disabled,
          "internal clock and disabled run sync are applied and read back");
    Check(caen_mock::state.thresholds[0] == 16156U,
          "CH0 1 mV threshold uses measured baseline 16164 minus 8 ADC");
    Check(caen_mock::state.thresholds[1] == 16247U,
          "CH1 1 mV threshold uses measured baseline 16255 minus 8 ADC");
    Check(caen_mock::state.thresholds[2] == 0U &&
              caen_mock::state.thresholds[3] == 0U,
          "CH2/CH3 record-only channels do not program discriminator registers");
    Check((caen_mock::state.registers[0x1084] & 0x7U) == 0x4U,
          "CH0 AND CH1 uses x730 over-threshold level AND field");
    Check((caen_mock::state.registers[0x810C] & 0x0FU) == 0x1U,
          "global trigger mask enables the CH0/CH1 pair request");
    Check((caen_mock::state.registers[0x810C] & (1U << 31)) == 0U,
          "software trigger is disabled before the physics run");

    const std::string metadata = ReadFile(metadata_path);
    Check(metadata.find("\"run_number\": 42") != std::string::npos,
          "runtime JSON records the run number");
    Check(metadata.find("\"config_sha256\": \"" +
                            Sha256Hex(ReadFile(config_path)) + "\"") !=
              std::string::npos,
          "runtime JSON authenticates the frozen config bytes");
    Check(metadata.find("\"binary_sha256\": \"" +
                            Sha256FileHex(mock_executable) + "\"") !=
              std::string::npos,
          "runtime JSON authenticates the exact launched executable");
    Check(metadata.find("\"raw_output_size_bytes\": 0") !=
              std::string::npos &&
              metadata.find(
                  "\"raw_output_sha256\": \"e3b0c44298fc1c149afbf4c8996fb924"
                  "27ae41e4649b934ca495991b7852b855\"") !=
                  std::string::npos,
          "completed runtime JSON authenticates the exact raw bytes");
    Check(metadata_before_start.find(
              "\"acquisition_status\": \"hardware_verified_not_started\"") !=
              std::string::npos,
          "runtime JSON distinguishes verified hardware from a started run");
    Check(metadata.find("\"acquisition_status\": \"completed\"") !=
              std::string::npos,
          "runtime JSON records successful acquisition completion");
    Check(metadata.find("\"model\": \"MOCK-DT5730S\"") !=
              std::string::npos &&
              metadata.find("\"serial_number\": 5730") != std::string::npos &&
              metadata.find("\"roc_firmware\": \"3.9\"") !=
                  std::string::npos &&
              metadata.find("\"amc_firmware\": \"4.8\"") !=
                  std::string::npos,
          "runtime JSON records actual board identity and firmware");
    Check(metadata.find("\"pair_logic\": \"AND\"") != std::string::npos,
          "runtime JSON records AND routing");
    Check(metadata.find("\"record_mask_readback\": 15") !=
              std::string::npos,
          "runtime JSON records the channel-enable mask readback");
    Check(metadata.find("\"measured_baseline_adc\": 16164") !=
              std::string::npos,
          "runtime JSON records the CH0 measured baseline");
    Check(metadata.find("\"written_threshold_adc\": 16156") !=
              std::string::npos,
          "runtime JSON records the CH0 absolute threshold");
    Check(metadata.find("\"readback_threshold_adc\": 16247") !=
              std::string::npos,
          "runtime JSON records the CH1 threshold readback");
    Check(metadata.find("\"threshold_mode\": \"not_used_record_only\"") !=
              std::string::npos,
          "runtime JSON identifies record-only channels without thresholds");
    Check(metadata.find(
              "\"channel\": 2, \"trigger_enabled\": false") !=
              std::string::npos &&
              metadata.find(
                  "\"channel\": 2, \"trigger_enabled\": false, "
                  "\"input_range_register\": 4648") != std::string::npos,
          "runtime JSON identifies CH2 as record-only");
    Check(metadata.find("\"measured_baseline_adc\": 8192") !=
              std::string::npos,
          "runtime JSON retains measured baseline for record-only channels");
    Check(std::filesystem::exists(output_path.string() + ".config.conf"),
          "runtime config snapshot is written beside raw output");

    const auto nonempty_output = test_dir / "nonempty.dat";
    const auto nonempty_metadata = test_dir / "nonempty.dat.run.json";
    {
      DAQManager manager(
          config_path.string(), nonempty_output.string(), 1, 0, 49,
          nonempty_metadata.string(), mock_executable,
          "mock-commit", "mock-build");
      caen_mock::state.pending_events = 1U;
      std::atomic<bool> keep_running{true};
      manager.Start(keep_running);
    }
    const auto nonempty_size = std::filesystem::file_size(nonempty_output);
    const std::string nonempty_json = ReadFile(nonempty_metadata);
    Check(nonempty_size > 0U &&
              nonempty_json.find("\"raw_output_size_bytes\": " +
                                     std::to_string(nonempty_size)) !=
                  std::string::npos,
          "non-empty acquisition records its finalized raw size");
    Check(nonempty_json.find("\"raw_output_sha256\": \"" +
                                 Sha256FileHex(nonempty_output.string()) +
                                 "\"") != std::string::npos,
          "non-empty acquisition hashes the exact O_RDWR raw descriptor");

    const auto existing_output = test_dir / "existing.dat";
    {
      std::ofstream sentinel(existing_output);
      sentinel << "do-not-overwrite";
    }
    CheckThrowsWith(
        [&]() {
          DAQManager manager(
              config_path.string(), existing_output.string(), 0, 0, 46,
              (test_dir / "existing.dat.run.json").string(),
              mock_executable, "mock-commit", "mock-build");
        },
        "refusing to overwrite",
        "an existing raw output path blocks setup before it can be modified");
    Check(ReadFile(existing_output) == "do-not-overwrite",
          "existing raw output content is preserved byte-for-byte");

    const auto shape_fault_output = test_dir / "shape_fault.dat";
    const auto shape_fault_metadata =
        test_dir / "shape_fault.dat.run.json";
    {
      DAQManager manager(
          config_path.string(), shape_fault_output.string(), 1, 0, 48,
          shape_fault_metadata.string(), mock_executable,
          "mock-commit", "mock-build");
      caen_mock::SetCorruptEventRecordLength(true);
      caen_mock::state.pending_events = 1U;
      std::atomic<bool> keep_running{true};
      CheckThrowsWith(
          [&]() { manager.Start(keep_running); },
          "channel mask/record length does not match",
          "decoded trace length differing from configured RecordLength fails");
      caen_mock::SetCorruptEventRecordLength(false);
    }
    Check(ReadFile(shape_fault_metadata).find(
              "\"acquisition_status\": \"failed\"") != std::string::npos,
          "runtime JSON records a decoded acquisition-shape failure");
    Check(ReadFile(shape_fault_metadata).find(
              "\"raw_output_sha256\": null") != std::string::npos,
          "failed runtime JSON never claims a finalized raw digest");

    const auto readout_fault_output = test_dir / "readout_fault.dat";
    const auto readout_fault_metadata =
        test_dir / "readout_fault.dat.run.json";
    {
      DAQManager manager(
          config_path.string(), readout_fault_output.string(), 1, 0, 47,
          readout_fault_metadata.string(), mock_executable,
          "mock-commit", "mock-build");
      caen_mock::SetReadoutFailure(true);
      std::atomic<bool> keep_running{true};
      CheckThrowsWith(
          [&]() { manager.Start(keep_running); },
          "Persistent CAEN readout failure",
          "three consecutive CAEN readout errors fail the run");
      caen_mock::SetReadoutFailure(false);
    }
    const std::string readout_fault_json = ReadFile(readout_fault_metadata);
    Check(readout_fault_json.find("\"acquisition_status\": \"failed\"") !=
              std::string::npos &&
              readout_fault_json.find("Persistent CAEN readout failure") !=
                  std::string::npos,
          "runtime JSON records persistent hardware/driver readout failure");

    const auto threshold_fault_output = test_dir / "threshold_fault.dat";
    const auto threshold_fault_metadata =
        test_dir / "threshold_fault.dat.run.json";
    caen_mock::SetThresholdReadbackFault(true);
    CheckThrowsWith(
        [&]() {
          DAQManager manager(
              config_path.string(), threshold_fault_output.string(), 0, 0, 43,
              threshold_fault_metadata.string(), mock_executable,
              "mock-commit", "mock-build");
        },
        "threshold readback mismatch",
        "a discriminator register mismatch blocks acquisition");
    Check(!std::filesystem::exists(threshold_fault_metadata),
          "threshold readback failure does not publish runtime metadata");
    caen_mock::SetThresholdReadbackFault(false);

    const auto routing_fault_output = test_dir / "routing_fault.dat";
    const auto routing_fault_metadata = test_dir / "routing_fault.dat.run.json";
    caen_mock::SetPairLogicReadbackFault(true);
    CheckThrowsWith(
        [&]() {
          DAQManager manager(
              config_path.string(), routing_fault_output.string(), 0, 0, 44,
              routing_fault_metadata.string(), mock_executable,
              "mock-commit", "mock-build");
        },
        "Adjacent-channel trigger logic register verification failed",
        "an AND/OR routing register mismatch blocks acquisition");
    Check(!std::filesystem::exists(routing_fault_metadata),
          "routing readback failure does not publish runtime metadata");
    caen_mock::SetPairLogicReadbackFault(false);

    const auto channel_mask_fault_output =
        test_dir / "channel_mask_fault.dat";
    const auto channel_mask_fault_metadata =
        test_dir / "channel_mask_fault.dat.run.json";
    caen_mock::SetChannelMaskReadbackFault(true);
    CheckThrowsWith(
        [&]() {
          DAQManager manager(
              config_path.string(), channel_mask_fault_output.string(), 0, 0,
              50, channel_mask_fault_metadata.string(), mock_executable,
              "mock-commit", "mock-build");
        },
        "Channel-enable mask readback mismatch",
        "a channel-enable mask mismatch blocks acquisition before recording");
    Check(!std::filesystem::exists(channel_mask_fault_metadata),
          "channel-mask readback failure does not publish runtime metadata");
    caen_mock::SetChannelMaskReadbackFault(false);

    const auto adc_bits_fault_output = test_dir / "adc_bits_fault.dat";
    const auto adc_bits_fault_metadata =
        test_dir / "adc_bits_fault.dat.run.json";
    caen_mock::SetAdcBitsFault(true);
    CheckThrowsWith(
        [&]() {
          DAQManager manager(
              config_path.string(), adc_bits_fault_output.string(), 0, 0, 51,
              adc_bits_fault_metadata.string(), mock_executable,
              "mock-commit", "mock-build");
        },
        "Board ADC resolution does not match config",
        "a non-14-bit board identity blocks threshold calibration");
    Check(!std::filesystem::exists(adc_bits_fault_metadata),
          "ADC-resolution mismatch does not publish runtime metadata");
    caen_mock::SetAdcBitsFault(false);

    const auto unstable_config = test_dir / "unstable.conf";
    const auto unstable_output = test_dir / "unstable.dat";
    WriteConfig(unstable_config, 15);
    caen_mock::SetUnstableBaseline(true);
    CheckThrowsWith(
        [&]() {
          DAQManager manager(
              unstable_config.string(), unstable_output.string(), 0, 0, 45,
              (test_dir / "unstable.dat.run.json").string(),
              mock_executable, "mock-commit", "mock-build");
        },
        "Baseline settling failed",
        "continuously drifting baselines fail before acquisition");
    Check(!std::filesystem::exists(test_dir / "unstable.dat.run.json"),
          "failed settling does not publish successful runtime metadata");
  } catch (const std::exception& error) {
    std::cerr << "[FATAL] DAQ manager mock test setup failed: "
              << error.what() << '\n';
    ++failures;
  }

  std::error_code cleanup_error;
  std::filesystem::remove_all(test_dir, cleanup_error);
  if (cleanup_error) {
    std::cerr << "[FAIL] Cannot clean mock test directory: "
              << cleanup_error.message() << '\n';
    ++failures;
  }

  if (failures != 0) {
    std::cerr << failures << " DAQ manager mock test(s) failed.\n";
    return 1;
  }
  std::cout << "All DAQ manager mock tests passed.\n";
  return 0;
}
