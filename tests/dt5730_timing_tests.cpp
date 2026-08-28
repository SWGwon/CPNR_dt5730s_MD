#include "DT5730Timing.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

void RequireNear(double actual, double expected, double tolerance,
                 const char* message) {
  if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
    std::cerr << message << ": actual=" << actual
              << ", expected=" << expected << '\n';
    std::exit(1);
  }
}

}  // namespace

int main() {
  static_assert(dt5730_timing::kTriggerTimeTagBits == 31U);
  static_assert(dt5730_timing::kTriggerTimeTagRawLsbNs == 8.0);
  static_assert(dt5730_timing::kTriggerTimeTagObservableResolutionNs == 16.0);
  static_assert(dt5730_timing::kAdcSamplePeriodNs == 2.0);

  // Observable x730 values advance in two raw 8 ns counts.
  RequireNear(dt5730_timing::ElapsedSeconds(11U, 13U), 16.0e-9, 1.0e-20,
              "two raw counts must represent the 16 ns resolution");
  // The 31-bit raw counter wraps after about 17.18 seconds, not 34.36.
  RequireNear(dt5730_timing::ElapsedSeconds(0U, (1ULL << 31U) - 1U),
              17.179869176, 1.0e-9,
              "31-bit raw counter span must use the 8 ns LSB");

  const double elapsed = dt5730_timing::ElapsedSeconds(1000U, 101000U);
  const double windows =
      dt5730_timing::RecordedWindowSumSeconds(100U, 512U);
  RequireNear(elapsed, 800.0e-6, 1.0e-15, "elapsed time conversion");
  RequireNear(windows, 102.4e-6, 1.0e-15,
              "record-window sum conversion");
  RequireNear(dt5730_timing::RecordedWindowToElapsedPercent(windows, elapsed),
              12.8, 1.0e-12, "record-window ratio");
  RequireNear(dt5730_timing::AverageRecordedEventRateHz(100U, elapsed),
              125000.0, 1.0e-8, "recorded-event rate");

  // A sum of record windows is allowed to exceed elapsed time when acquisition
  // windows overlap; this proves it is not a hardware dead-time measurement.
  const double overlap_windows =
      dt5730_timing::RecordedWindowSumSeconds(1000U, 102400U);
  if (dt5730_timing::RecordedWindowToElapsedPercent(overlap_windows, elapsed) <=
      100.0) {
    std::cerr << "overlapping record windows must not be clamped or called "
                 "dead time\n";
    return 1;
  }
  RequireNear(dt5730_timing::ElapsedSeconds(5U, 5U), 0.0, 0.0,
              "zero interval");
  RequireNear(dt5730_timing::AverageRecordedEventRateHz(1U, 0.0), 0.0, 0.0,
              "rate without elapsed interval");

  const auto forward = dt5730_timing::ExtendTriggerTimeTag(
      101U, 103U, 4U, 0.001);
  if (forward.status !=
          dt5730_timing::TriggerTimeTagExtensionStatus::kOk ||
      forward.rolled_over || forward.rollover_count != 4U ||
      forward.extended_value != ((4ULL << 31U) | 103U)) {
    std::cerr << "forward TTT extension failed\n";
    return 1;
  }

  const auto repeated = dt5730_timing::ExtendTriggerTimeTag(
      101U, 101U, 4U, 0.001);
  if (repeated.status !=
      dt5730_timing::TriggerTimeTagExtensionStatus::kRepeated) {
    std::cerr << "repeated TTT must be rejected\n";
    return 1;
  }

  // A raw backward transition is one valid rollover only while the supplied
  // observation-gap upper bound proves that two counter cycles are impossible.
  const auto rollover = dt5730_timing::ExtendTriggerTimeTag(
      dt5730_timing::kTriggerTimeTagMask - 1U, 0U, 0U, 0.001);
  if (rollover.status !=
          dt5730_timing::TriggerTimeTagExtensionStatus::kOk ||
      !rollover.rolled_over || rollover.rollover_count != 1U ||
      rollover.extended_value != (1ULL << 31U)) {
    std::cerr << "single-wrap backward TTT extension failed\n";
    return 1;
  }

  const double immediately_below_full_scale = std::nextafter(
      dt5730_timing::kTriggerTimeTagFullScaleSeconds, 0.0);
  if (dt5730_timing::TriggerTimeTagWrapCountAmbiguous(
          immediately_below_full_scale) ||
      !dt5730_timing::TriggerTimeTagWrapCountAmbiguous(
          dt5730_timing::kTriggerTimeTagFullScaleSeconds) ||
      !dt5730_timing::TriggerTimeTagWrapCountAmbiguous(-1.0) ||
      !dt5730_timing::TriggerTimeTagWrapCountAmbiguous(
          std::numeric_limits<double>::infinity()) ||
      !dt5730_timing::TriggerTimeTagWrapCountAmbiguous(
          std::numeric_limits<double>::quiet_NaN())) {
    std::cerr << "TTT multi-wrap ambiguity boundary failed\n";
    return 1;
  }
  const auto ambiguous = dt5730_timing::ExtendTriggerTimeTag(
      100U, 102U, 0U,
      dt5730_timing::kTriggerTimeTagFullScaleSeconds);
  if (ambiguous.status !=
      dt5730_timing::TriggerTimeTagExtensionStatus::kWrapCountAmbiguous) {
    std::cerr << "ambiguous wrap count must fail without a 17-second sleep\n";
    return 1;
  }

  const std::uint64_t maximum_rollover_count =
      std::numeric_limits<std::uint64_t>::max() >>
      dt5730_timing::kTriggerTimeTagBits;
  const auto overflow = dt5730_timing::ExtendTriggerTimeTag(
      dt5730_timing::kTriggerTimeTagMask, 0U, maximum_rollover_count, 0.001);
  if (overflow.status !=
      dt5730_timing::TriggerTimeTagExtensionStatus::kOverflow) {
    std::cerr << "TTT extension overflow must fail closed\n";
    return 1;
  }

  std::cout << "DT5730 timing tests passed\n";
  return 0;
}
