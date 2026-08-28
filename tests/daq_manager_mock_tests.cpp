#include "DAQManager.h"
#include "DT5730Status.h"
#include "DT5730Timing.h"
#include "Sha256.h"

#include <CAENDigitizer.h>
#include <zmq.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

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

void ReplaceFileText(const std::filesystem::path& path,
                     const std::string& from, const std::string& to) {
  std::string content = ReadFile(path);
  const std::size_t position = content.find(from);
  if (position == std::string::npos) {
    throw std::runtime_error("Cannot find test config token: " + from);
  }
  content.replace(position, from.size(), to);
  std::ofstream output(path, std::ios::trunc);
  if (!output) throw std::runtime_error("Cannot rewrite mock config");
  output << content;
}

void WriteConfig(const std::filesystem::path& path, uint32_t timeout_ms,
                 uint32_t minimum_free_mib = 1024,
                 uint32_t stop_free_mib = 512,
                 uint32_t record_length = 512,
                 uint32_t channel_mask = 15,
                 uint32_t settling_time_ms = 0,
                 uint64_t max_lost_events = 0,
                 double max_lost_fraction = 0.0) {
  std::ofstream output(path);
  if (!output) throw std::runtime_error("Cannot write mock config");
  output << std::setprecision(17);
  output << "[Connection]\n"
         << "Type=USB\n"
         << "Link=0\n"
         << "Node=0\n"
         << "BaseAddress=0\n"
         << "ExpectedModel=MOCK-DT5730S\n"
         << "ExpectedSerial=5730\n"
         << "[Digitizer]\n"
         << "RecordLength=" << record_length << "\n"
         << "ChannelMask=" << channel_mask << "\n"
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
         << "SettlingTimeMs=" << settling_time_ms << "\n"
         << "SettlingTimeoutMs=" << timeout_ms << "\n"
         << "MeasurementEvents=1\n"
         << "StabilityToleranceAdc=2.0\n"
         << "StableMeasurements=3\n"
         << "[Storage]\n"
         << "MinimumFreeMiB=" << minimum_free_mib << "\n"
         << "StopFreeMiB=" << stop_free_mib << "\n"
         << "[DataQuality]\n"
         << "MaxLostEvents=" << max_lost_events << "\n"
         << "MaxLostFraction=" << max_lost_fraction << "\n"
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
      DAQManager manager(config_path.string(), output_path.string(), 1, 0, 42,
                         metadata_path.string(), mock_executable,
                         "mock-commit", "mock-build");
      metadata_before_start = ReadFile(
          metadata_path.string() +
          ".status.hardware_verified_not_started.json");
      caen_mock::state.pending_events = 1U;
      std::atomic<bool> keep_running{true};
      manager.Start(keep_running);
    }

    Check(caen_mock::state.software_triggers_sent >= 4U,
          "software triggers collect stable baselines and final confirmation");
    Check(std::filesystem::exists(output_path) &&
              (caen_mock::state.registers[
                   dt5730_status::kAcquisitionStatusRegister] &
               1U) == 0U,
          "normal acquisition completes using x730 RUN bit 2 without the "
          "legacy bit-0 assumption");
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
    Check(metadata.find("\"schema_version\": 2") != std::string::npos,
          "runtime JSON uses the completed lifecycle/timing schema v2");
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
    Check(metadata.find("\"raw_output_size_bytes\": " +
                            std::to_string(
                                std::filesystem::file_size(output_path))) !=
              std::string::npos &&
              metadata.find("\"raw_output_sha256\": \"" +
                                Sha256FileHex(output_path.string()) +
                                "\"") != std::string::npos,
          "completed runtime JSON authenticates the exact raw bytes");
    Check(metadata_before_start.find(
              "\"acquisition_status\": \"hardware_verified_not_started\"") !=
              std::string::npos,
          "runtime JSON distinguishes verified hardware from a started run");
    Check(metadata.find("\"acquisition_status\": \"completed\"") !=
              std::string::npos,
          "runtime JSON records successful acquisition completion");
    Check(metadata.find("\"zmq_send_hwm_messages\": 5000") !=
                  std::string::npos &&
              metadata.find("\"zmq_send_hwm_approx_bytes\": 20600000") !=
                  std::string::npos,
          "runtime JSON records the byte-bounded monitoring queue capacity");
    Check(metadata.find("\"trigger_time_tag_raw_lsb_ns\": 8") !=
                  std::string::npos &&
              metadata.find(
                  "\"trigger_time_tag_observable_resolution_ns\": 16") !=
                  std::string::npos &&
              metadata.find("\"adc_sample_period_ns\": 2") !=
                  std::string::npos &&
              metadata.find(
                  "\"dead_time_measurement_available\": false") !=
                  std::string::npos &&
              metadata.find(
                  "\"dead_time_method\": "
                  "\"unavailable_no_hardware_busy_or_livetime_scaler\"") !=
                  std::string::npos,
          "runtime JSON distinguishes the 8 ns raw TTT LSB, 16 ns observable "
          "resolution, and unavailable measured dead time");
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

    const auto cancelled_output = test_dir / "cancelled.dat";
    const auto cancelled_metadata = test_dir / "cancelled.dat.run.json";
    {
      DAQManager manager(
          config_path.string(), cancelled_output.string(), 0, 0, 57,
          cancelled_metadata.string(), mock_executable,
          "mock-commit", "mock-build");
      std::atomic<bool> cancelled{false};
      manager.Start(cancelled);
    }
    const std::string cancelled_json = ReadFile(cancelled_metadata);
    Check(cancelled_json.find("\"acquisition_status\": \"cancelled\"") !=
              std::string::npos &&
              cancelled_json.find(
                  "\"termination_reason\": \"cancelled_before_start\"") !=
                  std::string::npos &&
              !std::filesystem::exists(cancelled_output) &&
              std::filesystem::exists(
                  cancelled_output.string() + ".partial"),
          "pre-start cancellation never masquerades as a completed empty run");

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
    Check(nonempty_json.find("\"recorded_events\": 1") !=
              std::string::npos &&
              nonempty_json.find("\"termination_reason\": \"event_limit\"") !=
                  std::string::npos &&
              nonempty_json.find(
                  "\"raw_digest_method\": "
                  "\"streaming_sha256_verified_by_descriptor_sha256\"") !=
                  std::string::npos &&
              nonempty_json.find("\"raw_recovery_performed\": false") !=
                  std::string::npos &&
              nonempty_json.find("\"lost_events_exact\": true") !=
                  std::string::npos,
          "runtime JSON records exact event-limit termination and streaming digest");

    const auto zmq_send_output = test_dir / "zmq_send_error.dat";
    const auto zmq_send_metadata =
        test_dir / "zmq_send_error.dat.run.json";
    {
      DAQManager manager(
          config_path.string(), zmq_send_output.string(), 1, 0, 60,
          zmq_send_metadata.string(), mock_executable,
          "mock-commit", "mock-build");
      zmq_mock::SetSendFailure(EIO);
      caen_mock::state.pending_events = 1U;
      std::atomic<bool> keep_running{true};
      manager.Start(keep_running);
      zmq_mock::SetSendFailure(0);
    }
    const std::string zmq_send_json = ReadFile(zmq_send_metadata);
    Check(std::filesystem::exists(zmq_send_output) &&
              zmq_send_json.find("\"acquisition_status\": \"completed\"") !=
                  std::string::npos &&
              zmq_send_json.find("\"zmq_send_errors\": 1") !=
                  std::string::npos &&
              zmq_send_json.find(
                  "\"zmq_nonblocking_send_failures\": 1") !=
                  std::string::npos &&
              zmq_send_json.find(
                  "\"subscriber_delivery_evidence\": "
                  "\"unavailable_pub_socket_may_drop_silently\"") !=
                  std::string::npos,
          "a monitoring send failure is recorded without overstating "
          "subscriber delivery or sacrificing raw data");

    const auto exact_limit_output = test_dir / "exact_limit.dat";
    const auto exact_limit_metadata =
        test_dir / "exact_limit.dat.run.json";
    {
      DAQManager manager(
          config_path.string(), exact_limit_output.string(), 2, 0, 52,
          exact_limit_metadata.string(), mock_executable,
          "mock-commit", "mock-build");
      caen_mock::state.pending_events = 5U;
      std::atomic<bool> keep_running{true};
      manager.Start(keep_running);
    }
    constexpr std::uintmax_t kMockEventBytes =
        sizeof(EventHeader) + 4U * 512U * sizeof(uint16_t);
    Check(std::filesystem::file_size(exact_limit_output) ==
              2U * kMockEventBytes,
          "a five-event CAEN block obeys an exact two-event output limit");
    const std::string exact_limit_json = ReadFile(exact_limit_metadata);
    Check(exact_limit_json.find("\"recorded_events\": 2") !=
              std::string::npos &&
              exact_limit_json.find(
                  "\"raw_output_size_bytes\": " +
                  std::to_string(2U * kMockEventBytes)) != std::string::npos,
          "exact-limit metadata matches the two complete records on disk");
    Check(exact_limit_json.find("\"first_extended_ttt\": 0") !=
                  std::string::npos &&
              exact_limit_json.find("\"last_extended_ttt\": 2") !=
                  std::string::npos &&
              exact_limit_json.find("\"elapsed_time_sec\": 1.6e-08") !=
                  std::string::npos &&
              exact_limit_json.find(
                  "\"recorded_window_to_elapsed_pct\": 12800") !=
                  std::string::npos,
          "timing summary uses the 8 ns raw TTT count and labels record-window "
          "load separately from dead time");
    Check(!std::filesystem::exists(exact_limit_output.string() + ".partial"),
          "a completed run atomically removes its partial raw name");

    const auto decode_fault_output = test_dir / "decode_fault.dat";
    const auto decode_fault_metadata =
        test_dir / "decode_fault.dat.run.json";
    {
      DAQManager manager(
          config_path.string(), decode_fault_output.string(), 3, 0, 53,
          decode_fault_metadata.string(), mock_executable,
          "mock-commit", "mock-build");
      caen_mock::SetDecodeFailureEventIndex(1);
      caen_mock::state.pending_events = 3U;
      std::atomic<bool> keep_running{true};
      CheckThrowsWith(
          [&]() { manager.Start(keep_running); },
          "remaining block was not silently skipped",
          "a mid-block decode error immediately fails instead of dropping "
          "the unread block remainder");
      caen_mock::SetDecodeFailureEventIndex(-1);
    }
    const auto decode_fault_partial =
        std::filesystem::path(decode_fault_output.string() + ".partial");
    const std::string decode_fault_json = ReadFile(decode_fault_metadata);
    Check(!std::filesystem::exists(decode_fault_output) &&
              std::filesystem::file_size(decode_fault_partial) ==
                  kMockEventBytes,
          "failed acquisition keeps only one complete event under the partial name");
    Check(decode_fault_json.find("\"recorded_events\": 1") !=
              std::string::npos &&
              decode_fault_json.find("\"raw_output_published\": false") !=
                  std::string::npos &&
              decode_fault_json.find("\"raw_output_sha256\": \"" +
                                         Sha256FileHex(
                                             decode_fault_partial.string()) +
                                         "\"") != std::string::npos,
          "failed metadata authenticates the exact durable event prefix");

    const auto null_wave_output = test_dir / "null_wave.dat";
    const auto null_wave_metadata = test_dir / "null_wave.dat.run.json";
    {
      DAQManager manager(
          config_path.string(), null_wave_output.string(), 1, 0, 59,
          null_wave_metadata.string(), mock_executable,
          "mock-commit", "mock-build");
      caen_mock::SetNullActiveWaveform(true);
      caen_mock::state.pending_events = 1U;
      std::atomic<bool> keep_running{true};
      CheckThrowsWith(
          [&]() { manager.Start(keep_running); },
          "waveform pointer is null",
          "null active-channel decode pointer fails without memory access");
      caen_mock::SetNullActiveWaveform(false);
    }
    Check(ReadFile(null_wave_metadata).find(
              "\"recorded_events\": 0") != std::string::npos,
          "null waveform failure records an empty authenticated prefix");

    const auto stop_fault_output = test_dir / "stop_fault.dat";
    const auto stop_fault_metadata = test_dir / "stop_fault.dat.run.json";
    {
      DAQManager manager(
          config_path.string(), stop_fault_output.string(), 1, 0, 60,
          stop_fault_metadata.string(), mock_executable,
          "mock-commit", "mock-build");
      caen_mock::state.pending_events = 1U;
      caen_mock::SetStopAcquisitionFailure(true);
      std::atomic<bool> keep_running{true};
      CheckThrowsWith(
          [&]() { manager.Start(keep_running); },
          "SWStopAcquisition",
          "hardware stop acknowledgement failure prevents completed status");
      caen_mock::SetStopAcquisitionFailure(false);
    }
    const std::string stop_fault_json = ReadFile(stop_fault_metadata);
    Check(stop_fault_json.find("\"acquisition_status\": \"failed\"") !=
              std::string::npos &&
              stop_fault_json.find("\"recorded_events\": 1") !=
                  std::string::npos,
          "stop acknowledgement failure still authenticates saved events");

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
    const std::string shape_fault_json = ReadFile(shape_fault_metadata);
    Check(shape_fault_json.find("\"raw_output_finalized\": true") !=
              std::string::npos &&
              shape_fault_json.find("\"raw_output_size_bytes\": 0") !=
                  std::string::npos &&
              shape_fault_json.find(
                  "\"raw_output_sha256\": "
                  "\"e3b0c44298fc1c149afbf4c8996fb924"
                  "27ae41e4649b934ca495991b7852b855\"") !=
                  std::string::npos,
          "failed runtime JSON authenticates the durable complete-event prefix");
    Check(!std::filesystem::exists(shape_fault_output) &&
              std::filesystem::exists(
                  shape_fault_output.string() + ".partial"),
          "a failed run never exposes its partial bytes under the final raw name");

    const auto zero_event_output = test_dir / "zero_event_block.dat";
    const auto zero_event_metadata =
        test_dir / "zero_event_block.dat.run.json";
    {
      DAQManager manager(
          config_path.string(), zero_event_output.string(), 1, 0, 63,
          zero_event_metadata.string(), mock_executable,
          "mock-commit", "mock-build");
      caen_mock::SetForceZeroEventCount(true);
      caen_mock::state.pending_events = 1U;
      std::atomic<bool> keep_running{true};
      CheckThrowsWith(
          [&]() { manager.Start(keep_running); },
          "non-empty readout block containing zero events",
          "a non-empty CAEN transfer cannot be silently treated as empty");
      caen_mock::SetForceZeroEventCount(false);
    }
    Check(ReadFile(zero_event_metadata).find(
              "\"acquisition_status\": \"failed\"") != std::string::npos,
          "zero-event block corruption is recorded in terminal metadata");

    const auto duplicate_counter_output =
        test_dir / "duplicate_counter.dat";
    const auto duplicate_counter_metadata =
        test_dir / "duplicate_counter.dat.run.json";
    {
      DAQManager manager(
          config_path.string(), duplicate_counter_output.string(), 2, 0, 64,
          duplicate_counter_metadata.string(), mock_executable,
          "mock-commit", "mock-build");
      caen_mock::SetEventCounterSequence({7U, 7U});
      caen_mock::state.pending_events = 2U;
      std::atomic<bool> keep_running{true};
      CheckThrowsWith(
          [&]() { manager.Start(keep_running); }, "event counter repeated",
          "duplicate board event counters fail before duplicate data is saved");
      caen_mock::SetEventCounterSequence({});
    }
    Check(ReadFile(duplicate_counter_metadata).find(
              "\"recorded_events\": 1") != std::string::npos,
          "duplicate-counter failure preserves only the authenticated prefix");

    const auto backward_counter_output =
        test_dir / "backward_counter.dat";
    const auto backward_counter_metadata =
        test_dir / "backward_counter.dat.run.json";
    {
      DAQManager manager(
          config_path.string(), backward_counter_output.string(), 2, 0, 65,
          backward_counter_metadata.string(), mock_executable,
          "mock-commit", "mock-build");
      caen_mock::SetEventCounterSequence({100U, 99U});
      caen_mock::state.pending_events = 2U;
      std::atomic<bool> keep_running{true};
      CheckThrowsWith(
          [&]() { manager.Start(keep_running); },
          "event counter moved backwards or reset",
          "a board counter reset cannot be misreported as millions of losses");
      caen_mock::SetEventCounterSequence({});
    }
    Check(ReadFile(backward_counter_metadata).find(
              "\"lost_events\": 0") != std::string::npos,
          "counter-reset failure does not publish a fabricated loss count");

    const auto event_header_failure_output =
        test_dir / "event_header_board_failure.dat";
    const auto event_header_failure_metadata =
        test_dir / "event_header_board_failure.dat.run.json";
    {
      DAQManager manager(
          config_path.string(), event_header_failure_output.string(), 2, 0,
          78, event_header_failure_metadata.string(), mock_executable,
          "mock-commit", "mock-build");
      caen_mock::SetEventHeaderBoardFailureIndex(1);
      caen_mock::state.pending_events = 2U;
      std::atomic<bool> keep_running{true};
      CheckThrowsWith(
          [&]() { manager.Start(keep_running); },
          "raw event header asserted Board Fail in word 1",
          "raw header word 1 bit 26 rejects a hardware-fault event before "
          "decode or persistence");
      caen_mock::SetEventHeaderBoardFailureIndex(-1);
    }
    const std::string event_header_failure_json =
        ReadFile(event_header_failure_metadata);
    Check(event_header_failure_json.find(
              "\"acquisition_status\": \"failed\"") !=
                  std::string::npos &&
              event_header_failure_json.find("\"recorded_events\": 1") !=
                  std::string::npos &&
              !std::filesystem::exists(event_header_failure_output) &&
              std::filesystem::file_size(
                  event_header_failure_output.string() + ".partial") ==
                  kMockEventBytes,
          "event-header Board Fail metadata and partial RAW describe exactly "
          "the one-event healthy prefix");

    const auto repeated_ttt_output = test_dir / "repeated_ttt.dat";
    const auto repeated_ttt_metadata =
        test_dir / "repeated_ttt.dat.run.json";
    {
      DAQManager manager(
          config_path.string(), repeated_ttt_output.string(), 2, 0, 79,
          repeated_ttt_metadata.string(), mock_executable,
          "mock-commit", "mock-build");
      caen_mock::SetTriggerTimeTagSequence({101U, 101U});
      caen_mock::state.pending_events = 2U;
      std::atomic<bool> keep_running{true};
      CheckThrowsWith(
          [&]() { manager.Start(keep_running); },
          "TriggerTimeTag repeated",
          "repeated raw TriggerTimeTag fails before a duplicate timestamp is "
          "persisted");
      caen_mock::SetTriggerTimeTagSequence({});
    }
    const std::string repeated_ttt_json = ReadFile(repeated_ttt_metadata);
    Check(repeated_ttt_json.find("\"recorded_events\": 1") !=
                  std::string::npos &&
              repeated_ttt_json.find("\"first_extended_ttt\": 101") !=
                  std::string::npos &&
              repeated_ttt_json.find("\"last_extended_ttt\": 101") !=
                  std::string::npos &&
              std::filesystem::file_size(
                  repeated_ttt_output.string() + ".partial") ==
                  kMockEventBytes,
          "repeated-TTT failure metadata authenticates only the first event");

    const auto rollover_ttt_output = test_dir / "rollover_ttt.dat";
    const auto rollover_ttt_metadata =
        test_dir / "rollover_ttt.dat.run.json";
    {
      DAQManager manager(
          config_path.string(), rollover_ttt_output.string(), 2, 0, 80,
          rollover_ttt_metadata.string(), mock_executable,
          "mock-commit", "mock-build");
      caen_mock::SetTriggerTimeTagSequence(
          {dt5730_timing::kTriggerTimeTagMask - 1U, 0U});
      caen_mock::state.pending_events = 2U;
      std::atomic<bool> keep_running{true};
      manager.Start(keep_running);
      caen_mock::SetTriggerTimeTagSequence({});
    }
    const std::string rollover_ttt_json = ReadFile(rollover_ttt_metadata);
    Check(rollover_ttt_json.find("\"acquisition_status\": \"completed\"") !=
                  std::string::npos &&
              rollover_ttt_json.find(
                  "\"first_extended_ttt\": 2147483646") !=
                  std::string::npos &&
              rollover_ttt_json.find(
                  "\"last_extended_ttt\": 2147483648") !=
                  std::string::npos &&
              rollover_ttt_json.find("\"elapsed_time_sec\": 1.6e-08") !=
                  std::string::npos,
          "a raw backward TTT is extended as exactly one wrap and still uses "
          "the 8 ns raw-count unit");

    const auto loss_policy_output = test_dir / "loss_policy.dat";
    const auto loss_policy_metadata =
        test_dir / "loss_policy.dat.run.json";
    {
      DAQManager manager(
          config_path.string(), loss_policy_output.string(), 2, 0, 71,
          loss_policy_metadata.string(), mock_executable,
          "mock-commit", "mock-build");
      caen_mock::SetEventCounterSequence({100U, 102U});
      caen_mock::state.pending_events = 2U;
      std::atomic<bool> keep_running{true};
      CheckThrowsWith(
          [&]() { manager.Start(keep_running); },
          "Accepted-trigger loss exceeded [DataQuality] limits",
          "default zero-loss policy fails on the first accepted-trigger gap");
      caen_mock::SetEventCounterSequence({});
    }
    const std::string loss_policy_json = ReadFile(loss_policy_metadata);
    Check(loss_policy_json.find("\"acquisition_status\": \"failed\"") !=
                  std::string::npos &&
              loss_policy_json.find("\"recorded_events\": 1") !=
                  std::string::npos &&
              loss_policy_json.find("\"lost_events\": 1") !=
                  std::string::npos &&
              loss_policy_json.find(
                  "\"lost_events_scope\": "
                  "\"accepted_trigger_counter_gaps_through_last_observed_event\"") !=
                  std::string::npos &&
              loss_policy_json.find("\"observed_event_count\": 2") !=
                  std::string::npos &&
              loss_policy_json.find(
                  "\"inferred_lost_event_count\": 1") !=
                  std::string::npos &&
              loss_policy_json.find("\"policy_exceeded\": true") !=
                  std::string::npos &&
              loss_policy_json.find(
                  "\"rejected_post_gap_event_persisted\": false") !=
                  std::string::npos,
          "quality-gate failure records the exact gap and preserves only the "
          "pre-gap raw prefix without using it as the policy denominator");

    const auto bounded_loss_config = test_dir / "bounded_loss.conf";
    WriteConfig(bounded_loss_config, 250, 1024, 512, 512, 15, 0, 1U,
                0.5);
    const auto bounded_loss_output = test_dir / "bounded_loss.dat";
    const auto bounded_loss_metadata =
        test_dir / "bounded_loss.dat.run.json";
    {
      DAQManager manager(
          bounded_loss_config.string(), bounded_loss_output.string(), 2, 0,
          81, bounded_loss_metadata.string(), mock_executable,
          "mock-commit", "mock-build");
      caen_mock::SetEventCounterSequence({100U, 102U});
      caen_mock::state.pending_events = 2U;
      std::atomic<bool> keep_running{true};
      manager.Start(keep_running);
      caen_mock::SetEventCounterSequence({});
    }
    const std::string bounded_loss_json = ReadFile(bounded_loss_metadata);
    Check(bounded_loss_json.find("\"acquisition_status\": \"completed\"") !=
                  std::string::npos &&
              bounded_loss_json.find("\"recorded_events\": 2") !=
                  std::string::npos &&
              bounded_loss_json.find("\"lost_events\": 1") !=
                  std::string::npos &&
              bounded_loss_json.find("\"observed_event_count\": 2") !=
                  std::string::npos &&
              bounded_loss_json.find(
                  "\"inferred_lost_event_count\": 1") !=
                  std::string::npos &&
              bounded_loss_json.find("\"policy_exceeded\": false") !=
                  std::string::npos &&
              std::filesystem::file_size(bounded_loss_output) ==
                  2U * kMockEventBytes,
          "loss count equal to MaxLostEvents and fraction below its limit is "
          "accepted, persisted, and reported consistently");

    const auto count_boundary_output =
        test_dir / "loss_count_boundary.dat";
    const auto count_boundary_metadata =
        test_dir / "loss_count_boundary.dat.run.json";
    {
      DAQManager manager(
          bounded_loss_config.string(), count_boundary_output.string(), 2, 0,
          82, count_boundary_metadata.string(), mock_executable,
          "mock-commit", "mock-build");
      caen_mock::SetEventCounterSequence({100U, 103U});
      caen_mock::state.pending_events = 2U;
      std::atomic<bool> keep_running{true};
      CheckThrowsWith(
          [&]() { manager.Start(keep_running); },
          "Accepted-trigger loss exceeded [DataQuality] limits",
          "loss count one above MaxLostEvents fails even when the fraction is "
          "exactly at its allowed boundary");
      caen_mock::SetEventCounterSequence({});
    }
    const std::string count_boundary_json =
        ReadFile(count_boundary_metadata);
    Check(count_boundary_json.find("\"recorded_events\": 1") !=
                  std::string::npos &&
              count_boundary_json.find("\"lost_events\": 2") !=
                  std::string::npos &&
              count_boundary_json.find("\"observed_event_count\": 2") !=
                  std::string::npos &&
              count_boundary_json.find(
                  "\"inferred_lost_event_count\": 2") !=
                  std::string::npos &&
              count_boundary_json.find("\"lost_fraction\": 0.5") !=
                  std::string::npos &&
              count_boundary_json.find("\"policy_exceeded\": true") !=
                  std::string::npos &&
              std::filesystem::file_size(
                  count_boundary_output.string() + ".partial") ==
                  kMockEventBytes,
          "count-boundary failure metadata distinguishes the persisted prefix "
          "from both observed events used by the policy");

    const auto fraction_boundary_config =
        test_dir / "loss_fraction_boundary.conf";
    WriteConfig(fraction_boundary_config, 250, 1024, 512, 512, 15, 0,
                10U, 1.0 / 3.0);
    const auto fraction_equal_output =
        test_dir / "loss_fraction_equal.dat";
    const auto fraction_equal_metadata =
        test_dir / "loss_fraction_equal.dat.run.json";
    {
      DAQManager manager(
          fraction_boundary_config.string(), fraction_equal_output.string(),
          2, 0, 83, fraction_equal_metadata.string(), mock_executable,
          "mock-commit", "mock-build");
      caen_mock::SetEventCounterSequence({100U, 102U});
      caen_mock::state.pending_events = 2U;
      std::atomic<bool> keep_running{true};
      manager.Start(keep_running);
      caen_mock::SetEventCounterSequence({});
    }
    const std::string fraction_equal_json =
        ReadFile(fraction_equal_metadata);
    Check(fraction_equal_json.find(
              "\"acquisition_status\": \"completed\"") !=
                  std::string::npos &&
              fraction_equal_json.find("\"recorded_events\": 2") !=
                  std::string::npos &&
              fraction_equal_json.find("\"lost_events\": 1") !=
                  std::string::npos &&
              fraction_equal_json.find("\"policy_exceeded\": false") !=
                  std::string::npos,
          "loss fraction exactly equal to MaxLostFraction remains allowed");

    const auto fraction_exceeded_output =
        test_dir / "loss_fraction_exceeded.dat";
    const auto fraction_exceeded_metadata =
        test_dir / "loss_fraction_exceeded.dat.run.json";
    {
      DAQManager manager(
          fraction_boundary_config.string(),
          fraction_exceeded_output.string(), 2, 0, 84,
          fraction_exceeded_metadata.string(), mock_executable,
          "mock-commit", "mock-build");
      caen_mock::SetEventCounterSequence({100U, 103U});
      caen_mock::state.pending_events = 2U;
      std::atomic<bool> keep_running{true};
      CheckThrowsWith(
          [&]() { manager.Start(keep_running); },
          "Accepted-trigger loss exceeded [DataQuality] limits",
          "loss fraction above MaxLostFraction fails while the absolute count "
          "remains allowed");
      caen_mock::SetEventCounterSequence({});
    }
    const std::string fraction_exceeded_json =
        ReadFile(fraction_exceeded_metadata);
    Check(fraction_exceeded_json.find("\"recorded_events\": 1") !=
                  std::string::npos &&
              fraction_exceeded_json.find("\"lost_events\": 2") !=
                  std::string::npos &&
              fraction_exceeded_json.find("\"observed_event_count\": 2") !=
                  std::string::npos &&
              fraction_exceeded_json.find("\"lost_fraction\": 0.5") !=
                  std::string::npos &&
              fraction_exceeded_json.find("\"policy_exceeded\": true") !=
                  std::string::npos,
          "fraction-boundary failure metadata reports the exact evaluation");

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

    const auto event_full_output = test_dir / "runtime_event_full.dat";
    const auto event_full_metadata =
        test_dir / "runtime_event_full.dat.run.json";
    {
      DAQManager manager(
          config_path.string(), event_full_output.string(), 0, 2, 76,
          event_full_metadata.string(), mock_executable,
          "mock-commit", "mock-build");
      caen_mock::SetEventFullFault(true);
      std::atomic<bool> keep_running{true};
      CheckThrowsWith(
          [&]() { manager.Start(keep_running); },
          "event memory reported FULL",
          "runtime event-full status stops acquisition before data integrity "
          "can be overstated");
      caen_mock::SetEventFullFault(false);
    }
    const std::string event_full_json = ReadFile(event_full_metadata);
    Check(event_full_json.find("\"acquisition_status\": \"failed\"") !=
                  std::string::npos &&
              event_full_json.find("event memory reported FULL") !=
                  std::string::npos &&
              !std::filesystem::exists(event_full_output),
          "runtime event-full failure is recorded without publishing raw data");

    const auto board_failure_output =
        test_dir / "runtime_board_failure.dat";
    const auto board_failure_metadata =
        test_dir / "runtime_board_failure.dat.run.json";
    {
      DAQManager manager(
          config_path.string(), board_failure_output.string(), 0, 2, 77,
          board_failure_metadata.string(), mock_executable,
          "mock-commit", "mock-build");
      caen_mock::SetBoardFailurePllFault(true);
      std::atomic<bool> keep_running{true};
      CheckThrowsWith(
          [&]() { manager.Start(keep_running); },
          "runtime board-health fault",
          "runtime board-failure register assertion stops acquisition");
      caen_mock::SetBoardFailurePllFault(false);
    }
    const std::string board_failure_json = ReadFile(board_failure_metadata);
    Check(board_failure_json.find("\"acquisition_status\": \"failed\"") !=
                  std::string::npos &&
              board_failure_json.find("runtime board-health fault") !=
                  std::string::npos &&
              !std::filesystem::exists(board_failure_output),
          "runtime board-failure status is preserved in terminal provenance");

    const auto health_fault_output = test_dir / "health_fault.dat";
    const auto health_fault_metadata =
        test_dir / "health_fault.dat.run.json";
    {
      DAQManager manager(
          config_path.string(), health_fault_output.string(), 0, 2, 54,
          health_fault_metadata.string(), mock_executable,
          "mock-commit", "mock-build");
      caen_mock::SetRuntimeHealthReadFailure(true);
      std::atomic<bool> keep_running{true};
      CheckThrowsWith(
          [&]() { manager.Start(keep_running); },
          "temperature/status safety cannot be verified",
          "three runtime health-register failures stop acquisition fail-safe");
      caen_mock::SetRuntimeHealthReadFailure(false);
    }
    Check(ReadFile(health_fault_metadata).find(
              "\"acquisition_status\": \"failed\"") != std::string::npos,
          "runtime health communication failure is recorded in metadata");

    const auto overtemp_output = test_dir / "overtemp.dat";
    const auto overtemp_metadata = test_dir / "overtemp.dat.run.json";
    {
      DAQManager manager(
          config_path.string(), overtemp_output.string(), 0, 2, 55,
          overtemp_metadata.string(), mock_executable,
          "mock-commit", "mock-build");
      caen_mock::SetRuntimeTemperature(83U);
      std::atomic<bool> keep_running{true};
      CheckThrowsWith(
          [&]() { manager.Start(keep_running); },
          "temperature reached the 82 C",
          "any enabled channel at the temperature limit stops acquisition");
      caen_mock::SetRuntimeTemperature(25U);
    }
    Check(ReadFile(overtemp_metadata).find(
              "\"acquisition_status\": \"failed\"") != std::string::npos,
          "over-temperature shutdown finalizes failed-run provenance");

    const auto run_status_output = test_dir / "run_status.dat";
    const auto run_status_metadata = test_dir / "run_status.dat.run.json";
    {
      DAQManager manager(
          config_path.string(), run_status_output.string(), 1, 0, 66,
          run_status_metadata.string(), mock_executable,
          "mock-commit", "mock-build");
      caen_mock::SetRunStatusStuckLow(true);
      caen_mock::state.registers[
          dt5730_status::kAcquisitionStatusRegister] |= 1U;
      std::atomic<bool> keep_running{true};
      CheckThrowsWith(
          [&]() { manager.Start(keep_running); },
          "did not assert the acquisition RUN status",
          "legacy bit 0 cannot mask a deasserted x730 RUN bit 2");
      caen_mock::state.registers[
          dt5730_status::kAcquisitionStatusRegister] &= ~1U;
      caen_mock::SetRunStatusStuckLow(false);
    }
    Check(ReadFile(run_status_metadata).find(
              "\"acquisition_status\": \"failed\"") != std::string::npos,
          "missing hardware RUN acknowledgement is recorded as a failure");

    const auto drift_output = test_dir / "runtime_drift.dat";
    const auto drift_metadata = test_dir / "runtime_drift.dat.run.json";
    {
      DAQManager manager(
          config_path.string(), drift_output.string(), 0, 3, 58,
          drift_metadata.string(), mock_executable,
          "mock-commit", "mock-build");
      ++caen_mock::state.thresholds[0];
      std::atomic<bool> keep_running{true};
      CheckThrowsWith(
          [&]() { manager.Start(keep_running); },
          "discriminator threshold changed",
          "periodic runtime readback stops on discriminator register drift");
    }
    Check(ReadFile(drift_metadata).find(
              "\"acquisition_status\": \"failed\"") != std::string::npos,
          "runtime configuration drift is recorded in terminal metadata");

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
    Check(ReadFile(threshold_fault_metadata).find(
              "\"termination_reason\": \"setup_failure\"") !=
              std::string::npos,
          "threshold readback failure publishes terminal setup provenance");
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
    Check(ReadFile(routing_fault_metadata).find(
              "\"termination_reason\": \"setup_failure\"") !=
              std::string::npos,
          "routing readback failure publishes terminal setup provenance");
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
    Check(ReadFile(channel_mask_fault_metadata).find(
              "\"termination_reason\": \"setup_failure\"") !=
              std::string::npos,
          "channel-mask readback failure publishes terminal setup provenance");
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
    Check(ReadFile(adc_bits_fault_metadata).find(
              "\"termination_reason\": \"setup_failure\"") !=
                  std::string::npos,
          "ADC-resolution mismatch publishes terminal setup provenance");
    caen_mock::SetAdcBitsFault(false);

    const auto baseline_header_fault_output =
        test_dir / "baseline_header_board_failure.dat";
    const auto baseline_header_fault_metadata =
        test_dir / "baseline_header_board_failure.dat.run.json";
    caen_mock::SetEventHeaderBoardFailureIndex(0);
    CheckThrowsWith(
        [&]() {
          DAQManager manager(
              config_path.string(), baseline_header_fault_output.string(), 0,
              0, 85, baseline_header_fault_metadata.string(), mock_executable,
              "mock-commit", "mock-build");
        },
        "Baseline calibration raw event header asserted Board Fail in word 1",
        "Board Fail in a software-triggered baseline event blocks threshold "
        "calibration and acquisition setup");
    caen_mock::SetEventHeaderBoardFailureIndex(-1);
    const std::string baseline_header_fault_json =
        ReadFile(baseline_header_fault_metadata);
    Check(baseline_header_fault_json.find(
              "\"termination_reason\": \"setup_failure\"") !=
                  std::string::npos &&
              baseline_header_fault_json.find(
                  "Baseline calibration raw event header asserted Board Fail") !=
                  std::string::npos &&
              !std::filesystem::exists(baseline_header_fault_output) &&
              !std::filesystem::exists(
                  baseline_header_fault_output.string() + ".partial"),
          "baseline Board Fail publishes terminal setup provenance without a "
          "RAW artifact");

    struct DeferredSetupHealthFaultCase {
      const char* artifact_stem;
      const char* description;
      void (*set_fault)(bool);
    };
    const std::array<DeferredSetupHealthFaultCase, 3>
        deferred_setup_health_faults{{
            {"setup_board_not_ready", "board-ready stays deasserted",
             &caen_mock::SetBoardNotReadyFault},
            {"setup_pll_unlock", "PLL no-unlock flag stays deasserted",
             &caen_mock::SetPllUnlockFault},
            {"setup_pll_failure", "board-failure PLL bit stays asserted",
             &caen_mock::SetBoardFailurePllFault},
        }};
    for (std::size_t index = 0;
         index < deferred_setup_health_faults.size(); ++index) {
      const auto& fault = deferred_setup_health_faults[index];
      const auto fault_output =
          test_dir / (std::string(fault.artifact_stem) + ".dat");
      const auto fault_metadata =
          test_dir / (std::string(fault.artifact_stem) + ".dat.run.json");
      fault.set_fault(true);
      CheckThrowsWith(
          [&]() {
            DAQManager manager(
                config_path.string(), fault_output.string(), 0, 0,
                70 + static_cast<int>(index), fault_metadata.string(),
                mock_executable, "mock-commit", "mock-build");
          },
          "board/PLL did not become stably ready within 2 seconds",
          std::string("setup waits fail-closed when ") + fault.description);
      fault.set_fault(false);
      Check(ReadFile(fault_metadata).find(
                "\"termination_reason\": \"setup_failure\"") !=
                    std::string::npos &&
                !std::filesystem::exists(fault_output),
            std::string("setup health timeout publishes no raw file for ") +
                fault.description);
    }

    struct ImmediateSetupHealthFaultCase {
      const char* artifact_stem;
      const char* description;
      void (*set_fault)(bool);
    };
    const std::array<ImmediateSetupHealthFaultCase, 3>
        immediate_setup_health_faults{{
            {"setup_channel_shutdown", "channel shutdown",
             &caen_mock::SetChannelShutdownFault},
            {"setup_dt_temperature", "DT temperature alarm",
             &caen_mock::SetDtTemperatureFault},
            {"setup_adc_power_down", "ADC power-down",
             &caen_mock::SetBoardFailureAdcPowerDownFault},
        }};
    for (std::size_t index = 0;
         index < immediate_setup_health_faults.size(); ++index) {
      const auto& fault = immediate_setup_health_faults[index];
      const auto fault_output =
          test_dir / (std::string(fault.artifact_stem) + ".dat");
      const auto fault_metadata =
          test_dir / (std::string(fault.artifact_stem) + ".dat.run.json");
      fault.set_fault(true);
      CheckThrowsWith(
          [&]() {
            DAQManager manager(
                config_path.string(), fault_output.string(), 0, 0,
                73 + static_cast<int>(index), fault_metadata.string(),
                mock_executable, "mock-commit", "mock-build");
          },
          "fatal board condition during setup",
          std::string("setup immediately rejects ") + fault.description);
      fault.set_fault(false);
      Check(ReadFile(fault_metadata).find(
                "\"termination_reason\": \"setup_failure\"") !=
                    std::string::npos &&
                !std::filesystem::exists(fault_output),
            std::string("fatal setup health status publishes no raw file for ") +
                fault.description);
    }

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
    Check(ReadFile(test_dir / "unstable.dat.run.json").find(
              "\"termination_reason\": \"setup_failure\"") !=
              std::string::npos,
          "failed settling publishes terminal setup provenance");
    caen_mock::SetUnstableBaseline(false);

    const auto setup_cancel_config = test_dir / "setup_cancel.conf";
    const auto setup_cancel_output = test_dir / "setup_cancel.dat";
    const auto setup_cancel_metadata =
        test_dir / "setup_cancel.dat.run.json";
    WriteConfig(setup_cancel_config, 2000, 1024, 512, 512, 15, 1500);
    caen_mock::ResetLifecycleInstrumentation();
    std::atomic<bool> continue_setup{true};
    std::thread setup_canceller([&]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue_setup.store(false, std::memory_order_relaxed);
    });
    bool caught_setup_cancellation = false;
    try {
      DAQManager manager(
          setup_cancel_config.string(), setup_cancel_output.string(), 0, 0, 64,
          setup_cancel_metadata.string(), mock_executable,
          "mock-commit", "mock-build", &continue_setup);
      Check(false, "baseline cancellation should stop construction");
    } catch (const DAQSetupCancelled&) {
      caught_setup_cancellation = true;
    } catch (const std::exception& error) {
      Check(false, "baseline cancellation used the wrong exception type: " +
                       std::string(error.what()));
    }
    setup_canceller.join();
    const std::string setup_cancel_json = ReadFile(setup_cancel_metadata);
    Check(caught_setup_cancellation &&
              setup_cancel_json.find(
                  "\"acquisition_status\": \"cancelled\"") !=
                  std::string::npos &&
              setup_cancel_json.find(
                  "\"termination_reason\": \"cancelled_during_setup\"") !=
                  std::string::npos &&
              setup_cancel_json.find("\"failure_reason\": null") !=
                  std::string::npos &&
              !std::filesystem::exists(setup_cancel_output) &&
              !std::filesystem::exists(setup_cancel_output.string() +
                                       ".partial"),
          "baseline cancellation records terminal non-failure provenance "
          "without reserving raw output");
    Check(caen_mock::open_calls == 1U && caen_mock::close_calls == 1U,
          "baseline cancellation closes the opened CAEN handle");

    const auto low_capacity_config = test_dir / "low_capacity.conf";
    WriteConfig(low_capacity_config, 250);
    caen_mock::ResetLifecycleInstrumentation();
    CheckThrowsWith(
        [&]() {
          DAQManager manager(
              low_capacity_config.string(),
              (test_dir / "low_capacity.dat").string(),
              std::numeric_limits<int>::max(), 0, 56,
              (test_dir / "low_capacity.dat.run.json").string(),
              mock_executable, "mock-commit", "mock-build");
        },
        "Insufficient output-filesystem capacity",
        "storage reserve preflight rejects an impossible run size");
    Check(caen_mock::open_calls == 0U,
          "storage preflight runs before the CAEN device is opened or reset");

    caen_mock::ResetLifecycleInstrumentation();
    const auto reset_fault_output = test_dir / "reset_fault.dat";
    const auto reset_fault_metadata = test_dir / "reset_fault.dat.run.json";
    caen_mock::SetResetFailure(true);
    CheckThrowsWith(
        [&]() {
          DAQManager manager(
              config_path.string(), reset_fault_output.string(), 1, 0, 67,
              reset_fault_metadata.string(), mock_executable,
              "mock-commit", "mock-build");
        },
        "CAEN_DGTZ_Reset",
        "a reset failure closes the opened handle and records setup failure");
    caen_mock::SetResetFailure(false);
    Check(caen_mock::open_calls == 1U && caen_mock::close_calls == 1U,
          "reset failure cannot leak the successfully opened CAEN handle");
    Check(ReadFile(reset_fault_metadata).find(
              "\"termination_reason\": \"setup_failure\"") !=
                  std::string::npos &&
              !std::filesystem::exists(reset_fault_output) &&
              !std::filesystem::exists(reset_fault_output.string() +
                                       ".partial"),
          "reset failure provenance is terminal and publishes no raw file");

    const auto wrong_serial_config = test_dir / "wrong_serial.conf";
    const auto wrong_serial_output = test_dir / "wrong_serial.dat";
    const auto wrong_serial_metadata =
        test_dir / "wrong_serial.dat.run.json";
    WriteConfig(wrong_serial_config, 250);
    ReplaceFileText(wrong_serial_config, "ExpectedSerial=5730",
                    "ExpectedSerial=9999");
    caen_mock::ResetLifecycleInstrumentation();
    CheckThrowsWith(
        [&]() {
          DAQManager manager(
              wrong_serial_config.string(), wrong_serial_output.string(), 1,
              0, 69, wrong_serial_metadata.string(), mock_executable,
              "mock-commit", "mock-build");
        },
        "serial mismatch before reset",
        "unexpected USB board serial is rejected before reset");
    Check(caen_mock::open_calls == 1U && caen_mock::reset_calls == 0U &&
              caen_mock::close_calls == 1U,
          "serial mismatch closes the handle without resetting hardware");

    const auto wrong_model_config = test_dir / "wrong_model.conf";
    const auto wrong_model_output = test_dir / "wrong_model.dat";
    const auto wrong_model_metadata = test_dir / "wrong_model.dat.run.json";
    WriteConfig(wrong_model_config, 250);
    ReplaceFileText(wrong_model_config, "ExpectedModel=MOCK-DT5730S",
                    "ExpectedModel=OTHER-BOARD");
    caen_mock::ResetLifecycleInstrumentation();
    CheckThrowsWith(
        [&]() {
          DAQManager manager(
              wrong_model_config.string(), wrong_model_output.string(), 1, 0,
              70, wrong_model_metadata.string(), mock_executable,
              "mock-commit", "mock-build");
        },
        "model mismatch before reset",
        "unexpected USB board model is rejected before reset");
    Check(caen_mock::open_calls == 1U && caen_mock::reset_calls == 0U &&
              caen_mock::close_calls == 1U,
          "model mismatch closes the handle without resetting hardware");

    const auto large_event_config = test_dir / "large_event.conf";
    const auto large_event_output = test_dir / "large_event.dat";
    WriteConfig(large_event_config, 250, 1024, 512, 102400, 15);
    {
      DAQManager manager(
          large_event_config.string(), large_event_output.string(), 0, 0, 68,
          (test_dir / "large_event.dat.run.json").string(), mock_executable,
          "mock-commit", "mock-build");
      constexpr uint64_t kLargeRawEventBytes =
          sizeof(EventHeader) + 4U * 102400U * sizeof(uint16_t);
      constexpr int kExpectedLargeEventHwm =
          static_cast<int>((64U * 1024U * 1024U) / kLargeRawEventBytes);
      Check(zmq_mock::last_send_hwm == kExpectedLargeEventHwm &&
                static_cast<uint64_t>(zmq_mock::last_send_hwm) *
                        kLargeRawEventBytes <=
                    64U * 1024U * 1024U,
            "large waveforms reduce the ZMQ message HWM to a 64 MiB budget");
    }

    caen_mock::ResetLifecycleInstrumentation();
    const auto alias_output = test_dir / "artifact_alias.dat";
    CheckThrowsWith(
        [&]() {
          DAQManager manager(
              config_path.string(), alias_output.string(), 1, 0, 61,
              alias_output.string() + ".partial", mock_executable,
              "mock-commit", "mock-build");
        },
        "Runtime artifact paths alias",
        "metadata cannot alias the in-progress raw artifact");
    Check(caen_mock::open_calls == 0U,
          "artifact alias validation runs before the CAEN device is opened");

    const auto real_artifact_dir = test_dir / "real_artifacts";
    const auto alias_artifact_dir = test_dir / "alias_artifacts";
    std::filesystem::create_directory(real_artifact_dir);
    std::filesystem::create_directory_symlink(real_artifact_dir,
                                              alias_artifact_dir);
    const auto symlink_alias_output = real_artifact_dir / "run.dat";
    CheckThrowsWith(
        [&]() {
          DAQManager manager(
              config_path.string(), symlink_alias_output.string(), 1, 0, 62,
              (alias_artifact_dir / "run.dat.partial").string(),
              mock_executable, "mock-commit", "mock-build");
        },
        "Runtime artifact paths alias",
        "artifact alias detection resolves symlinked parent directories");
    Check(caen_mock::open_calls == 0U,
          "symlink alias validation also precedes CAEN device access");

    // Exercise the cleanup mismatch path through a constructor failure.  A
    // concurrent operator process moves the reserved partial inode aside and
    // puts an unrelated sentinel at the public name before forcing the first
    // status publication to fail.  Cleanup must preserve both inodes and must
    // not strand its private quarantine directory.
    caen_mock::SetUnstableBaseline(false);
    const auto cleanup_race_config = test_dir / "cleanup_race.conf";
    WriteConfig(cleanup_race_config, 250);
    {
      std::ofstream padding(cleanup_race_config, std::ios::app);
      const std::string comment(1024U, 'x');
      for (int line = 0; line < 8192; ++line) {
        padding << "#" << comment << "\n";
      }
    }
    const auto cleanup_race_output = test_dir / "cleanup_race.dat";
    const auto cleanup_race_partial =
        std::filesystem::path(cleanup_race_output.string() + ".partial");
    const auto displaced_partial = test_dir / "cleanup_race.original";
    const auto cleanup_race_metadata =
        test_dir / "cleanup_race.dat.run.json";
    const auto hardware_status = std::filesystem::path(
        cleanup_race_metadata.string() +
        ".status.hardware_verified_not_started.json");
    std::string replacer_error;
    std::thread replacer([&]() {
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!std::filesystem::exists(cleanup_race_partial) &&
             std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      std::error_code rename_error;
      std::filesystem::rename(cleanup_race_partial, displaced_partial,
                              rename_error);
      if (rename_error) {
        replacer_error = "cannot displace partial: " + rename_error.message();
        return;
      }
      {
        std::ofstream sentinel(cleanup_race_partial);
        sentinel << "unrelated-sentinel";
      }
      {
        std::ofstream collision(hardware_status);
        collision << "force-status-publication-failure";
      }
    });
    CheckThrowsWith(
        [&]() {
          DAQManager manager(
              cleanup_race_config.string(), cleanup_race_output.string(), 1,
              0, 63, cleanup_race_metadata.string(), mock_executable,
              "mock-commit", "mock-build");
        },
        "Cannot publish runtime artifact",
        "constructor cleanup handles a concurrently replaced partial name");
    replacer.join();
    Check(replacer_error.empty(),
          "cleanup-race test replacement completed: " + replacer_error);
    Check(std::filesystem::exists(displaced_partial) &&
              std::filesystem::exists(cleanup_race_partial) &&
              ReadFile(cleanup_race_partial) == "unrelated-sentinel",
          "cleanup preserves both the acquired inode and an unrelated replacement");
    bool cleanup_quarantine_left = false;
    for (const auto& entry : std::filesystem::directory_iterator(test_dir)) {
      if (entry.path().filename().string().find(".cpnr-cleanup-") == 0U) {
        cleanup_quarantine_left = true;
      }
    }
    Check(!cleanup_quarantine_left,
          "successful mismatch restoration removes the private quarantine");

    const auto zmq_fault_output = test_dir / "zmq_fault.dat";
    const auto zmq_fault_metadata = test_dir / "zmq_fault.dat.run.json";
    zmq_mock::SetBindFailure(true);
    CheckThrowsWith(
        [&]() {
          DAQManager manager(
              config_path.string(), zmq_fault_output.string(), 0, 0, 59,
              zmq_fault_metadata.string(), mock_executable,
              "mock-commit", "mock-build");
        },
        "Cannot bind ZeroMQ publisher",
        "a monitoring endpoint collision blocks an ambiguous DAQ start");
    zmq_mock::SetBindFailure(false);
    Check(!std::filesystem::exists(zmq_fault_output) &&
              !std::filesystem::exists(zmq_fault_output.string() +
                                       ".partial"),
          "ZeroMQ setup failure does not publish or retain a raw file");
    Check(ReadFile(zmq_fault_metadata).find(
              "\"termination_reason\": \"setup_failure\"") !=
              std::string::npos,
          "ZeroMQ setup failure publishes terminal setup provenance");
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
