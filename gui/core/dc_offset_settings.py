"""Qt-free DT5730S DC-offset configuration and preview helpers.

The waveform ADC is 14-bit, while the independently programmable DC-offset
DAC is 16-bit.  The nominal DAC conversion in this module is deliberately
used only as a starting estimate/GUI preview: runtime code must measure the
actual per-channel baseline and close the loop before acquisition.
"""

from __future__ import annotations

import math
from dataclasses import dataclass


DC_OFFSET_MODE_TARGET = "TargetBaseline"
DC_OFFSET_MODE_RAW = "RawDac"
DC_OFFSET_MODES = (DC_OFFSET_MODE_TARGET, DC_OFFSET_MODE_RAW)

DT5730S_ADC_BITS = 14
DT5730S_DAC_BITS = 16
MIN_BASELINE_TARGET_PERCENT = 5.0
MAX_BASELINE_TARGET_PERCENT = 95.0
SUPPORTED_INPUT_RANGES_MV = (500, 2000)


@dataclass(frozen=True)
class DCOffsetSettings:
    mode: str
    target_percent: float | None
    raw_dac: int | None
    legacy_raw: bool = False


@dataclass(frozen=True)
class DCOffsetPreview:
    target_percent: float
    target_adc: int
    nominal_dac: int
    falling_headroom_mv: float
    rising_headroom_mv: float


def _round_positive_like_cpp(value: float) -> int:
    """Match ``std::llround`` for the non-negative values used here."""

    return int(math.floor(value + 0.5))


def validate_baseline_target_percent(value: float) -> float:
    value = float(value)
    if not math.isfinite(value):
        raise ValueError("BaselineTargetPercent must be finite")
    if not MIN_BASELINE_TARGET_PERCENT <= value <= MAX_BASELINE_TARGET_PERCENT:
        raise ValueError(
            "BaselineTargetPercent must be in the inclusive range 5..95"
        )
    return value


def calculate_dc_offset_preview(
    target_percent: float,
    input_range_mv: int,
    adc_bits: int = DT5730S_ADC_BITS,
    dac_bits: int = DT5730S_DAC_BITS,
) -> DCOffsetPreview:
    """Return target ADC, nominal DAC seed, and rail headroom.

    CAEN's offset direction is inverted relative to the ADC baseline: a low
    DAC code nominally places the baseline high in the ADC range.  Analogue
    tolerance and input bias mean that ``nominal_dac`` is not a calibration;
    it is only the first code tried by the runtime closed-loop procedure.
    """

    target_percent = validate_baseline_target_percent(target_percent)
    if input_range_mv not in SUPPORTED_INPUT_RANGES_MV:
        raise ValueError("input range must be exactly 500 or 2000 mVpp")
    if adc_bits != DT5730S_ADC_BITS:
        raise ValueError("DT5730S ADCBits must be exactly 14")
    if dac_bits != DT5730S_DAC_BITS:
        raise ValueError("DT5730S DC-offset DAC must be exactly 16-bit")

    adc_max = (1 << adc_bits) - 1
    dac_max = (1 << dac_bits) - 1
    # Keep the percentage arithmetic in this order so exact decimal presets
    # such as 90.0 do not slip just below a half-code through ``1.0 - 0.9``.
    fraction = target_percent / 100.0
    return DCOffsetPreview(
        target_percent=target_percent,
        target_adc=_round_positive_like_cpp(
            (target_percent * adc_max) / 100.0
        ),
        nominal_dac=_round_positive_like_cpp(
            ((100.0 - target_percent) * dac_max) / 100.0
        ),
        falling_headroom_mv=fraction * input_range_mv,
        rising_headroom_mv=(1.0 - fraction) * input_range_mv,
    )


def nominal_baseline_percent_from_dac(raw_dac: int) -> float:
    """Infer the nominal baseline percentage for a legacy raw DAC code."""

    if isinstance(raw_dac, bool) or not isinstance(raw_dac, int):
        raise ValueError("DCOffset must be an integer")
    dac_max = (1 << DT5730S_DAC_BITS) - 1
    if not 0 <= raw_dac <= dac_max:
        raise ValueError("DCOffset must be in the inclusive range 0..65535")
    return (1.0 - (raw_dac / dac_max)) * 100.0


def parse_dc_offset_settings(
    mode: str | None,
    baseline_target_percent: str | float | None,
    raw_dac: str | int | None,
) -> DCOffsetSettings:
    """Validate one channel's mutually-exclusive offset schema.

    A missing mode plus a raw ``DCOffset`` is the sole legacy form accepted.
    Explicit modes must use exactly one matching value.
    """

    normalized_mode = None if mode is None else str(mode).strip()
    target_present = (
        baseline_target_percent is not None
        and str(baseline_target_percent).strip() != ""
    )
    raw_present = raw_dac is not None and str(raw_dac).strip() != ""

    if normalized_mode in (None, ""):
        if not raw_present:
            raise ValueError(
                "DCOffsetMode is required unless a legacy DCOffset is present"
            )
        if target_present:
            raise ValueError(
                "legacy DCOffset cannot be combined with BaselineTargetPercent"
            )
        parsed_raw = _parse_raw_dac(raw_dac)
        return DCOffsetSettings(
            mode=DC_OFFSET_MODE_RAW,
            target_percent=None,
            raw_dac=parsed_raw,
            legacy_raw=True,
        )

    if normalized_mode not in DC_OFFSET_MODES:
        raise ValueError(
            "DCOffsetMode must be TargetBaseline or RawDac"
        )

    if normalized_mode == DC_OFFSET_MODE_TARGET:
        if not target_present:
            raise ValueError(
                "TargetBaseline mode requires BaselineTargetPercent"
            )
        if raw_present:
            raise ValueError(
                "TargetBaseline mode must not contain DCOffset"
            )
        try:
            target = float(str(baseline_target_percent).strip())
        except (TypeError, ValueError) as exc:
            raise ValueError("BaselineTargetPercent must be a number") from exc
        return DCOffsetSettings(
            mode=normalized_mode,
            target_percent=validate_baseline_target_percent(target),
            raw_dac=None,
        )

    if not raw_present:
        raise ValueError("RawDac mode requires DCOffset")
    if target_present:
        raise ValueError("RawDac mode must not contain BaselineTargetPercent")
    return DCOffsetSettings(
        mode=normalized_mode,
        target_percent=None,
        raw_dac=_parse_raw_dac(raw_dac),
    )


def _parse_raw_dac(value: str | int | None) -> int:
    if isinstance(value, bool):
        raise ValueError("DCOffset must be a decimal integer")
    text = str(value).strip()
    if not text or not text.lstrip("+-").isdigit():
        raise ValueError("DCOffset must be a decimal integer")
    parsed = int(text, 10)
    nominal_baseline_percent_from_dac(parsed)
    return parsed
