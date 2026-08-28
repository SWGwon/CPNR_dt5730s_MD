#include "RawRootFidelity.h"

#include "EventHeader.h"
#include "Sha256.h"
#include "WaveformDsp.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace cpnr {
namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t kMaximumRecordLength = 102400U;
constexpr std::uint16_t kAdcMaximum = 16383U;

struct FileIdentity {
  std::uint64_t device = 0U;
  std::uint64_t inode = 0U;
  std::uint64_t mode = 0U;
  std::uint64_t size = 0U;
  std::int64_t mtime_seconds = 0;
  std::int64_t mtime_nanoseconds = 0;
  std::int64_t ctime_seconds = 0;
  std::int64_t ctime_nanoseconds = 0;
};

FileIdentity IdentityFromStatus(const struct stat& status,
                                const std::string& description) {
  if (!S_ISREG(status.st_mode) || status.st_size < 0) {
    throw std::runtime_error(description + " is not a regular file");
  }
  return {static_cast<std::uint64_t>(status.st_dev),
          static_cast<std::uint64_t>(status.st_ino),
          static_cast<std::uint64_t>(status.st_mode),
          static_cast<std::uint64_t>(status.st_size),
          static_cast<std::int64_t>(status.st_mtim.tv_sec),
          static_cast<std::int64_t>(status.st_mtim.tv_nsec),
          static_cast<std::int64_t>(status.st_ctim.tv_sec),
          static_cast<std::int64_t>(status.st_ctim.tv_nsec)};
}

FileIdentity DescriptorIdentity(int descriptor) {
  struct stat status {};
  if (::fstat(descriptor, &status) != 0) {
    throw std::runtime_error("Cannot fstat pinned RAW descriptor (" +
                             std::string(std::strerror(errno)) + ")");
  }
  return IdentityFromStatus(status, "Pinned RAW descriptor");
}

FileIdentity PathIdentity(const std::string& path) {
  struct stat status {};
  if (::lstat(path.c_str(), &status) != 0) {
    throw std::runtime_error("Cannot lstat resolved RAW path '" + path +
                             "' (" + std::strerror(errno) + ")");
  }
  return IdentityFromStatus(status, "Resolved RAW path '" + path + "'");
}

bool SameIdentity(const FileIdentity& left, const FileIdentity& right) {
  return left.device == right.device && left.inode == right.inode &&
         left.mode == right.mode && left.size == right.size &&
         left.mtime_seconds == right.mtime_seconds &&
         left.mtime_nanoseconds == right.mtime_nanoseconds &&
         left.ctime_seconds == right.ctime_seconds &&
         left.ctime_nanoseconds == right.ctime_nanoseconds;
}

std::string AbsoluteLexicalPath(const std::string& path) {
  std::error_code error;
  const fs::path absolute = fs::absolute(fs::path(path), error);
  if (error) {
    throw std::runtime_error("Cannot resolve RAW path '" + path + "' (" +
                             error.message() + ")");
  }
  return absolute.lexically_normal().string();
}

bool IsLowerHexDigest(const std::string& digest) {
  return digest.size() == 64U &&
         std::all_of(digest.begin(), digest.end(), [](char value) {
           return (value >= '0' && value <= '9') ||
                  (value >= 'a' && value <= 'f');
         });
}

std::uint64_t CheckedMultiply(std::uint64_t left, std::uint64_t right,
                              const std::string& description) {
  if (left != 0U &&
      right > std::numeric_limits<std::uint64_t>::max() / left) {
    throw std::runtime_error(description + " overflows uint64_t");
  }
  return left * right;
}

std::uint64_t CheckedAdd(std::uint64_t left, std::uint64_t right,
                         const std::string& description) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    throw std::runtime_error(description + " overflows uint64_t");
  }
  return left + right;
}

}  // namespace

