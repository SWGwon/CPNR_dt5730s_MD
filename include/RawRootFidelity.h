#ifndef CPNR_RAW_ROOT_FIDELITY_H
#define CPNR_RAW_ROOT_FIDELITY_H

#include "WaveformDsp.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace cpnr {

constexpr std::size_t kRawRootFidelityChannels = 8U;

struct RawRootFidelitySettings {
  // This must be the authenticated current locator embedded as
  // ResolvedRawInputPath, never the possibly-stale original acquisition path.
  std::string resolved_raw_path;
  std::uint64_t expected_size_bytes = 0U;
  std::string expected_sha256;
  std::uint64_t expected_events = 0U;
  std::uint32_t expected_record_length = 0U;
  std::uint16_t expected_channel_mask = 0U;
  bool falling_polarity = true;
  WaveformDspSettings waveform_dsp{};
  bool compare_short_charge = false;
  std::function<bool()> cancelled;
  std::function<void(double)> authentication_progress;
};

struct RawRootEventView {
  std::uint64_t entry = 0U;
  std::uint64_t sync_time_ttt = 0U;
  std::uint32_t event_id = 0U;
  std::uint32_t record_length = 0U;
  std::uint16_t channel_mask = 0U;
  std::uint16_t pattern = 0U;
  std::uint32_t board_event_counter = 0U;
  std::array<double, kRawRootFidelityChannels> charge{};
  std::array<double, kRawRootFidelityChannels> short_charge{};
  std::array<double, kRawRootFidelityChannels> pulse_height{};
  std::array<double, kRawRootFidelityChannels> pulse_start_ns{};
  std::array<double, kRawRootFidelityChannels> baseline{};
  std::array<const std::vector<std::uint16_t>*,
             kRawRootFidelityChannels>
      waveforms{};
  bool waveforms_saved = false;
};

struct RawRootFidelityResult {
  std::string resolved_raw_path;
  std::string authenticated_sha256;
  // SHA-256 accumulated from the exact header/payload bytes consumed by the
  // comparison pass. It must independently match authenticated_sha256.
  std::string compared_bytes_sha256;
  std::uint64_t raw_size_bytes = 0U;
  std::uint64_t bytes_consumed = 0U;
  std::uint64_t events_compared = 0U;
  std::uint64_t header_field_mismatches = 0U;
  std::array<std::uint64_t, kRawRootFidelityChannels>
      scalar_field_mismatches{};
  std::array<std::uint64_t, kRawRootFidelityChannels>
      waveform_sample_mismatches{};
  std::uint64_t waveform_samples_compared = 0U;
  bool waveforms_compared = false;

  std::uint64_t TotalScalarFieldMismatches() const;
  std::uint64_t TotalWaveformSampleMismatches() const;
  bool ExactMatch() const;
};

class RawRootFidelityCancelled final : public std::runtime_error {
 public:
  explicit RawRootFidelityCancelled(const std::string& stage);
};

// Opens and pins the resolved RAW inode with O_NOFOLLOW, authenticates every
// byte against the recorded SHA-256, then consumes exactly one raw event for
// each CompareEvent call. Finish() proves exact EOF and descriptor/path
// identity stability. Any malformed/truncated/changed RAW condition throws.
class RawRootFidelityVerifier {
 public:
  explicit RawRootFidelityVerifier(const RawRootFidelitySettings& settings);
  ~RawRootFidelityVerifier();

  RawRootFidelityVerifier(const RawRootFidelityVerifier&) = delete;
  RawRootFidelityVerifier& operator=(const RawRootFidelityVerifier&) = delete;
  RawRootFidelityVerifier(RawRootFidelityVerifier&&) noexcept;
  RawRootFidelityVerifier& operator=(RawRootFidelityVerifier&&) noexcept;

  void CompareEvent(const RawRootEventView& root_event);
  RawRootFidelityResult Finish();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cpnr

#endif  // CPNR_RAW_ROOT_FIDELITY_H
