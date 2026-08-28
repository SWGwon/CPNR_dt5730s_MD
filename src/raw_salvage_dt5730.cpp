#include "ConfigParser.h"
#include "DAQConfig.h"
#include "EventHeader.h"
#include "RaceSafeCleanup.h"
#include "Sha256.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <getopt.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>

#ifndef CPNR_GIT_COMMIT
#define CPNR_GIT_COMMIT "unknown"
#endif

#ifndef CPNR_BUILD_TIMESTAMP
#define CPNR_BUILD_TIMESTAMP "unknown"
#endif

namespace {

class UniqueFd {
 public:
  UniqueFd() = default;
  explicit UniqueFd(int descriptor) : descriptor_(descriptor) {}
  ~UniqueFd() {
    if (descriptor_ >= 0) ::close(descriptor_);
  }

  UniqueFd(UniqueFd&& other) noexcept : descriptor_(other.Release()) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      if (descriptor_ >= 0) ::close(descriptor_);
      descriptor_ = other.Release();
    }
    return *this;
  }

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  int Get() const noexcept { return descriptor_; }
  explicit operator bool() const noexcept { return descriptor_ >= 0; }

 private:
  int Release() noexcept {
    const int result = descriptor_;
    descriptor_ = -1;
    return result;
  }

  int descriptor_ = -1;
};

struct FileIdentity {
  dev_t device = 0;
  ino_t inode = 0;
  off_t size = 0;
  timespec modified{};
  timespec changed{};
};

struct Options {
  std::string input;
  std::string output;
  std::string config;
};

struct ScanResult {
  uint64_t events = 0;
  uint64_t valid_bytes = 0;
  std::string stop_reason = "clean_end";
  std::string detail;
  std::string source_sha256;
  std::string output_sha256;
};

class CommandLineError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

constexpr std::size_t kLegacyEventHeaderBytes = 24U;
static_assert(sizeof(EventHeader) == kLegacyEventHeaderBytes,
              "The legacy packed EventHeader ABI must remain 24 bytes");

std::string ErrnoText(const std::string& action) {
  return action + ": " + std::strerror(errno);
}

void PrintUsage(std::ostream& output, const char* program) {
  output << "Usage: "
         << (program == nullptr ? "raw_salvage_dt5730" : program)
         << " -i input.dat.partial -o recovered.dat -c runtime.conf\n"
         << "Validates the legacy EventHeader stream and publishes only the "
            "complete, contiguous prefix. The source is never modified.\n";
}

std::string RequireText(const char* value, char option) {
  if (value == nullptr || *value == '\0') {
    throw CommandLineError(std::string("Option -") + option +
                           " requires a non-empty value");
  }
  return value;
}

Options ParseCommandLine(int argc, char** argv) {
  Options options;
  bool saw_input = false;
  bool saw_output = false;
  bool saw_config = false;
  opterr = 0;
  int option = 0;
  while ((option = getopt(argc, argv, ":i:o:c:h")) != -1) {
    switch (option) {
      case 'i':
        if (saw_input) throw CommandLineError("Option -i was specified twice");
        options.input = RequireText(optarg, 'i');
        saw_input = true;
        break;
      case 'o':
        if (saw_output) throw CommandLineError("Option -o was specified twice");
        options.output = RequireText(optarg, 'o');
        saw_output = true;
        break;
      case 'c':
        if (saw_config) throw CommandLineError("Option -c was specified twice");
        options.config = RequireText(optarg, 'c');
        saw_config = true;
        break;
      case 'h':
        PrintUsage(std::cout, argv[0]);
        std::exit(0);
      case ':':
        throw CommandLineError(std::string("Option -") +
                               static_cast<char>(optopt) +
                               " requires an argument");
      case '?':
      default:
        throw CommandLineError(std::string("Unknown option: -") +
                               static_cast<char>(optopt));
    }
  }
  if (optind != argc) {
    throw CommandLineError("Positional arguments are not accepted");
  }
  if (!saw_input || !saw_output || !saw_config) {
    throw CommandLineError("Options -i, -o, and -c are all required");
  }
  return options;
}

std::string AbsolutePath(const std::string& path) {
  return std::filesystem::absolute(path).lexically_normal().string();
}