struct RawRootFidelityVerifier::Impl {
  explicit Impl(const RawRootFidelitySettings& input_settings)
      : settings(input_settings) {
    if (settings.resolved_raw_path.empty() ||
        !fs::path(settings.resolved_raw_path).is_absolute()) {
      throw std::invalid_argument(
          "ResolvedRawInputPath must be a non-empty absolute path");
    }
    if (!IsLowerHexDigest(settings.expected_sha256)) {
      throw std::invalid_argument(
          "Recorded RAW SHA-256 must be 64 lowercase hexadecimal digits");
    }
    if (settings.expected_record_length < 128U ||
        settings.expected_record_length > kMaximumRecordLength ||
        settings.expected_record_length % 8U != 0U) {
      throw std::invalid_argument(
          "Authenticated RAW record length is outside the converter contract");
    }
    if (settings.expected_channel_mask == 0U ||
        (settings.expected_channel_mask & ~0xFFU) != 0U) {
      throw std::invalid_argument(
          "Authenticated RAW channel mask is outside the converter contract");
    }

    result.resolved_raw_path =
        AbsoluteLexicalPath(settings.resolved_raw_path);
    descriptor = ::open(result.resolved_raw_path.c_str(),
                        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) {
      throw std::runtime_error("Cannot open resolved RAW artifact safely '" +
                               result.resolved_raw_path + "' (" +
                               std::strerror(errno) + ")");
    }
    try {
      initial_identity = DescriptorIdentity(descriptor);
      const FileIdentity path_identity = PathIdentity(result.resolved_raw_path);
      if (!SameIdentity(initial_identity, path_identity)) {
        throw std::runtime_error(
            "Resolved RAW path changed while its descriptor was opened");
      }
      if (initial_identity.size != settings.expected_size_bytes) {
        throw std::runtime_error(
            "Resolved RAW size does not match authenticated metadata");
      }

      const std::uint64_t active_channels = static_cast<std::uint64_t>(
          __builtin_popcount(settings.expected_channel_mask));
      const std::uint64_t payload_bytes = CheckedMultiply(
          CheckedMultiply(settings.expected_record_length, active_channels,
                          "RAW samples per event"),
          sizeof(std::uint16_t), "RAW payload bytes per event");
      expected_event_bytes = CheckedAdd(sizeof(EventHeader), payload_bytes,
                                        "RAW bytes per event");
      const std::uint64_t shape_expected_size = CheckedMultiply(
          settings.expected_events, expected_event_bytes,
          "Expected RAW stream size");
      if (shape_expected_size != settings.expected_size_bytes) {
        throw std::runtime_error(
            "Authenticated RAW size is inconsistent with ROOT entries and "
            "the recorded event shape");
      }

      result.raw_size_bytes = initial_identity.size;
      result.authenticated_sha256 = Authenticate();
      if (result.authenticated_sha256 != settings.expected_sha256) {
        throw std::runtime_error(
            "Resolved RAW SHA-256 does not match authenticated metadata");
      }
      CheckStable("while authenticating RAW bytes");
    } catch (...) {
      ::close(descriptor);
      descriptor = -1;
      throw;
    }
  }

  ~Impl() {
    if (descriptor >= 0) ::close(descriptor);
  }

  void CheckCancelled(const std::string& stage) const {
    if (settings.cancelled && settings.cancelled()) {
      throw RawRootFidelityCancelled(stage);
    }
  }

  void ReadExact(std::uint64_t offset, void* destination, std::size_t size,
                 const std::string& description) const {
    std::size_t completed = 0U;
    auto* bytes = static_cast<unsigned char*>(destination);
    while (completed < size) {
      CheckCancelled("reading " + description);
      const std::uint64_t current_offset =
          CheckedAdd(offset, completed, "RAW read offset");
      if (current_offset >
          static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        throw std::runtime_error("RAW read offset exceeds off_t capacity");
      }
      const ssize_t count =
          ::pread(descriptor, bytes + completed, size - completed,
                  static_cast<off_t>(current_offset));
      if (count < 0 && errno == EINTR) continue;
      if (count < 0) {
        throw std::runtime_error("Cannot read " + description + " (" +
                                 std::strerror(errno) + ")");
      }
      if (count == 0) {
        throw std::runtime_error("Truncated RAW while reading " +
                                 description);
      }
      completed += static_cast<std::size_t>(count);
    }
  }

  void ReadExactForComparison(std::uint64_t read_offset, void* destination,
                              std::size_t size,
                              const std::string& description) {
    ReadExact(read_offset, destination, size, description);
    comparison_sha256.Update(destination, size);
  }

  std::string Authenticate() {
    CheckCancelled("authenticating resolved RAW artifact");
    if (settings.authentication_progress) {
      settings.authentication_progress(initial_identity.size == 0U ? 1.0
                                                                    : 0.0);
    }
    Sha256Accumulator accumulator;
    std::array<unsigned char, 1024U * 1024U> buffer{};
    std::uint64_t offset = 0U;
    while (offset < initial_identity.size) {
      CheckCancelled("authenticating resolved RAW artifact");
      const std::size_t count = static_cast<std::size_t>(
          std::min<std::uint64_t>(buffer.size(),
                                  initial_identity.size - offset));
      ReadExact(offset, buffer.data(), count, "RAW authentication block");
      accumulator.Update(buffer.data(), count);
      offset += count;
      if (settings.authentication_progress) {
        settings.authentication_progress(
            static_cast<double>(offset) /
            static_cast<double>(initial_identity.size));
      }
    }
    CheckCancelled("authenticating resolved RAW artifact");
    return accumulator.FinalHex();
  }

