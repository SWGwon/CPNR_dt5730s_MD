#ifndef CPNR_WAVEFORM_DSP_H
#define CPNR_WAVEFORM_DSP_H

#include <cstddef>
#include <cstdint>

namespace cpnr {

inline constexpr double kWaveformDspAbsoluteTolerance = 1e-9;
inline constexpr double kWaveformDspRelativeTolerance = 1e-9;

struct WaveformDspSettings {
  std::size_t baseline_samples = 150U;
  std::size_t short_gate_samples = 40U;
  std::size_t long_gate_samples = 200U;
  double pulse_start_threshold_adc = 30.0;
  bool integrate_from_pulse_start = true;
  bool legacy_adaptive_baseline = false;
};

// Canonical production conversion result for one active channel.  Keeping
// this algorithm in a shared, ROOT-free component prevents the converter and
// offline fidelity checker from silently drifting apart.
struct WaveformDspValues {
  double baseline = 0.0;
  double short_charge = 0.0;
  double charge = 0.0;
  double pulse_height = 0.0;
  double pulse_start_ns = -1.0;
};

WaveformDspValues ComputeWaveformDsp(const std::uint16_t* trace,
                                     std::size_t trace_length,
                                     bool falling_polarity,
                                     const WaveformDspSettings& settings = {});

bool DspValueApproximatelyEqual(double observed, double expected);

}  // namespace cpnr

#endif  // CPNR_WAVEFORM_DSP_H
