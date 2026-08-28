#include "DAQManager.h"
#include "Sha256.h"
#include "EventHeader.h"
#include "TriggerCalibration.h"
#include "CAENComm.h" 
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zmq.h>

namespace {

constexpr uint32_t kGlobalTriggerMaskRegister = 0x810C;
constexpr uint32_t kPairTriggerLogicBase = 0x1084;
constexpr uint32_t kPairRegisterStride = 0x200;
constexpr uint32_t kInputRangeBase = 0x1028;
constexpr uint32_t kChannelRegisterStride = 0x100;
constexpr uint32_t kInputRangeMask = 0x1;
constexpr uint32_t kPairTriggerRequestMask = 0x0F;
constexpr uint32_t kPairLogicFieldMask = 0x07;
constexpr uint32_t kMajorityLevelMask = 0x07U << 24;
constexpr uint32_t kExternalTriggerEnableMask = 1U << 30;
constexpr uint32_t kSoftwareTriggerEnableMask = 1U << 31;
constexpr uint32_t kExternalClockSelectMask = 1U << 6;
constexpr int kDppFirmwareMajorBase = 128;

class FatalAcquisitionError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct FileIdentity {
  uint64_t device = 0;
  uint64_t inode = 0;
};

std::string Hex32(uint32_t value) {
  std::ostringstream stream;
  stream << "0x" << std::uppercase << std::hex << std::setw(8)
         << std::setfill('0') << value;
  return stream.str();
}

std::string AbsolutePath(const std::string& path) {
  if (path.empty()) return {};
  return std::filesystem::absolute(std::filesystem::path(path))
      .lexically_normal().string();
}

std::string ReadRequiredTextFile(const std::string& path,
                                 const std::string& description) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("Cannot open " + description + ": " + path);
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  if (input.bad()) {
    throw std::runtime_error("Cannot read " + description + ": " + path);
  }
  const std::string result = contents.str();
  if (result.empty()) {
    throw std::runtime_error(description + " is empty: " + path);
  }
  return result;
}

std::string RequireAbsentFile(const std::string& path,
                              const std::string& description) {
  const std::string absolute = AbsolutePath(path);
  if (absolute.empty()) {
    throw std::runtime_error(description + " path cannot be empty");
  }
  std::error_code status_error;
  const auto status = std::filesystem::symlink_status(absolute, status_error);
  if (status_error &&
      status_error != std::errc::no_such_file_or_directory) {
    throw std::runtime_error("Cannot inspect " + description + " path " +
                             absolute + ": " + status_error.message());
  }
  if (!status_error &&
      status.type() != std::filesystem::file_type::not_found) {
    throw std::runtime_error(
        description + " already exists; refusing to overwrite: " + absolute);
  }
  return absolute;
}

std::string MetadataOutputPath(const std::string& output_file,
                               const std::string& metadata_file) {
  const std::string final_path = RequireAbsentFile(
      metadata_file.empty() ? AbsolutePath(output_file) + ".run.json"
                            : metadata_file,
      "Runtime metadata");
  RequireAbsentFile(
      final_path + ".status.hardware_verified_not_started.json",
      "Hardware-verified runtime status");
  RequireAbsentFile(final_path + ".status.running.json",
                    "Running runtime status");
  return final_path;
}

std::string ConfigSnapshotPath(const std::string& config_file,
                               const std::string& output_file) {
  const std::string snapshot = AbsolutePath(output_file) + ".config.conf";
  if (AbsolutePath(config_file) == snapshot) return snapshot;
  return RequireAbsentFile(snapshot, "Runtime config snapshot");
}

std::pair<int, FileIdentity> ReserveOutputFile(
    const std::string& output_file) {
  const int descriptor =
      ::open(output_file.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
  if (descriptor < 0) {
    throw std::runtime_error(
        "Cannot create raw output without overwriting an existing file: " +
        output_file + " (" + std::strerror(errno) + ")");
  }
  struct stat status {};
  if (::fstat(descriptor, &status) != 0) {
    const int status_error = errno;
    ::close(descriptor);
    ::unlink(output_file.c_str());
    throw std::runtime_error("Cannot identify newly reserved raw output " +
                             output_file + " (" +
                             std::strerror(status_error) + ")");
  }
  return {descriptor,
          {static_cast<uint64_t>(status.st_dev),
           static_cast<uint64_t>(status.st_ino)}};
}

bool PathMatchesIdentity(const std::string& path,
                         const FileIdentity& identity) {
  struct stat status {};
  return ::lstat(path.c_str(), &status) == 0 &&
         static_cast<uint64_t>(status.st_dev) == identity.device &&
         static_cast<uint64_t>(status.st_ino) == identity.inode;
}

bool RemoveIfSameFile(const std::string& path,
                      const FileIdentity& identity) {
  return PathMatchesIdentity(path, identity) && ::unlink(path.c_str()) == 0;
}

void LinkDescriptorNoReplace(int descriptor, const std::string& target,
                             const std::string& description) {
  // Bind the destination to the already-open inode.  Publishing by pathname
  // here would allow a concurrent pathname replacement after fsync.
  constexpr int kAtEmptyPath = 0x1000;
  if (::linkat(descriptor, "", AT_FDCWD, target.c_str(), kAtEmptyPath) == 0) {
    return;
  }
  const int descriptor_link_error = errno;

  const std::string descriptor_path =
      "/proc/self/fd/" + std::to_string(descriptor);
  if (::linkat(AT_FDCWD, descriptor_path.c_str(), AT_FDCWD, target.c_str(),
               AT_SYMLINK_FOLLOW) == 0) {
    return;
  }
  const int proc_link_error = errno;
  throw std::runtime_error(
      "Cannot publish " + description + " without overwrite: " + target +
      " (descriptor link: " + std::strerror(descriptor_link_error) +
      ", /proc fallback: " + std::strerror(proc_link_error) + ")");
}

void WriteAll(int descriptor, const std::string& contents,
              const std::string& description) {
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t written = ::write(descriptor, contents.data() + offset,
                                    contents.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      throw std::runtime_error("Cannot write " + description + " (" +
                               std::strerror(errno) + ")");
    }
    offset += static_cast<std::size_t>(written);
  }
}

void SyncParentDirectory(const std::string& path) {
  const std::filesystem::path parent =
      std::filesystem::path(path).parent_path().empty()
          ? std::filesystem::path(".")
          : std::filesystem::path(path).parent_path();
  const int descriptor =
      ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (descriptor < 0) {
    throw std::runtime_error(
        "Cannot open runtime artifact directory for sync: " +
        parent.string() + " (" + std::strerror(errno) + ")");
  }
  const int result = ::fsync(descriptor);
  const int sync_error = errno;
  ::close(descriptor);
  if (result != 0) {
    throw std::runtime_error(
        "Cannot sync runtime artifact directory: " + parent.string() +
        " (" + std::strerror(sync_error) + ")");
  }
}