FileIdentity IdentityFromStat(const struct stat& status) {
  FileIdentity identity;
  identity.device = status.st_dev;
  identity.inode = status.st_ino;
  identity.size = status.st_size;
  identity.modified = status.st_mtim;
  identity.changed = status.st_ctim;
  return identity;
}

bool SameIdentity(const FileIdentity& left, const FileIdentity& right) {
  return left.device == right.device && left.inode == right.inode &&
         left.size == right.size &&
         left.modified.tv_sec == right.modified.tv_sec &&
         left.modified.tv_nsec == right.modified.tv_nsec &&
         left.changed.tv_sec == right.changed.tv_sec &&
         left.changed.tv_nsec == right.changed.tv_nsec;
}

FileIdentity DescriptorIdentity(int descriptor) {
  struct stat status {};
  if (::fstat(descriptor, &status) != 0) {
    throw std::runtime_error(ErrnoText("fstat failed"));
  }
  if (!S_ISREG(status.st_mode)) {
    throw std::runtime_error("Input must be a regular file");
  }
  if (status.st_size < 0) {
    throw std::runtime_error("Input reports a negative file size");
  }
  return IdentityFromStat(status);
}

bool PathMatchesIdentity(const std::string& path,
                         const FileIdentity& expected) {
  struct stat status {};
  if (::lstat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode)) {
    return false;
  }
  return SameIdentity(IdentityFromStat(status), expected);
}

struct NodeIdentity {
  dev_t device = 0;
  ino_t inode = 0;
};

bool PathMatchesNode(const std::string& path,
                     const NodeIdentity& expected) noexcept {
  struct stat status {};
  return ::lstat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) &&
         status.st_dev == expected.device && status.st_ino == expected.inode;
}

bool RemoveIfSameNode(const std::string& path,
                      const NodeIdentity& expected) noexcept {
  return cpnr::RemovePathIfSameNode(path, expected.device, expected.inode,
                                    "[Raw Recovery]");
}

UniqueFd OpenImmutableInput(const std::string& path, FileIdentity* identity) {
  const int descriptor =
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    throw std::runtime_error(ErrnoText("Cannot open input " + path));
  }
  UniqueFd result(descriptor);
  *identity = DescriptorIdentity(descriptor);
  if (!PathMatchesIdentity(path, *identity)) {
    throw std::runtime_error("Input path identity changed while opening: " +
                             path);
  }
  return result;
}

void VerifyImmutableInput(const std::string& label, const std::string& path,
                          int descriptor, const FileIdentity& expected) {
  const FileIdentity current = DescriptorIdentity(descriptor);
  if (!SameIdentity(expected, current) ||
      !PathMatchesIdentity(path, expected)) {
    throw std::runtime_error(label +
                             " changed while recovery was in progress; no "
                             "output was published");
  }
}

std::size_t ReadExact(int descriptor, void* destination, std::size_t size,
                      Sha256Accumulator* digest) {
  auto* bytes = static_cast<unsigned char*>(destination);
  std::size_t total = 0;
  while (total < size) {
    const ssize_t count = ::read(descriptor, bytes + total, size - total);
    if (count > 0) {
      digest->Update(bytes + total, static_cast<std::size_t>(count));
      total += static_cast<std::size_t>(count);
      continue;
    }
    if (count == 0) break;
    if (errno == EINTR) continue;
    throw std::runtime_error(ErrnoText("Input read failed"));
  }
  return total;
}

void DrainInput(int descriptor, Sha256Accumulator* digest) {
  std::vector<unsigned char> buffer(1024U * 1024U);
  for (;;) {
    const ssize_t count = ::read(descriptor, buffer.data(), buffer.size());
    if (count > 0) {
      digest->Update(buffer.data(), static_cast<std::size_t>(count));
      continue;
    }
    if (count == 0) return;
    if (errno == EINTR) continue;
    throw std::runtime_error(ErrnoText("Input read failed"));
  }
}

