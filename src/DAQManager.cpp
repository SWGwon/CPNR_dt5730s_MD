#include "DAQManager.h"
#include "Sha256.h"
#include "EventHeader.h"
#include "TriggerCalibration.h"
#include "CAENComm.h" 
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
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
#include <sys/statvfs.h>
#include <sys/syscall.h>
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
constexpr uint32_t kAcquisitionStatusRegister = 0x8104;
constexpr uint32_t kAcquisitionRunMask = 1U << 0;
constexpr int kDppFirmwareMajorBase = 128;
constexpr uint64_t kZmqQueueByteBudget = 64U * 1024U * 1024U;
constexpr uint32_t kZmqMaximumHwmMessages = 5000U;

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

uint32_t EnabledChannelCount(uint32_t mask) {
  uint32_t count = 0U;
  while (mask != 0U) {
    count += mask & 1U;
    mask >>= 1U;
  }
  return count;
}

uint64_t RawEventBytes(const DAQHardwareSettings& settings) {
  const uint64_t waveform_bytes =
      static_cast<uint64_t>(EnabledChannelCount(settings.channel_mask)) *
      settings.record_length * sizeof(uint16_t);
  if (waveform_bytes >
      std::numeric_limits<uint64_t>::max() - sizeof(EventHeader)) {
    throw std::runtime_error("Configured raw event size overflows uint64_t");
  }
  return sizeof(EventHeader) + waveform_bytes;
}

std::filesystem::path OutputParent(const std::string& path) {
  const auto parent = std::filesystem::path(path).parent_path();
  return parent.empty() ? std::filesystem::path(".") : parent;
}

uint64_t FilesystemFreeBytes(const std::string& output_path) {
  const auto parent = OutputParent(output_path);
  struct statvfs filesystem {};
  if (::statvfs(parent.c_str(), &filesystem) != 0) {
    throw std::runtime_error(
        "Cannot inspect free space on output filesystem " +
        parent.string() + " (" + std::strerror(errno) + ")");
  }
  if (filesystem.f_frsize != 0U &&
      static_cast<uint64_t>(filesystem.f_bavail) >
          std::numeric_limits<uint64_t>::max() /
              static_cast<uint64_t>(filesystem.f_frsize)) {
    return std::numeric_limits<uint64_t>::max();
  }
  return static_cast<uint64_t>(filesystem.f_bavail) *
         static_cast<uint64_t>(filesystem.f_frsize);
}

uint64_t ExpectedRawBytes(const DAQHardwareSettings& settings,
                          int max_events) {
  if (max_events <= 0) return 0U;
  const uint64_t event_bytes = RawEventBytes(settings);
  if (static_cast<uint64_t>(max_events) >
      std::numeric_limits<uint64_t>::max() / event_bytes) {
    throw std::runtime_error("Requested event count overflows raw size estimate");
  }
  return event_bytes * static_cast<uint64_t>(max_events);
}

std::string PrepareWorkingOutputPath(
    const std::string& final_output, const DAQHardwareSettings& settings,
    int max_events, int run_time_sec) {
  if (max_events < 0 || run_time_sec < 0) {
    throw std::runtime_error("Event and time limits cannot be negative");
  }
  if (max_events > 0 && run_time_sec > 0) {
    throw std::runtime_error(
        "Event and time limits are mutually exclusive");
  }
  const std::string working =
      RequireAbsentFile(final_output + ".partial", "Raw partial output");
  const uint64_t available = FilesystemFreeBytes(working);
  const uint64_t expected = ExpectedRawBytes(settings, max_events);
  const uint64_t reserve = settings.storage.minimum_free_bytes;
  if (available < reserve || expected > available - reserve) {
    std::ostringstream message;
    message << "Insufficient output-filesystem capacity: available="
            << available << " bytes, expected_raw=" << expected
            << " bytes, required_reserve=" << reserve << " bytes";
    throw std::runtime_error(message.str());
  }
  return working;
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

std::string ConfigSnapshotPathAndValidateArtifacts(
    const std::string& config_file, const std::string& output_file,
    const std::string& working_output_file,
    const std::string& metadata_file) {
  const std::string snapshot =
      ConfigSnapshotPath(config_file, output_file);
  const std::array<std::pair<const char*, std::string>, 6> artifacts{{
      {"raw output", output_file},
      {"partial raw output", working_output_file},
      {"runtime metadata", metadata_file},
      {"runtime config snapshot", snapshot},
      {"hardware-verified status",
       metadata_file + ".status.hardware_verified_not_started.json"},
      {"running status", metadata_file + ".status.running.json"},
  }};

  struct TargetKey {
    uint64_t parent_device = 0;
    uint64_t parent_inode = 0;
    std::string filename;
  };
  const auto target_key = [](const std::string& target) {
    const std::filesystem::path path(target);
    const std::filesystem::path parent = path.parent_path().empty()
                                             ? std::filesystem::path(".")
                                             : path.parent_path();
    const int descriptor =
        ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
      throw std::runtime_error(
          "Cannot open runtime artifact directory " + parent.string() +
          " (" + std::strerror(errno) + ")");
    }
    struct stat status {};
    const int stat_result = ::fstat(descriptor, &status);
    const int stat_error = errno;
    ::close(descriptor);
    if (stat_result != 0 || !S_ISDIR(status.st_mode)) {
      throw std::runtime_error(
          "Cannot identify runtime artifact directory " + parent.string() +
          " (" + std::strerror(stat_error) + ")");
    }
    return TargetKey{static_cast<uint64_t>(status.st_dev),
                     static_cast<uint64_t>(status.st_ino),
                     path.filename().string()};
  };
  std::array<TargetKey, 6> keys;
  for (std::size_t index = 0; index < artifacts.size(); ++index) {
    keys[index] = target_key(artifacts[index].second);
  }
  for (std::size_t left = 0; left < artifacts.size(); ++left) {
    for (std::size_t right = left + 1U; right < artifacts.size(); ++right) {
      if (keys[left].parent_device == keys[right].parent_device &&
          keys[left].parent_inode == keys[right].parent_inode &&
          keys[left].filename == keys[right].filename) {
        throw std::runtime_error(
            "Runtime artifact paths alias: " +
            std::string(artifacts[left].first) + " and " +
            artifacts[right].first + " both resolve to " +
            artifacts[left].second);
      }
    }
  }
  return snapshot;
}