  void CheckStable(const std::string& stage) const {
    const FileIdentity descriptor_identity = DescriptorIdentity(descriptor);
    const FileIdentity path_identity = PathIdentity(result.resolved_raw_path);
    if (!SameIdentity(initial_identity, descriptor_identity) ||
        !SameIdentity(initial_identity, path_identity)) {
      throw std::runtime_error("Resolved RAW artifact changed " + stage);
    }
  }

  void CountScalarMismatch(std::size_t channel, double observed,
                           double expected) {
    if (!DspValueApproximatelyEqual(observed, expected)) {
      ++result.scalar_field_mismatches[channel];
    }
  }

  void CompareEvent(const RawRootEventView& root_event) {
    if (finished) {
      throw std::logic_error("RAW fidelity verifier is already finalized");
    }
    CheckCancelled("cross-checking RAW event content");
    if (root_event.entry != result.events_compared) {
      throw std::logic_error(
          "ROOT events must be supplied to the RAW fidelity verifier in "
          "strict entry order");
    }
    if (result.events_compared >= settings.expected_events) {
      throw std::runtime_error("ROOT contains more events than authenticated RAW");
    }
    if (offset > initial_identity.size ||
        expected_event_bytes > initial_identity.size - offset) {
      throw std::runtime_error(
          "Authenticated RAW ends before the next complete event");
    }

    EventHeader header{};
    ReadExactForComparison(offset, &header, sizeof(header), "EventHeader");
    offset += sizeof(header);
    if (header.RecordLength < 128U ||
        header.RecordLength > kMaximumRecordLength ||
        header.RecordLength % 8U != 0U || header.ChannelMask == 0U ||
        (header.ChannelMask & ~0xFFU) != 0U ||
        header.BoardEventCounter > 0xFFFFFFU ||
        header.RecordLength != settings.expected_record_length ||
        header.ChannelMask != settings.expected_channel_mask) {
      throw std::runtime_error(
          "Malformed or metadata-inconsistent EventHeader at RAW event " +
          std::to_string(result.events_compared));
    }

    result.header_field_mismatches +=
        static_cast<std::uint64_t>(header.ExtendedTTT !=
                                   root_event.sync_time_ttt);
    result.header_field_mismatches +=
        static_cast<std::uint64_t>(header.EventID != root_event.event_id);
    result.header_field_mismatches += static_cast<std::uint64_t>(
        header.RecordLength != root_event.record_length);
    result.header_field_mismatches += static_cast<std::uint64_t>(
        header.ChannelMask != root_event.channel_mask);
    result.header_field_mismatches +=
        static_cast<std::uint64_t>(header.Pattern != root_event.pattern);
    result.header_field_mismatches += static_cast<std::uint64_t>(
        header.BoardEventCounter != root_event.board_event_counter);

    raw_waveform.resize(header.RecordLength);
    const std::size_t waveform_bytes =
        static_cast<std::size_t>(header.RecordLength) * sizeof(std::uint16_t);
    for (std::size_t channel = 0U; channel < kRawRootFidelityChannels;
         ++channel) {
      const bool active = ((header.ChannelMask >> channel) & 1U) != 0U;
      if (!active) {
        if (settings.compare_short_charge) {
          CountScalarMismatch(channel, root_event.short_charge[channel], 0.0);
        }
        CountScalarMismatch(channel, root_event.charge[channel], 0.0);
        CountScalarMismatch(channel, root_event.pulse_height[channel], 0.0);
        CountScalarMismatch(channel, root_event.pulse_start_ns[channel], -1.0);
        CountScalarMismatch(channel, root_event.baseline[channel], 0.0);
        if (root_event.waveforms_saved && root_event.waveforms[channel] != nullptr &&
            !root_event.waveforms[channel]->empty()) {
          result.waveform_sample_mismatches[channel] +=
              root_event.waveforms[channel]->size();
        }
        continue;
      }

      ReadExactForComparison(
          offset, raw_waveform.data(), waveform_bytes,
          "CH" + std::to_string(channel) + " waveform");
      offset += waveform_bytes;
      if (std::any_of(raw_waveform.begin(), raw_waveform.end(),
                      [](std::uint16_t sample) {
                        return sample > kAdcMaximum;
                      })) {
        throw std::runtime_error(
            "RAW waveform contains an ADC code outside the 14-bit range at "
            "event " + std::to_string(result.events_compared) + " CH" +
            std::to_string(channel));
      }

      const WaveformDspValues dsp = ComputeWaveformDsp(
          raw_waveform.data(), raw_waveform.size(),
          settings.falling_polarity, settings.waveform_dsp);
      CountScalarMismatch(channel, root_event.baseline[channel], dsp.baseline);
      if (settings.compare_short_charge) {
        CountScalarMismatch(channel, root_event.short_charge[channel],
                            dsp.short_charge);
      }
      CountScalarMismatch(channel, root_event.charge[channel], dsp.charge);
      CountScalarMismatch(channel, root_event.pulse_height[channel],
                          dsp.pulse_height);
      CountScalarMismatch(channel, root_event.pulse_start_ns[channel],
                          dsp.pulse_start_ns);

      if (root_event.waveforms_saved) {
        result.waveforms_compared = true;
        const std::vector<std::uint16_t>* root_waveform =
            root_event.waveforms[channel];
        if (root_waveform == nullptr) {
          result.waveform_sample_mismatches[channel] += raw_waveform.size();
        } else {
          const std::size_t shared =
              std::min(root_waveform->size(), raw_waveform.size());
          for (std::size_t sample = 0U; sample < shared; ++sample) {
            result.waveform_sample_mismatches[channel] +=
                static_cast<std::uint64_t>((*root_waveform)[sample] !=
                                           raw_waveform[sample]);
          }
          const std::size_t size_difference =
              root_waveform->size() > raw_waveform.size()
                  ? root_waveform->size() - raw_waveform.size()
                  : raw_waveform.size() - root_waveform->size();
          result.waveform_sample_mismatches[channel] += size_difference;
        }
        result.waveform_samples_compared += raw_waveform.size();
      }
    }
    ++result.events_compared;
    result.bytes_consumed = offset;
  }

