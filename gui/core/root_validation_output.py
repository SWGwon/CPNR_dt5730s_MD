"""Qt-free parsing helpers for the ROOT validation process.

The validator deliberately keeps progress on stderr and emits one JSON
document on stdout.  Keeping these helpers independent from Qt makes the
process protocol straightforward to regression-test.
"""

from __future__ import annotations

import json
import math
import re
from collections.abc import Mapping
from pathlib import Path
from typing import Any


_ANSI_ESCAPE = re.compile(
    r"\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])|\r"
)
_PROGRESS_PATTERN = re.compile(
    r"^\s*\[ValidationProgress\]\s*"
    r"(?P<percent>[+-]?(?:\d+(?:\.\d*)?|\.\d+))\s*%?\s*"
    r"\|\s*(?P<stage>.*?)\s*$"
)
_JSON_PREFIXES = ("REPORT_JSON:", "[ValidationResult]")


def _reject_nonfinite_json(value: str):
    raise ValueError(f"non-finite JSON number is not allowed: {value}")


def strip_ansi(value: str) -> str:
    """Remove terminal colour/control sequences from process output."""

    return _ANSI_ESCAPE.sub("", value)


def parse_validation_progress(line: str) -> tuple[float, str] | None:
    """Parse one validator progress line.

    Percentages are clamped to the QProgressBar domain so a defensive GUI
    never displays an invalid value if a future validator briefly reports an
    overrun while finalising its report.
    """

    if not isinstance(line, str):
        return None
    match = _PROGRESS_PATTERN.match(strip_ansi(line).strip())
    if match is None:
        return None
    try:
        percent = float(match.group("percent"))
    except ValueError:
        return None
    if not math.isfinite(percent):
        return None
    stage = match.group("stage").strip()
    if not stage:
        return None
    return min(100.0, max(0.0, percent)), stage


def _remove_json_prefix(line: str) -> str:
    candidate = strip_ansi(line).strip()
    for prefix in _JSON_PREFIXES:
        if candidate.startswith(prefix):
            candidate = candidate[len(prefix):].lstrip(" :\t")
            break
    return candidate


def parse_validation_json_line(line: str) -> dict[str, Any] | None:
    """Return a report object when *line* is a complete JSON object."""

    if not isinstance(line, str):
        return None
    candidate = _remove_json_prefix(line)
    if not candidate:
        return None
    try:
        report = json.loads(candidate, parse_constant=_reject_nonfinite_json)
    except (TypeError, ValueError, json.JSONDecodeError):
        return None
    return report if isinstance(report, dict) else None


def parse_validation_output(output: str) -> dict[str, Any]:
    """Parse the validator's complete stdout JSON document.

    The documented protocol is a single JSON document.  The line and raw
    decoder fallbacks are intentionally retained for compatibility with a
    prefixed result line or an accidental informational line from ROOT.
    """

    if not isinstance(output, str):
        raise ValueError("validator stdout must be text")
    candidate = _remove_json_prefix(output)
    try:
        report = json.loads(candidate, parse_constant=_reject_nonfinite_json)
        if isinstance(report, dict):
            return report
    except (TypeError, ValueError, json.JSONDecodeError):
        pass

    for line in reversed(output.splitlines()):
        report = parse_validation_json_line(line)
        if report is not None:
            return report

    clean = strip_ansi(output)
    decoder = json.JSONDecoder(parse_constant=_reject_nonfinite_json)
    best_report: dict[str, Any] | None = None
    best_span = -1
    for index, character in enumerate(clean):
        if character != "{":
            continue
        try:
            value, end = decoder.raw_decode(clean, index)
        except (json.JSONDecodeError, ValueError):
            continue
        span = end - index
        if isinstance(value, dict) and span > best_span:
            best_report = value
            best_span = span
    if best_report is not None:
        return best_report
    raise ValueError("validator stdout did not contain a JSON object")


