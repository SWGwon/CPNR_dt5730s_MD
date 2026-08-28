"""Qt-free trigger-threshold calculations shared by the GUI and tests."""

from __future__ import annotations

import math
from dataclasses import dataclass


SUPPORTED_INPUT_RANGES_MV = (500, 2000)
DT5730S_ADC_BITS = 14


@dataclass(frozen=True)
class ThresholdPreview:
    baseline_adc: int
    requested_threshold_mv: float
    lsb_mv: float
    delta_adc: int
    polarity: int
    absolute_threshold_adc: int
    effective_threshold_mv: float


def _adc_code_count(input_range_mv: int, adc_bits: int) -> int:
    if input_range_mv not in SUPPORTED_INPUT_RANGES_MV:
        raise ValueError("input range must be exactly 500 or 2000 mVpp")
    if adc_bits != DT5730S_ADC_BITS:
        raise ValueError("DT5730S ADCBits must be exactly 14")
    return 1 << adc_bits


def millivolts_to_adc_delta(threshold_mv: float, input_range_mv: int,
                            adc_bits: int) -> int:
    """Convert a baseline-relative voltage using C++ ``std::llround`` rules."""

    code_count = _adc_code_count(input_range_mv, adc_bits)
    if not math.isfinite(threshold_mv) or threshold_mv <= 0.0:
        raise ValueError("threshold mV must be finite and positive")
    lsb_mv = input_range_mv / code_count
    # All valid inputs are positive, so floor(x + 0.5) exactly matches
    # std::llround rather than Python's ties-to-even round().
    delta_adc = int(math.floor((threshold_mv / lsb_mv) + 0.5))
    if delta_adc <= 0 or delta_adc >= code_count:
        raise ValueError("threshold is outside the representable ADC delta range")
    return delta_adc


def calculate_threshold_preview(
    baseline_adc: float,
    threshold_mv: float,
    input_range_mv: int,
    adc_bits: int,
    polarity: int,
) -> ThresholdPreview:
    """Preview the runtime absolute code for a supplied measured baseline.

    ``polarity`` follows CAENDigitizer: 0 is rising and 1 is falling.
    The DAQ uses the same operation after measuring each channel independently;
    ConfigTab supplies only a clearly labelled visual baseline estimate.
    """

    code_count = _adc_code_count(input_range_mv, adc_bits)
    max_code = code_count - 1
    if not math.isfinite(baseline_adc) or not 0.0 <= baseline_adc <= max_code:
        raise ValueError("baseline is outside the ADC range")
    if polarity not in (0, 1):
        raise ValueError("polarity must be 0 (rising) or 1 (falling)")
    delta_adc = millivolts_to_adc_delta(
        threshold_mv, input_range_mv, adc_bits
    )
    rounded_baseline = int(math.floor(baseline_adc + 0.5))
    absolute = (
        rounded_baseline - delta_adc
        if polarity == 1 else rounded_baseline + delta_adc
    )
    if not 0 <= absolute <= max_code:
        raise ValueError("baseline-relative threshold is outside the ADC range")
    lsb_mv = input_range_mv / code_count
    return ThresholdPreview(
        baseline_adc=rounded_baseline,
        requested_threshold_mv=threshold_mv,
        lsb_mv=lsb_mv,
        delta_adc=delta_adc,
        polarity=polarity,
        absolute_threshold_adc=absolute,
        effective_threshold_mv=abs(baseline_adc - absolute) * lsb_mv,
    )
