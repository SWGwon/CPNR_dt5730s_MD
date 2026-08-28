#include "TriggerCalibration.h"

#include "DAQConfig.h"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace {

constexpr uint32_t kPairAnd = 0;
constexpr uint32_t kEvenChannelOnly = 1;
constexpr uint32_t kOddChannelOnly = 2;
constexpr uint32_t kPairOr = 3;
constexpr uint32_t kUseOverThresholdSignal = 1U << 2;

uint32_t AdcCodeCount(uint32_t adc_bits) {
  if (adc_bits == 0 || adc_bits >= 32) {
    throw std::invalid_argument("ADC bits must be in the range 1..31");
  }
  return 1U << adc_bits;
}

void ValidateBaselineMeasurement(const BaselineMeasurement& measurement,
                                 uint32_t channel_mask) {
  for (std::size_t ch = 0; ch < measurement.size(); ++ch) {
    if (((channel_mask >> ch) & 1U) != 0U &&
        !std::isfinite(measurement[ch])) {
      throw std::invalid_argument(
          "Active-channel baseline measurement must be finite");
    }
  }
}

}  // namespace

uint32_t MillivoltsToAdcDelta(double threshold_mv, uint32_t input_range_mv,
                             uint32_t adc_bits) {
  if (!std::isfinite(threshold_mv) || threshold_mv <= 0.0) {
    throw std::invalid_argument("Threshold millivolts must be finite and positive");
  }
  if (input_range_mv == 0U) {
    throw std::invalid_argument("Input range must be positive");
  }

  const uint32_t code_count = AdcCodeCount(adc_bits);
  const double adc_lsb_mv =
      static_cast<double>(input_range_mv) / static_cast<double>(code_count);
  const long long rounded = std::llround(threshold_mv / adc_lsb_mv);
  if (rounded <= 0 || rounded >= static_cast<long long>(code_count)) {
    throw std::out_of_range(
        "Requested threshold is outside the representable ADC delta range");
  }
  return static_cast<uint32_t>(rounded);
}

TriggerThresholdCalculation CalculateAbsoluteTriggerThreshold(
    double measured_baseline_adc, double requested_threshold_mv,
    uint32_t input_range_mv, uint32_t adc_bits, int trigger_polarity) {
  const uint32_t code_count = AdcCodeCount(adc_bits);
  const uint32_t max_code = code_count - 1U;
  if (!std::isfinite(measured_baseline_adc) || measured_baseline_adc < 0.0 ||
      measured_baseline_adc > static_cast<double>(max_code)) {
    throw std::out_of_range("Measured baseline is outside the ADC range");
  }
  if (trigger_polarity != 0 && trigger_polarity != 1) {
    throw std::invalid_argument("Trigger polarity must be 0 (rising) or 1 (falling)");
  }

  TriggerThresholdCalculation result;
  result.measured_baseline_adc = measured_baseline_adc;
  result.requested_threshold_mv = requested_threshold_mv;
  result.delta_adc =
      MillivoltsToAdcDelta(requested_threshold_mv, input_range_mv, adc_bits);

  const long long rounded_baseline = std::llround(measured_baseline_adc);
  const long long absolute =
      trigger_polarity == 1
          ? rounded_baseline - static_cast<long long>(result.delta_adc)
          : rounded_baseline + static_cast<long long>(result.delta_adc);
  if (absolute < 0 || absolute > static_cast<long long>(max_code)) {
    throw std::out_of_range(
        "Baseline-relative trigger threshold is outside the ADC range");
  }

  result.absolute_threshold_adc = static_cast<uint32_t>(absolute);
  result.effective_threshold_mv =
      std::abs(measured_baseline_adc - static_cast<double>(absolute)) *
      (static_cast<double>(input_range_mv) / static_cast<double>(code_count));
  return result;
}

bool BaselinesAreStable(const BaselineMeasurement& previous,
                        const BaselineMeasurement& current,
                        uint32_t channel_mask, double tolerance_adc) {
  if (channel_mask == 0U ||
      (channel_mask >> kTriggerCalibrationChannelCount) != 0U) {
    throw std::invalid_argument("Baseline channel mask is empty or out of range");
  }
  if (!std::isfinite(tolerance_adc) || tolerance_adc < 0.0) {
    throw std::invalid_argument("Baseline tolerance must be finite and non-negative");
  }
  ValidateBaselineMeasurement(previous, channel_mask);
  ValidateBaselineMeasurement(current, channel_mask);

  for (std::size_t ch = 0; ch < previous.size(); ++ch) {
    if (((channel_mask >> ch) & 1U) != 0U &&
        std::abs(current[ch] - previous[ch]) > tolerance_adc) {
      return false;
    }
  }
  return true;
}

BaselineMeasurement RequireSettledBaselines(
    const std::vector<BaselineMeasurement>& measurements,
    uint32_t channel_mask, double tolerance_adc,
    uint32_t stable_measurements) {
  if (stable_measurements < 2U) {
    throw std::invalid_argument("At least two stable baseline measurements are required");
  }
  if (measurements.empty()) {
    throw std::runtime_error("Baseline settling failed: no measurements");
  }

  ValidateBaselineMeasurement(measurements.front(), channel_mask);
  uint32_t consecutive = 1U;
  for (std::size_t index = 1; index < measurements.size(); ++index) {
    if (BaselinesAreStable(measurements[index - 1], measurements[index],
                           channel_mask, tolerance_adc)) {
      ++consecutive;
      if (consecutive >= stable_measurements) return measurements[index];
    } else {
      consecutive = 1U;
    }
  }
  throw std::runtime_error(
      "Baseline settling failed before the configured timeout");
}

uint32_t X730PairTriggerLogicField(uint32_t pair_bits,
                                  DAQPairLogic pair_logic) {
  switch (pair_bits) {
    case 0x1U:
      return kUseOverThresholdSignal | kEvenChannelOnly;
    case 0x2U:
      return kUseOverThresholdSignal | kOddChannelOnly;
    case 0x3U:
      return kUseOverThresholdSignal |
             (pair_logic == DAQPairLogic::kAnd ? kPairAnd : kPairOr);
    default:
      throw std::invalid_argument(
          "x730 pair selection must enable the even, odd, or both channels");
  }
}
