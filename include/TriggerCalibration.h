#ifndef TRIGGER_CALIBRATION_H
#define TRIGGER_CALIBRATION_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

enum class DAQPairLogic : uint32_t;

constexpr std::size_t kTriggerCalibrationChannelCount = 8;
using BaselineMeasurement =
    std::array<double, kTriggerCalibrationChannelCount>;

struct TriggerThresholdCalculation {
  double measured_baseline_adc = 0.0;
  double requested_threshold_mv = 0.0;
  uint32_t delta_adc = 0;
  uint32_t absolute_threshold_adc = 0;
  double effective_threshold_mv = 0.0;
};

// Converts a voltage distance from baseline into ADC codes.  The denominator
// is the number of ADC codes (2^bits), not the maximum code (2^bits - 1).
uint32_t MillivoltsToAdcDelta(double threshold_mv, uint32_t input_range_mv,
                             uint32_t adc_bits);

// trigger_polarity follows CAENDigitizer: 0=rising, 1=falling.
TriggerThresholdCalculation CalculateAbsoluteTriggerThreshold(
    double measured_baseline_adc, double requested_threshold_mv,
    uint32_t input_range_mv, uint32_t adc_bits, int trigger_polarity);

bool BaselinesAreStable(const BaselineMeasurement& previous,
                        const BaselineMeasurement& current,
                        uint32_t channel_mask, double tolerance_adc);

// Returns as soon as the requested number of consecutive measurements is
// stable.  Throws when the supplied measurements never settle.
BaselineMeasurement RequireSettledBaselines(
    const std::vector<BaselineMeasurement>& measurements,
    uint32_t channel_mask, double tolerance_adc,
    uint32_t stable_measurements);

// Value for bits [2:0] of the x730 Self Trigger Logic register (0x1n84).
// pair_bits is the two-bit enable selection for an adjacent channel pair.
uint32_t X730PairTriggerLogicField(uint32_t pair_bits,
                                  DAQPairLogic pair_logic);

#endif  // TRIGGER_CALIBRATION_H
