#ifndef CPNR_DATA_QUALITY_H
#define CPNR_DATA_QUALITY_H

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace cpnr {

struct LostEventPolicy {
  std::uint64_t max_lost_events = 0U;
  double max_lost_fraction = 0.0;
};

struct LostEventPolicyEvaluation {
  std::uint64_t observed_events = 0U;
  std::uint64_t inferred_lost_events = 0U;
  double lost_fraction = 0.0;
  bool count_exceeded = false;
  bool fraction_exceeded = false;

  bool Exceeded() const noexcept {
    return count_exceeded || fraction_exceeded;
  }
};

inline double LostEventFraction(std::uint64_t recorded_events,
                                std::uint64_t lost_events) noexcept {
  const long double accepted = static_cast<long double>(recorded_events) +
                               static_cast<long double>(lost_events);
  return accepted > 0.0L
             ? static_cast<double>(static_cast<long double>(lost_events) /
                                   accepted)
             : 0.0;
}

inline bool LostEventPolicyExceeded(
    std::uint64_t recorded_events, std::uint64_t lost_events,
    const LostEventPolicy& policy) noexcept {
  return lost_events > policy.max_lost_events ||
         LostEventFraction(recorded_events, lost_events) >
             policy.max_lost_fraction;
}

inline LostEventPolicyEvaluation EvaluateLostEventPolicy(
    std::uint64_t observed_events, std::uint64_t inferred_lost_events,
    const LostEventPolicy& policy) noexcept {
  const double fraction =
      LostEventFraction(observed_events, inferred_lost_events);
  return {observed_events,
          inferred_lost_events,
          fraction,
          inferred_lost_events > policy.max_lost_events,
          fraction > policy.max_lost_fraction};
}

// In standard waveform firmware, Board Fail is bit 26 of the second 32-bit
// word in the raw four-word CAEN event header.  CAEN_DGTZ_EventInfo_t::Pattern
// contains the pattern field, not that complete header word, so inspecting
// Pattern cannot reliably detect this hardware-fault flag.
inline constexpr std::size_t kCaenStandardEventHeaderBytes =
    4U * sizeof(std::uint32_t);
inline constexpr std::uint32_t kCaenEventHeaderBoardFailureMask = 1U << 26U;

enum class CaenEventHeaderQuality {
  kOk,
  kTruncated,
  kMalformed,
  kBoardFailure,
};

inline CaenEventHeaderQuality InspectCaenEventHeader(
    const void* event_data, std::size_t event_size_bytes,
    std::uint32_t* second_word = nullptr) noexcept {
  if (event_data == nullptr ||
      event_size_bytes < kCaenStandardEventHeaderBytes) {
    return CaenEventHeaderQuality::kTruncated;
  }
  std::uint32_t raw_first_word = 0U;
  std::uint32_t raw_second_word = 0U;
  std::memcpy(&raw_first_word, event_data, sizeof(raw_first_word));
  std::memcpy(&raw_second_word,
              static_cast<const std::uint8_t*>(event_data) +
                  sizeof(std::uint32_t),
              sizeof(raw_second_word));
  if (second_word != nullptr) *second_word = raw_second_word;
  constexpr std::uint32_t kHeaderTagMask = 0xF0000000U;
  constexpr std::uint32_t kStandardHeaderTag = 0xA0000000U;
  constexpr std::uint32_t kEventSizeWordsMask = 0x0FFFFFFFU;
  const std::size_t size_from_header =
      static_cast<std::size_t>(raw_first_word & kEventSizeWordsMask) *
      sizeof(std::uint32_t);
  if ((raw_first_word & kHeaderTagMask) != kStandardHeaderTag ||
      size_from_header != event_size_bytes) {
    return CaenEventHeaderQuality::kMalformed;
  }
  return (raw_second_word & kCaenEventHeaderBoardFailureMask) != 0U
             ? CaenEventHeaderQuality::kBoardFailure
             : CaenEventHeaderQuality::kOk;
}

}  // namespace cpnr

#endif  // CPNR_DATA_QUALITY_H