FileIdentity WriteNewAtomicFile(const std::string& target,
                                const std::string& contents) {
  std::string template_path = target + ".tmp.XXXXXX";
  std::vector<char> template_buffer(template_path.begin(), template_path.end());
  template_buffer.push_back('\0');
  const int descriptor = ::mkstemp(template_buffer.data());
  if (descriptor < 0) {
    throw std::runtime_error("Cannot create temporary file beside " + target +
                             " (" + std::strerror(errno) + ")");
  }
  const std::string temporary_path(template_buffer.data());
  struct stat temporary_status {};
  if (::fstat(descriptor, &temporary_status) != 0) {
    const int status_error = errno;
    ::close(descriptor);
    ::unlink(temporary_path.c_str());
    throw std::runtime_error(
        "Cannot identify temporary runtime artifact " + temporary_path +
        " (" + std::strerror(status_error) + ")");
  }
  const FileIdentity temporary_identity{
      static_cast<uint64_t>(temporary_status.st_dev),
      static_cast<uint64_t>(temporary_status.st_ino)};
  bool descriptor_open = true;
  bool target_link_created = false;
  try {
    if (::fchmod(descriptor, 0644) != 0) {
      throw std::runtime_error("Cannot set permissions on " + temporary_path);
    }
    WriteAll(descriptor, contents, "temporary runtime artifact");
    if (::fsync(descriptor) != 0) {
      throw std::runtime_error("Cannot sync temporary runtime artifact " +
                               temporary_path + " (" +
                               std::strerror(errno) + ")");
    }
    if (!PathMatchesIdentity(temporary_path, temporary_identity)) {
      throw std::runtime_error(
          "Temporary runtime artifact inode changed before publication: " +
          temporary_path);
    }

    LinkDescriptorNoReplace(descriptor, target, "runtime artifact");
    target_link_created = true;
    if (!PathMatchesIdentity(target, temporary_identity)) {
      throw std::runtime_error(
          "Published runtime artifact inode does not match the validated "
          "descriptor: " + target);
    }

    try {
      SyncParentDirectory(target);
    } catch (...) {
      if (RemoveIfSameFile(target, temporary_identity)) {
        try {
          SyncParentDirectory(target);
        } catch (...) {
          // Preserve the original durability failure.
        }
      }
      throw;
    }
    if (!PathMatchesIdentity(target, temporary_identity)) {
      throw std::runtime_error(
          "Published runtime artifact identity changed during durable "
          "commit: " + target);
    }

    if (::close(descriptor) != 0) {
      std::cerr << "[Run Provenance] Published artifact is durable, but "
                   "closing its temporary descriptor failed: "
                << std::strerror(errno) << "\n";
    }
    descriptor_open = false;

    if (!RemoveIfSameFile(temporary_path, temporary_identity)) {
      std::cerr << "[Run Provenance] Published artifact is durable, but "
                   "temporary-name cleanup was unsafe or failed: "
                << temporary_path << "\n";
    } else {
      try {
        SyncParentDirectory(target);
      } catch (const std::exception& error) {
        std::cerr << "[Run Provenance] Published artifact is durable, but "
                     "temporary-name cleanup sync failed: "
                  << error.what() << "\n";
      }
    }
    return temporary_identity;
  } catch (...) {
    if (descriptor_open) ::close(descriptor);
    RemoveIfSameFile(temporary_path, temporary_identity);
    if (target_link_created) {
      RemoveIfSameFile(target, temporary_identity);
    }
    throw;
  }
}

int PositiveRunNumber(int run_number) {
  if (run_number <= 0) {
    throw std::runtime_error("Run number must be a positive integer");
  }
  return run_number;
}

std::string JsonEscape(const std::string& value) {
  std::ostringstream escaped;
  for (const unsigned char character : value) {
    switch (character) {
      case '\"': escaped << "\\\""; break;
      case '\\': escaped << "\\\\"; break;
      case '\b': escaped << "\\b"; break;
      case '\f': escaped << "\\f"; break;
      case '\n': escaped << "\\n"; break;
      case '\r': escaped << "\\r"; break;
      case '\t': escaped << "\\t"; break;
      default:
        if (character < 0x20) {
          escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                  << static_cast<unsigned int>(character) << std::dec;
        } else {
          escaped << character;
        }
    }
  }
  return escaped.str();
}

double Median(std::vector<uint16_t>& values) {
  if (values.empty()) {
    throw std::runtime_error("Cannot calculate a baseline from zero ADC samples");
  }
  const std::size_t middle = values.size() / 2;
  std::nth_element(values.begin(), values.begin() + middle, values.end());
  if ((values.size() & 1U) != 0U) return values[middle];
  const uint16_t upper = values[middle];
  const uint16_t lower = *std::max_element(values.begin(), values.begin() + middle);
  return (static_cast<double>(lower) + static_cast<double>(upper)) / 2.0;
}

int ParseFirmwareMajor(const char* release) {
  std::istringstream stream(release);
  int major = -1;
  stream >> major;
  if (!stream || major < 0) {
    throw std::runtime_error(
        "Cannot identify AMC firmware revision '" + std::string(release) +
        "'; explicit trigger-register routing was not applied");
  }
  return major;
}

uint32_t ReadRegister(int handle, uint32_t address) {
  uint32_t value = 0;
  CAEN_CHECK(CAEN_DGTZ_ReadRegister(handle, address, &value));
  return value;
}

void WriteMaskedRegisterAndVerify(int handle, uint32_t address, uint32_t mask,
                                  uint32_t requested_value,
                                  const std::string& description) {
  const uint32_t current_value = ReadRegister(handle, address);
  const uint32_t updated_value =
      (current_value & ~mask) | (requested_value & mask);
  CAEN_CHECK(CAEN_DGTZ_WriteRegister(handle, address, updated_value));

  const uint32_t readback = ReadRegister(handle, address);
  if ((readback & mask) != (requested_value & mask)) {
    throw std::runtime_error(
        description + " register verification failed at " + Hex32(address) +
        ": requested " + Hex32(requested_value & mask) + ", read " +
        Hex32(readback & mask));
  }
}

uint32_t PairRequestMask(uint32_t self_trigger_mask) {
  uint32_t request_mask = 0;
  for (int pair = 0; pair < MAX_CH / 2; ++pair) {
    if ((self_trigger_mask & (0x3U << (pair * 2))) != 0U) {
      request_mask |= 1U << pair;
    }
  }
  return request_mask;
}

std::string DescribeTriggerRouting(uint32_t self_trigger_mask,
                                   DAQPairLogic pair_logic) {
  std::ostringstream description;
  bool first = true;
  for (int pair = 0; pair < MAX_CH / 2; ++pair) {
    const int even_ch = pair * 2;
    const uint32_t pair_bits = (self_trigger_mask >> even_ch) & 0x3U;
    if (pair_bits == 0U) continue;

    if (!first) description << " OR ";
    first = false;
    if (pair_bits == 0x1U) {
      description << "CH" << even_ch;
    } else if (pair_bits == 0x2U) {
      description << "CH" << even_ch + 1;
    } else {
      description << "(CH" << even_ch
                  << (pair_logic == DAQPairLogic::kAnd ? " AND CH" : " OR CH")
                  << even_ch + 1 << ")";
    }
  }
  return description.str();
}

}  // namespace

DAQManager::DAQManager(const std::string &config_file,
                       const std::string &output_file, int max_events,
                       int run_time_sec, int run_number,
                       const std::string &metadata_file,
                       const std::string &executable_path,
                       const std::string &git_commit,
                       const std::string &build_timestamp,
                       const std::atomic<bool> *cancellation_flag)
    : config_file_(AbsolutePath(config_file)),
      config_contents_(ReadRequiredTextFile(config_file_, "DAQ config")),
      config_(ConfigParser::FromText(config_contents_, config_file_)),
      hardware_settings_(LoadDAQHardwareSettings(config_)),
      output_file_(RequireAbsentFile(output_file, "Raw output")),
      metadata_file_(MetadataOutputPath(output_file, metadata_file)),
      config_snapshot_file_(ConfigSnapshotPath(config_file, output_file)),
      executable_path_(executable_path.empty() ? "unknown" : executable_path),
      git_commit_(git_commit), build_timestamp_(build_timestamp),
      run_number_(PositiveRunNumber(run_number)), max_events_(max_events),
      run_time_sec_(run_time_sec), running_(false),
      cancellation_flag_(cancellation_flag),
      digitizer_(CAEN_DGTZ_USB, 0, 0, 0) {
  // Hash the inode the kernel is actually executing. Reopening argv[0] can
  // hash a replacement binary after an in-place rebuild.
  executable_sha256_ = Sha256FileHex("/proc/self/exe");
  zmq_ctx_ = zmq_ctx_new();
  if (!zmq_ctx_) throw std::runtime_error("Cannot create ZeroMQ context");
  zmq_pub_ = zmq_socket(zmq_ctx_, ZMQ_PUB);
  if (!zmq_pub_) {
    zmq_ctx_destroy(zmq_ctx_);
    zmq_ctx_ = nullptr;
    throw std::runtime_error("Cannot create ZeroMQ publisher socket");
  }
  int hwm = 5000;
  zmq_setsockopt(zmq_pub_, ZMQ_SNDHWM, &hwm, sizeof(hwm));
  
  int linger = 0;
  zmq_setsockopt(zmq_pub_, ZMQ_LINGER, &linger, sizeof(linger));
  if (zmq_bind(zmq_pub_, "tcp://127.0.0.1:5555") != 0) {
    zmq_close(zmq_pub_);
    zmq_ctx_destroy(zmq_ctx_);
    zmq_pub_ = nullptr;
    zmq_ctx_ = nullptr;
    throw std::runtime_error("Cannot bind ZeroMQ publisher to tcp://127.0.0.1:5555");
  }

  bool output_created = false;
  int output_descriptor = -1;
  try {
    SetupHardware();
    const auto reservation = ReserveOutputFile(output_file_);
    output_descriptor = reservation.first;
    output_device_ = reservation.second.device;
    output_inode_ = reservation.second.inode;
    output_created = true;
    output_stream_ = ::fdopen(output_descriptor, "w+b");
    if (!output_stream_) {
      const int stream_error = errno;
      ::close(output_descriptor);
      output_descriptor = -1;
      throw std::runtime_error("Cannot attach a stream to reserved output " +
                               output_file_ + " (" +
                               std::strerror(stream_error) + ")");
    }
    output_descriptor = -1;
    output_write_buffer_.resize(4 * 1024 * 1024);
    if (::setvbuf(output_stream_, output_write_buffer_.data(), _IOFBF,
                  output_write_buffer_.size()) != 0) {
      throw std::runtime_error("Cannot configure raw-output write buffer");
    }
    WriteRuntimeArtifacts();
  } catch (...) {
    if (output_stream_) {
      ::fclose(output_stream_);
      output_stream_ = nullptr;
    } else if (output_descriptor >= 0) {
      ::close(output_descriptor);
    }
    if (output_created) {
      RemoveIfSameFile(output_file_, {output_device_, output_inode_});
    }
    zmq_close(zmq_pub_);
    zmq_ctx_destroy(zmq_ctx_);
    zmq_pub_ = nullptr;
    zmq_ctx_ = nullptr;
    throw;
  }
}

