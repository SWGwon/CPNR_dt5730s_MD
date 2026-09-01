"""Hardware-native timing constraints for DT5730 waveform acquisition.

The x730 waveform firmware stores a custom record in ten-sample locations.
The CAENDigitizer 2.19.x post-trigger API separately converts an integer
percentage through eight-sample register locations.  Keeping those two
quantizers in one Qt-free module lets config authoring, launch preflight,
storage accounting, and live monitoring share the same contract.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Optional


ADC_SAMPLE_PERIOD_NS = 2.0
TRIGGER_LATENCY_NS = 120.0
MIN_PRE_TRIGGER_NS = 160.0
MIN_PRE_TRIGGER_SAMPLES = int(MIN_PRE_TRIGGER_NS / ADC_SAMPLE_PERIOD_NS)

MIN_REQUESTED_RECORD_LENGTH = 128
MIN_RECORD_LENGTH = 130
MAX_RECORD_LENGTH = 102400
RECORD_LENGTH_GRANULARITY = 10
POST_TRIGGER_GRANULARITY = 8
MIN_AUTO_POST_TRIGGER_PERCENT = 10
MAX_AUTO_POST_TRIGGER_PERCENT = 90

DEFAULT_SHORT_GATE_SAMPLES = 40
DEFAULT_LONG_GATE_SAMPLES = 200
MAX_PROVENANCE_GATE_SAMPLES = (1 << 31) - 1

MIN_SOFTWARE_RANDOM_TRIGGER_RATE_HZ = 0.001
MAX_SOFTWARE_RANDOM_TRIGGER_RATE_HZ = 100_000.0


@dataclass(frozen=True)
class PostTriggerTruth:
    """The x730 register truth produced by one requested percentage."""

    requested_percent: int
    register_locations: int
    actual_post_samples: int
    actual_pre_samples: int
    readback_percent: int

    @property
    def exact_readback(self) -> bool:
        return self.readback_percent == self.requested_percent


@dataclass(frozen=True)
class TimeDspPlan:
    """A hardware-valid set of coupled time and waveform-DSP settings."""

    requested_record_length: int
    record_length: int
    target_t0_ns: int
    post_trigger_percent: int
    post_trigger_readback_percent: int
    post_trigger_register_locations: int
    actual_pre_samples: int
    actual_post_samples: int
    achieved_t0_ns: float
    baseline_samples: int
    short_gate_samples: int
    long_gate_samples: int
    preserved_gate_settings: bool

    @property
    def record_length_adjusted(self) -> bool:
        return self.record_length != self.requested_record_length

    @property
    def post_trigger_exact_readback(self) -> bool:
        return self.post_trigger_percent == self.post_trigger_readback_percent


def _require_integer(name: str, value: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{name} must be an integer")
    return value


def normalize_record_length(requested_record_length: int) -> int:
    """Round a supported request upward to the x730 ten-sample grid.

    Rounding upward deliberately never shortens the acquisition window the
    operator requested.  The first hardware-native value in the application's
    historical 128-sample range is therefore 130.
    """

    requested = _require_integer(
        "RecordLength", requested_record_length
    )
    if not MIN_REQUESTED_RECORD_LENGTH <= requested <= MAX_RECORD_LENGTH:
        raise ValueError(
            "RecordLength must be in the supported 128..102400 range"
        )
    return (
        (requested + RECORD_LENGTH_GRANULARITY - 1)
        // RECORD_LENGTH_GRANULARITY
        * RECORD_LENGTH_GRANULARITY
    )


def validate_effective_record_length(record_length: int) -> int:
    """Return a record length only when it is directly representable."""

    record = _require_integer("RecordLength", record_length)
    if not MIN_RECORD_LENGTH <= record <= MAX_RECORD_LENGTH:
        raise ValueError(
            "RecordLength must be in the hardware-native 130..102400 range"
        )
    if record % RECORD_LENGTH_GRANULARITY:
        raise ValueError(
            "RecordLength must be a multiple of 10 samples for DT5730"
        )
    return record


def predict_post_trigger(
    record_length: int, post_trigger_percent: int
) -> PostTriggerTruth:
    """Mirror CAENDigitizer's x730 Set/GetPostTriggerSize conversion.

    For record length ``R`` and requested percentage ``P``, the library uses
    ``M=floor(R/8)``, ``K=ceil(P*M/100)``, and programs ``K`` locations.  A
    readback reports ``floor(8*K*100/R)`` percent.
    """

    record = _require_integer("RecordLength", record_length)
    percent = _require_integer("PostTrigger", post_trigger_percent)
    if not MIN_REQUESTED_RECORD_LENGTH <= record <= MAX_RECORD_LENGTH:
        raise ValueError("RecordLength is outside 128..102400")
    if not 0 <= percent <= 100:
        raise ValueError("PostTrigger must be in the range 0..100")

    available_locations = record // POST_TRIGGER_GRANULARITY
    register_locations = (
        percent * available_locations + 99
    ) // 100
    if register_locations > available_locations:
        register_locations = 0
    actual_post = register_locations * POST_TRIGGER_GRANULARITY
    actual_pre = record - actual_post
    readback = actual_post * 100 // record
    return PostTriggerTruth(
        requested_percent=percent,
        register_locations=register_locations,
        actual_post_samples=actual_post,
        actual_pre_samples=actual_pre,
        readback_percent=readback,
    )


def _valid_gate_pair(
    short_gate: Optional[int], long_gate: Optional[int], post_samples: int
) -> bool:
    return (
        isinstance(short_gate, int)
        and not isinstance(short_gate, bool)
        and isinstance(long_gate, int)
        and not isinstance(long_gate, bool)
        and 1 <= short_gate <= long_gate <= post_samples
    )


def derive_time_dsp_plan(
    requested_record_length: int,
    target_t0_ns: int,
    *,
    current_short_gate: Optional[int] = None,
    current_long_gate: Optional[int] = None,
) -> TimeDspPlan:
    """Derive one coherent DT5730 time/DSP plan.

    Percent candidates are limited to the calculator's operator-facing
    10..90 policy.  Hardware pre-trigger distance is the primary objective;
    an exact Set/Get percentage round trip wins equal-distance ties.  A final
    lower-percentage tie break retains more pre-trigger history.
    """

    target = _require_integer("TargetT0", target_t0_ns)
    if target < 0:
        raise ValueError("TargetT0 must not be negative")
    record = normalize_record_length(requested_record_length)
    target_pre_samples = (
        target / ADC_SAMPLE_PERIOD_NS
        + TRIGGER_LATENCY_NS / ADC_SAMPLE_PERIOD_NS
    )

    candidates = []
    for percent in range(
        MIN_AUTO_POST_TRIGGER_PERCENT,
        MAX_AUTO_POST_TRIGGER_PERCENT + 1,
    ):
        truth = predict_post_trigger(record, percent)
        if truth.actual_pre_samples < MIN_PRE_TRIGGER_SAMPLES:
            continue
        candidates.append((
            abs(truth.actual_pre_samples - target_pre_samples),
            0 if truth.exact_readback else 1,
            percent,
            truth,
        ))
    if not candidates:  # pragma: no cover - guarded by current ranges
        raise ValueError(
            "RecordLength/PostTrigger cannot provide 160 ns of pre-trigger"
        )
    _distance, _readback_rank, percent, truth = min(candidates)

    target_samples = target / ADC_SAMPLE_PERIOD_NS
    baseline = max(
        1,
        min(int(target_samples * 0.8), truth.actual_pre_samples),
    )
    preserve_gates = _valid_gate_pair(
        current_short_gate,
        current_long_gate,
        truth.actual_post_samples,
    )
    if preserve_gates:
        short_gate = int(current_short_gate)
        long_gate = int(current_long_gate)
    else:
        long_gate = min(
            DEFAULT_LONG_GATE_SAMPLES, truth.actual_post_samples
        )
        short_gate = min(DEFAULT_SHORT_GATE_SAMPLES, long_gate)

    achieved_t0 = (
        truth.actual_pre_samples * ADC_SAMPLE_PERIOD_NS
        - TRIGGER_LATENCY_NS
    )
    return TimeDspPlan(
        requested_record_length=requested_record_length,
        record_length=record,
        target_t0_ns=target,
        post_trigger_percent=percent,
        post_trigger_readback_percent=truth.readback_percent,
        post_trigger_register_locations=truth.register_locations,
        actual_pre_samples=truth.actual_pre_samples,
        actual_post_samples=truth.actual_post_samples,
        achieved_t0_ns=achieved_t0,
        baseline_samples=baseline,
        short_gate_samples=short_gate,
        long_gate_samples=long_gate,
        preserved_gate_settings=preserve_gates,
    )
