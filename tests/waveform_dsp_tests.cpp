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
  settings.integration_mode =
      cpnr::WaveformDspIntegrationMode::kThresholdAnchoredGates;
  return settings;
}

}  // namespace

int main() {
  Check(cpnr::kWaveformDspSchemaClampedCharge == 1U,
        "clamped-charge DSP schema remains version 1");
  Check(cpnr::kWaveformDspSchemaSignedCharge == 2U,
        "signed-charge DSP schema is version 2");
  Check(cpnr::kWaveformDspSchemaPeakCenteredCharge == 3U,
        "peak-centered signed-charge DSP schema is version 3");
  Check(cpnr::kWaveformDspCurrentSchema ==
            cpnr::kWaveformDspSchemaPeakCenteredCharge,
        "current DSP schema uses peak-centered signed charge");

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

  std::vector<std::uint16_t> net_negative_falling(128U, 1000U);
  net_negative_falling[64U] = 990U;
  for (std::size_t sample = 65U; sample < 68U; ++sample) {
    net_negative_falling[sample] = 1010U;
  }
  const auto signed_falling = cpnr::ComputeWaveformDsp(
      net_negative_falling.data(), net_negative_falling.size(), true,
      Settings(32U, 4U, 4U));
  CheckNear(signed_falling.short_charge, -20.0,
            "schema-2 falling short integral preserves a negative net value");
  CheckNear(signed_falling.charge, -20.0,
            "schema-2 falling long integral preserves a negative net value");

  std::vector<std::uint16_t> net_negative_rising(128U, 1000U);
  net_negative_rising[64U] = 1010U;
  for (std::size_t sample = 65U; sample < 68U; ++sample) {
    net_negative_rising[sample] = 990U;
  }
  const auto signed_rising = cpnr::ComputeWaveformDsp(
      net_negative_rising.data(), net_negative_rising.size(), false,
      Settings(32U, 4U, 4U));
  CheckNear(signed_rising.short_charge, -20.0,
            "schema-2 rising short integral preserves a negative net value");
  CheckNear(signed_rising.charge, -20.0,
            "schema-2 rising long integral preserves a negative net value");

  auto legacy_charge_settings = Settings(32U, 4U, 4U);
  legacy_charge_settings.preserve_signed_charge = false;
  const auto clamped_falling = cpnr::ComputeWaveformDsp(
      net_negative_falling.data(), net_negative_falling.size(), true,
      legacy_charge_settings);
  const auto clamped_rising = cpnr::ComputeWaveformDsp(
      net_negative_rising.data(), net_negative_rising.size(), false,
      legacy_charge_settings);
  CheckNear(clamped_falling.short_charge, 0.0,
            "schema-1 falling short integral clamps a negative net value");
  CheckNear(clamped_falling.charge, 0.0,
            "schema-1 falling long integral clamps a negative net value");
  CheckNear(clamped_rising.short_charge, 0.0,
            "schema-1 rising short integral clamps a negative net value");
  CheckNear(clamped_rising.charge, 0.0,
            "schema-1 rising long integral clamps a negative net value");

  std::vector<std::uint16_t> saturated(128U, 1000U);
  saturated[64U] = 0U;
  const auto saturated_result = cpnr::ComputeWaveformDsp(
      saturated.data(), saturated.size(), true, Settings(32U, 1U, 1U));
  CheckNear(saturated_result.pulse_height, 1000.0,
            "saturated falling pulse retains its full amplitude");
  CheckNear(saturated_result.charge, 1000.0,
            "single-sample long gate includes a saturated pulse exactly");

  cpnr::WaveformDspSettings peak_settings;
  peak_settings.baseline_samples = 32U;
  // These legacy settings must not influence schema-3 charge or T0.
  peak_settings.short_gate_samples = 1U;
  peak_settings.long_gate_samples = 1U;
  peak_settings.pulse_start_threshold_adc = 9999.0;
  peak_settings.integration_mode =
      cpnr::WaveformDspIntegrationMode::kPeakCenteredWindow;

  std::vector<std::uint16_t> peak_falling(128U, 1000U);
  for (std::size_t sample = 54U; sample < 84U; ++sample) {
    peak_falling[sample] = 999U;
  }
  peak_falling[53U] = 950U;  // peak - 11: outside the half-open window.
  peak_falling[64U] = 900U;
  peak_falling[83U] = 1010U;
  peak_falling[84U] = 950U;  // peak + 20: outside the half-open window.
  peak_falling[100U] = 950U;
  const auto peak_falling_result = cpnr::ComputeWaveformDsp(
      peak_falling.data(), peak_falling.size(), true, peak_settings);
  CheckNear(peak_falling_result.baseline, 1000.0,
            "peak-window baseline uses exactly the configured samples");
  CheckNear(peak_falling_result.pulse_height, 100.0,
            "falling peak-window mode finds the largest directed height");
  CheckNear(peak_falling_result.pulse_start_ns, 128.0,
            "schema-3 compatibility T0 stores the peak sample time");
  CheckNear(peak_falling_result.charge, 118.0,
            "peak-window charge integrates signed [-20 ns,+40 ns) samples");
  CheckNear(peak_falling_result.short_charge, peak_falling_result.charge,
            "schema-3 ShortCharge is an explicit Charge alias");

  std::vector<std::uint16_t> peak_rising(128U, 1000U);
  for (std::size_t sample = 54U; sample < 84U; ++sample) {
    peak_rising[sample] = 1001U;
  }
  peak_rising[53U] = 1050U;
  peak_rising[64U] = 1100U;
  peak_rising[83U] = 990U;
  peak_rising[84U] = 1050U;
  peak_rising[100U] = 1050U;
  const auto peak_rising_result = cpnr::ComputeWaveformDsp(
      peak_rising.data(), peak_rising.size(), false, peak_settings);
  CheckNear(peak_rising_result.charge, peak_falling_result.charge,
            "rising and falling peak-window charges are symmetric");
  CheckNear(peak_rising_result.pulse_start_ns,
            peak_falling_result.pulse_start_ns,
            "rising and falling peak locations are symmetric");

  auto early_peak_settings = peak_settings;
  early_peak_settings.baseline_samples = 4U;
  std::vector<std::uint16_t> early_peak(128U, 1000U);
  early_peak[5U] = 900U;
  const auto early_peak_result = cpnr::ComputeWaveformDsp(
      early_peak.data(), early_peak.size(), true, early_peak_settings);
  CheckNear(early_peak_result.charge, 100.0,
            "peak window clips safely at the record beginning");
  CheckNear(early_peak_result.pulse_start_ns, 10.0,
            "early record-edge peak time remains exact");

  std::vector<std::uint16_t> tied_peaks(128U, 1000U);
  tied_peaks[64U] = 900U;
  tied_peaks[80U] = 900U;
  const auto tied_peak_result = cpnr::ComputeWaveformDsp(
      tied_peaks.data(), tied_peaks.size(), true, peak_settings);
  CheckNear(tied_peak_result.pulse_start_ns, 128.0,
            "equal maximum heights deterministically select the first peak");

  std::vector<std::uint16_t> edge_peak(128U, 1000U);
  edge_peak[127U] = 900U;
  const auto edge_peak_result = cpnr::ComputeWaveformDsp(
      edge_peak.data(), edge_peak.size(), true, peak_settings);
  CheckNear(edge_peak_result.charge, 100.0,
            "peak window clips safely at the record end");
  CheckNear(edge_peak_result.pulse_start_ns, 254.0,
            "record-edge peak time remains exact");

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