DAQManager::~DAQManager() {
  Stop();
  if (output_stream_) {
    ::fclose(output_stream_);
    output_stream_ = nullptr;
  }
  if (zmq_pub_) zmq_close(zmq_pub_);
  if (zmq_ctx_) zmq_ctx_destroy(zmq_ctx_);
}

void DAQManager::SetupHardware() {
  CheckSetupCancellation();
  std::cout << "\033[1;36m[DAQManager]\033[0m Configuring Hardware from Config...\n";
  const int handle = digitizer_.GetHandle();

  CAEN_DGTZ_BoardInfo_t board_info{};
  CAEN_CHECK(CAEN_DGTZ_GetInfo(handle, &board_info));
  board_model_ = board_info.ModelName;
  board_roc_firmware_ = board_info.ROC_FirmwareRel;
  board_amc_firmware_ = board_info.AMC_FirmwareRel;
  board_serial_number_ = board_info.SerialNumber;
  board_adc_bits_ = board_info.ADC_NBits;
  if (board_info.FamilyCode != CAEN_DGTZ_XX730_FAMILY_CODE) {
    throw std::runtime_error(
        "Input-range and trigger calibration require an x730-family digitizer; "
        "connected model is " + std::string(board_info.ModelName));
  }
  if (ParseFirmwareMajor(board_info.AMC_FirmwareRel) >=
      kDppFirmwareMajorBase) {
    throw std::runtime_error(
        "This acquisition path requires standard waveform firmware; connected "
        "AMC firmware is " + std::string(board_info.AMC_FirmwareRel) +
        " (DPP family)");
  }
  if (board_adc_bits_ != hardware_settings_.adc_bits) {
    throw std::runtime_error(
        "Board ADC resolution does not match config: board reports " +
        std::to_string(board_adc_bits_) + " bits, config requests " +
        std::to_string(hardware_settings_.adc_bits));
  }

  // The board was reset by CaenDigitizer.  Apply and verify the complete
  // analog setup before enabling any trigger source.
  uint32_t acq_ctrl = 0;
  CAEN_CHECK(CAEN_DGTZ_ReadRegister(handle, 0x8100, &acq_ctrl));
  const uint32_t requested_acq_ctrl =
      (acq_ctrl & ~(static_cast<uint32_t>(1U << 3) |
                    kExternalClockSelectMask)) |
      (hardware_settings_.clock_source != 0 ? kExternalClockSelectMask : 0U);
  CAEN_CHECK(CAEN_DGTZ_WriteRegister(handle, 0x8100, requested_acq_ctrl));
  const uint32_t acq_ctrl_readback = ReadRegister(handle, 0x8100);
  clock_source_readback_ =
      (acq_ctrl_readback & kExternalClockSelectMask) != 0U ? 1U : 0U;
  if (clock_source_readback_ !=
      static_cast<uint32_t>(hardware_settings_.clock_source)) {
    throw std::runtime_error(
        "Clock-source register verification failed at 0x00008100");
  }

  const uint32_t record_length = hardware_settings_.record_length;
  const uint32_t channel_mask = hardware_settings_.channel_mask;

  CAEN_CHECK(CAEN_DGTZ_SetRecordLength(handle, record_length));
  uint32_t record_length_readback = 0;
  CAEN_CHECK(CAEN_DGTZ_GetRecordLength(handle, &record_length_readback));
  if (record_length_readback != record_length) {
    throw std::runtime_error(
        "Record-length readback mismatch: requested " +
        std::to_string(record_length) + ", read back " +
        std::to_string(record_length_readback));
  }
  CAEN_CHECK(CAEN_DGTZ_SetChannelEnableMask(handle, channel_mask));
  CAEN_CHECK(CAEN_DGTZ_GetChannelEnableMask(handle,
                                            &channel_mask_readback_));
  if (channel_mask_readback_ != channel_mask) {
    throw std::runtime_error(
        "Channel-enable mask readback mismatch: requested " +
        Hex32(channel_mask) + ", read back " +
        Hex32(channel_mask_readback_));
  }
  std::cout << "\033[1;36m[Digitizer Readback]\033[0m record_length="
            << record_length_readback << ", channel_mask="
            << Hex32(channel_mask_readback_) << "\n";
  CAEN_CHECK(CAEN_DGTZ_SetPostTriggerSize(handle,
                                          hardware_settings_.post_trigger));
  CAEN_CHECK(CAEN_DGTZ_SetAcquisitionMode(handle, CAEN_DGTZ_SW_CONTROLLED));
  const auto requested_run_sync = static_cast<CAEN_DGTZ_RunSyncMode_t>(
      hardware_settings_.run_sync_mode);
  CAEN_CHECK(CAEN_DGTZ_SetRunSynchronizationMode(handle,
                                                 requested_run_sync));
  CAEN_DGTZ_RunSyncMode_t run_sync_readback{};
  CAEN_CHECK(CAEN_DGTZ_GetRunSynchronizationMode(handle,
                                                 &run_sync_readback));
  run_sync_mode_readback_ = static_cast<int>(run_sync_readback);
  if (run_sync_mode_readback_ != hardware_settings_.run_sync_mode) {
    throw std::runtime_error("Run-synchronization mode readback mismatch");
  }
  clock_source_readback_ =
      (ReadRegister(handle, 0x8100) & kExternalClockSelectMask) != 0U ? 1U
                                                                      : 0U;
  if (clock_source_readback_ !=
      static_cast<uint32_t>(hardware_settings_.clock_source)) {
    throw std::runtime_error(
        "Clock source changed while applying run synchronization");
  }
  std::cout << "\033[1;36m[Synchronization Readback]\033[0m clock_source="
            << clock_source_readback_ << " (0=internal, 1=external), mode="
            << run_sync_mode_readback_ << "\n";
  CAEN_CHECK(CAEN_DGTZ_SetExtTriggerInputMode(
      handle, CAEN_DGTZ_TRGMODE_DISABLED));
  CAEN_CHECK(CAEN_DGTZ_SetChannelSelfTrigger(
      handle, CAEN_DGTZ_TRGMODE_DISABLED, 0xFF));
  CAEN_CHECK(CAEN_DGTZ_SetSWTriggerMode(handle, CAEN_DGTZ_TRGMODE_ACQ_ONLY));

  ConfigureInputRangeAndOffsets(handle);
  CheckSetupCancellation();

  digitizer_.AllocateBuffers();
  const size_t max_safe_size =
      sizeof(EventHeader) +
      (record_length + 1024) * sizeof(uint16_t) * MAX_CH;
  raw_buffer_pool_.resize(max_safe_size);

  uint32_t calibration_mask = 0;
  for (int ch = 0; ch < MAX_CH; ++ch) {
    if (((hardware_settings_.self_trigger_mask >> ch) & 1U) != 0U &&
        hardware_settings_.channels[ch].threshold_is_relative_mv) {
      calibration_mask |= 1U << ch;
    }
  }

  std::array<double, MAX_CH> baselines{};
  if (calibration_mask != 0U) {
    baselines = WaitForStableBaselines(handle);
    ProgramAndVerifyThresholds(handle, baselines);

    // Measure once more immediately before installing the trigger route.  A
    // moved baseline causes one complete re-settling/recalibration attempt;
    // acquisition never starts with a stale threshold.
    auto final_baselines = MeasureBaselineBatch(
        handle, std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(hardware_settings_
                                                  .trigger_calibration
                                                  .settling_timeout_ms));
    if (!BaselinesAreStable(baselines, final_baselines, calibration_mask,
                            hardware_settings_.trigger_calibration
                                .stability_tolerance_adc)) {
      std::cout << "\033[1;33m[Trigger Calibration]\033[0m Baseline moved "
                   "before acquisition; repeating settling and calibration.\n";
      baselines = WaitForStableBaselines(handle);
      ProgramAndVerifyThresholds(handle, baselines);
      final_baselines = MeasureBaselineBatch(
          handle, std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(hardware_settings_
                                                    .trigger_calibration
                                                    .settling_timeout_ms));
      if (!BaselinesAreStable(
              baselines, final_baselines, calibration_mask,
              hardware_settings_.trigger_calibration.stability_tolerance_adc)) {
        throw std::runtime_error(
            "Baseline moved again immediately before acquisition; refusing "
            "to start with an invalid hardware threshold");
      }
    }
    ProgramAndVerifyThresholds(handle, final_baselines);
  } else {
    ProgramAndVerifyThresholds(handle, baselines);
  }

  CheckSetupCancellation();
  ConfigureAndVerifyTriggerRouting(handle);
  acquisition_status_ = "hardware_verified_not_started";
}

