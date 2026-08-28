"""Strict, hardware-free helpers for the live ZeroMQ monitor.

The monitoring socket is an untrusted, lossy observation channel.  These
helpers deliberately keep its wire decoding and sequence accounting separate
from Qt so malformed frames cannot be mistaken for DAQ data loss or reach the
plotting code as partially decoded waveforms.
"""

from __future__ import annotations

import configparser
import hashlib
import hmac
import os
import stat
import struct
from dataclasses import dataclass
from pathlib import Path
from types import MappingProxyType
from typing import Callable, Mapping, Optional, Sequence, Tuple, Type

import numpy as np


EVENT_HEADER_FORMAT = "<QIIHHI"
EVENT_HEADER_BYTES = struct.calcsize(EVENT_HEADER_FORMAT)
MAX_CHANNELS = 8
SUPPORTED_CHANNEL_MASK = (1 << MAX_CHANNELS) - 1
MIN_RECORD_LENGTH = 128
MAX_RECORD_LENGTH = 102400
RECORD_LENGTH_GRANULARITY = 8
ADC_BITS = 14
ADC_MAX_CODE = (1 << ADC_BITS) - 1
MAX_MONITOR_FRAME_BYTES = (
    EVENT_HEADER_BYTES
    + MAX_CHANNELS * MAX_RECORD_LENGTH * np.dtype("<u2").itemsize
)
MAX_RUNTIME_CONFIG_BYTES = 1024 * 1024
UINT32_MODULUS = 1 << 32
UINT32_HALF_RANGE = 1 << 31


class MonitorFrameError(ValueError):
    """Raised when a monitoring frame is not an exact supported event."""


class MonitorConfigError(ValueError):
    """Raised when live-monitor polarity cannot be trusted from a config."""


@dataclass(frozen=True)
class MonitorEventHeader:
    extended_ttt: int
    event_id: int
    record_length: int
    channel_mask: int
    pattern: int
    board_event_counter: int


@dataclass(frozen=True)
class DecodedMonitorFrame:
    header: MonitorEventHeader
    waveforms: Mapping[int, np.ndarray]
    frame_bytes: int


@dataclass(frozen=True)
class EventSequenceObservation:
    event_id: int
    previous_event_id: Optional[int]
    observed_subscriber_gap: int
    classification: str


class EventSequenceTracker:
    """Track gaps visible to this SUB socket, including uint32 wraparound.

    This is intentionally *not* a DAQ loss counter.  PUB/SUB HWM, ZeroMQ
    conflation, GUI decimation, a paused monitor, or transport loss can all
    cause an EventID gap that is visible only to the subscriber.
    """

    def __init__(self) -> None:
        self.reset()

    def reset(self) -> None:
        self.previous_event_id: Optional[int] = None
        self.observed_subscriber_gaps = 0
        self.duplicate_event_ids = 0
        self.sequence_discontinuities = 0
        self.observations = 0

    def start_new_observation_window(self) -> None:
        """Forget only the predecessor after an intentional monitor pause."""

        self.previous_event_id = None

    def observe(self, event_id: int) -> EventSequenceObservation:
        if isinstance(event_id, bool) or not isinstance(event_id, int):
            raise TypeError("EventID must be an integer")
        if event_id < 0 or event_id >= UINT32_MODULUS:
            raise ValueError("EventID must fit uint32")

        previous = self.previous_event_id
        self.observations += 1
        if previous is None:
            observation = EventSequenceObservation(
                event_id, None, 0, "first"
            )
        else:
            delta = (event_id - previous) & (UINT32_MODULUS - 1)
            if delta == 0:
                self.duplicate_event_ids += 1
                observation = EventSequenceObservation(
                    event_id, previous, 0, "duplicate"
                )
            elif delta < UINT32_HALF_RANGE:
                gap = delta - 1
                self.observed_subscriber_gaps += gap
                observation = EventSequenceObservation(
                    event_id,
                    previous,
                    gap,
                    "gap" if gap else "consecutive",
                )
            else:
                # A reset/backward jump must not be converted into billions of
                # alleged losses.  Start a new local sequence instead.
                self.sequence_discontinuities += 1
                observation = EventSequenceObservation(
                    event_id, previous, 0, "discontinuity"
                )

        self.previous_event_id = event_id
        return observation


