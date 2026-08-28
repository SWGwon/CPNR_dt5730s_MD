#include "DAQConfig.h"
#include "TriggerCalibration.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "[FAIL] " << message << '\n';
    ++failures;
  }
}

void CheckThrows(const std::function<void()>& action,
                 const std::string& description) {
  try {
    action();
    Check(false, description + ": expected an exception");
  } catch (const std::exception&) {
  } catch (...) {
    Check(false, description + ": unexpected exception type");
  }
}

BaselineMeasurement Baselines(double ch0, double ch1) {
  BaselineMeasurement result{};
  result[0] = ch0;
  result[1] = ch1;
  return result;
}

}  // namespace

int main() {
  Check(MillivoltsToAdcDelta(1.0, 2000, 14) == 8,
        "2 Vpp / 14-bit converts 1 mV to 8 ADC counts");
  Check(MillivoltsToAdcDelta(1.0, 500, 14) == 33,
        "0.5 Vpp / 14-bit converts 1 mV to 33 ADC counts");

  const auto falling_ch0 =
      CalculateAbsoluteTriggerThreshold(16163.0, 1.0, 2000, 14, 1);
  const auto falling_ch1 =
      CalculateAbsoluteTriggerThreshold(16255.0, 1.0, 2000, 14, 1);
  Check(falling_ch0.delta_adc == 8, "falling threshold delta");
  Check(falling_ch0.absolute_threshold_adc == 16155,
        "CH0 threshold uses the CH0 measured baseline");
  Check(falling_ch1.absolute_threshold_adc == 16247,
        "CH1 threshold uses the CH1 measured baseline");
  Check(falling_ch0.absolute_threshold_adc !=
            falling_ch1.absolute_threshold_adc,
        "different baselines produce different absolute thresholds");
  Check(std::abs(falling_ch0.effective_threshold_mv - 0.9765625) < 1e-12,
        "effective voltage reports ADC quantization");

  const auto fractional_falling =
      CalculateAbsoluteTriggerThreshold(16163.5, 1.0, 2000, 14, 1);
  const auto fractional_rising =
      CalculateAbsoluteTriggerThreshold(8191.5, 1.0, 2000, 14, 0);
  const double adc_lsb_mv = 2000.0 / static_cast<double>(1U << 14);
  Check(fractional_falling.absolute_threshold_adc == 16156,
        "falling threshold uses the rounded fractional baseline");
  Check(fractional_rising.absolute_threshold_adc == 8200,
        "rising threshold uses the rounded fractional baseline");
  Check(std::abs(
            fractional_falling.effective_threshold_mv -
            std::abs(16163.5 -
                     fractional_falling.absolute_threshold_adc) * adc_lsb_mv) <
            1e-12,
        "falling effective voltage retains the measured fractional baseline");
  Check(std::abs(
            fractional_rising.effective_threshold_mv -
            std::abs(8191.5 -
                     fractional_rising.absolute_threshold_adc) * adc_lsb_mv) <
            1e-12,
        "rising effective voltage retains the measured fractional baseline");
  Check(std::abs(fractional_falling.effective_threshold_mv -
                 fractional_rising.effective_threshold_mv) > 0.1,
        "effective voltage exposes baseline-rounding direction instead of hiding it");

  const auto rising =
      CalculateAbsoluteTriggerThreshold(8192.0, 1.0, 2000, 14, 0);
  Check(rising.absolute_threshold_adc == 8200,
        "rising edge adds the ADC delta to baseline");
  Check(falling_ch0.absolute_threshold_adc == 16163U - 8U,
        "falling edge subtracts the ADC delta from baseline");

  CheckThrows(
      []() { CalculateAbsoluteTriggerThreshold(3.0, 1.0, 2000, 14, 1); },
      "falling threshold below ADC range fails closed");
  CheckThrows(
      []() {
        CalculateAbsoluteTriggerThreshold(16380.0, 1.0, 2000, 14, 0);
      },
      "rising threshold above ADC range fails closed");
  CheckThrows([]() { MillivoltsToAdcDelta(2000.0, 2000, 14); },
              "unrepresentable threshold delta fails closed");

  const std::vector<BaselineMeasurement> settled_measurements = {
      Baselines(15500.0, 15580.0), Baselines(16000.0, 16090.0),
      Baselines(16163.0, 16254.0), Baselines(16164.0, 16255.0),
      Baselines(16163.5, 16254.5)};
  const auto settled =
      RequireSettledBaselines(settled_measurements, 0x3, 2.0, 3);
  Check(settled[0] == 16163.5 && settled[1] == 16254.5,
        "settling requires stable per-channel measurements");

  const std::vector<BaselineMeasurement> drifting_measurements = {
      Baselines(15500.0, 15580.0), Baselines(15700.0, 15790.0),
      Baselines(15900.0, 15990.0), Baselines(16100.0, 16190.0)};
  CheckThrows(
      [&]() {
        RequireSettledBaselines(drifting_measurements, 0x3, 2.0, 3);
      },
      "baseline settling timeout fails closed");

  Check(X730PairTriggerLogicField(0x3, DAQPairLogic::kAnd) == 0x4,
        "x730 CH0 AND CH1 selects level comparator AND");
  Check(X730PairTriggerLogicField(0x3, DAQPairLogic::kOr) == 0x7,
        "x730 CH0 OR CH1 selects level comparator OR");
  Check(X730PairTriggerLogicField(0x1, DAQPairLogic::kAnd) == 0x5,
        "x730 even-channel-only routing field");
  Check(X730PairTriggerLogicField(0x2, DAQPairLogic::kOr) == 0x6,
        "x730 odd-channel-only routing field");
  CheckThrows(
      []() { X730PairTriggerLogicField(0, DAQPairLogic::kAnd); },
      "empty x730 trigger pair fails closed");

  if (failures != 0) {
    std::cerr << failures << " trigger calibration test(s) failed.\n";
    return 1;
  }
  std::cout << "All trigger calibration tests passed.\n";
  return 0;
}