void DAQManager::CheckSetupCancellation() const {
  if (cancellation_flag_ != nullptr &&
      !cancellation_flag_->load(std::memory_order_relaxed)) {
    throw std::runtime_error(
        "Hardware setup/baseline calibration cancelled by user");
  }
}

void DAQManager::ConfigureInputRangeAndOffsets(int handle) {
  const uint32_t requested_range_bit =
      hardware_settings_.input_range_mv == 500U ? 1U : 0U;
  const auto requested_polarity =
      hardware_settings_.trigger_polarity == 0
          ? CAEN_DGTZ_TriggerOnRisingEdge
          : CAEN_DGTZ_TriggerOnFallingEdge;

  for (int ch = 0; ch < MAX_CH; ++ch) {
    if (((hardware_settings_.channel_mask >> ch) & 1U) == 0U) continue;

    auto& runtime = runtime_channels_[ch];
    runtime.enabled = true;
    runtime.participates_in_trigger =
        ((hardware_settings_.self_trigger_mask >> ch) & 1U) != 0U;
    runtime.requested_dc_offset = hardware_settings_.channels[ch].dc_offset;
    runtime.input_range_register =
        kInputRangeBase + kChannelRegisterStride * ch;

    WriteMaskedRegisterAndVerify(
        handle, runtime.input_range_register, kInputRangeMask,
        requested_range_bit, "Channel input range");
    runtime.input_range_readback =
        ReadRegister(handle, runtime.input_range_register) & kInputRangeMask;

    CAEN_CHECK(CAEN_DGTZ_SetChannelDCOffset(
        handle, ch, runtime.requested_dc_offset));
    CAEN_CHECK(CAEN_DGTZ_GetChannelDCOffset(
        handle, ch, &runtime.readback_dc_offset));
    if (runtime.readback_dc_offset != runtime.requested_dc_offset) {
      throw std::runtime_error(
          "CH" + std::to_string(ch) + " DC-offset readback mismatch: wrote " +
          std::to_string(runtime.requested_dc_offset) + ", read " +
          std::to_string(runtime.readback_dc_offset));
    }

    CAEN_CHECK(CAEN_DGTZ_SetTriggerPolarity(handle, ch,
                                             requested_polarity));
    CAEN_DGTZ_TriggerPolarity_t polarity_readback{};
    CAEN_CHECK(CAEN_DGTZ_GetTriggerPolarity(handle, ch,
                                             &polarity_readback));
    runtime.polarity_readback =
        polarity_readback == CAEN_DGTZ_TriggerOnFallingEdge ? 1 : 0;
    if (runtime.polarity_readback != hardware_settings_.trigger_polarity) {
      throw std::runtime_error(
          "CH" + std::to_string(ch) + " trigger-polarity readback mismatch");
    }

    std::cout << "\033[1;36m[Analog Setup]\033[0m CH" << ch
              << " input_range=" << hardware_settings_.input_range_mv
              << " mVpp (register " << Hex32(runtime.input_range_register)
              << " readback=" << runtime.input_range_readback << ")"
              << ", DCOffset=" << runtime.requested_dc_offset
              << " readback=" << runtime.readback_dc_offset
              << ", polarity="
              << (runtime.polarity_readback == 1 ? "falling" : "rising")
              << "\n";
  }
}

std::array<double, MAX_CH> DAQManager::MeasureBaselineBatch(
    int handle, std::chrono::steady_clock::time_point deadline) {
  std::array<std::vector<uint16_t>, MAX_CH> samples;
  const uint32_t requested_events =
      hardware_settings_.trigger_calibration.measurement_events;
  uint32_t decoded_events = 0;
  bool acquisition_started = false;

  try {
    CheckSetupCancellation();
    if (std::chrono::steady_clock::now() >= deadline) {
      throw std::runtime_error(
          "Baseline settling failed: measurement deadline expired before "
          "software acquisition started");
    }
    CAEN_CHECK(CAEN_DGTZ_ClearData(handle));
    CAEN_CHECK(CAEN_DGTZ_SWStartAcquisition(handle));
    acquisition_started = true;
    for (uint32_t event = 0; event < requested_events; ++event) {
      CheckSetupCancellation();
      if (std::chrono::steady_clock::now() >= deadline) break;
      CAEN_CHECK(CAEN_DGTZ_SendSWtrigger(handle));
      if (event + 1U < requested_events) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        std::this_thread::sleep_until(
            std::min(deadline, now + std::chrono::milliseconds(1)));
      }
    }

    char* readout_buffer = digitizer_.GetReadoutBuffer();
    auto* decoded_event = digitizer_.GetDecodedEvent();
    while (decoded_events < requested_events &&
           std::chrono::steady_clock::now() < deadline) {
      CheckSetupCancellation();
      uint32_t bytes_read = 0;
      CAEN_CHECK(CAEN_DGTZ_ReadData(
          handle, CAEN_DGTZ_SLAVE_TERMINATED_READOUT_MBLT,
          readout_buffer, &bytes_read));
      if (std::chrono::steady_clock::now() >= deadline) {
        break;
      }
      if (bytes_read == 0U) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }

      uint32_t event_count = 0;
      CAEN_CHECK(CAEN_DGTZ_GetNumEvents(
          handle, readout_buffer, bytes_read, &event_count));
      for (uint32_t event_index = 0;
           event_index < event_count && decoded_events < requested_events;
           ++event_index) {
        CAEN_DGTZ_EventInfo_t event_info{};
        char* event_pointer = nullptr;
        CAEN_CHECK(CAEN_DGTZ_GetEventInfo(
            handle, readout_buffer, bytes_read, event_index, &event_info,
            &event_pointer));
        CAEN_CHECK(CAEN_DGTZ_DecodeEvent(
            handle, event_pointer, reinterpret_cast<void**>(&decoded_event)));

        for (int ch = 0; ch < MAX_CH; ++ch) {
          if (((hardware_settings_.channel_mask >> ch) & 1U) == 0U ||
              ((event_info.ChannelMask >> ch) & 1U) == 0U) {
            continue;
          }
          const uint32_t trace_size = decoded_event->ChSize[ch];
          const uint16_t* trace = decoded_event->DataChannel[ch];
          if (trace == nullptr || trace_size == 0U) continue;
          samples[ch].insert(samples[ch].end(), trace, trace + trace_size);
        }
        ++decoded_events;
      }
    }
    CAEN_CHECK(CAEN_DGTZ_SWStopAcquisition(handle));
    acquisition_started = false;
    CAEN_CHECK(CAEN_DGTZ_ClearData(handle));
  } catch (...) {
    if (acquisition_started) CAEN_DGTZ_SWStopAcquisition(handle);
    CAEN_DGTZ_ClearData(handle);
    throw;
  }

  if (decoded_events < requested_events) {
    throw std::runtime_error(
        "Baseline settling failed: software-trigger measurement timed out "
        "after " + std::to_string(decoded_events) + "/" +
        std::to_string(requested_events) + " events");
  }

  std::array<double, MAX_CH> baselines{};
  for (int ch = 0; ch < MAX_CH; ++ch) {
    if (((hardware_settings_.channel_mask >> ch) & 1U) == 0U) continue;
    baselines[ch] = Median(samples[ch]);
  }
  return baselines;
}

