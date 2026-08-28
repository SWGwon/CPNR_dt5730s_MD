import re
from typing import Any, Dict, Optional


_DECIMAL = r"([0-9]+(?:\.[0-9]+)?)"
_LIVE_PATTERNS = {
    "real_time": (re.compile(r"\bRealTime:\s*" + _DECIMAL), " s"),
    "window_load": (re.compile(r"\bWindow:\s*" + _DECIMAL), " %"),
    "events": (re.compile(r"\bEvents:\s*([0-9]+)"), ""),
    "rate": (re.compile(r"\bRate:\s*" + _DECIMAL), " Hz"),
    "speed": (re.compile(r"\bSpeed:\s*" + _DECIMAL), " MB/s"),
    "publisher_send_failures": (
        re.compile(r"\bPubSendFail:\s*([0-9]+)"),
        "",
    ),
}
_TEMPERATURE_PATTERN = re.compile(r"\[STATUS\] TEMP:\s*" + _DECIMAL)
_LED_PATTERN = re.compile(
    r"\[STATUS\] LED:\s*LOCK=(\d),\s*CLKEXT=(\d),\s*READY=(\d),"
    r"\s*RUN=(\d),\s*TRG=(\d),\s*DRDY=(\d),\s*FULL=(\d)"
)


def parse_live_daq_stats(line: str) -> Dict[str, str]:
    """Extract DAQ statistics even when status records share the same line."""
    if "[LIVE DAQ]" not in line:
        return {}

    stats: Dict[str, str] = {}
    for key, (pattern, suffix) in _LIVE_PATTERNS.items():
        match = pattern.search(line)
        if match:
            stats[key] = f"{match.group(1)}{suffix}"
    return stats


def parse_temperature(line: str) -> Optional[float]:
    match = _TEMPERATURE_PATTERN.search(line)
    return float(match.group(1)) if match else None


def parse_led_status(line: str) -> Optional[Dict[str, int]]:
    match = _LED_PATTERN.search(line)
    if not match:
        return None
    return {
        "PLL LOCK": int(match.group(1)),
        "CLK EXT": int(match.group(2)),
        "BOARD READY": int(match.group(3)),
        "RUN": int(match.group(4)),
        "TRG": int(match.group(5)),
        "DRDY": int(match.group(6)),
        "FULL": int(match.group(7)),
    }


def parse_drop_count(value: Any) -> int:
    """Return a leading non-negative integer, or zero for malformed input."""
    match = re.match(r"\s*([0-9]+)", str(value))
    return int(match.group(1)) if match else 0