@dataclass(frozen=True)
class DrainBatch:
    latest_frame: Optional[DecodedMonitorFrame]
    received_frames: int
    valid_frames: int
    malformed_frames: int
    decimated_valid_frames: int
    observed_subscriber_gaps: int
    last_decode_error: Optional[str]
    stop_reason: str


def decode_monitor_frame(frame: bytes) -> DecodedMonitorFrame:
    """Decode one exact little-endian EventHeader + waveform payload.

    Only immutable ``bytes`` are accepted.  This both matches ``pyzmq.recv``
    and prevents a caller from mutating sample memory after validation.
    """

    if not isinstance(frame, bytes):
        raise MonitorFrameError("monitor frame must be immutable bytes")
    frame_size = len(frame)
    if frame_size < EVENT_HEADER_BYTES:
        raise MonitorFrameError(
            f"truncated EventHeader: {frame_size}/{EVENT_HEADER_BYTES} bytes"
        )
    if frame_size > MAX_MONITOR_FRAME_BYTES:
        raise MonitorFrameError(
            "oversize monitor frame exceeds the supported maximum "
            f"({frame_size}>{MAX_MONITOR_FRAME_BYTES} bytes)"
        )

    unpacked = struct.unpack_from(EVENT_HEADER_FORMAT, frame, 0)
    header = MonitorEventHeader(*unpacked)

    if header.channel_mask == 0:
        raise MonitorFrameError("ChannelMask must enable at least one channel")
    unsupported = header.channel_mask & ~SUPPORTED_CHANNEL_MASK
    if unsupported:
        raise MonitorFrameError(
            f"ChannelMask contains unsupported bits: 0x{unsupported:04x}"
        )
    if not MIN_RECORD_LENGTH <= header.record_length <= MAX_RECORD_LENGTH:
        raise MonitorFrameError(
            "RecordLength is outside the supported 128..102400 range"
        )
    if header.record_length % RECORD_LENGTH_GRANULARITY:
        raise MonitorFrameError("RecordLength must be a multiple of 8")
    if header.board_event_counter > 0xFFFFFF:
        raise MonitorFrameError("BoardEventCounter exceeds the hardware 24-bit range")

    active_channels = header.channel_mask.bit_count()
    sample_count = header.record_length * active_channels
    expected_size = EVENT_HEADER_BYTES + sample_count * 2
    if frame_size < expected_size:
        raise MonitorFrameError(
            f"truncated waveform payload: {frame_size}/{expected_size} bytes"
        )
    if frame_size > expected_size:
        raise MonitorFrameError(
            f"oversize waveform payload: {frame_size}/{expected_size} bytes"
        )

    samples = np.frombuffer(
        frame,
        dtype=np.dtype("<u2"),
        count=sample_count,
        offset=EVENT_HEADER_BYTES,
    )
    maximum = int(samples.max())
    if maximum > ADC_MAX_CODE:
        bad_index = int(np.argmax(samples > ADC_MAX_CODE))
        raise MonitorFrameError(
            "waveform sample exceeds the 14-bit ADC range at payload index "
            f"{bad_index}: {int(samples[bad_index])}"
        )

    waveforms = {}
    sample_offset = 0
    for channel in range(MAX_CHANNELS):
        if not ((header.channel_mask >> channel) & 1):
            continue
        waveform = samples[
            sample_offset : sample_offset + header.record_length
        ]
        waveform.setflags(write=False)
        waveforms[channel] = waveform
        sample_offset += header.record_length

    return DecodedMonitorFrame(
        header=header,
        waveforms=MappingProxyType(waveforms),
        frame_bytes=frame_size,
    )