std::array<double, MAX_CH> DAQManager::WaitForStableBaselines(int handle) {
  const auto& calibration = hardware_settings_.trigger_calibration;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(
                            calibration.settling_timeout_ms);
  const auto settling_end = std::min(
      deadline, std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(calibration.settling_time_ms));
  while (std::chrono::steady_clock::now() < settling_end) {
    CheckSetupCancellation();
    std::this_thread::sleep_until(std::min(
        settling_end, std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(50)));
  }

  uint32_t calibration_mask = 0;
  for (int ch = 0; ch < MAX_CH; ++ch) {
    if (((hardware_settings_.self_trigger_mask >> ch) & 1U) != 0U &&
        hardware_settings_.channels[ch].threshold_is_relative_mv) {
      calibration_mask |= 1U << ch;
    }
  }

  std::vector<BaselineMeasurement> measurements;
  while (std::chrono::steady_clock::now() < deadline) {
    CheckSetupCancellation();
    measurements.push_back(MeasureBaselineBatch(handle, deadline));
    std::cout << "\033[1;36m[Baseline]\033[0m measurement "
              << measurements.size();
    for (int ch = 0; ch < MAX_CH; ++ch) {
      if (((calibration_mask >> ch) & 1U) != 0U) {
        std::cout << " CH" << ch << "=" << std::fixed
                  << std::setprecision(3) << measurements.back()[ch]
                  << " ADC";
      }
    }
    std::cout << "\n";

    if (measurements.size() >= calibration.stable_measurements) {
      try {
        return RequireSettledBaselines(
            measurements, calibration_mask,
            calibration.stability_tolerance_adc,
            calibration.stable_measurements);
      } catch (const std::runtime_error&) {
        // More measurements may still settle before the configured deadline.
      }
    }
  }
  throw std::runtime_error(
      "Baseline settling failed before SettlingTimeoutMs=" +
      std::to_string(calibration.settling_timeout_ms));
}

void DAQManager::ProgramAndVerifyThresholds(
    int handle, const std::array<double, MAX_CH>& baselines) {
  const double lsb_mv =
      static_cast<double>(hardware_settings_.input_range_mv) /
      static_cast<double>(1U << hardware_settings_.adc_bits);
  bool relative_baselines_available = false;
  for (int ch = 0; ch < MAX_CH; ++ch) {
    if (((hardware_settings_.self_trigger_mask >> ch) & 1U) != 0U &&
        hardware_settings_.channels[ch].threshold_is_relative_mv) {
      relative_baselines_available = true;
      break;
    }
  }

  for (int ch = 0; ch < MAX_CH; ++ch) {
    if (((hardware_settings_.channel_mask >> ch) & 1U) == 0U) continue;

    const auto& configured = hardware_settings_.channels[ch];
    auto& runtime = runtime_channels_[ch];
    if (relative_baselines_available) {
      runtime.baseline_measured = true;
      runtime.measured_baseline_adc = baselines[ch];
    }
    if (!runtime.participates_in_trigger) {
      std::cout << "\033[1;36m[Trigger Threshold]\033[0m CH" << ch
                << " record-only; discriminator register is not used or "
                   "programmed";
      if (runtime.baseline_measured) {
        std::cout << ", measured baseline=" << std::fixed
                  << std::setprecision(3) << runtime.measured_baseline_adc
                  << " ADC";
      }
      std::cout << "\n";
      continue;
    }
    runtime.threshold_programmed = true;
    if (configured.threshold_is_relative_mv) {
      const auto result = CalculateAbsoluteTriggerThreshold(
          baselines[ch], configured.trigger_threshold_mv,
          hardware_settings_.input_range_mv, hardware_settings_.adc_bits,
          hardware_settings_.trigger_polarity);
      runtime.measured_threshold = true;
      runtime.measured_baseline_adc = result.measured_baseline_adc;
      runtime.requested_threshold_mv = result.requested_threshold_mv;
      runtime.delta_adc = result.delta_adc;
      runtime.written_threshold_adc = result.absolute_threshold_adc;
      runtime.effective_threshold_mv = result.effective_threshold_mv;
      if (std::abs(runtime.effective_threshold_mv -
                   runtime.requested_threshold_mv) > lsb_mv + 1e-9) {
        throw std::runtime_error(
            "CH" + std::to_string(ch) +
            " effective threshold differs from the request by more than one "
            "ADC count after baseline/threshold quantization");
      }
    } else {
      runtime.measured_threshold = false;
      runtime.written_threshold_adc = configured.trigger_threshold;
    }

    CAEN_CHECK(CAEN_DGTZ_SetChannelTriggerThreshold(
        handle, ch, runtime.written_threshold_adc));
    CAEN_CHECK(CAEN_DGTZ_GetChannelTriggerThreshold(
        handle, ch, &runtime.readback_threshold_adc));
    if (runtime.readback_threshold_adc != runtime.written_threshold_adc) {
      throw std::runtime_error(
          "CH" + std::to_string(ch) + " threshold readback mismatch: wrote " +
          std::to_string(runtime.written_threshold_adc) + ", read " +
          std::to_string(runtime.readback_threshold_adc));
    }

    if (runtime.measured_threshold) {
      std::cout << "\033[1;32m[Trigger Calibration]\033[0m CH" << ch
                << " baseline=" << std::fixed << std::setprecision(3)
                << runtime.measured_baseline_adc << " ADC, requested="
                << runtime.requested_threshold_mv << " mV, delta="
                << runtime.delta_adc << " ADC, written="
                << runtime.written_threshold_adc << ", readback="
                << runtime.readback_threshold_adc << ", effective="
                << std::setprecision(6) << runtime.effective_threshold_mv
                << " mV\n";
    } else {
      std::cout << "\033[1;33m[Trigger Threshold]\033[0m CH" << ch
                << " legacy absolute ADC code written/readback="
                << runtime.readback_threshold_adc << "\n";
    }
  }
}

void DAQManager::ConfigureAndVerifyTriggerRouting(int handle) {
  const auto trigger_mode = CAEN_DGTZ_TRGMODE_ACQ_ONLY;
  const int external_trigger = hardware_settings_.ext_trigger_mode;
  const int self_trigger = hardware_settings_.self_trigger_mode;
  const uint32_t self_trigger_mask = hardware_settings_.self_trigger_mask;

  CAEN_CHECK(CAEN_DGTZ_SetExtTriggerInputMode(
      handle, external_trigger > 0 ? trigger_mode
                                   : CAEN_DGTZ_TRGMODE_DISABLED));
  CAEN_CHECK(CAEN_DGTZ_SetChannelSelfTrigger(
      handle, self_trigger > 0 ? trigger_mode
                               : CAEN_DGTZ_TRGMODE_DISABLED,
      self_trigger > 0 ? self_trigger_mask : 0xFF));

  // Explicit split-mask configurations do not allow software triggers to
  // bypass the selected physical route.  Legacy configs preserve old behavior.
  const bool software_trigger_enabled =
      !hardware_settings_.explicit_trigger_routing;
  CAEN_CHECK(CAEN_DGTZ_SetSWTriggerMode(
      handle, software_trigger_enabled ? trigger_mode
                                       : CAEN_DGTZ_TRGMODE_DISABLED));

  if (self_trigger > 0 && hardware_settings_.explicit_trigger_routing) {
    for (int pair = 0; pair < MAX_CH / 2; ++pair) {
      const uint32_t pair_bits = (self_trigger_mask >> (pair * 2)) & 0x3U;
      if (pair_bits == 0U) continue;

      const uint32_t pair_logic_register =
          kPairTriggerLogicBase + kPairRegisterStride * pair;
      // Bit 2 = 1 selects the actual over/under-threshold comparator signal.
      // Bits [1:0] select AND, even-only, odd-only, or OR for the pair.
      WriteMaskedRegisterAndVerify(
          handle, pair_logic_register, kPairLogicFieldMask,
          X730PairTriggerLogicField(pair_bits, hardware_settings_.pair_logic),
          "Adjacent-channel trigger logic");
      pair_logic_readback_[pair] =
          ReadRegister(handle, pair_logic_register) & kPairLogicFieldMask;
      std::cout << "\033[1;36m[FPGA Trigger Readback]\033[0m pair CH"
                << pair * 2 << "/CH" << pair * 2 + 1
                << " register=" << Hex32(pair_logic_register)
                << ", requested_field="
                << Hex32(X730PairTriggerLogicField(
                       pair_bits, hardware_settings_.pair_logic))
                << ", readback_field="
                << Hex32(pair_logic_readback_[pair]) << "\n";
    }

    // A level of zero combines enabled pair requests with OR. This is distinct
    // from the AND/OR operation performed inside each adjacent channel pair.
    WriteMaskedRegisterAndVerify(handle, kGlobalTriggerMaskRegister,
                                 kMajorityLevelMask, 0,
                                 "Global pair-request logic");
  }

  global_trigger_mask_readback_ =
      ReadRegister(handle, kGlobalTriggerMaskRegister);
  const uint32_t expected_pair_requests =
      self_trigger > 0 ? PairRequestMask(self_trigger_mask) : 0U;
  const uint32_t source_field_mask =
      kPairTriggerRequestMask | kExternalTriggerEnableMask |
      kSoftwareTriggerEnableMask;
  const uint32_t expected_source_fields =
      expected_pair_requests |
      (external_trigger > 0 ? kExternalTriggerEnableMask : 0U) |
      (software_trigger_enabled ? kSoftwareTriggerEnableMask : 0U);
  if ((global_trigger_mask_readback_ & source_field_mask) !=
      expected_source_fields) {
    throw std::runtime_error(
        "Global trigger source verification failed at " +
        Hex32(kGlobalTriggerMaskRegister) + ": requested " +
        Hex32(expected_source_fields) + ", read " +
        Hex32(global_trigger_mask_readback_ & source_field_mask));
  }

  std::cout << "\033[1;36m[FPGA Trigger]\033[0m Record mask="
            << Hex32(hardware_settings_.channel_mask)
            << ", self-trigger mask="
            << Hex32(self_trigger_mask);
  if (self_trigger > 0) {
    std::cout << ", route="
              << DescribeTriggerRouting(self_trigger_mask,
                                        hardware_settings_.pair_logic);
    if (hardware_settings_.explicit_trigger_routing) {
      std::cout << ", comparator source=over-threshold";
    } else {
      std::cout << " (legacy pair-register behavior)";
    }
  } else {
    std::cout << ", self-trigger disabled";
  }
  std::cout << ", global readback=" << Hex32(global_trigger_mask_readback_)
            << ".\n";
}

