#include "WaveformDsp.h"

#include "DT5730Timing.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace cpnr {

WaveformDspValues ComputeWaveformDsp(const std::uint16_t* trace,
                                     std::size_t trace_length,
                                     bool falling_polarity,
                                     const WaveformDspSettings& settings) {
  WaveformDspValues values;
  if (trace_length == 0U) return values;
  if (trace == nullptr) {
    throw std::invalid_argument("Non-empty waveform has a null data pointer");
  }
  if (settings.baseline_samples == 0U ||
      settings.short_gate_samples == 0U ||
      settings.long_gate_samples == 0U ||
      settings.short_gate_samples > settings.long_gate_samples ||
      !std::isfinite(settings.pulse_start_threshold_adc) ||
      settings.pulse_start_threshold_adc <= 0.0) {
    throw std::invalid_argument("Invalid waveform DSP settings");
  }

  const std::size_t initial_window = std::min<std::size_t>(5U, trace_length);
  double initial_baseline = 0.0;
  for (std::size_t sample = 0U; sample < initial_window; ++sample) {
    initial_baseline += trace[sample];
  }
  initial_baseline /= static_cast<double>(initial_window);

  std::size_t baseline_samples = settings.legacy_adaptive_baseline
                                     ? trace_length / 4U
                                     : std::min(settings.baseline_samples,
                                                trace_length);
  const std::size_t baseline_scan_limit = settings.legacy_adaptive_baseline
                                              ? trace_length
                                              : baseline_samples;
  for (std::size_t sample = initial_window; sample < baseline_scan_limit;
       ++sample) {
    const double baseline_excursion =
        falling_polarity
            ? initial_baseline - static_cast<double>(trace[sample])
            : static_cast<double>(trace[sample]) - initial_baseline;
    if (baseline_excursion > settings.pulse_start_threshold_adc) {
      baseline_samples = sample > 5U ? sample - 5U : 1U;
      break;
    }
  }
  if (settings.legacy_adaptive_baseline) {
    baseline_samples = std::min<std::size_t>(baseline_samples, 150U);
  }
  if (baseline_samples == 0U) baseline_samples = 1U;

  for (std::size_t sample = 0U; sample < baseline_samples; ++sample) {
    values.baseline += trace[sample];
  }
  values.baseline /= static_cast<double>(baseline_samples);

  double pulse_extremum = values.baseline;
  if (falling_polarity) {
    for (std::size_t sample = baseline_samples; sample < trace_length;
         ++sample) {
      pulse_extremum =
          std::min(pulse_extremum, static_cast<double>(trace[sample]));
    }
    values.pulse_height =
        std::max(0.0, values.baseline - pulse_extremum);
  } else {
    for (std::size_t sample = baseline_samples; sample < trace_length;
         ++sample) {
      pulse_extremum =
          std::max(pulse_extremum, static_cast<double>(trace[sample]));
    }
    values.pulse_height =
        std::max(0.0, pulse_extremum - values.baseline);
  }
  const double start_threshold = falling_polarity
                                     ? values.baseline -
                                           settings.pulse_start_threshold_adc
                                     : values.baseline +
                                           settings.pulse_start_threshold_adc;
  std::size_t gate_start = baseline_samples;
  for (std::size_t sample = baseline_samples; sample < trace_length;
       ++sample) {
    const bool crossed =
        falling_polarity
            ? static_cast<double>(trace[sample]) < start_threshold
            : static_cast<double>(trace[sample]) > start_threshold;
    if (crossed) {
      if (settings.integrate_from_pulse_start) gate_start = sample;
      values.pulse_start_ns =
          dt5730_timing::kAdcSamplePeriodNs * static_cast<double>(sample);
      break;
    }
  }

  const auto integrate = [&](std::size_t gate_samples) {
    const std::size_t gate_end =
        gate_samples > trace_length - gate_start
            ? trace_length
            : gate_start + gate_samples;
    double result = 0.0;
    for (std::size_t sample = gate_start; sample < gate_end; ++sample) {
      result += falling_polarity
                    ? values.baseline - static_cast<double>(trace[sample])
                    : static_cast<double>(trace[sample]) - values.baseline;
    }
    return std::max(0.0, result);
  };
  values.short_charge = integrate(settings.short_gate_samples);
  values.charge = integrate(settings.long_gate_samples);
  return values;
}

bool DspValueApproximatelyEqual(double observed, double expected) {
  return std::abs(observed - expected) <=
         std::max(kWaveformDspAbsoluteTolerance,
                  kWaveformDspRelativeTolerance *
                      std::max(std::abs(observed), std::abs(expected)));
}

}  // namespace cpnr
