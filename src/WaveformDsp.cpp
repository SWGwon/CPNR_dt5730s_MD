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
  const bool threshold_gate_mode =
      settings.integration_mode ==
      WaveformDspIntegrationMode::kThresholdAnchoredGates;
  const bool peak_window_mode =
      settings.integration_mode ==
      WaveformDspIntegrationMode::kPeakCenteredWindow;
  if (settings.baseline_samples == 0U ||
      (!threshold_gate_mode && !peak_window_mode) ||
      (threshold_gate_mode &&
       (settings.short_gate_samples == 0U ||
        settings.long_gate_samples == 0U ||
        settings.short_gate_samples > settings.long_gate_samples ||
        !std::isfinite(settings.pulse_start_threshold_adc) ||
        settings.pulse_start_threshold_adc <= 0.0))) {
    throw std::invalid_argument("Invalid waveform DSP settings");
  }

  std::size_t baseline_samples =
      std::min(settings.baseline_samples, trace_length);
  if (threshold_gate_mode) {
    const std::size_t initial_window = std::min<std::size_t>(5U, trace_length);
    double initial_baseline = 0.0;
    for (std::size_t sample = 0U; sample < initial_window; ++sample) {
      initial_baseline += trace[sample];
    }
    initial_baseline /= static_cast<double>(initial_window);

    baseline_samples = settings.legacy_adaptive_baseline
                           ? trace_length / 4U
                           : baseline_samples;
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
  }
  if (baseline_samples == 0U) baseline_samples = 1U;

  for (std::size_t sample = 0U; sample < baseline_samples; ++sample) {
    values.baseline += trace[sample];
  }
  values.baseline /= static_cast<double>(baseline_samples);

  // Schema 3 interprets "waveform height" literally across the complete
  // record. Schemas 1/2 retain their historical PulseHeight behavior, which
  // excluded the samples consumed by the baseline estimate.
  const std::size_t peak_search_begin =
      peak_window_mode ? 0U : baseline_samples;
  std::size_t peak_sample = peak_window_mode ? 0U : peak_search_begin;
  double maximum_directed_height =
      peak_window_mode
          ? (falling_polarity
                 ? values.baseline - static_cast<double>(trace[0])
                 : static_cast<double>(trace[0]) - values.baseline)
          : 0.0;
  const std::size_t peak_search_next =
      peak_window_mode ? 1U : peak_search_begin;
  for (std::size_t sample = peak_search_next; sample < trace_length; ++sample) {
    const double directed_height =
        falling_polarity
            ? values.baseline - static_cast<double>(trace[sample])
            : static_cast<double>(trace[sample]) - values.baseline;
    if (directed_height > maximum_directed_height) {
      maximum_directed_height = directed_height;
      peak_sample = sample;
    }
  }
  values.pulse_height = std::max(0.0, maximum_directed_height);

  const auto integrate = [&](std::size_t gate_start,
                             std::size_t gate_end) {
    double result = 0.0;
    for (std::size_t sample = gate_start; sample < gate_end; ++sample) {
      result += falling_polarity
                    ? values.baseline - static_cast<double>(trace[sample])
                    : static_cast<double>(trace[sample]) - values.baseline;
    }
    return settings.preserve_signed_charge ? result : std::max(0.0, result);
  };

  if (peak_window_mode) {
    const std::size_t gate_start =
        peak_sample >= kPeakCenteredChargePreSamples
            ? peak_sample - kPeakCenteredChargePreSamples
            : 0U;
    const std::size_t samples_after_peak = trace_length - peak_sample;
    const std::size_t gate_end =
        kPeakCenteredChargePostSamples >= samples_after_peak
            ? trace_length
            : peak_sample + kPeakCenteredChargePostSamples;
    values.charge = integrate(gate_start, gate_end);
    // The legacy branch remains present so downstream readers keep a stable
    // tree shape. Schema 3 explicitly records that it aliases Charge_CHn.
    values.short_charge = values.charge;
    values.pulse_start_ns =
        dt5730_timing::kAdcSamplePeriodNs *
        static_cast<double>(peak_sample);
    return values;
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

  const auto integrate_gate = [&](std::size_t gate_samples) {
    const std::size_t gate_end =
        gate_samples > trace_length - gate_start
            ? trace_length
            : gate_start + gate_samples;
    return integrate(gate_start, gate_end);
  };
  values.short_charge = integrate_gate(settings.short_gate_samples);
  values.charge = integrate_gate(settings.long_gate_samples);
  return values;
}

bool DspValueApproximatelyEqual(double observed, double expected) {
  return std::abs(observed - expected) <=
         std::max(kWaveformDspAbsoluteTolerance,
                  kWaveformDspRelativeTolerance *
                      std::max(std::abs(observed), std::abs(expected)));
}

}  // namespace cpnr