void DAQManager::WriteRuntimeArtifacts() {
  if (!config_snapshot_written_ && config_file_ != config_snapshot_file_) {
    WriteNewAtomicFile(config_snapshot_file_, config_contents_);
    config_snapshot_written_ = true;
  } else if (config_file_ == config_snapshot_file_) {
    if (ReadRequiredTextFile(config_file_, "runtime config snapshot") !=
        config_contents_) {
      throw std::runtime_error(
          "Runtime config snapshot changed after it was parsed; refusing to "
          "record provenance or continue acquisition");
    }
    config_snapshot_written_ = true;
  }

  // Status snapshots are immutable.  The canonical metadata path is created
  // exactly once for a terminal state, so no path-based replacement race can
  // substitute unvalidated bytes for an already published run record.
  const bool terminal_status =
      acquisition_status_ == "completed" || acquisition_status_ == "failed";
  const std::string published_metadata_path =
      terminal_status
          ? metadata_file_
          : metadata_file_ + ".status." + acquisition_status_ + ".json";

  std::ostringstream metadata;
  metadata << std::setprecision(12);

  const auto timestamp = std::time(nullptr);
  metadata << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"run_number\": " << run_number_ << ",\n"
           << "  \"acquisition_status\": \""
           << JsonEscape(acquisition_status_) << "\",\n"
           << "  \"failure_reason\": ";
  if (failure_reason_.empty()) metadata << "null";
  else metadata << "\"" << JsonEscape(failure_reason_) << "\"";
  metadata << ",\n"
           << "  \"created_unix_time\": " << timestamp << ",\n"
           << "  \"raw_output_path\": \"" << JsonEscape(output_file_)
           << "\",\n"
           << "  \"raw_output_size_bytes\": ";
  if (raw_output_finalized_) metadata << raw_output_size_bytes_;
  else metadata << "null";
  metadata << ",\n  \"raw_output_sha256\": ";
  if (raw_output_finalized_) {
    metadata << "\"" << raw_output_sha256_ << "\"";
  } else {
    metadata << "null";
  }
  metadata << ",\n"
           << "  \"metadata_path\": \""
           << JsonEscape(published_metadata_path)
           << "\",\n"
           << "  \"config_path\": \"" << JsonEscape(config_snapshot_file_)
           << "\",\n"
           << "  \"config_sha256\": \"" << Sha256Hex(config_contents_)
           << "\",\n"
           << "  \"source_config_path\": \"" << JsonEscape(config_file_)
           << "\",\n"
           << "  \"binary_path\": \"" << JsonEscape(executable_path_)
           << "\",\n"
           << "  \"binary_sha256\": \"" << executable_sha256_ << "\",\n"
           << "  \"git_commit\": \"" << JsonEscape(git_commit_) << "\",\n"
           << "  \"build_timestamp\": \"" << JsonEscape(build_timestamp_)
           << "\",\n"
           << "  \"hardware\": {\n"
           << "    \"model\": \"" << JsonEscape(board_model_) << "\",\n"
           << "    \"serial_number\": " << board_serial_number_ << ",\n"
           << "    \"roc_firmware\": \""
           << JsonEscape(board_roc_firmware_) << "\",\n"
           << "    \"amc_firmware\": \""
           << JsonEscape(board_amc_firmware_) << "\",\n"
           << "    \"input_range_mvpp\": "
           << hardware_settings_.input_range_mv << ",\n"
           << "    \"adc_bits\": " << board_adc_bits_ << ",\n"
           << "    \"dc_offset_dac_bits\": 16,\n"
           << "    \"clock_source\": " << hardware_settings_.clock_source
           << ",\n"
           << "    \"clock_source_readback\": "
           << clock_source_readback_ << ",\n"
           << "    \"run_sync_mode\": " << hardware_settings_.run_sync_mode
           << ",\n"
           << "    \"run_sync_mode_readback\": "
           << run_sync_mode_readback_ << ",\n"
           << "    \"trigger_polarity\": \""
           << (hardware_settings_.trigger_polarity == 1 ? "falling" : "rising")
           << "\",\n"
           << "    \"record_mask\": " << hardware_settings_.channel_mask
           << ",\n"
           << "    \"record_mask_readback\": "
           << channel_mask_readback_ << ",\n"
           << "    \"record_length\": " << hardware_settings_.record_length
           << ",\n"
           << "    \"post_trigger_percent\": "
           << hardware_settings_.post_trigger << ",\n"
           << "    \"external_trigger_mode\": "
           << hardware_settings_.ext_trigger_mode << ",\n"
           << "    \"self_trigger_mode\": "
           << hardware_settings_.self_trigger_mode << ",\n"
           << "    \"self_trigger_mask\": "
           << hardware_settings_.self_trigger_mask << ",\n"
           << "    \"pair_logic\": \""
           << (hardware_settings_.pair_logic == DAQPairLogic::kAnd ? "AND" : "OR")
           << "\",\n"
           << "    \"explicit_trigger_routing\": "
           << (hardware_settings_.explicit_trigger_routing ? "true" : "false")
           << ",\n"
           << "    \"global_trigger_mask_readback\": "
           << global_trigger_mask_readback_ << ",\n"
           << "    \"pair_logic_readback\": [";
  for (std::size_t pair = 0; pair < pair_logic_readback_.size(); ++pair) {
    if (pair != 0U) metadata << ", ";
    metadata << pair_logic_readback_[pair];
  }
  metadata << "]\n  },\n  \"channels\": [\n";

  bool first_channel = true;
  for (int ch = 0; ch < MAX_CH; ++ch) {
    const auto& runtime = runtime_channels_[ch];
    if (!runtime.enabled) continue;
    if (!first_channel) metadata << ",\n";
    first_channel = false;
    metadata << "    {\"channel\": " << ch
             << ", \"trigger_enabled\": "
             << (runtime.participates_in_trigger ? "true" : "false")
             << ", \"input_range_register\": "
             << runtime.input_range_register
             << ", \"input_range_readback\": "
             << runtime.input_range_readback
             << ", \"requested_dc_offset\": "
             << runtime.requested_dc_offset
             << ", \"readback_dc_offset\": "
             << runtime.readback_dc_offset
             << ", \"polarity_readback\": \""
             << (runtime.polarity_readback == 1 ? "falling" : "rising")
             << "\", \"threshold_mode\": \""
             << (!runtime.threshold_programmed
                     ? "not_used_record_only"
                     : (runtime.measured_threshold
                            ? "baseline_relative_mv"
                            : "legacy_absolute_adc"))
             << "\", \"measured_baseline_adc\": ";
    if (runtime.baseline_measured) metadata << runtime.measured_baseline_adc;
    else metadata << "null";
    metadata << ", \"requested_threshold_mv\": ";
    if (runtime.measured_threshold) metadata << runtime.requested_threshold_mv;
    else metadata << "null";
    metadata << ", \"delta_adc\": ";
    if (runtime.measured_threshold) metadata << runtime.delta_adc;
    else metadata << "null";
    metadata << ", \"written_threshold_adc\": ";
    if (runtime.threshold_programmed) metadata << runtime.written_threshold_adc;
    else metadata << "null";
    metadata << ", \"readback_threshold_adc\": ";
    if (runtime.threshold_programmed) metadata << runtime.readback_threshold_adc;
    else metadata << "null";
    metadata << ", \"effective_threshold_mv\": ";
    if (runtime.measured_threshold) metadata << runtime.effective_threshold_mv;
    else metadata << "null";
    metadata << "}";
  }
  metadata << "\n  ]\n}\n";
  if (!metadata) {
    throw std::runtime_error("Failed while composing runtime metadata");
  }
  WriteNewAtomicFile(published_metadata_path, metadata.str());

  std::cout << "\033[1;32m[Run Provenance]\033[0m config snapshot="
            << config_snapshot_file_ << ", metadata="
            << published_metadata_path
            << ", run=" << run_number_ << "\n";
}

