#ifndef DT5730_TIMING_H
#define DT5730_TIMING_H

#include <cmath>
#include <cstdint>
#include <limits>

namespace dt5730_timing {

// Standard waveform firmware exposes a 31-bit TriggerTimeTag counter whose
// raw count unit is 8 ns.  On the x730 family the value is sampled every two
// counter ticks, so observable values are quantized at 16 ns.  Keeping these
// two concepts separate avoids the tempting, but incorrect, conversion
// TriggerTimeTag * 16 ns (which would double elapsed time).
inline constexpr std::uint32_t kTriggerTimeTagBits = 31U;
inline constexpr double kTriggerTimeTagRawLsbNs = 8.0;
inline constexpr double kTriggerTimeTagObservableResolutionNs = 16.0;
inline constexpr double kAdcSamplePeriodNs = 2.0;
inline constexpr double kNanosecondsPerSecond = 1.0e9;
inline constexpr double kTriggerTimeTagFullScaleSeconds =
    static_cast<double>(std::uint64_t{1} << kTriggerTimeTagBits) *
    kTriggerTimeTagRawLsbNs / kNanosecondsPerSecond;
inline constexpr std::uint32_t kTriggerTimeTagMask =
    (std::uint32_t{1} << kTriggerTimeTagBits) - 1U;

enum class TriggerTimeTagExtensionStatus {
  kOk,
  kRepeated,
  kWrapCountAmbiguous,
  kOverflow,
};

struct TriggerTimeTagExtension {
  TriggerTimeTagExtensionStatus status = TriggerTimeTagExtensionStatus::kOk;
  std::uint64_t extended_value = 0U;
  std::uint64_t rollover_count = 0U;
  bool rolled_over = false;
};

// The 31-bit tag alone cannot distinguish one wrap from multiple wraps.  A
// known upper bound on the interval between observations must be shorter than
// the raw-counter full scale before a single-wrap extension is defensible.
// Non-finite/negative intervals are invalid bounds and therefore ambiguous.
inline bool TriggerTimeTagWrapCountAmbiguous(
    double observation_gap_upper_bound_seconds) noexcept {
  return !std::isfinite(observation_gap_upper_bound_seconds) ||
         observation_gap_upper_bound_seconds < 0.0 ||
         observation_gap_upper_bound_seconds >=
             kTriggerTimeTagFullScaleSeconds;
}

inline TriggerTimeTagExtension ExtendTriggerTimeTag(
    std::uint32_t previous_raw_ttt, std::uint32_t current_raw_ttt,
    std::uint64_t previous_rollover_count,
    double observation_gap_upper_bound_seconds) noexcept {
  previous_raw_ttt &= kTriggerTimeTagMask;
  current_raw_ttt &= kTriggerTimeTagMask;
  if (TriggerTimeTagWrapCountAmbiguous(
          observation_gap_upper_bound_seconds)) {
    return {TriggerTimeTagExtensionStatus::kWrapCountAmbiguous, 0U,
            previous_rollover_count, false};
  }
  if (current_raw_ttt == previous_raw_ttt) {
    return {TriggerTimeTagExtensionStatus::kRepeated, 0U,
            previous_rollover_count, false};
  }

  const bool rolled_over = current_raw_ttt < previous_raw_ttt;
  std::uint64_t rollover_count = previous_rollover_count;
  const std::uint64_t maximum_rollover_count =
      std::numeric_limits<std::uint64_t>::max() >> kTriggerTimeTagBits;
  if (rolled_over) {
    if (rollover_count >= maximum_rollover_count) {
      return {TriggerTimeTagExtensionStatus::kOverflow, 0U,
              previous_rollover_count, false};
    }
    ++rollover_count;
  }
  return {TriggerTimeTagExtensionStatus::kOk,
          (rollover_count << kTriggerTimeTagBits) | current_raw_ttt,
          rollover_count, rolled_over};
}

inline double ElapsedSeconds(std::uint64_t first_extended_ttt,
                             std::uint64_t last_extended_ttt) {
  if (last_extended_ttt <= first_extended_ttt) return 0.0;
  return static_cast<double>(last_extended_ttt - first_extended_ttt) *
         (kTriggerTimeTagRawLsbNs / kNanosecondsPerSecond);
}

inline double RecordedWindowSumSeconds(std::uint64_t recorded_events,
                                       std::uint32_t record_length) {
  return static_cast<double>(recorded_events) *
         static_cast<double>(record_length) *
         (kAdcSamplePeriodNs / kNanosecondsPerSecond);
}

inline double RecordedWindowToElapsedPercent(double recorded_window_sum_sec,
                                             double elapsed_sec) {
  return elapsed_sec > 0.0
             ? 100.0 * recorded_window_sum_sec / elapsed_sec
             : 0.0;
}

inline double AverageRecordedEventRateHz(std::uint64_t recorded_events,
                                         double elapsed_sec) {
  return elapsed_sec > 0.0
             ? static_cast<double>(recorded_events) / elapsed_sec
             : 0.0;
}

}  // namespace dt5730_timing

#endif  // DT5730_TIMING_H