void WriteAll(int descriptor, const void* source, std::size_t size) {
  const auto* bytes = static_cast<const unsigned char*>(source);
  std::size_t total = 0;
  while (total < size) {
    const ssize_t count = ::write(descriptor, bytes + total, size - total);
    if (count > 0) {
      total += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    throw std::runtime_error(ErrnoText("Output write failed"));
  }
}

std::string JsonEscape(const std::string& value) {
  std::ostringstream output;
  for (const unsigned char character : value) {
    switch (character) {
      case '"': output << "\\\""; break;
      case '\\': output << "\\\\"; break;
      case '\b': output << "\\b"; break;
      case '\f': output << "\\f"; break;
      case '\n': output << "\\n"; break;
      case '\r': output << "\\r"; break;
      case '\t': output << "\\t"; break;
      default:
        if (character < 0x20U) {
          output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                 << static_cast<unsigned int>(character) << std::dec;
        } else {
          output << static_cast<char>(character);
        }
    }
  }
  return output.str();
}

std::string UtcNow() {
  const std::time_t now = std::time(nullptr);
  std::tm utc {};
  if (::gmtime_r(&now, &utc) == nullptr) {
    throw std::runtime_error("Cannot format recovery timestamp");
  }
  char buffer[32]{};
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc) ==
      0) {
    throw std::runtime_error("Cannot format recovery timestamp");
  }
  return buffer;
}

std::string ExecutablePath() {
  std::vector<char> buffer(256U);
  while (buffer.size() <= 1024U * 1024U) {
    const ssize_t size =
        ::readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (size < 0) {
      throw std::runtime_error(ErrnoText("Cannot resolve /proc/self/exe"));
    }
    if (static_cast<std::size_t>(size) < buffer.size()) {
      return std::string(buffer.data(), static_cast<std::size_t>(size));
    }
    buffer.resize(buffer.size() * 2U);
  }
  throw std::runtime_error("Resolved executable path exceeds 1 MiB");
}

bool PathExists(const std::string& path) {
  struct stat status {};
  if (::lstat(path.c_str(), &status) == 0) return true;
  if (errno == ENOENT) return false;
  throw std::runtime_error(ErrnoText("Cannot inspect output path " + path));
}

UniqueFd CreateTemporaryFile(const std::string& final_path,
                             std::string* temporary_path,
                             NodeIdentity* temporary_identity) {
  const std::filesystem::path final(final_path);
  const std::filesystem::path parent =
      final.has_parent_path() ? final.parent_path() : ".";
  std::error_code error;
  if (!std::filesystem::is_directory(parent, error) || error) {
    throw std::runtime_error("Output parent is not an accessible directory: " +
                             parent.string());
  }
  std::string pattern = final_path + ".salvage.tmp.XXXXXX";
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  const int descriptor = ::mkstemp(writable.data());
  if (descriptor < 0) {
    throw std::runtime_error(ErrnoText("Cannot create temporary output"));
  }
  *temporary_path = writable.data();
  struct stat status {};
  if (::fstat(descriptor, &status) != 0) {
    const int saved_errno = errno;
    ::close(descriptor);
    errno = saved_errno;
    throw std::runtime_error(
        ErrnoText("Cannot identify temporary output; preserving " +
                  *temporary_path));
  }
  *temporary_identity = {status.st_dev, status.st_ino};
  if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0 ||
      ::fchmod(descriptor, S_IRUSR | S_IWUSR | S_IRGRP) != 0) {
    const int saved_errno = errno;
    ::close(descriptor);
    if (!RemoveIfSameNode(*temporary_path, *temporary_identity)) {
      std::cerr << "[Raw Recovery] Cannot safely remove unsecured temporary "
                   "output; preserving "
                << *temporary_path << "\n";
    }
    errno = saved_errno;
    throw std::runtime_error(ErrnoText("Cannot secure temporary output"));
  }
  return UniqueFd(descriptor);
}

void SyncParentDirectory(const std::string& path) {
  const std::filesystem::path item(path);
  const std::string parent =
      (item.has_parent_path() ? item.parent_path() : std::filesystem::path("."))
          .string();
  UniqueFd descriptor(
      ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!descriptor) {
    throw std::runtime_error(ErrnoText("Cannot open output directory"));
  }
  if (::fsync(descriptor.Get()) != 0) {
    throw std::runtime_error(ErrnoText("Cannot sync output directory"));
  }
}