def drain_latest_frames(
    receive_nonblocking: Callable[[], bytes],
    sequence_tracker: EventSequenceTracker,
    *,
    max_messages: int,
    time_budget_ns: int,
    clock_ns: Callable[[], int],
    empty_exceptions: Tuple[Type[BaseException], ...] = (BlockingIOError,),
) -> DrainBatch:
    """Read a bounded batch and retain only its newest valid frame.

    Every received frame is strictly decoded and every trusted EventID is
    observed, but plotting/DSP is intentionally left to the caller for only
    ``latest_frame``.  Consequently a publisher flood cannot turn one Qt timer
    callback into an unbounded drain-and-render loop.
    """

    if isinstance(max_messages, bool) or not isinstance(max_messages, int):
        raise TypeError("max_messages must be an integer")
    if max_messages < 1:
        raise ValueError("max_messages must be at least one")
    if isinstance(time_budget_ns, bool) or not isinstance(time_budget_ns, int):
        raise TypeError("time_budget_ns must be an integer")
    if time_budget_ns < 1:
        raise ValueError("time_budget_ns must be positive")
    if not isinstance(empty_exceptions, tuple) or not all(
        isinstance(exc, type) and issubclass(exc, BaseException)
        for exc in empty_exceptions
    ):
        raise TypeError("empty_exceptions must be a tuple of exception types")

    started_ns = clock_ns()
    latest = None
    received = 0
    valid = 0
    malformed = 0
    gaps = 0
    last_error = None
    stop_reason = "empty"

    while received < max_messages:
        if received and clock_ns() - started_ns >= time_budget_ns:
            stop_reason = "time_budget"
            break
        try:
            message = receive_nonblocking()
        except empty_exceptions:
            stop_reason = "empty"
            break

        received += 1
        try:
            decoded = decode_monitor_frame(message)
        except MonitorFrameError as exc:
            malformed += 1
            last_error = str(exc)
        else:
            observation = sequence_tracker.observe(decoded.header.event_id)
            gaps += observation.observed_subscriber_gap
            latest = decoded
            valid += 1

        if clock_ns() - started_ns >= time_budget_ns:
            stop_reason = "time_budget"
            break
    else:
        stop_reason = "message_budget"

    return DrainBatch(
        latest_frame=latest,
        received_frames=received,
        valid_frames=valid,
        malformed_frames=malformed,
        decimated_valid_frames=max(0, valid - 1),
        observed_subscriber_gaps=gaps,
        last_decode_error=last_error,
        stop_reason=stop_reason,
    )


@dataclass(frozen=True)
class RuntimeConfigReference:
    path: os.PathLike[str] | str
    expected_sha256: Optional[str] = None


@dataclass(frozen=True)
class RuntimePolarity:
    polarity: str
    source_path: str
    identity: Tuple[int, int, int, int]
    sha256: str
    authenticated: bool