std::pair<int, FileIdentity> ReserveOutputFile(
    const std::string& output_file) {
  const int descriptor =
      ::open(output_file.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  if (descriptor < 0) {
    throw std::runtime_error(
        "Cannot create raw output without overwriting an existing file: " +
        output_file + " (" + std::strerror(errno) + ")");
  }
  struct stat status {};
  if (::fstat(descriptor, &status) != 0) {
    const int status_error = errno;
    ::close(descriptor);
    throw std::runtime_error("Cannot identify newly reserved raw output " +
                             output_file + " (" +
                             std::strerror(status_error) +
                             "); preserving the path because its identity "
                             "could not be established");
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

int RenameNoReplace(int old_directory, const char* old_path,
                    int new_directory, const char* new_path) {
#ifdef SYS_renameat2
  constexpr unsigned int kRenameNoReplace = 1U;
  return static_cast<int>(::syscall(SYS_renameat2, old_directory, old_path,
                                    new_directory, new_path,
                                    kRenameNoReplace));
#else
  (void)old_directory;
  (void)old_path;
  (void)new_directory;
  (void)new_path;
  errno = ENOSYS;
  return -1;
#endif
}

bool RemoveIfSameFile(const std::string& path,
                      const FileIdentity& identity) {
  // There is no POSIX unlink-by-inode operation.  Checking with lstat() and
  // then unlinking the public pathname is unsafe because another process can
  // replace the name between those calls.  Instead, atomically move whichever
  // entry currently owns the public name into a freshly-created quarantine,
  // inspect it through the pinned directory descriptor, and delete it only
  // after the inode matches.  A mismatching entry is restored without
  // overwrite; if the public name was concurrently reused, preserve the entry
  // in quarantine rather than deleting either file.
  const auto target_path = std::filesystem::path(path);
  const auto parent = target_path.parent_path().empty()
                          ? std::filesystem::path(".")
                          : target_path.parent_path();
  std::string quarantine_template =
      (parent / ".cpnr-cleanup-XXXXXX").string();
  std::vector<char> quarantine_buffer(quarantine_template.begin(),
                                      quarantine_template.end());
  quarantine_buffer.push_back('\0');
  char* const quarantine_created = ::mkdtemp(quarantine_buffer.data());
  if (quarantine_created == nullptr) return false;
  const std::string quarantine_path(quarantine_created);

  const int quarantine_descriptor =
      ::open(quarantine_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (quarantine_descriptor < 0) {
    ::rmdir(quarantine_path.c_str());
    return false;
  }

  constexpr const char* kQuarantinedEntry = "candidate";
  const auto close_empty_quarantine = [&]() {
    ::close(quarantine_descriptor);
    if (::rmdir(quarantine_path.c_str()) != 0 && errno != ENOENT) {
      std::cerr << "[Run Provenance] Cannot remove empty cleanup quarantine: "
                << quarantine_path << " (" << std::strerror(errno) << ")\n";
    }
  };
  const auto preserve_or_restore = [&]() {
    if (RenameNoReplace(quarantine_descriptor, kQuarantinedEntry, AT_FDCWD,
                        path.c_str()) == 0) {
      close_empty_quarantine();
      return;
    }
    const int restore_error = errno;
    ::close(quarantine_descriptor);
    std::cerr << "[Run Provenance] Preserved a cleanup-race entry in "
              << quarantine_path << "/" << kQuarantinedEntry
              << " because its public name could not be restored without "
                 "overwrite: "
              << path << " (" << std::strerror(restore_error) << ")\n";
  };

  if (RenameNoReplace(AT_FDCWD, path.c_str(), quarantine_descriptor,
                      kQuarantinedEntry) != 0) {
    close_empty_quarantine();
    return false;
  }

  struct stat quarantined_status {};
  if (::fstatat(quarantine_descriptor, kQuarantinedEntry,
                &quarantined_status, AT_SYMLINK_NOFOLLOW) != 0) {
    preserve_or_restore();
    return false;
  }
  const bool identity_matches =
      static_cast<uint64_t>(quarantined_status.st_dev) == identity.device &&
      static_cast<uint64_t>(quarantined_status.st_ino) == identity.inode;
  if (!identity_matches) {
    preserve_or_restore();
    return false;
  }

  if (::unlinkat(quarantine_descriptor, kQuarantinedEntry, 0) != 0) {
    preserve_or_restore();
    return false;
  }
  close_empty_quarantine();
  return true;
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
    throw std::runtime_error(
        "Cannot identify temporary runtime artifact " + temporary_path +
        " (" + std::strerror(status_error) +
        "); preserving it because its identity could not be established");
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

double HistogramMedian(const std::vector<uint64_t>& histogram) {
  const uint64_t sample_count =
      std::accumulate(histogram.begin(), histogram.end(), uint64_t{0});
  if (sample_count == 0U) {
    throw std::runtime_error(
        "Cannot calculate a baseline from zero ADC samples");
  }
  const uint64_t lower_rank = (sample_count - 1U) / 2U;
  const uint64_t upper_rank = sample_count / 2U;
  uint64_t cumulative = 0U;
  uint32_t lower_value = 0U;
  uint32_t upper_value = 0U;
  bool lower_found = false;
  for (std::size_t code = 0; code < histogram.size(); ++code) {
    cumulative += histogram[code];
    if (!lower_found && cumulative > lower_rank) {
      lower_value = static_cast<uint32_t>(code);
      lower_found = true;
    }
    if (cumulative > upper_rank) {
      upper_value = static_cast<uint32_t>(code);
      break;
    }
  }
  return (static_cast<double>(lower_value) +
          static_cast<double>(upper_value)) /
         2.0;
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
      working_output_file_(PrepareWorkingOutputPath(
          output_file_, hardware_settings_, max_events, run_time_sec)),
      metadata_file_(MetadataOutputPath(output_file, metadata_file)),
      config_snapshot_file_(ConfigSnapshotPathAndValidateArtifacts(
          config_file_, output_file_, working_output_file_, metadata_file_)),
      executable_path_(executable_path.empty() ? "unknown" : executable_path),
      git_commit_(git_commit), build_timestamp_(build_timestamp),
      run_number_(PositiveRunNumber(run_number)), max_events_(max_events),
      run_time_sec_(run_time_sec), running_(false),
      cancellation_flag_(cancellation_flag) {
  expected_raw_bytes_ = ExpectedRawBytes(hardware_settings_, max_events_);
  output_free_bytes_at_start_ = FilesystemFreeBytes(working_output_file_);
  std::cout << "\033[1;36m[Storage Preflight]\033[0m filesystem_free="
            << output_free_bytes_at_start_ << " bytes, expected_raw="
            << expected_raw_bytes_ << " bytes, reserve="
            << hardware_settings_.storage.minimum_free_bytes
            << " bytes, runtime_stop_watermark="
            << hardware_settings_.storage.stop_free_bytes << " bytes\n";
  // Hash the inode the kernel is actually executing. Reopening argv[0] can
  // hash a replacement binary after an in-place rebuild.
  executable_sha256_ = Sha256FileHex("/proc/self/exe");

  bool output_created = false;
  int output_descriptor = -1;
  const auto cleanup_failed_construction = [&]() {
    if (output_stream_) {
      ::fclose(output_stream_);
      output_stream_ = nullptr;
    } else if (output_descriptor >= 0) {
      ::close(output_descriptor);
      output_descriptor = -1;
    }
    if (output_created) {
      RemoveIfSameFile(working_output_file_,
                       {output_device_, output_inode_});
    }
    if (zmq_pub_) zmq_close(zmq_pub_);
    if (zmq_ctx_) zmq_ctx_destroy(zmq_ctx_);
    zmq_pub_ = nullptr;
    zmq_ctx_ = nullptr;
  };
  const auto record_failed_setup = [&](const std::string& reason) {
    acquisition_status_ = "failed";
    termination_reason_ = "setup_failure";
    failure_reason_ = reason;
    acquisition_end_unix_time_ = std::time(nullptr);
    try {
      WriteRuntimeArtifacts();
    } catch (const std::exception& metadata_error) {
      std::cerr << "[Run Provenance] Cannot record setup failure: "
                << metadata_error.what() << "\n";
    }
  };
  const auto record_cancelled_setup = [&]() {
    acquisition_status_ = "cancelled";
    termination_reason_ = "cancelled_during_setup";
    failure_reason_.clear();
    acquisition_end_unix_time_ = std::time(nullptr);
    WriteRuntimeArtifacts();
  };
  try {
    zmq_ctx_ = zmq_ctx_new();
    if (!zmq_ctx_) {
      throw std::runtime_error("Cannot create ZeroMQ context");
    }
    zmq_pub_ = zmq_socket(zmq_ctx_, ZMQ_PUB);
    if (!zmq_pub_) {
      throw std::runtime_error("Cannot create ZeroMQ publisher socket");
    }
    const uint64_t raw_event_bytes = RawEventBytes(hardware_settings_);
    const uint64_t byte_limited_hwm =
        std::max<uint64_t>(1U, kZmqQueueByteBudget / raw_event_bytes);
    zmq_send_hwm_messages_ = static_cast<uint32_t>(
        std::min<uint64_t>(kZmqMaximumHwmMessages, byte_limited_hwm));
    zmq_send_hwm_approx_bytes_ =
        raw_event_bytes * static_cast<uint64_t>(zmq_send_hwm_messages_);
    const int hwm = static_cast<int>(zmq_send_hwm_messages_);
    if (zmq_setsockopt(zmq_pub_, ZMQ_SNDHWM, &hwm, sizeof(hwm)) != 0) {
      throw std::runtime_error(
          "Cannot configure ZeroMQ send watermark: " +
          std::string(zmq_strerror(zmq_errno())));
    }
    int linger = 0;
    if (zmq_setsockopt(zmq_pub_, ZMQ_LINGER, &linger, sizeof(linger)) != 0) {
      throw std::runtime_error("Cannot configure ZeroMQ linger: " +
                               std::string(zmq_strerror(zmq_errno())));
    }
    if (zmq_bind(zmq_pub_, "tcp://127.0.0.1:5555") != 0) {
      throw std::runtime_error(
          "Cannot bind ZeroMQ publisher to tcp://127.0.0.1:5555: " +
          std::string(zmq_strerror(zmq_errno())));
    }
    std::cout << "\033[1;36m[ZeroMQ]\033[0m send_hwm="
              << zmq_send_hwm_messages_ << " messages, approximate_queue="
              << zmq_send_hwm_approx_bytes_ << "/" << kZmqQueueByteBudget
              << " bytes\n";
    digitizer_ =
        std::make_unique<CaenDigitizer>(CAEN_DGTZ_USB, 0, 0, 0);
    SetupHardware();
    hardware_verified_unix_time_ = std::time(nullptr);
    const auto reservation = ReserveOutputFile(working_output_file_);
    output_descriptor = reservation.first;
    output_device_ = reservation.second.device;
    output_inode_ = reservation.second.inode;
    output_created = true;
    SyncParentDirectory(working_output_file_);
    output_stream_ = ::fdopen(output_descriptor, "w+b");
    if (!output_stream_) {
      const int stream_error = errno;
      ::close(output_descriptor);
      output_descriptor = -1;
      throw std::runtime_error("Cannot attach a stream to reserved output " +
                               working_output_file_ + " (" +
                               std::strerror(stream_error) + ")");
    }
    output_descriptor = -1;
    output_write_buffer_.resize(4 * 1024 * 1024);
    if (::setvbuf(output_stream_, output_write_buffer_.data(), _IOFBF,
                  output_write_buffer_.size()) != 0) {
      throw std::runtime_error("Cannot configure raw-output write buffer");
    }
    WriteRuntimeArtifacts();
  } catch (const DAQSetupCancelled&) {
    try {
      record_cancelled_setup();
    } catch (const std::exception& metadata_error) {
      cleanup_failed_construction();
      throw std::runtime_error(
          "Hardware setup was cancelled, but terminal metadata could not be "
          "recorded: " + std::string(metadata_error.what()));
    } catch (...) {
      cleanup_failed_construction();
      throw std::runtime_error(
          "Hardware setup was cancelled, but terminal metadata failed with "
          "an unknown non-standard exception");
    }
    cleanup_failed_construction();
    throw;
  } catch (const std::exception& error) {
    record_failed_setup(error.what());
    cleanup_failed_construction();
    throw;
  } catch (...) {
    record_failed_setup("unknown non-standard setup exception");
    cleanup_failed_construction();
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
  if (!digitizer_) {
    throw std::logic_error("DAQ hardware object is not initialized");
  }
  const int handle = digitizer_->GetHandle();

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
  CAEN_CHECK(CAEN_DGTZ_GetPostTriggerSize(handle,
                                          &post_trigger_readback_));
  if (post_trigger_readback_ != hardware_settings_.post_trigger) {
    throw std::runtime_error(
        "Post-trigger readback mismatch: requested " +
        std::to_string(hardware_settings_.post_trigger) + ", read back " +
        std::to_string(post_trigger_readback_));
  }
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

  digitizer_->AllocateBuffers();
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
    throw DAQSetupCancelled(
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
  const std::size_t adc_codes =
      std::size_t{1} << hardware_settings_.adc_bits;
  std::array<std::vector<uint64_t>, MAX_CH> histograms;
  for (int ch = 0; ch < MAX_CH; ++ch) {
    if (((hardware_settings_.channel_mask >> ch) & 1U) != 0U) {
      histograms[ch].assign(adc_codes, 0U);
    }
  }
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
    char* readout_buffer = digitizer_->GetReadoutBuffer();
    auto* decoded_event = digitizer_->GetDecodedEvent();
    while (decoded_events < requested_events &&
           std::chrono::steady_clock::now() < deadline) {
      CheckSetupCancellation();
      // Issue and drain one calibration trigger at a time.  Sending all
      // MeasurementEvents before reading can overflow the digitizer FIFO at
      // otherwise-valid high settings.
      CAEN_CHECK(CAEN_DGTZ_SendSWtrigger(handle));
      bool trigger_drained = false;
      while (!trigger_drained &&
             std::chrono::steady_clock::now() < deadline) {
        CheckSetupCancellation();
        uint32_t bytes_read = 0;
        CAEN_CHECK(CAEN_DGTZ_ReadData(
            handle, CAEN_DGTZ_SLAVE_TERMINATED_READOUT_MBLT,
            readout_buffer, &bytes_read));
        if (bytes_read == 0U) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }

        uint32_t event_count = 0;
        CAEN_CHECK(CAEN_DGTZ_GetNumEvents(
            handle, readout_buffer, bytes_read, &event_count));
        if (event_count == 0U) continue;
        trigger_drained = true;
        for (uint32_t event_index = 0;
             event_index < event_count && decoded_events < requested_events;
             ++event_index) {
        CAEN_DGTZ_EventInfo_t event_info{};
        char* event_pointer = nullptr;
        CAEN_CHECK(CAEN_DGTZ_GetEventInfo(
            handle, readout_buffer, bytes_read, event_index, &event_info,
            &event_pointer));
        if (event_pointer == nullptr) {
          throw std::runtime_error(
              "Baseline event lookup returned a null event pointer");
        }
        CAEN_CHECK(CAEN_DGTZ_DecodeEvent(
            handle, event_pointer, reinterpret_cast<void**>(&decoded_event)));
        if (decoded_event == nullptr) {
          throw std::runtime_error(
              "Baseline event decode returned a null waveform object");
        }
        if (event_info.ChannelMask != hardware_settings_.channel_mask) {
          throw std::runtime_error(
              "Baseline event channel mask does not match the configured "
              "readout mask");
        }

        for (int ch = 0; ch < MAX_CH; ++ch) {
          if (((hardware_settings_.channel_mask >> ch) & 1U) == 0U ||
              ((event_info.ChannelMask >> ch) & 1U) == 0U) {
            continue;
          }
          const uint32_t trace_size = decoded_event->ChSize[ch];
          const uint16_t* trace = decoded_event->DataChannel[ch];
          if (trace == nullptr || trace_size == 0U ||
              trace_size != hardware_settings_.record_length) {
            throw std::runtime_error(
                "Baseline event has a missing or malformed CH" +
                std::to_string(ch) + " waveform");
          }
          for (uint32_t sample = 0; sample < trace_size; ++sample) {
            const uint16_t code = trace[sample];
            if (code >= adc_codes) {
              throw std::runtime_error(
                  "Baseline waveform contains an ADC code outside the "
                  "configured resolution");
            }
            ++histograms[ch][code];
          }
        }
        ++decoded_events;
        }
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
    baselines[ch] = HistogramMedian(histograms[ch]);
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
    if (measurements.size() <= 10U ||
        measurements.size() % 50U == 0U) {
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
    }

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

void DAQManager::VerifyRuntimeConfiguration(int handle) {
  uint32_t record_length = 0U;
  CAEN_CHECK(CAEN_DGTZ_GetRecordLength(handle, &record_length));
  if (record_length != hardware_settings_.record_length) {
    throw FatalAcquisitionError(
        "Runtime configuration drift: record length changed from " +
        std::to_string(hardware_settings_.record_length) + " to " +
        std::to_string(record_length));
  }

  uint32_t channel_mask = 0U;
  CAEN_CHECK(CAEN_DGTZ_GetChannelEnableMask(handle, &channel_mask));
  if (channel_mask != channel_mask_readback_) {
    throw FatalAcquisitionError(
        "Runtime configuration drift: channel-enable mask changed from " +
        Hex32(channel_mask_readback_) + " to " + Hex32(channel_mask));
  }

  uint32_t post_trigger = 0U;
  CAEN_CHECK(CAEN_DGTZ_GetPostTriggerSize(handle, &post_trigger));
  if (post_trigger != post_trigger_readback_) {
    throw FatalAcquisitionError(
        "Runtime configuration drift: post-trigger percentage changed from " +
        std::to_string(post_trigger_readback_) + " to " +
        std::to_string(post_trigger));
  }

  const uint32_t clock_source =
      (ReadRegister(handle, 0x8100U) & kExternalClockSelectMask) != 0U ? 1U
                                                                      : 0U;
  if (clock_source != clock_source_readback_) {
    throw FatalAcquisitionError(
        "Runtime configuration drift: digitizer clock source changed");
  }

  CAEN_DGTZ_RunSyncMode_t run_sync_mode{};
  CAEN_CHECK(CAEN_DGTZ_GetRunSynchronizationMode(handle, &run_sync_mode));
  if (static_cast<int>(run_sync_mode) != run_sync_mode_readback_) {
    throw FatalAcquisitionError(
        "Runtime configuration drift: run-synchronization mode changed");
  }

  for (int ch = 0; ch < MAX_CH; ++ch) {
    const auto& runtime = runtime_channels_[ch];
    if (!runtime.enabled) continue;
    const uint32_t range =
        ReadRegister(handle, runtime.input_range_register) & kInputRangeMask;
    if (range != runtime.input_range_readback) {
      throw FatalAcquisitionError(
          "Runtime configuration drift: CH" + std::to_string(ch) +
          " input range register changed");
    }
    uint32_t dc_offset = 0U;
    CAEN_CHECK(CAEN_DGTZ_GetChannelDCOffset(handle, ch, &dc_offset));
    if (dc_offset != runtime.readback_dc_offset) {
      throw FatalAcquisitionError(
          "Runtime configuration drift: CH" + std::to_string(ch) +
          " DC offset changed");
    }
    CAEN_DGTZ_TriggerPolarity_t polarity{};
    CAEN_CHECK(CAEN_DGTZ_GetTriggerPolarity(handle, ch, &polarity));
    const int polarity_value =
        polarity == CAEN_DGTZ_TriggerOnFallingEdge ? 1 : 0;
    if (polarity_value != runtime.polarity_readback) {
      throw FatalAcquisitionError(
          "Runtime configuration drift: CH" + std::to_string(ch) +
          " trigger polarity changed");
    }
    if (runtime.threshold_programmed) {
      uint32_t threshold = 0U;
      CAEN_CHECK(CAEN_DGTZ_GetChannelTriggerThreshold(
          handle, ch, &threshold));
      if (threshold != runtime.readback_threshold_adc) {
        throw FatalAcquisitionError(
            "Runtime configuration drift: CH" + std::to_string(ch) +
            " discriminator threshold changed");
      }
    }
  }

  const uint32_t global_trigger = ReadRegister(handle, kGlobalTriggerMaskRegister);
  constexpr uint32_t kAuditedGlobalTriggerFields =
      kPairTriggerRequestMask | kMajorityLevelMask |
      kExternalTriggerEnableMask | kSoftwareTriggerEnableMask;
  if ((global_trigger & kAuditedGlobalTriggerFields) !=
      (global_trigger_mask_readback_ & kAuditedGlobalTriggerFields)) {
    throw FatalAcquisitionError(
        "Runtime configuration drift: global trigger-routing register changed");
  }
  if (hardware_settings_.explicit_trigger_routing) {
    for (int pair = 0; pair < MAX_CH / 2; ++pair) {
      if (((hardware_settings_.self_trigger_mask >> (pair * 2)) & 0x3U) ==
          0U) {
        continue;
      }
      const uint32_t observed =
          ReadRegister(handle,
                       kPairTriggerLogicBase + kPairRegisterStride * pair) &
          kPairLogicFieldMask;
      if (observed != pair_logic_readback_[pair]) {
        throw FatalAcquisitionError(
            "Runtime configuration drift: CH" + std::to_string(pair * 2) +
            "/CH" + std::to_string(pair * 2 + 1) +
            " pair-trigger logic changed");
      }
    }
  }
  ++runtime_configuration_checks_;
}

void DAQManager::VerifyAcquisitionStartCapacity() {
  const uint64_t available = FilesystemFreeBytes(working_output_file_);
  const uint64_t reserve = hardware_settings_.storage.minimum_free_bytes;
  if (available < reserve ||
      expected_raw_bytes_ > available - reserve) {
    throw std::runtime_error(
        "Output-filesystem capacity changed during hardware setup: "
        "available=" + std::to_string(available) +
        " bytes, expected_raw=" + std::to_string(expected_raw_bytes_) +
        " bytes, required_reserve=" + std::to_string(reserve) + " bytes");
  }
  output_free_bytes_at_start_ = available;
  std::cout << "\033[1;36m[Storage Start Check]\033[0m filesystem_free="
            << available << " bytes, expected_raw=" << expected_raw_bytes_
            << " bytes, reserve=" << reserve << " bytes\n";
}

void DAQManager::CheckRuntimeHealthAndStorage(int handle,
                                              bool strict_readback,
                                              bool require_running) {
  ++health_check_count_;
  bool health_read_ok = true;
  float max_temperature = 0.0F;
  for (int ch = 0; ch < MAX_CH; ++ch) {
    if (((hardware_settings_.channel_mask >> ch) & 1U) == 0U) continue;
    uint32_t temperature_register = 0U;
    const uint32_t address = 0x10A8U + kChannelRegisterStride * ch;
    if (CAEN_DGTZ_ReadRegister(handle, address, &temperature_register) !=
        CAEN_DGTZ_Success) {
      health_read_ok = false;
      break;
    }
    const uint32_t temperature_code = temperature_register & 0xFFU;
    const float temperature = static_cast<float>(temperature_code);
    temperature_observed_[ch] = true;
    max_temperature_c_[ch] =
        std::max(max_temperature_c_[ch], temperature_code);
    max_temperature = std::max(max_temperature, temperature);
    if (temperature >= 82.0F) {
      std::cout << "\n[FATAL] OVER_TEMP_SOFT_KILL" << std::endl;
      throw FatalAcquisitionError(
          "Digitizer CH" + std::to_string(ch) +
          " temperature reached the 82 C software shutdown limit");
    }
  }

  uint32_t status_register = 0U;
  if (health_read_ok &&
      CAEN_DGTZ_ReadRegister(handle, kAcquisitionStatusRegister,
                             &status_register) !=
          CAEN_DGTZ_Success) {
    health_read_ok = false;
  }
  if (health_read_ok) {
    consecutive_health_read_errors_ = 0U;
    latest_status_register_ = status_register;
    latest_max_temperature_c_ = max_temperature;
    health_readback_available_ = true;
    if (require_running &&
        (status_register & kAcquisitionRunMask) == 0U) {
      throw FatalAcquisitionError(
          "Digitizer acquisition RUN status dropped while software still "
          "expected the board to be recording");
    }
  } else {
    ++consecutive_health_read_errors_;
    ++health_read_error_count_;
    std::cerr << "\n\033[1;33m[Warning] Runtime health-register read failed "
              << consecutive_health_read_errors_
              << (strict_readback ? "/1" : "/3")
              << "; temperature protection cannot be verified\033[0m\n";
    if (strict_readback || consecutive_health_read_errors_ >= 3U) {
      throw FatalAcquisitionError(
          "Runtime health-register communication failed; stopping because "
          "temperature/status safety cannot be verified");
    }
  }

  const uint64_t filesystem_free =
      FilesystemFreeBytes(working_output_file_);
  if (filesystem_free < hardware_settings_.storage.stop_free_bytes) {
    throw FatalAcquisitionError(
        "Output filesystem reached the configured safety watermark: free=" +
        std::to_string(filesystem_free) + " bytes, stop_free=" +
        std::to_string(hardware_settings_.storage.stop_free_bytes) +
        " bytes");
  }
}

void DAQManager::WaitForAcquisitionRunning(int handle) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(500);
  uint32_t status_register = 0U;
  do {
    CAEN_CHECK(CAEN_DGTZ_ReadRegister(handle, kAcquisitionStatusRegister,
                                      &status_register));
    if ((status_register & kAcquisitionRunMask) != 0U) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  } while (std::chrono::steady_clock::now() < deadline);

  throw FatalAcquisitionError(
      "Digitizer did not assert the acquisition RUN status within 500 ms "
      "after SWStartAcquisition");
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
      acquisition_status_ == "completed" || acquisition_status_ == "failed" ||
      acquisition_status_ == "cancelled";
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
           << "  \"termination_reason\": \""
           << JsonEscape(termination_reason_) << "\",\n"
           << "  \"requested_max_events\": " << max_events_ << ",\n"
           << "  \"requested_run_time_sec\": " << run_time_sec_ << ",\n"
           << "  \"hardware_verified_unix_time\": ";
  if (hardware_verified_unix_time_ != 0) metadata << hardware_verified_unix_time_;
  else metadata << "null";
  metadata << ",\n  \"acquisition_start_unix_time\": ";
  if (acquisition_start_unix_time_ != 0) metadata << acquisition_start_unix_time_;
  else metadata << "null";
  metadata << ",\n  \"acquisition_end_unix_time\": ";
  if (acquisition_end_unix_time_ != 0) metadata << acquisition_end_unix_time_;
  else metadata << "null";
  metadata << ",\n"
           << "  \"recorded_events\": " << recorded_events_ << ",\n"
           << "  \"lost_events\": " << lost_events_ << ",\n"
           << "  \"failure_reason\": ";
  if (failure_reason_.empty()) metadata << "null";
  else metadata << "\"" << JsonEscape(failure_reason_) << "\"";
  metadata << ",\n"
           << "  \"created_unix_time\": " << timestamp << ",\n"
           << "  \"raw_output_path\": \""
           << JsonEscape(raw_output_published_ ? output_file_
                                               : working_output_file_)
           << "\",\n"
           << "  \"requested_raw_output_path\": \""
           << JsonEscape(output_file_) << "\",\n"
           << "  \"raw_output_published\": "
           << (raw_output_published_ ? "true" : "false") << ",\n"
           << "  \"raw_output_finalized\": "
           << (raw_output_finalized_ ? "true" : "false") << ",\n"
           << "  \"raw_finalization_error\": ";
  if (raw_finalization_error_.empty()) metadata << "null";
  else metadata << "\"" << JsonEscape(raw_finalization_error_) << "\"";
  metadata << ",\n"
           << "  \"raw_digest_method\": \""
           << JsonEscape(raw_digest_method_) << "\",\n"
           << "  \"raw_recovery_performed\": "
           << (raw_recovery_performed_ ? "true" : "false") << ",\n"
           << "  \"raw_events_before_recovery\": ";
  if (raw_recovery_performed_) metadata << raw_events_before_recovery_;
  else metadata << "null";
  metadata << ",\n"
           << "  \"lost_events_exact\": "
           << (lost_events_exact_ ? "true" : "false") << ",\n"
           << "  \"raw_format_version\": 1,\n"
           << "  \"raw_event_header_bytes\": " << sizeof(EventHeader)
           << ",\n"
           << "  \"raw_event_bytes\": " << RawEventBytes(hardware_settings_)
           << ",\n"
           << "  \"last_complete_offset\": " << complete_raw_bytes_
           << ",\n"
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
           << "  \"storage\": {\"free_bytes_at_start\": "
           << output_free_bytes_at_start_
           << ", \"free_bytes_at_end\": ";
  if (output_free_bytes_at_end_known_) metadata << output_free_bytes_at_end_;
  else metadata << "null";
  metadata << ", \"expected_raw_bytes\": " << expected_raw_bytes_
           << ", \"minimum_free_bytes\": "
           << hardware_settings_.storage.minimum_free_bytes
           << ", \"stop_free_bytes\": "
           << hardware_settings_.storage.stop_free_bytes << "},\n"
           << "  \"runtime_counters\": {\"readout_errors\": "
           << readout_error_count_ << ", \"health_checks\": "
           << health_check_count_ << ", \"health_read_errors\": "
           << health_read_error_count_ << ", \"zmq_drops\": "
           << zmq_drop_count_
           << ", \"zmq_send_errors\": " << zmq_send_error_count_
           << ", \"zmq_send_hwm_messages\": " << zmq_send_hwm_messages_
           << ", \"zmq_send_hwm_approx_bytes\": "
           << zmq_send_hwm_approx_bytes_
           << ", \"runtime_configuration_checks\": "
           << runtime_configuration_checks_
           << ", \"max_temperature_c\": [";
  for (int ch = 0; ch < MAX_CH; ++ch) {
    if (ch != 0) metadata << ", ";
    if (temperature_observed_[ch]) {
      metadata << max_temperature_c_[ch];
    } else {
      metadata << "null";
    }
  }
  metadata << "]},\n"
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
           << "    \"post_trigger_readback_percent\": "
           << post_trigger_readback_ << ",\n"
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

void DAQManager::FinalizeRawOutput(bool truncate_to_complete_prefix) {
  if (raw_output_finalized_) return;
  if (!output_stream_) {
    throw FatalAcquisitionError(
        "Raw output stream is unavailable during finalization");
  }
  if (raw_output_digest_.SizeBytes() != complete_raw_bytes_) {
    throw FatalAcquisitionError(
        "Streaming raw digest byte count differs from the last complete "
        "event boundary");
  }

  const int stream_descriptor = ::fileno(output_stream_);
  if (stream_descriptor < 0) {
    throw FatalAcquisitionError(
        "Cannot obtain the raw output descriptor during finalization");
  }
  const int finalization_descriptor = ::dup(stream_descriptor);
  if (finalization_descriptor < 0) {
    throw FatalAcquisitionError(
        "Cannot preserve the raw output descriptor during finalization (" +
        std::string(std::strerror(errno)) + ")");
  }

  bool descriptor_open = true;
  try {
    // Always attempt to drain the stdio buffer.  On failure, the descriptor's
    // actual length decides the recoverable boundary; never extend a short
    // file with zeroes to the intended byte count.
    const int flush_result = ::fflush(output_stream_);
    const int close_result = ::fclose(output_stream_);
    output_stream_ = nullptr;

    const bool stream_error = flush_result != 0 || close_result != 0;
    struct stat pre_sync_status {};
    if (::fstat(finalization_descriptor, &pre_sync_status) != 0 ||
        pre_sync_status.st_size < 0 ||
        static_cast<uint64_t>(pre_sync_status.st_dev) != output_device_ ||
        static_cast<uint64_t>(pre_sync_status.st_ino) != output_inode_) {
      throw FatalAcquisitionError(
          "Cannot inspect the raw output after closing its write stream");
    }

    const uint64_t event_bytes = RawEventBytes(hardware_settings_);
    const uint64_t observed_bytes =
        static_cast<uint64_t>(pre_sync_status.st_size);
    const uint64_t bounded_bytes =
        std::min(observed_bytes, complete_raw_bytes_);
    const uint64_t recoverable_bytes =
        bounded_bytes - (bounded_bytes % event_bytes);
    const bool size_recovery_needed =
        observed_bytes != complete_raw_bytes_;
    if (stream_error) {
      raw_recovery_performed_ = true;
      raw_events_before_recovery_ = recorded_events_;
    }
    if (size_recovery_needed) {
      if (recoverable_bytes > observed_bytes) {
        throw FatalAcquisitionError(
            "Internal error: raw recovery would extend the output file");
      }
      if (::ftruncate(finalization_descriptor,
                      static_cast<off_t>(recoverable_bytes)) != 0) {
        throw FatalAcquisitionError(
            "Cannot truncate failed raw output to its last complete event (" +
            std::string(std::strerror(errno)) + ")");
      }
      raw_recovery_performed_ = true;
      raw_events_before_recovery_ = recorded_events_;
      complete_raw_bytes_ = recoverable_bytes;
      recorded_events_ = recoverable_bytes / event_bytes;
      // The running counter may include board-counter gaps observed only in
      // events that could not be made durable.
      lost_events_exact_ = false;
    }

    if (::fsync(finalization_descriptor) != 0) {
      throw FatalAcquisitionError(
          "Raw output sync failed; run is marked failed (" +
          std::string(std::strerror(errno)) + ")");
    }
    // Once writing is complete, make accidental pathname-based modification
    // fail before authenticating and publishing the inode.  The retained
    // failed-run prefix remains readable by the owner/group for salvage.
    if (::fchmod(finalization_descriptor, S_IRUSR | S_IRGRP) != 0 ||
        ::fsync(finalization_descriptor) != 0) {
      throw FatalAcquisitionError(
          "Cannot make the finalized raw inode read-only and durable (" +
          std::string(std::strerror(errno)) + ")");
    }
    if (!PathMatchesIdentity(working_output_file_,
                             {output_device_, output_inode_})) {
      throw FatalAcquisitionError(
          "Raw output path changed during acquisition; run is marked failed");
    }

    struct stat raw_status {};
    if (::fstat(finalization_descriptor, &raw_status) != 0 ||
        raw_status.st_size < 0 ||
        static_cast<uint64_t>(raw_status.st_dev) != output_device_ ||
        static_cast<uint64_t>(raw_status.st_ino) != output_inode_) {
      throw FatalAcquisitionError(
          "Cannot verify the finalized raw output inode and size");
    }
    if (static_cast<uint64_t>(raw_status.st_size) != complete_raw_bytes_) {
      throw FatalAcquisitionError(
          "Finalized raw output size differs from the last complete event "
          "boundary: expected " + std::to_string(complete_raw_bytes_) +
          ", observed " + std::to_string(raw_status.st_size));
    }

    // Authenticate the exact synced inode, not merely the bytes submitted to
    // stdio.  The streaming digest remains an independent in-process check;
    // the descriptor digest also detects same-inode modification through the
    // visible .partial pathname before publication.
    raw_output_size_bytes_ = complete_raw_bytes_;
    struct stat before_hash_status {};
    if (::fstat(finalization_descriptor, &before_hash_status) != 0) {
      throw FatalAcquisitionError(
          "Cannot snapshot raw inode identity before descriptor hashing");
    }
    raw_output_sha256_ = Sha256FileDescriptorHex(
        finalization_descriptor, raw_output_size_bytes_);
    struct stat after_hash_status {};
    if (::fstat(finalization_descriptor, &after_hash_status) != 0 ||
        before_hash_status.st_dev != after_hash_status.st_dev ||
        before_hash_status.st_ino != after_hash_status.st_ino ||
        before_hash_status.st_size != after_hash_status.st_size ||
        before_hash_status.st_mtim.tv_sec !=
            after_hash_status.st_mtim.tv_sec ||
        before_hash_status.st_mtim.tv_nsec !=
            after_hash_status.st_mtim.tv_nsec ||
        before_hash_status.st_ctim.tv_sec !=
            after_hash_status.st_ctim.tv_sec ||
        before_hash_status.st_ctim.tv_nsec !=
            after_hash_status.st_ctim.tv_nsec ||
        !PathMatchesIdentity(working_output_file_,
                             {output_device_, output_inode_})) {
      throw FatalAcquisitionError(
          "Raw inode changed while its descriptor SHA-256 was computed");
    }
    if (raw_output_digest_.SizeBytes() == raw_output_size_bytes_) {
      if (raw_output_sha256_ != raw_output_digest_.FinalHex()) {
        throw FatalAcquisitionError(
            "Final raw descriptor digest differs from the streaming write "
            "digest");
      }
      raw_digest_method_ =
          "streaming_sha256_verified_by_descriptor_sha256";
    } else {
      raw_digest_method_ = "recovered_descriptor_sha256";
    }
    SyncParentDirectory(working_output_file_);
    output_free_bytes_at_end_ = FilesystemFreeBytes(working_output_file_);
    output_free_bytes_at_end_known_ = true;

    if (!truncate_to_complete_prefix && !stream_error &&
        !size_recovery_needed) {
      bool final_link_created = false;
      try {
        LinkDescriptorNoReplace(finalization_descriptor, output_file_,
                                "final raw output");
        final_link_created = true;
        if (!PathMatchesIdentity(output_file_,
                                 {output_device_, output_inode_})) {
          throw FatalAcquisitionError(
              "Published raw output identity differs from the acquired inode");
        }
        SyncParentDirectory(output_file_);
        raw_output_published_ = true;
      } catch (...) {
        if (final_link_created) {
          RemoveIfSameFile(output_file_, {output_device_, output_inode_});
          try {
            SyncParentDirectory(output_file_);
          } catch (...) {
          }
        }
        throw;
      }

      if (!RemoveIfSameFile(working_output_file_,
                            {output_device_, output_inode_})) {
        std::cerr << "[Run Provenance] Final raw output is durable, but the "
                     "partial-name cleanup was unsafe or failed: "
                  << working_output_file_ << "\n";
      } else {
        try {
          SyncParentDirectory(output_file_);
        } catch (const std::exception& cleanup_error) {
          std::cerr << "[Run Provenance] Final raw output is durable, but "
                       "partial-name cleanup sync failed: "
                    << cleanup_error.what() << "\n";
        }
      }
    }

    if (::close(finalization_descriptor) != 0) {
      std::cerr << "[Run Provenance] Raw bytes are synced, but closing the "
                   "duplicate finalization descriptor failed: "
                << std::strerror(errno) << "\n";
    }
    descriptor_open = false;
    raw_output_finalized_ = true;
    raw_finalization_error_.clear();
    std::cout << "\n\033[1;32m[Run Provenance]\033[0m raw_size="
              << raw_output_size_bytes_ << " bytes, raw_sha256="
              << raw_output_sha256_ << ", events=" << recorded_events_
              << ((truncate_to_complete_prefix || stream_error ||
                   size_recovery_needed)
                      ? " (failed-run prefix)"
                      : "")
              << "\n";
    if (!truncate_to_complete_prefix &&
        (stream_error || size_recovery_needed)) {
      throw FatalAcquisitionError(
          "Raw output flush/size verification failed; only the complete "
          "descriptor prefix was recovered and authenticated");
    }
  } catch (...) {
    if (output_stream_) {
      ::fclose(output_stream_);
      output_stream_ = nullptr;
    }
    if (descriptor_open) ::close(finalization_descriptor);
    throw;
  }
}

void DAQManager::Start(std::atomic<bool>& is_running) {
  if (acquisition_status_ != "hardware_verified_not_started") {
    throw std::logic_error(
        "DAQManager::Start can be called exactly once after verified setup");
  }
  std::cout << "\033[1;32m[DAQManager]\033[0m Starting Acquisition...\n";
  std::cout << " - Stop Condition : ";
  if (max_events_ > 0) std::cout << max_events_ << " Events\n";
  else if (run_time_sec_ > 0) std::cout << run_time_sec_ << " Seconds\n";
  else std::cout << "Unlimited (Manual Stop)\n";

  const auto finalize_prestart_cancellation = [&]() {
    termination_reason_ = "cancelled_before_start";
    acquisition_status_ = "cancelled";
    acquisition_end_unix_time_ = std::time(nullptr);
    FinalizeRawOutput(true);
    WriteRuntimeArtifacts();
    std::cout << "\033[1;33m[DAQManager]\033[0m Acquisition was cancelled "
                 "before the hardware start command.\n";
  };
  if (!is_running.load(std::memory_order_relaxed)) {
    finalize_prestart_cancellation();
    return;
  }

  bool acquisition_started = false;
  const auto record_failed_run = [&](const std::string& primary_error) {
    running_ = false;
    if (acquisition_started) {
      CAEN_DGTZ_SWStopAcquisition(digitizer_->GetHandle());
      acquisition_started = false;
    }
    termination_reason_ = "failure";
    acquisition_end_unix_time_ = std::time(nullptr);
    failure_reason_ = primary_error;
    try {
      FinalizeRawOutput(true);
    } catch (const std::exception& finalization_error) {
      raw_finalization_error_ = finalization_error.what();
      failure_reason_ += "; raw prefix finalization also failed: " +
                         raw_finalization_error_;
    }
    acquisition_status_ = "failed";
    try {
      WriteRuntimeArtifacts();
    } catch (const std::exception& metadata_error) {
      std::cerr << "\n[Run Provenance] Cannot record failed status: "
                << metadata_error.what() << "\n";
    }
  };
  try {
    VerifyAcquisitionStartCapacity();
    CheckRuntimeHealthAndStorage(digitizer_->GetHandle(), true, false);
    VerifyRuntimeConfiguration(digitizer_->GetHandle());
    if (!is_running.load(std::memory_order_relaxed)) {
      finalize_prestart_cancellation();
      return;
    }
    CAEN_CHECK(CAEN_DGTZ_SWStartAcquisition(digitizer_->GetHandle()));
    acquisition_started = true;
    WaitForAcquisitionRunning(digitizer_->GetHandle());
    CheckRuntimeHealthAndStorage(digitizer_->GetHandle(), true, true);
    acquisition_start_unix_time_ = std::time(nullptr);
    running_ = true;
    termination_reason_ = "running";
    acquisition_status_ = "running";
    failure_reason_.clear();
    WriteRuntimeArtifacts();
    AcquisitionLoop(is_running);
    acquisition_started = false;
    running_ = false;
    CheckRuntimeHealthAndStorage(digitizer_->GetHandle(), true, false);
    VerifyRuntimeConfiguration(digitizer_->GetHandle());
    acquisition_end_unix_time_ = std::time(nullptr);
    FinalizeRawOutput(false);
    acquisition_status_ = "completed";
    failure_reason_.clear();
    WriteRuntimeArtifacts();
  } catch (const std::exception& error) {
    record_failed_run(error.what());
    throw;
  } catch (...) {
    record_failed_run("unknown non-standard exception");
    throw;
  }
}

void DAQManager::Stop() {
  running_ = false;
}

void DAQManager::AcquisitionLoop(std::atomic<bool>& is_running) {
  EventHeader header{};
  
  int handle = digitizer_->GetHandle();
  char *caen_buffer = digitizer_->GetReadoutBuffer();
  CAEN_DGTZ_UINT16_EVENT_t *caen_event = digitizer_->GetDecodedEvent();
  
  const uint32_t TTT_MASK = 0x7FFFFFFF;
  
  bool is_first_event = true;
  uint32_t first_ttt = 0, current_ttt = 0, prev_ttt = 0, prev_event_counter = 0;
  uint64_t ttt_rollovers = 0;

  auto start_time = std::chrono::steady_clock::now();
  auto last_log_time = start_time;
  auto last_health_check = start_time;
  auto last_configuration_check = start_time;
  uint32_t log_events = 0, zmq_drops = 0, loop_counter = 0;
  uint32_t consecutive_readout_errors = 0;
  size_t total_bytes_written = 0, last_bytes_written = 0; 

  while (is_running && running_) {
    if (max_events_ > 0 &&
        recorded_events_ >= static_cast<uint64_t>(max_events_)) {
      termination_reason_ = "event_limit";
      break;
    }
    if (run_time_sec_ > 0) {
      auto now = std::chrono::steady_clock::now();
      if (std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count() >= run_time_sec_) {
        termination_reason_ = "time_limit";
        break;
      }
    }

    uint32_t bsize = 0; 

    try {
      CAEN_CHECK(CAEN_DGTZ_ReadData(handle, CAEN_DGTZ_SLAVE_TERMINATED_READOUT_MBLT, caen_buffer, &bsize));
      consecutive_readout_errors = 0;
    } catch (const std::exception& e) {
      ++consecutive_readout_errors;
      ++readout_error_count_;
      std::cerr << "\n\033[1;33m[Warning] Readout error "
                << consecutive_readout_errors << "/3: \033[0m"
                << e.what() << "\n";
      if (consecutive_readout_errors >= 3U) {
        CAEN_DGTZ_SWStopAcquisition(handle);
        throw FatalAcquisitionError(
            "Persistent CAEN readout failure after 3 consecutive errors: " +
            std::string(e.what()));
      }
      continue;
    }

    if (bsize == 0U) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    try {
      if (bsize > 0U) {
        uint32_t num_events = 0;
        CAEN_CHECK(CAEN_DGTZ_GetNumEvents(handle, caen_buffer, bsize, &num_events));
        if (num_events == 0U) {
          throw FatalAcquisitionError(
              "CAEN reported a non-empty readout block containing zero events");
        }

        for (uint32_t i = 0; i < num_events; ++i) {
          // A single CAEN transfer can contain many events.  Enforce every
          // stop condition at the event boundary so -n N always produces
          // exactly N complete records and operator stop does not drain a
          // potentially large stale block.
          if (!is_running.load(std::memory_order_relaxed) ||
              !running_.load(std::memory_order_relaxed)) {
            termination_reason_ = "operator_stop";
            break;
          }
          if (max_events_ > 0 &&
              recorded_events_ >= static_cast<uint64_t>(max_events_)) {
            termination_reason_ = "event_limit";
            break;
          }
          if (run_time_sec_ > 0 &&
              std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::steady_clock::now() - start_time).count() >=
                  run_time_sec_) {
            termination_reason_ = "time_limit";
            break;
          }
          CAEN_DGTZ_EventInfo_t evt_info;
          char *evt_ptr = nullptr;
          CAEN_CHECK(CAEN_DGTZ_GetEventInfo(handle, caen_buffer, bsize, i, &evt_info, &evt_ptr));
          if (evt_ptr == nullptr) {
            throw FatalAcquisitionError(
                "CAEN event lookup reported success but returned a null "
                "event pointer");
          }
          CAEN_CHECK(CAEN_DGTZ_DecodeEvent(handle, evt_ptr, (void **)&caen_event));
          if (caen_event == nullptr) {
            throw FatalAcquisitionError(
                "CAEN decode reported success but returned a null event");
          }

          const uint32_t candidate_ttt =
              evt_info.TriggerTimeTag & TTT_MASK;
          const uint32_t current_event_counter =
              evt_info.EventCounter & 0xFFFFFFU;

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
            if (((evt_info.ChannelMask >> ch) & 1U) == 0U) continue;
            if (caen_event->ChSize[ch] != actual_trace_size) {
              throw FatalAcquisitionError(
                  "Decoded event has inconsistent active-channel trace sizes");
            }
            if (caen_event->DataChannel[ch] == nullptr) {
              throw FatalAcquisitionError(
                  "Decoded active CH" + std::to_string(ch) +
                  " waveform pointer is null");
            }
          }

          if (recorded_events_ >
              static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
            throw FatalAcquisitionError(
                "EventID reached the raw-format 32-bit limit; start a new "
                "run before continuing acquisition");
          }
          uint64_t candidate_rollovers = ttt_rollovers;
          uint64_t newly_lost_events = 0U;
          if (!is_first_event) {
            if (candidate_ttt < prev_ttt) ++candidate_rollovers;
            const uint32_t counter_difference =
                (current_event_counter - prev_event_counter) & 0xFFFFFFU;
            if (counter_difference == 0U) {
              throw FatalAcquisitionError(
                  "Digitizer event counter repeated; refusing to record a "
                  "duplicate or stale event");
            }
            if (counter_difference > 0x800000U) {
              throw FatalAcquisitionError(
                  "Digitizer event counter moved backwards or reset during "
                  "acquisition");
            }
            if (counter_difference > 1U) {
              newly_lost_events = counter_difference - 1U;
              if (newly_lost_events >
                  std::numeric_limits<uint64_t>::max() - lost_events_) {
                throw FatalAcquisitionError(
                    "Lost-event counter overflowed uint64_t");
              }
            }
          }

          header = {};
          header.ExtendedTTT =
              (candidate_rollovers << 31) | candidate_ttt;
          header.EventID = static_cast<uint32_t>(recorded_events_);
          header.RecordLength = actual_trace_size;
          header.ChannelMask = evt_info.ChannelMask;
          header.Pattern = evt_info.Pattern;
          header.BoardEventCounter = current_event_counter;
          std::memcpy(raw_buffer_pool_.data(), &header, sizeof(header));

          size_t payload_size = sizeof(EventHeader);
          for (int ch = 0; ch < MAX_CH; ++ch) {
            if ((header.ChannelMask >> ch) & 1U) {
              uint16_t *wave_src = caen_event->DataChannel[ch];
              uint32_t trace_size = caen_event->ChSize[ch];
              if (payload_size + trace_size * sizeof(uint16_t) >
                  raw_buffer_pool_.size()) {
                throw FatalAcquisitionError(
                    "Decoded event exceeds the reserved raw-output buffer");
              }

              std::memcpy(raw_buffer_pool_.data() + payload_size, wave_src,
                          trace_size * sizeof(uint16_t));
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
          raw_output_digest_.Update(raw_buffer_pool_.data(), payload_size);
          complete_raw_bytes_ += static_cast<uint64_t>(payload_size);
          ++recorded_events_;
          if (is_first_event) {
            first_ttt = candidate_ttt;
            is_first_event = false;
          }
          current_ttt = candidate_ttt;
          prev_ttt = candidate_ttt;
          prev_event_counter = current_event_counter;
          ttt_rollovers = candidate_rollovers;
          lost_events_ += newly_lost_events;
          total_bytes_written += payload_size;
          if (zmq_send(zmq_pub_, raw_buffer_pool_.data(), payload_size,
                       ZMQ_DONTWAIT) < 0) {
            const int publish_error = zmq_errno();
            if (publish_error != EAGAIN) {
              ++zmq_send_error_count_;
              if (zmq_send_error_count_ == 1U ||
                  (zmq_send_error_count_ &
                   (zmq_send_error_count_ - 1U)) == 0U) {
                std::cerr
                    << "\n\033[1;33m[Warning] ZeroMQ monitoring publish "
                       "failed; raw acquisition continues independently "
                    << "(count=" << zmq_send_error_count_ << ", error="
                    << zmq_strerror(publish_error) << ")\033[0m\n";
              }
            }
            ++zmq_drops;
            ++zmq_drop_count_;
          }
          log_events++;
        }
      }
    } catch (const FatalAcquisitionError&) {
      CAEN_DGTZ_SWStopAcquisition(handle);
      throw;
    } catch (const std::exception& e) {
      CAEN_DGTZ_SWStopAcquisition(handle);
      throw FatalAcquisitionError(
          "CAEN event-block decoding failed after data was read; the "
          "remaining block was not silently skipped: " +
          std::string(e.what()));
    }

    if (bsize > 0 ||
        std::chrono::steady_clock::now() - last_health_check >=
            std::chrono::milliseconds(250) ||
        ++loop_counter % 10000 == 0) {
        auto now = std::chrono::steady_clock::now();
        if (run_time_sec_ > 0) {
            if (std::chrono::duration_cast<std::chrono::seconds>(
                    now - start_time).count() >= run_time_sec_) {
              termination_reason_ = "time_limit";
              break;
            }
        }

        if (now - last_health_check >= std::chrono::milliseconds(250)) {
            CheckRuntimeHealthAndStorage(handle, false, true);
            last_health_check = now;
        }

        if (now - last_configuration_check >= std::chrono::seconds(1)) {
            VerifyRuntimeConfiguration(handle);
            last_configuration_check = now;
        }

        double elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log_time).count();
        if (elapsed_ms >= 1000.0) {
            auto total_sec = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
            int mins = total_sec / 60;
            int secs = total_sec % 60;

            double rate = (log_events / elapsed_ms) * 1000.0;
            double speed_mbps = ((total_bytes_written - last_bytes_written) / 1048576.0) / (elapsed_ms / 1000.0);
            last_bytes_written = total_bytes_written;

            if (health_readback_available_) {
                const uint32_t status_reg = latest_status_register_;
                int run      = (status_reg >> 0) & 0x1;
                int drdy     = (status_reg >> 2) & 0x1;
                int busy     = (status_reg >> 3) & 0x1;
                int pll_lock = ((status_reg >> 5) & 0x1) == 0 ? 1 : 0;
                int trg      = (rate > 0.0) ? 1 : 0; 
                int pll_byps = 0; 

                std::cout << "[STATUS] TEMP: "
                          << std::fixed << std::setprecision(1)
                          << latest_max_temperature_c_ << std::endl;
                std::cout << "[STATUS] LED: LOCK=" << pll_lock << ", BYPS=" << pll_byps
                          << ", RUN=" << run << ", TRG=" << trg << ", DRDY=" << drdy
                          << ", BUSY=" << busy << std::endl;
            }

            uint32_t record_length = hardware_settings_.record_length;
            uint64_t total_ticks = (ttt_rollovers << 31) + current_ttt - first_ttt;
            
            double hw_real_time_sec = total_ticks * 8e-9; 
            double dead_time_sec = recorded_events_ * (record_length * 2e-9);
            double live_time_sec = hw_real_time_sec - dead_time_sec;
            if (live_time_sec < 0) live_time_sec = 0.0;
            
            double dead_time_pct = (hw_real_time_sec > 0) ? (dead_time_sec / hw_real_time_sec * 100.0) : 0.0;

            std::cout << "\r\033[K\033[1;36m[LIVE DAQ]\033[0m "
                      << "Time: \033[1m" << std::setfill('0') << std::setw(2) << mins << ":" << std::setw(2) << secs << "\033[0m | "
                      << "RealTime: \033[1m" << std::fixed << std::setprecision(2) << hw_real_time_sec << " s\033[0m | "
                      << "Live: \033[1m" << std::fixed << std::setprecision(2) << live_time_sec << " s\033[0m | " 
                      << "DT: \033[1;31m" << std::fixed << std::setprecision(4) << dead_time_pct << " %\033[0m | "
                      << "Rate: \033[1;35m" << std::fixed << std::setprecision(1) << rate << " Hz\033[0m | " 
                      << "Events: \033[1;33m" << recorded_events_ << "\033[0m | "
                      << "Speed: \033[1;32m" << std::fixed << std::setprecision(2) << speed_mbps << " MB/s\033[0m | "
                      << "Drops: " << zmq_drops
                      << std::flush;
              
            log_events = 0; zmq_drops = 0; last_log_time = now;
        }
    }
  }

  if (termination_reason_ == "running") {
    termination_reason_ =
        (!is_running.load(std::memory_order_relaxed) ||
         !running_.load(std::memory_order_relaxed))
            ? "operator_stop"
            : "completed";
  }

  CAEN_CHECK(CAEN_DGTZ_SWStopAcquisition(handle));
  std::cout << "\n\033[1;31m[DAQManager] Stopped Acquisition.\033[0m\n";

  auto t = std::time(nullptr);
  auto tm = *std::localtime(&t);
  
  uint32_t record_length = hardware_settings_.record_length;
  uint64_t final_total_ticks = (ttt_rollovers << 31) + current_ttt - first_ttt;
  
  double final_real_time_sec = final_total_ticks * 8e-9;
  double final_dead_time_sec = recorded_events_ * (record_length * 2e-9);
  double final_live_time_sec = final_real_time_sec - final_dead_time_sec;
  if (final_live_time_sec < 0) final_live_time_sec = 0.0;
  
  double final_dead_time_pct = (final_real_time_sec > 0) ? (final_dead_time_sec / final_real_time_sec * 100.0) : 0.0;
  auto wall_clock_duration = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time).count();
  
  double avg_rate = (final_real_time_sec > 0) ? (recorded_events_ / final_real_time_sec) : 0.0;

  std::cout << "\n\033[1;36m========== [ DAQ Run Summary ] ==========\033[0m\n"
            << " - End Time        : " << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "\n"
            << " - Wall Clock Time : " << wall_clock_duration << " seconds\n"
            << " - HW Real Time    : " << std::fixed << std::setprecision(2) << final_real_time_sec << " seconds\n"
            << " - HW Live Time    : " << std::fixed << std::setprecision(2) << final_live_time_sec << " seconds\n"
            << " - True Dead Time  : " << std::fixed << std::setprecision(5) << final_dead_time_pct << " %\n"
            << " - Total Events    : " << recorded_events_ << " events\n"
            << " - Avg Trig Rate   : " << std::fixed << std::setprecision(2) << avg_rate << " Hz\n" 
            << " - Lost Events     : " << lost_events_ << " events (Buffer Full)\n"
            << " - Data Size Saved : " << std::fixed << std::setprecision(2) << (total_bytes_written / (1024.0 * 1024.0)) << " MB\n"
            << "\033[1;36m=========================================\033[0m\n\n";
}