  RawRootFidelityResult Finish() {
    if (finished) {
      throw std::logic_error("RAW fidelity verifier was finalized twice");
    }
    CheckCancelled("finalizing RAW fidelity cross-check");
    finished = true;
    if (result.events_compared != settings.expected_events) {
      throw std::runtime_error(
          "ROOT/RAW event-count cross-check did not consume every event");
    }
    if (offset != initial_identity.size) {
      throw std::runtime_error(
          "Authenticated RAW contains trailing or unconsumed bytes");
    }
    if (comparison_sha256.SizeBytes() != initial_identity.size) {
      throw std::runtime_error(
          "RAW comparison digest did not consume the authenticated byte count");
    }
    result.compared_bytes_sha256 = comparison_sha256.FinalHex();
    if (result.compared_bytes_sha256 != settings.expected_sha256 ||
        result.compared_bytes_sha256 != result.authenticated_sha256) {
      throw std::runtime_error(
          "RAW bytes consumed by comparison differ from the authenticated "
          "SHA-256 content");
    }
    CheckStable("during RAW-to-ROOT fidelity validation");
    result.bytes_consumed = offset;
    return result;
  }

  RawRootFidelitySettings settings;
  int descriptor = -1;
  FileIdentity initial_identity;
  std::uint64_t expected_event_bytes = 0U;
  std::uint64_t offset = 0U;
  bool finished = false;
  std::vector<std::uint16_t> raw_waveform;
  Sha256Accumulator comparison_sha256;
  RawRootFidelityResult result;
};

std::uint64_t RawRootFidelityResult::TotalScalarFieldMismatches() const {
  return std::accumulate(scalar_field_mismatches.begin(),
                         scalar_field_mismatches.end(), std::uint64_t{0});
}

std::uint64_t RawRootFidelityResult::TotalWaveformSampleMismatches() const {
  return std::accumulate(waveform_sample_mismatches.begin(),
                         waveform_sample_mismatches.end(), std::uint64_t{0});
}

bool RawRootFidelityResult::ExactMatch() const {
  return header_field_mismatches == 0U &&
         TotalScalarFieldMismatches() == 0U &&
         TotalWaveformSampleMismatches() == 0U;
}

RawRootFidelityCancelled::RawRootFidelityCancelled(const std::string& stage)
    : std::runtime_error("RAW fidelity validation cancelled while " + stage) {}

RawRootFidelityVerifier::RawRootFidelityVerifier(
    const RawRootFidelitySettings& settings)
    : impl_(std::make_unique<Impl>(settings)) {}

RawRootFidelityVerifier::~RawRootFidelityVerifier() = default;
RawRootFidelityVerifier::RawRootFidelityVerifier(
    RawRootFidelityVerifier&&) noexcept = default;
RawRootFidelityVerifier& RawRootFidelityVerifier::operator=(
    RawRootFidelityVerifier&&) noexcept = default;

void RawRootFidelityVerifier::CompareEvent(
    const RawRootEventView& root_event) {
  if (!impl_) throw std::logic_error("Moved-from RAW fidelity verifier");
  impl_->CompareEvent(root_event);
}

RawRootFidelityResult RawRootFidelityVerifier::Finish() {
  if (!impl_) throw std::logic_error("Moved-from RAW fidelity verifier");
  return impl_->Finish();
}

}  // namespace cpnr