def load_runtime_polarity(
    config_path: os.PathLike[str] | str,
    *,
    expected_sha256: Optional[str] = None,
) -> RuntimePolarity:
    """Read TriggerPolarity from one stable, bounded regular config file."""

    if expected_sha256 is not None:
        if (
            not isinstance(expected_sha256, str)
            or len(expected_sha256) != 64
            or any(character not in "0123456789abcdef" for character in expected_sha256)
        ):
            raise MonitorConfigError(
                "expected runtime config SHA-256 must be 64 lowercase hex characters"
            )

    try:
        path = Path(config_path).expanduser().resolve(strict=True)
    except (OSError, RuntimeError, TypeError, ValueError) as exc:
        raise MonitorConfigError(
            f"runtime config path cannot be resolved: {config_path!r}"
        ) from exc

    try:
        with path.open("rb") as config_file:
            before = os.fstat(config_file.fileno())
            if not stat.S_ISREG(before.st_mode):
                raise MonitorConfigError("runtime config is not a regular file")
            if before.st_size > MAX_RUNTIME_CONFIG_BYTES:
                raise MonitorConfigError(
                    "runtime config exceeds the 1 MiB monitor safety limit"
                )
            raw = config_file.read(MAX_RUNTIME_CONFIG_BYTES + 1)
            after = os.fstat(config_file.fileno())
    except MonitorConfigError:
        raise
    except OSError as exc:
        raise MonitorConfigError(
            f"runtime config cannot be read: {path} ({exc})"
        ) from exc

    before_identity = (
        before.st_dev,
        before.st_ino,
        before.st_size,
        before.st_mtime_ns,
    )
    after_identity = (
        after.st_dev,
        after.st_ino,
        after.st_size,
        after.st_mtime_ns,
    )
    if before_identity != after_identity or len(raw) != before.st_size:
        raise MonitorConfigError("runtime config changed while it was read")
    if len(raw) > MAX_RUNTIME_CONFIG_BYTES:
        raise MonitorConfigError(
            "runtime config exceeds the 1 MiB monitor safety limit"
        )
    try:
        text = raw.decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise MonitorConfigError("runtime config is not valid UTF-8") from exc

    actual_sha256 = hashlib.sha256(raw).hexdigest()
    if expected_sha256 is not None and not hmac.compare_digest(
        actual_sha256, expected_sha256
    ):
        raise MonitorConfigError(
            "runtime config SHA-256 does not match the launched run context"
        )

    parser = configparser.ConfigParser(
        interpolation=None,
        strict=True,
        inline_comment_prefixes=("#", ";"),
    )
    parser.optionxform = str
    try:
        parser.read_string(text, source=str(path))
        raw_polarity = parser["Digitizer"]["TriggerPolarity"].strip()
    except (configparser.Error, KeyError) as exc:
        raise MonitorConfigError(
            "runtime config requires [Digitizer] TriggerPolarity"
        ) from exc

    if raw_polarity == "1":
        polarity = "falling"
    elif raw_polarity == "0":
        polarity = "rising"
    else:
        raise MonitorConfigError("TriggerPolarity must be exactly 0 or 1")

    return RuntimePolarity(
        polarity=polarity,
        source_path=str(path),
        identity=after_identity,
        sha256=actual_sha256,
        authenticated=expected_sha256 is not None,
    )


@dataclass(frozen=True)
class MonitorDspResult:
    baseline: float
    baseline_samples: int
    pulse_index: int
    charge: float
    pulse_height: float


def analyze_monitor_waveform(
    waveform: Sequence[int] | np.ndarray,
    polarity: str,
) -> MonitorDspResult:
    """Compute monitor-only charge/height in the configured pulse direction."""

    if polarity not in ("falling", "rising"):
        raise ValueError("polarity must be 'falling' or 'rising'")
    samples = np.asarray(waveform)
    if samples.ndim != 1 or samples.size < 21:
        raise ValueError("monitor DSP requires at least 21 one-dimensional samples")
    if samples.dtype.kind not in ("i", "u"):
        raise ValueError("monitor DSP samples must be integers")
    minimum = int(samples.min())
    maximum = int(samples.max())
    if minimum < 0 or maximum > ADC_MAX_CODE:
        raise ValueError("monitor DSP sample is outside the 14-bit ADC range")

    if polarity == "falling":
        pulse_index = int(np.argmin(samples))
    else:
        pulse_index = int(np.argmax(samples))

    if pulse_index > 10:
        baseline_samples = min(samples.size // 4, pulse_index - 5)
    else:
        baseline_samples = 10
    baseline_samples = min(samples.size, max(5, int(baseline_samples)))
    baseline = float(np.mean(samples[:baseline_samples], dtype=np.float64))

    if polarity == "falling":
        directed = baseline - samples
        pulse_height = max(0.0, baseline - minimum)
    else:
        directed = samples - baseline
        pulse_height = max(0.0, maximum - baseline)
    charge = float(np.maximum(directed, 0.0).sum(dtype=np.float64))

    return MonitorDspResult(
        baseline=baseline,
        baseline_samples=baseline_samples,
        pulse_index=pulse_index,
        charge=charge,
        pulse_height=pulse_height,
    )