void DAQManager::Start(std::atomic<bool>& is_running) {
  std::cout << "\033[1;32m[DAQManager]\033[0m Starting Acquisition...\n";
  std::cout << " - Stop Condition : ";
  if (max_events_ > 0) std::cout << max_events_ << " Events\n";
  else if (run_time_sec_ > 0) std::cout << run_time_sec_ << " Seconds\n";
  else std::cout << "Unlimited (Manual Stop)\n";

  bool acquisition_started = false;
  try {
    CAEN_CHECK(CAEN_DGTZ_SWStartAcquisition(digitizer_.GetHandle()));
    acquisition_started = true;
    running_ = true;
    acquisition_status_ = "running";
    failure_reason_.clear();
    WriteRuntimeArtifacts();
    AcquisitionLoop(is_running);
    acquisition_started = false;
    running_ = false;
    acquisition_status_ = "completed";
    failure_reason_.clear();
    WriteRuntimeArtifacts();
  } catch (const std::exception& error) {
    running_ = false;
    if (acquisition_started) {
      CAEN_DGTZ_SWStopAcquisition(digitizer_.GetHandle());
    }
    acquisition_status_ = "failed";
    failure_reason_ = error.what();
    try {
      WriteRuntimeArtifacts();
    } catch (const std::exception& metadata_error) {
      std::cerr << "\n[Run Provenance] Cannot record failed status: "
                << metadata_error.what() << "\n";
    }
    throw;
  } catch (...) {
    running_ = false;
    if (acquisition_started) {
      CAEN_DGTZ_SWStopAcquisition(digitizer_.GetHandle());
    }
    acquisition_status_ = "failed";
    failure_reason_ = "unknown non-standard exception";
    try {
      WriteRuntimeArtifacts();
    } catch (const std::exception& metadata_error) {
      std::cerr << "\n[Run Provenance] Cannot record failed status: "
                << metadata_error.what() << "\n";
    }
    throw;
  }
}

void DAQManager::Stop() {
  running_ = false;
}

