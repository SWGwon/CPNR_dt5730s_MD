#ifndef CPNR_WAVEFORM_DSP_H
#define CPNR_WAVEFORM_DSP_H

#include <cstddef>
#include <cstdint>

namespace cpnr {

inline constexpr double kWaveformDspAbsoluteTolerance = 1e-9;
inline constexpr double kWaveformDspRelativeTolerance = 1e-9;
inline constexpr std::uint32_t kWaveformDspSchemaClampedCharge = 1U;
inline constexpr std::uint32_t kWaveformDspSchemaSignedCharge = 2U;
inline constexpr std::uint32_t kWaveformDspSchemaPeakCenteredCharge = 3U;
inline constexpr std::uint32_t kWaveformDspCurrentSchema =
    kWaveformDspSchemaPeakCenteredCharge;

inline constexpr std::size_t kPeakCenteredChargePreSamples = 10U;
inline constexpr std::size_t kPeakCenteredChargePostSamples = 20U;
inline constexpr std::size_t kPeakCenteredChargeWindowSamples =
    kPeakCenteredChargePreSamples + kPeakCenteredChargePostSamples;
inline constexpr std::uint32_t kPeakCenteredChargePreNs = 20U;
inline constexpr std::uint32_t kPeakCenteredChargePostNs = 40U;
inline constexpr char kPeakCenteredChargeAnchor[] =
    "polarity_corrected_peak";
inline constexpr char kPeakCenteredShortChargeSemantics[] =
    "alias_of_charge";
inline constexpr char kPeakCenteredPulseTimeSemantics[] =
    "polarity_corrected_peak_sample";

enum class WaveformDspIntegrationMode : std::uint32_t {
  // Schemas 1 and 2: threshold crossing anchors configurable short/long gates.
  kThresholdAnchoredGates = 0U,
  // Schema 3: polarity-corrected peak anchors the fixed [-20 ns, +40 ns)
  // signed integration window.
  kPeakCenteredWindow = 1U,
};

struct WaveformDspSettings {
  std::size_t baseline_samples = 150U;
  std::size_t short_gate_samples = 40U;
  std::size_t long_gate_samples = 200U;
  double pulse_start_threshold_adc = 30.0;
  bool integrate_from_pulse_start = true;
  bool legacy_adaptive_baseline = false;
  // Schema 2 preserves a negative net integral after positive and negative
  // baseline excursions cancel. Schema 1 clamps that final net value to zero.
  bool preserve_signed_charge = true;
  WaveformDspIntegrationMode integration_mode =
      WaveformDspIntegrationMode::kPeakCenteredWindow;
};

// Canonical production conversion result for one active channel.  Keeping
// this algorithm in a shared, ROOT-free component prevents the converter and
// offline fidelity checker from silently drifting apart.
struct WaveformDspValues {
  double baseline = 0.0;
  double short_charge = 0.0;
  double charge = 0.0;
  double pulse_height = 0.0;
  // Schemas 1/2 store threshold-crossing time. Schema 3 stores the
  // polarity-corrected maximum-height sample time for branch compatibility.
  double pulse_start_ns = -1.0;
};

WaveformDspValues ComputeWaveformDsp(const std::uint16_t* trace,
                                     std::size_t trace_length,
                                     bool falling_polarity,
                                     const WaveformDspSettings& settings = {});

bool DspValueApproximatelyEqual(double observed, double expected);

}  // namespace cpnr

#endif  // CPNR_WAVEFORM_DSP_H