std::string ReadAuthenticatedConfig(int descriptor, const std::string& path,
                                    std::string* digest_hex,
                                    const FileIdentity& identity) {
  if (identity.size > 16 * 1024 * 1024) {
    throw std::runtime_error("Configuration file exceeds 16 MiB safety limit");
  }
  std::string contents(static_cast<std::size_t>(identity.size), '\0');
  Sha256Accumulator digest;
  const std::size_t size =
      ReadExact(descriptor, contents.data(), contents.size(), &digest);
  if (size != contents.size()) {
    throw std::runtime_error("Configuration file was truncated while reading");
  }
  VerifyImmutableInput("Configuration file", path, descriptor, identity);
  *digest_hex = digest.FinalHex();
  return contents;
}

uint16_t ReadLe16(const unsigned char* bytes) {
  return static_cast<uint16_t>(bytes[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8U);
}

uint32_t ReadLe32(const unsigned char* bytes) {
  return static_cast<uint32_t>(bytes[0]) |
         (static_cast<uint32_t>(bytes[1]) << 8U) |
         (static_cast<uint32_t>(bytes[2]) << 16U) |
         (static_cast<uint32_t>(bytes[3]) << 24U);
}

uint64_t ReadLe64(const unsigned char* bytes) {
  return static_cast<uint64_t>(ReadLe32(bytes)) |
         (static_cast<uint64_t>(ReadLe32(bytes + 4U)) << 32U);
}

EventHeader DecodeLegacyEventHeader(
    const std::array<unsigned char, kLegacyEventHeaderBytes>& bytes) {
  EventHeader header{};
  header.ExtendedTTT = ReadLe64(bytes.data());
  header.EventID = ReadLe32(bytes.data() + 8U);
  header.RecordLength = ReadLe32(bytes.data() + 12U);
  header.ChannelMask = ReadLe16(bytes.data() + 16U);
  header.Pattern = ReadLe16(bytes.data() + 18U);
  header.BoardEventCounter = ReadLe32(bytes.data() + 20U);
  return header;
}

ScanResult ScanAndCopy(int input_descriptor, int output_descriptor,
                       const DAQHardwareSettings& settings) {
  ScanResult result;
  Sha256Accumulator source_digest;
  Sha256Accumulator output_digest;
  std::vector<unsigned char> waveform;
  const uint32_t adc_limit = uint32_t{1} << settings.adc_bits;

  for (;;) {
    std::array<unsigned char, kLegacyEventHeaderBytes> header_bytes_raw{};
    const std::size_t header_bytes = ReadExact(
        input_descriptor, header_bytes_raw.data(), header_bytes_raw.size(),
        &source_digest);
    if (header_bytes == 0U) {
      result.stop_reason = "clean_end";
      break;
    }
    if (header_bytes != header_bytes_raw.size()) {
      result.stop_reason = "truncated_header";
      result.detail = "incomplete EventHeader after the valid prefix";
      DrainInput(input_descriptor, &source_digest);
      break;
    }
    const EventHeader header = DecodeLegacyEventHeader(header_bytes_raw);

    if (header.EventID != result.events) {
      result.stop_reason = "noncontiguous_event_id";
      result.detail = "expected EventID " + std::to_string(result.events) +
                      ", observed " + std::to_string(header.EventID);
      DrainInput(input_descriptor, &source_digest);
      break;
    }
    if (header.RecordLength != settings.record_length ||
        header.RecordLength < 128U || header.RecordLength > 102400U ||
        header.RecordLength % 8U != 0U ||
        header.ChannelMask != settings.channel_mask ||
        header.ChannelMask == 0U ||
        (header.ChannelMask & ~((uint16_t{1} << MAX_CH) - 1U)) != 0U) {
      result.stop_reason = "invalid_header";
      result.detail = "event " + std::to_string(result.events) +
                      " does not match configured RecordLength/ChannelMask";
      DrainInput(input_descriptor, &source_digest);
      break;
    }

    const uint32_t active_channels =
        static_cast<uint32_t>(__builtin_popcount(
            static_cast<unsigned int>(header.ChannelMask)));
    const uint64_t sample_count =
        static_cast<uint64_t>(header.RecordLength) * active_channels;
    if (sample_count >
        std::numeric_limits<std::size_t>::max() / sizeof(uint16_t)) {
      throw std::runtime_error("Waveform payload size overflows this build");
    }
    const std::size_t payload_bytes =
        static_cast<std::size_t>(sample_count * sizeof(uint16_t));
    waveform.resize(payload_bytes);
    const std::size_t received = ReadExact(input_descriptor, waveform.data(),
                                           payload_bytes, &source_digest);
    if (received != payload_bytes) {
      result.stop_reason = "truncated_payload";
      result.detail = "event " + std::to_string(result.events) +
                      " waveform payload is incomplete";
      DrainInput(input_descriptor, &source_digest);
      break;
    }

    bool adc_valid = true;
    for (std::size_t offset = 0; offset < waveform.size();
         offset += sizeof(uint16_t)) {
      const uint16_t sample = ReadLe16(waveform.data() + offset);
      if (sample >= adc_limit) {
        adc_valid = false;
        result.detail = "event " + std::to_string(result.events) +
                        " contains ADC code " + std::to_string(sample) +
                        " outside the " + std::to_string(settings.adc_bits) +
                        "-bit range";
        break;
      }
    }
    if (!adc_valid) {
      result.stop_reason = "adc_out_of_range";
      DrainInput(input_descriptor, &source_digest);
      break;
    }

    WriteAll(output_descriptor, header_bytes_raw.data(), header_bytes_raw.size());
    WriteAll(output_descriptor, waveform.data(), waveform.size());
    output_digest.Update(header_bytes_raw.data(), header_bytes_raw.size());
    output_digest.Update(waveform.data(), waveform.size());
    ++result.events;
    result.valid_bytes += header_bytes_raw.size() + waveform.size();
  }

  result.source_sha256 = source_digest.FinalHex();
  result.output_sha256 = output_digest.FinalHex();
  return result;
}

std::string RecoveryManifest(const Options& options,
                             const DAQHardwareSettings& settings,
                             const FileIdentity& source_identity,
                             const FileIdentity& config_identity,
                             const std::string& config_sha256,
                             const ScanResult& result,
                             const std::string& executable_path,
                             const std::string& executable_sha256) {
  std::ostringstream json;
  json << "{\n"
       << "  \"schema\": \"cpnr.raw-recovery/v1\",\n"
       << "  \"created_utc\": \"" << UtcNow() << "\",\n"
       << "  \"status\": \""
       << (result.stop_reason == "clean_end" ? "verified_copy"
                                              : "recovered_prefix")
       << "\",\n"
       << "  \"stop_reason\": \"" << JsonEscape(result.stop_reason)
       << "\",\n"
       << "  \"detail\": \"" << JsonEscape(result.detail) << "\",\n"
       << "  \"source_path\": \"" << JsonEscape(AbsolutePath(options.input))
       << "\",\n"
       << "  \"source_size_bytes\": " << source_identity.size << ",\n"
       << "  \"source_device\": "
       << static_cast<unsigned long long>(source_identity.device) << ",\n"
       << "  \"source_inode\": "
       << static_cast<unsigned long long>(source_identity.inode) << ",\n"
       << "  \"source_mtime_seconds\": " << source_identity.modified.tv_sec
       << ",\n"
       << "  \"source_mtime_nanoseconds\": " << source_identity.modified.tv_nsec
       << ",\n"
       << "  \"source_ctime_seconds\": " << source_identity.changed.tv_sec
       << ",\n"
       << "  \"source_ctime_nanoseconds\": " << source_identity.changed.tv_nsec
       << ",\n"
       << "  \"source_sha256\": \"" << result.source_sha256 << "\",\n"
       << "  \"output_path\": \"" << JsonEscape(AbsolutePath(options.output))
       << "\",\n"
       << "  \"output_size_bytes\": " << result.valid_bytes << ",\n"
       << "  \"output_sha256\": \"" << result.output_sha256 << "\",\n"
       << "  \"recovered_events\": " << result.events << ",\n"
       << "  \"last_complete_offset\": " << result.valid_bytes << ",\n"
       << "  \"discarded_tail_bytes\": "
       << (static_cast<uint64_t>(source_identity.size) - result.valid_bytes)
       << ",\n"
       << "  \"event_header_bytes\": " << kLegacyEventHeaderBytes << ",\n"
       << "  \"record_length\": " << settings.record_length << ",\n"
       << "  \"channel_mask\": " << settings.channel_mask << ",\n"
       << "  \"adc_bits\": " << settings.adc_bits << ",\n"
       << "  \"config_path\": \"" << JsonEscape(AbsolutePath(options.config))
       << "\",\n"
       << "  \"config_size_bytes\": " << config_identity.size << ",\n"
       << "  \"config_device\": "
       << static_cast<unsigned long long>(config_identity.device) << ",\n"
       << "  \"config_inode\": "
       << static_cast<unsigned long long>(config_identity.inode) << ",\n"
       << "  \"config_mtime_seconds\": " << config_identity.modified.tv_sec
       << ",\n"
       << "  \"config_mtime_nanoseconds\": " << config_identity.modified.tv_nsec
       << ",\n"
       << "  \"config_ctime_seconds\": " << config_identity.changed.tv_sec
       << ",\n"
       << "  \"config_ctime_nanoseconds\": " << config_identity.changed.tv_nsec
       << ",\n"
       << "  \"config_sha256\": \"" << config_sha256 << "\",\n"
       << "  \"binary_path\": \"" << JsonEscape(executable_path) << "\",\n"
       << "  \"binary_sha256\": \"" << executable_sha256 << "\",\n"
       << "  \"git_commit\": \"" << JsonEscape(CPNR_GIT_COMMIT) << "\",\n"
       << "  \"build_timestamp\": \"" << JsonEscape(CPNR_BUILD_TIMESTAMP)
       << "\"\n"
       << "}\n";
  return json.str();
}

void PublishPair(const std::string& data_temporary,
                 const std::string& data_final,
                 const NodeIdentity& data_identity,
                 const std::string& manifest_temporary,
                 const std::string& manifest_final,
                 const NodeIdentity& manifest_identity,
                 const std::function<void()>& validate_inputs) {
  bool manifest_published = false;
  bool data_published = false;
  try {
    if (::link(manifest_temporary.c_str(), manifest_final.c_str()) != 0) {
      throw std::runtime_error(
          ErrnoText("Cannot publish recovery manifest without overwriting"));
    }
    manifest_published = true;
    if (::link(data_temporary.c_str(), data_final.c_str()) != 0) {
      throw std::runtime_error(
          ErrnoText("Cannot publish recovered data without overwriting"));
    }
    data_published = true;
    if (!PathMatchesNode(manifest_final, manifest_identity) ||
        !PathMatchesNode(data_final, data_identity)) {
      throw std::runtime_error(
          "Recovery publication paths changed before they could be synced");
    }
    validate_inputs();
    SyncParentDirectory(data_final);
  } catch (...) {
    const bool data_removed =
        !data_published || RemoveIfSameNode(data_final, data_identity);
    const bool manifest_removed =
        !manifest_published ||
        RemoveIfSameNode(manifest_final, manifest_identity);
    if (data_published || manifest_published) {
      try {
        SyncParentDirectory(data_final);
      } catch (...) {
      }
    }
    if (!data_removed || !manifest_removed) {
      throw std::runtime_error(
          "Recovery publication failed and rollback refused to unlink a "
          "path whose inode was replaced concurrently");
    }
    throw;
  }
}

int Run(const Options& options) {
  const std::string input_path = AbsolutePath(options.input);
  const std::string output_path = AbsolutePath(options.output);
  const std::string config_path = AbsolutePath(options.config);
  const std::string manifest_path = output_path + ".recovery.json";

  if (PathExists(output_path) || PathExists(manifest_path)) {
    throw std::runtime_error(
        "Refusing to overwrite an existing recovered file or manifest");
  }

  FileIdentity config_identity;
  UniqueFd config_input = OpenImmutableInput(config_path, &config_identity);
  std::string config_sha256;
  const std::string config_contents = ReadAuthenticatedConfig(
      config_input.Get(), config_path, &config_sha256, config_identity);
  const ConfigParser config =
      ConfigParser::FromText(config_contents, config_path);
  const DAQHardwareSettings settings = LoadDAQHardwareSettings(config);

  FileIdentity source_identity;
  UniqueFd input = OpenImmutableInput(input_path, &source_identity);
  if (source_identity.size == 0) {
    throw std::runtime_error("Input contains no complete event");
  }

  std::string data_temporary;
  std::string manifest_temporary;
  NodeIdentity data_temporary_identity;
  NodeIdentity manifest_temporary_identity;
  bool data_identity_known = false;
  bool manifest_identity_known = false;
  UniqueFd output = CreateTemporaryFile(
      output_path, &data_temporary, &data_temporary_identity);
  data_identity_known = true;
  try {
    const ScanResult result =
        ScanAndCopy(input.Get(), output.Get(), settings);
    if (result.events == 0) {
      throw std::runtime_error(
          "Input contains no complete, valid event; no output was published (" +
          result.stop_reason + ")");
    }

    VerifyImmutableInput("Input", input_path, input.Get(), source_identity);
    VerifyImmutableInput("Configuration file", config_path, config_input.Get(),
                         config_identity);
    if (result.valid_bytes > static_cast<uint64_t>(source_identity.size)) {
      throw std::runtime_error("Internal recovery byte-count inconsistency");
    }
    if (::fsync(output.Get()) != 0) {
      throw std::runtime_error(ErrnoText("Cannot sync recovered data"));
    }

    const std::string executable_path = ExecutablePath();
    const std::string executable_sha256 = Sha256FileHex("/proc/self/exe");
    const std::string manifest = RecoveryManifest(
        options, settings, source_identity, config_identity, config_sha256, result,
        executable_path, executable_sha256);
    UniqueFd manifest_output =
        CreateTemporaryFile(manifest_path, &manifest_temporary,
                            &manifest_temporary_identity);
    manifest_identity_known = true;
    WriteAll(manifest_output.Get(), manifest.data(), manifest.size());
    if (::fsync(manifest_output.Get()) != 0) {
      throw std::runtime_error(ErrnoText("Cannot sync recovery manifest"));
    }

    VerifyImmutableInput("Input", input_path, input.Get(), source_identity);
    VerifyImmutableInput("Configuration file", config_path, config_input.Get(),
                         config_identity);
    PublishPair(data_temporary, output_path, data_temporary_identity,
                manifest_temporary, manifest_path, manifest_temporary_identity,
                [&]() {
                  VerifyImmutableInput("Input", input_path, input.Get(),
                                       source_identity);
                  VerifyImmutableInput("Configuration file", config_path,
                                       config_input.Get(), config_identity);
                });
    if (!RemoveIfSameNode(data_temporary, data_temporary_identity)) {
      std::cerr << "[Raw Recovery] Published output, but temporary data-name "
                   "cleanup was unsafe or failed: "
                << data_temporary << "\n";
    }
    data_temporary.clear();
    if (!RemoveIfSameNode(manifest_temporary, manifest_temporary_identity)) {
      std::cerr << "[Raw Recovery] Published output, but temporary manifest-name "
                   "cleanup was unsafe or failed: "
                << manifest_temporary << "\n";
    }
    manifest_temporary.clear();
    SyncParentDirectory(output_path);

    std::cout << "[Raw Recovery] status="
              << (result.stop_reason == "clean_end" ? "verified_copy"
                                                     : "recovered_prefix")
              << " events=" << result.events
              << " bytes=" << result.valid_bytes
              << " stop_reason=" << result.stop_reason
              << " output=" << output_path
              << " manifest=" << manifest_path << "\n";
  } catch (...) {
    if (manifest_identity_known && !manifest_temporary.empty() &&
        !RemoveIfSameNode(manifest_temporary, manifest_temporary_identity)) {
      std::cerr << "[Raw Recovery] Preserving temporary manifest after unsafe "
                   "cleanup: "
                << manifest_temporary << "\n";
    }
    if (data_identity_known && !data_temporary.empty() &&
        !RemoveIfSameNode(data_temporary, data_temporary_identity)) {
      std::cerr << "[Raw Recovery] Preserving temporary data after unsafe "
                   "cleanup: "
                << data_temporary << "\n";
    }
    throw;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = ParseCommandLine(argc, argv);
    return Run(options);
  } catch (const CommandLineError& error) {
    std::cerr << "[Argument Error] " << error.what() << "\n";
    PrintUsage(std::cerr, argc > 0 ? argv[0] : nullptr);
    return 2;
  } catch (const std::exception& error) {
    std::cerr << "[Recovery Error] " << error.what() << "\n";
    return 1;
  }
}