void DAQManager::AcquisitionLoop(std::atomic<bool>& is_running) {
  EventHeader *header = reinterpret_cast<EventHeader *>(raw_buffer_pool_.data());
  uint16_t *wave_dest = reinterpret_cast<uint16_t *>(raw_buffer_pool_.data() + sizeof(EventHeader));
  
  int handle = digitizer_.GetHandle();
  char *caen_buffer = digitizer_.GetReadoutBuffer();
  CAEN_DGTZ_UINT16_EVENT_t *caen_event = digitizer_.GetDecodedEvent();
  
  uint32_t event_count = 0;
  const uint32_t TTT_MASK = 0x7FFFFFFF;
  
  bool is_first_event = true;
  uint32_t first_ttt = 0, current_ttt = 0, prev_ttt = 0, prev_event_counter = 0;
  uint64_t ttt_rollovers = 0, lost_events = 0;

  auto start_time = std::chrono::steady_clock::now();
  auto last_log_time = start_time;
  uint32_t log_events = 0, zmq_drops = 0, loop_counter = 0;
  uint32_t consecutive_readout_errors = 0;
  size_t total_bytes_written = 0, last_bytes_written = 0; 

  while (is_running && running_) {
    if (max_events_ > 0 && (int)event_count >= max_events_) break;
    if (run_time_sec_ > 0) {
      auto now = std::chrono::steady_clock::now();
      if (std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count() >= run_time_sec_) break;
    }

    uint32_t bsize = 0; 

    try {
      CAEN_CHECK(CAEN_DGTZ_ReadData(handle, CAEN_DGTZ_SLAVE_TERMINATED_READOUT_MBLT, caen_buffer, &bsize));
      
      if (bsize > 0) {
        uint32_t num_events = 0;
        CAEN_CHECK(CAEN_DGTZ_GetNumEvents(handle, caen_buffer, bsize, &num_events));

        for (uint32_t i = 0; i < num_events; ++i) {
          CAEN_DGTZ_EventInfo_t evt_info;
          char *evt_ptr = nullptr;
          CAEN_CHECK(CAEN_DGTZ_GetEventInfo(handle, caen_buffer, bsize, i, &evt_info, &evt_ptr));
          CAEN_CHECK(CAEN_DGTZ_DecodeEvent(handle, evt_ptr, (void **)&caen_event));

          current_ttt = evt_info.TriggerTimeTag & TTT_MASK;
          uint32_t current_event_counter = ((uint32_t*)evt_ptr)[2] & 0xFFFFFF;

          if (is_first_event) {
              first_ttt = current_ttt; prev_ttt = current_ttt; prev_event_counter = current_event_counter; is_first_event = false;
          } else {
              if (current_ttt < prev_ttt) ttt_rollovers++;
              uint32_t diff = (current_event_counter - prev_event_counter) & 0xFFFFFF;
              if (diff > 1) lost_events += (diff - 1); 
          }
          prev_ttt = current_ttt; prev_event_counter = current_event_counter;

          uint32_t actual_trace_size = 0;
          for (int ch = 0; ch < MAX_CH; ++ch) {
              if ((evt_info.ChannelMask >> ch) & 1) { actual_trace_size = caen_event->ChSize[ch]; break; }
          }
          if (actual_trace_size == 0U ||
              actual_trace_size != hardware_settings_.record_length ||
              evt_info.ChannelMask != hardware_settings_.channel_mask) {
            throw FatalAcquisitionError(
                "Decoded event channel mask/record length does not match the "
                "configured acquisition shape");
          }
          for (int ch = 0; ch < MAX_CH; ++ch) {
            if (((evt_info.ChannelMask >> ch) & 1U) != 0U &&
                caen_event->ChSize[ch] != actual_trace_size) {
              throw FatalAcquisitionError(
                  "Decoded event has inconsistent active-channel trace sizes");
            }
          }

          std::memset(header, 0, sizeof(EventHeader));
          header->ExtendedTTT = (ttt_rollovers << 31) | current_ttt;
          header->EventID = event_count++;
          header->RecordLength = actual_trace_size; 
          header->ChannelMask = evt_info.ChannelMask;
          header->Pattern = evt_info.Pattern;
          header->BoardEventCounter = current_event_counter;

          size_t payload_size = sizeof(EventHeader);
          for (int ch = 0; ch < MAX_CH; ++ch) {
            if ((header->ChannelMask >> ch) & 1) {
              uint16_t *wave_src = caen_event->DataChannel[ch];
              uint32_t trace_size = caen_event->ChSize[ch];
              if (trace_size == 0) continue;
              if (payload_size + trace_size * sizeof(uint16_t) >
                  raw_buffer_pool_.size()) {
                throw FatalAcquisitionError(
                    "Decoded event exceeds the reserved raw-output buffer");
              }

              std::memcpy(wave_dest + (payload_size - sizeof(EventHeader)) / sizeof(uint16_t), wave_src, trace_size * sizeof(uint16_t));
              payload_size += trace_size * sizeof(uint16_t);
            }
          }

          if (!output_stream_) {
            throw FatalAcquisitionError("Raw output stream is not open");
          }
          const std::size_t bytes_written =
              ::fwrite(raw_buffer_pool_.data(), 1, payload_size,
                       output_stream_);
          if (bytes_written != payload_size || ::ferror(output_stream_)) {
            throw FatalAcquisitionError(
                "Raw output write failed; acquisition stopped to prevent "
                "silent data loss");
          }
          total_bytes_written += payload_size;
          if (zmq_send(zmq_pub_, raw_buffer_pool_.data(), payload_size, ZMQ_DONTWAIT) < 0) { if (zmq_errno() == EAGAIN) zmq_drops++; }
          log_events++;
        }
      }
      consecutive_readout_errors = 0;
    } catch (const FatalAcquisitionError&) {
        CAEN_DGTZ_SWStopAcquisition(handle);
        throw;
    } catch (const std::exception& e) {
        ++consecutive_readout_errors;
        std::cerr << "\n\033[1;33m[Warning] Readout error "
                  << consecutive_readout_errors << "/3: \033[0m"
                  << e.what() << "\n";
        if (consecutive_readout_errors >= 3U) {
          CAEN_DGTZ_SWStopAcquisition(handle);
          throw FatalAcquisitionError(
              "Persistent CAEN readout failure after 3 consecutive errors: " +
              std::string(e.what()));
        }
    }

    if (bsize > 0 || ++loop_counter % 10000 == 0) {
        auto now = std::chrono::steady_clock::now();
        if (run_time_sec_ > 0) {
            if (std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count() >= run_time_sec_) break;
        }

        double elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log_time).count();
        if (elapsed_ms >= 1000.0) {
            auto total_sec = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
            int mins = total_sec / 60;
            int secs = total_sec % 60;

            double rate = (log_events / elapsed_ms) * 1000.0;
            double speed_mbps = ((total_bytes_written - last_bytes_written) / 1048576.0) / (elapsed_ms / 1000.0);
            last_bytes_written = total_bytes_written;

            uint32_t temp_reg = 0, status_reg = 0;
            if (CAEN_DGTZ_ReadRegister(handle, 0x10A8, &temp_reg) == CAEN_DGTZ_Success) {
                float temp_celsius = static_cast<float>(temp_reg & 0xFF);
                if (temp_celsius >= 82.0) {
                    std::cout << "\n[FATAL] OVER_TEMP_SOFT_KILL" << std::endl;
                    throw FatalAcquisitionError(
                        "Digitizer temperature reached the 82 C software "
                        "shutdown limit");
                }
            }
            
            if (CAEN_DGTZ_ReadRegister(handle, 0x8104, &status_reg) == CAEN_DGTZ_Success) {
                int run      = (status_reg >> 0) & 0x1; 
                int drdy     = (status_reg >> 2) & 0x1; 
                int busy     = (status_reg >> 3) & 0x1; 
                int pll_lock = ((status_reg >> 5) & 0x1) == 0 ? 1 : 0; 
                int trg      = (rate > 0.0) ? 1 : 0; 
                int pll_byps = 0; 

                std::cout << "[STATUS] LED: LOCK=" << pll_lock << ", BYPS=" << pll_byps
                          << ", RUN=" << run << ", TRG=" << trg << ", DRDY=" << drdy
                          << ", BUSY=" << busy << std::endl;
            }

            uint32_t record_length = hardware_settings_.record_length;
            uint64_t total_ticks = (ttt_rollovers << 31) + current_ttt - first_ttt;
            
            double hw_real_time_sec = total_ticks * 8e-9; 
            double dead_time_sec = event_count * (record_length * 2e-9); 
            double live_time_sec = hw_real_time_sec - dead_time_sec;
            if (live_time_sec < 0) live_time_sec = 0.0;
            
            double dead_time_pct = (hw_real_time_sec > 0) ? (dead_time_sec / hw_real_time_sec * 100.0) : 0.0;

            std::cout << "\r\033[K\033[1;36m[LIVE DAQ]\033[0m "
                      << "Time: \033[1m" << std::setfill('0') << std::setw(2) << mins << ":" << std::setw(2) << secs << "\033[0m | "
                      << "RealTime: \033[1m" << std::fixed << std::setprecision(2) << hw_real_time_sec << " s\033[0m | "
                      << "Live: \033[1m" << std::fixed << std::setprecision(2) << live_time_sec << " s\033[0m | " 
                      << "DT: \033[1;31m" << std::fixed << std::setprecision(4) << dead_time_pct << " %\033[0m | "
                      << "Rate: \033[1;35m" << std::fixed << std::setprecision(1) << rate << " Hz\033[0m | " 
                      << "Events: \033[1;33m" << event_count << "\033[0m | "
                      << "Speed: \033[1;32m" << std::fixed << std::setprecision(2) << speed_mbps << " MB/s\033[0m | "
                      << "Drops: " << zmq_drops
                      << std::flush;
              
            log_events = 0; zmq_drops = 0; last_log_time = now;
        }
    }
  }

  CAEN_CHECK(CAEN_DGTZ_SWStopAcquisition(handle));
  if (!output_stream_ || ::fflush(output_stream_) != 0 ||
      ::ferror(output_stream_)) {
    throw FatalAcquisitionError(
        "Raw output flush failed; run is marked failed");
  }
  const int output_descriptor = ::fileno(output_stream_);
  if (output_descriptor < 0 || ::fsync(output_descriptor) != 0) {
    throw FatalAcquisitionError(
        "Raw output sync failed; run is marked failed");
  }
  if (!PathMatchesIdentity(output_file_, {output_device_, output_inode_})) {
    throw FatalAcquisitionError(
        "Raw output path changed during acquisition; run is marked failed");
  }
  struct stat raw_before_hash {};
  if (::fstat(output_descriptor, &raw_before_hash) != 0 ||
      raw_before_hash.st_size < 0 ||
      static_cast<uint64_t>(raw_before_hash.st_dev) != output_device_ ||
      static_cast<uint64_t>(raw_before_hash.st_ino) != output_inode_) {
    throw FatalAcquisitionError(
        "Cannot verify the finalized raw output inode and size");
  }
  raw_output_size_bytes_ = static_cast<uint64_t>(raw_before_hash.st_size);
  raw_output_sha256_ =
      Sha256FileDescriptorHex(output_descriptor, raw_output_size_bytes_);
  struct stat raw_after_hash {};
  if (::fstat(output_descriptor, &raw_after_hash) != 0 ||
      raw_after_hash.st_dev != raw_before_hash.st_dev ||
      raw_after_hash.st_ino != raw_before_hash.st_ino ||
      raw_after_hash.st_size != raw_before_hash.st_size ||
      raw_after_hash.st_mtim.tv_sec != raw_before_hash.st_mtim.tv_sec ||
      raw_after_hash.st_mtim.tv_nsec != raw_before_hash.st_mtim.tv_nsec) {
    throw FatalAcquisitionError(
        "Raw output changed while its SHA-256 was being finalized");
  }
  raw_output_finalized_ = true;
  std::cout << "\n\033[1;32m[Run Provenance]\033[0m raw_size="
            << raw_output_size_bytes_ << " bytes, raw_sha256="
            << raw_output_sha256_ << "\n";
  std::cout << "\n\033[1;31m[DAQManager] Stopped Acquisition.\033[0m\n";

  auto t = std::time(nullptr);
  auto tm = *std::localtime(&t);
  
  uint32_t record_length = hardware_settings_.record_length;
  uint64_t final_total_ticks = (ttt_rollovers << 31) + current_ttt - first_ttt;
  
  double final_real_time_sec = final_total_ticks * 8e-9;
  double final_dead_time_sec = event_count * (record_length * 2e-9);
  double final_live_time_sec = final_real_time_sec - final_dead_time_sec;
  if (final_live_time_sec < 0) final_live_time_sec = 0.0;
  
  double final_dead_time_pct = (final_real_time_sec > 0) ? (final_dead_time_sec / final_real_time_sec * 100.0) : 0.0;
  auto wall_clock_duration = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time).count();
  
  double avg_rate = (final_real_time_sec > 0) ? (event_count / final_real_time_sec) : 0.0;

  std::cout << "\n\033[1;36m========== [ DAQ Run Summary ] ==========\033[0m\n"
            << " - End Time        : " << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "\n"
            << " - Wall Clock Time : " << wall_clock_duration << " seconds\n"
            << " - HW Real Time    : " << std::fixed << std::setprecision(2) << final_real_time_sec << " seconds\n"
            << " - HW Live Time    : " << std::fixed << std::setprecision(2) << final_live_time_sec << " seconds\n"
            << " - True Dead Time  : " << std::fixed << std::setprecision(5) << final_dead_time_pct << " %\n"
            << " - Total Events    : " << event_count << " events\n"
            << " - Avg Trig Rate   : " << std::fixed << std::setprecision(2) << avg_rate << " Hz\n" 
            << " - Lost Events     : " << lost_events << " events (Buffer Full)\n"
            << " - Data Size Saved : " << std::fixed << std::setprecision(2) << (total_bytes_written / (1024.0 * 1024.0)) << " MB\n"
            << "\033[1;36m=========================================\033[0m\n\n";
}