def normalize_status(value: object) -> str:
    """Map compatible status spellings to the GUI's stable vocabulary."""

    status = str(value or "INFO").strip().upper()
    aliases = {
        "OK": "PASS",
        "SUCCESS": "PASS",
        "PASSED": "PASS",
        "WARNING": "WARN",
        "FAILED": "FAIL",
        "ERROR": "FAIL",
        "SKIPPED": "SKIP",
        "NOT_APPLICABLE": "SKIP",
        "N/A": "SKIP",
    }
    return aliases.get(status, status)


def status_counts(report: Mapping[str, Any]) -> dict[str, int]:
    """Return PASS/WARN/FAIL/SKIP counts, deriving them when needed."""

    supplied = report.get("counts")
    if isinstance(supplied, Mapping):
        result: dict[str, int] = {}
        for status in ("pass", "warn", "fail", "skip"):
            try:
                result[status] = max(0, int(supplied.get(status, 0)))
            except (TypeError, ValueError):
                result[status] = 0
        return result

    result = {"pass": 0, "warn": 0, "fail": 0, "skip": 0}
    checks = report.get("checks")
    if not isinstance(checks, list):
        return result
    for check in checks:
        if not isinstance(check, Mapping):
            continue
        status = normalize_status(check.get("status"))
        key = status.lower()
        if key in result:
            result[key] += 1
    return result


def flatten_mapping(
    value: Mapping[str, Any], prefix: str = ""
) -> list[tuple[str, Any]]:
    """Flatten nested report fields for a future-proof metrics table."""

    flattened: list[tuple[str, Any]] = []
    for key in sorted(value, key=str):
        item = value[key]
        name = f"{prefix}.{key}" if prefix else str(key)
        if isinstance(item, Mapping):
            flattened.extend(flatten_mapping(item, name))
        else:
            flattened.append((name, item))
    return flattened


def display_value(value: object) -> str:
    """Render JSON-compatible values without losing false/zero/null."""

    if value is None:
        return "—"
    if isinstance(value, bool):
        return "Yes" if value else "No"
    if isinstance(value, float):
        if not math.isfinite(value):
            return str(value)
        return f"{value:.8g}"
    if isinstance(value, (list, dict)):
        return json.dumps(value, ensure_ascii=False, sort_keys=True)
    return str(value)


def validate_report_envelope(
    report: Mapping[str, Any],
    *,
    input_path: str,
    validator_path: str,
    validator_sha256: str,
) -> None:
    """Authenticate a report against the exact selected input and worker."""

    if not isinstance(report, Mapping):
        raise ValueError("validator report root must be an object")
    if report.get("schema_version") != 1:
        raise ValueError(
            "unsupported validator report schema_version: "
            f"{report.get('schema_version')!r}"
        )
    input_info = report.get("input")
    if not isinstance(input_info, Mapping) or not input_info.get("path"):
        raise ValueError("validator report is missing input.path")
    reported_input = Path(str(input_info["path"])).expanduser().resolve()
    expected_input = Path(input_path).expanduser().resolve()
    if reported_input != expected_input:
        raise ValueError(
            "validator report input does not match the selected file: "
            f"reported={reported_input}, selected={expected_input}"
        )
    validator = report.get("validator")
    if not isinstance(validator, Mapping):
        raise ValueError("validator report is missing validator identity")
    reported_validator = Path(
        str(validator.get("executable_path", ""))
    ).expanduser().resolve()
    expected_validator = Path(validator_path).expanduser().resolve()
    if reported_validator != expected_validator:
        raise ValueError(
            "validator executable path changed across launch: "
            f"reported={reported_validator}, expected={expected_validator}"
        )
    if validator.get("executable_sha256") != validator_sha256:
        raise ValueError("validator executable SHA-256 changed across launch")
    if not isinstance(report.get("checks"), list):
        raise ValueError("validator report is missing checks[]")
    if not isinstance(report.get("channels"), list):
        raise ValueError("validator report is missing channels[]")
