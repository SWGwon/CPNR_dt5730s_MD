#include "WaveformDsp.h"

#include <cmath>
#include <cstdint>
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

void CheckNear(double observed, double expected,
               const std::string& message) {
  Check(std::abs(observed - expected) < 1e-12, message);
}

cpnr::WaveformDspSettings Settings(std::size_t baseline,
                                   std::size_t short_gate,
                                   std::size_t long_gate,
                                   double threshold = 5.0) {
  cpnr::WaveformDspSettings settings;
  settings.baseline_samples = baseline;
  settings.short_gate_samples = short_gate;
  settings.long_gate_samples = long_gate;
  settings.pulse_start_threshold_adc = threshold;
  return settings;
}

}  // namespace

int main() {
  std::vector<std::uint16_t> falling(128U, 1000U);
  for (std::size_t sample = 64U; sample < 96U; ++sample) {
    falling[sample] = 990U;
  }
  const auto falling_result = cpnr::ComputeWaveformDsp(
      falling.data(), falling.size(), true, Settings(32U, 10U, 20U));
  CheckNear(falling_result.baseline, 1000.0,
            "falling baseline uses configured samples");
  CheckNear(falling_result.short_charge, 100.0,
            "ShortGate controls the short integral");
  CheckNear(falling_result.charge, 200.0,
            "LongGate controls the long integral");
  CheckNear(falling_result.pulse_height, 10.0,
            "falling pulse height has positive directed amplitude");
  CheckNear(falling_result.pulse_start_ns, 128.0,
            "falling T0 uses the 2 ns ADC period");

  std::vector<std::uint16_t> rising(128U, 1000U);
  for (std::size_t sample = 64U; sample < 96U; ++sample) {
    rising[sample] = 1010U;
  }
  const auto rising_result = cpnr::ComputeWaveformDsp(
      rising.data(), rising.size(), false, Settings(32U, 10U, 20U));
  CheckNear(rising_result.baseline, falling_result.baseline,
            "rising and falling baselines are symmetric");
  CheckNear(rising_result.short_charge, falling_result.short_charge,
            "rising and falling short integrals are symmetric");
  CheckNear(rising_result.charge, falling_result.charge,
            "rising and falling long integrals are symmetric");
  CheckNear(rising_result.pulse_height, falling_result.pulse_height,
            "rising and falling amplitudes are symmetric");
  CheckNear(rising_result.pulse_start_ns, falling_result.pulse_start_ns,
            "rising and falling T0 values are symmetric");

  const auto longer_gate = cpnr::ComputeWaveformDsp(
      falling.data(), falling.size(), true, Settings(32U, 20U, 32U));
  CheckNear(longer_gate.short_charge, 200.0,
            "changing ShortGate changes only the short integral as expected");
  CheckNear(longer_gate.charge, 320.0,
            "changing LongGate changes the long integral as expected");

  std::vector<std::uint16_t> baseline_fixture(128U, 1000U);
  for (std::size_t sample = 16U; sample < 32U; ++sample) {
    baseline_fixture[sample] = 1004U;
  }
  const auto baseline_16 = cpnr::ComputeWaveformDsp(
      baseline_fixture.data(), baseline_fixture.size(), true,
      Settings(16U, 8U, 16U, 10.0));
  const auto baseline_32 = cpnr::ComputeWaveformDsp(
      baseline_fixture.data(), baseline_fixture.size(), true,
      Settings(32U, 8U, 16U, 10.0));
  CheckNear(baseline_16.baseline, 1000.0,
            "BaselineSamples=16 selects exactly the first 16 samples");
  CheckNear(baseline_32.baseline, 1002.0,
            "BaselineSamples=32 changes the computed baseline");

  const std::vector<std::uint16_t> quiet(128U, 1000U);
  const auto quiet_result = cpnr::ComputeWaveformDsp(
      quiet.data(), quiet.size(), true, Settings(32U, 10U, 20U));
  CheckNear(quiet_result.short_charge, 0.0, "quiet short charge is zero");
  CheckNear(quiet_result.charge, 0.0, "quiet long charge is zero");
  CheckNear(quiet_result.pulse_height, 0.0, "quiet pulse height is zero");
  CheckNear(quiet_result.pulse_start_ns, -1.0, "quiet waveform has no T0");

  std::vector<std::uint16_t> saturated(128U, 1000U);
  saturated[64U] = 0U;
  const auto saturated_result = cpnr::ComputeWaveformDsp(
      saturated.data(), saturated.size(), true, Settings(32U, 1U, 1U));
  CheckNear(saturated_result.pulse_height, 1000.0,
            "saturated falling pulse retains its full amplitude");
  CheckNear(saturated_result.charge, 1000.0,
            "single-sample long gate includes a saturated pulse exactly");

  auto invalid = Settings(32U, 20U, 10U);
  try {
    (void)cpnr::ComputeWaveformDsp(falling.data(), falling.size(), true,
                                   invalid);
    Check(false, "ShortGate greater than LongGate must throw");
  } catch (const std::invalid_argument&) {
  }

  if (failures != 0) return 1;
  std::cout << "Waveform DSP tests passed\n";
  return 0;
}
