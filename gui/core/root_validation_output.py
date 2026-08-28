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
    max_events: int,
    input_identity_start: Mapping[str, int],
    input_identity_end: Mapping[str, int],
    validator_path: str,
    validator_sha256: str,
    raw_fidelity_requested: bool = False,
) -> None:
    """Authenticate a report against the exact launch request and identities."""

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
    if isinstance(max_events, bool) or not isinstance(max_events, int):
        raise ValueError("requested max_events must be an integer")
    if max_events < 0:
        raise ValueError("requested max_events cannot be negative")
    if "max_events" not in input_info:
        raise ValueError("validator report is missing input.max_events")
    reported_max_events = input_info["max_events"]
    expected_max_events = None if max_events == 0 else max_events
    valid_reported_max_events = (
        reported_max_events is None
        if expected_max_events is None
        else (
            isinstance(reported_max_events, int)
            and not isinstance(reported_max_events, bool)
            and reported_max_events == expected_max_events
        )
    )
    if not valid_reported_max_events:
        raise ValueError(
            "validator report max_events does not match the launch request: "
            f"reported={reported_max_events!r}, "
            f"requested={expected_max_events!r}"
        )
    if not isinstance(raw_fidelity_requested, bool):
        raise ValueError("requested raw_fidelity flag must be boolean")
    reported_raw_fidelity = input_info.get("raw_fidelity_requested")
    if (
        not isinstance(reported_raw_fidelity, bool)
        or reported_raw_fidelity != raw_fidelity_requested
    ):
        raise ValueError(
            "validator report RAW fidelity mode does not match the launch "
            f"request: reported={reported_raw_fidelity!r}, "
            f"requested={raw_fidelity_requested!r}"
        )

    identity_fields = (
        "device",
        "inode",
        "mode",
        "size_bytes",
        "mtime_seconds",
        "mtime_nanoseconds",
        "ctime_seconds",
        "ctime_nanoseconds",
    )
    def require_identity(label, reported_value, expected_value):
        if not isinstance(expected_value, Mapping):
            raise ValueError(f"input {label} identity is unavailable")
        if not isinstance(reported_value, Mapping):
            raise ValueError(
                f"validator report is missing input.identity_{label}"
            )
        for field in identity_fields:
            expected_field = expected_value.get(field)
            if (
                isinstance(expected_field, bool)
                or not isinstance(expected_field, int)
            ):
                raise ValueError(
                    f"input {label} identity has invalid {field}: "
                    f"{expected_field!r}"
                )
            reported_field = reported_value.get(field)
            if (
                isinstance(reported_field, bool)
                or not isinstance(reported_field, int)
                or reported_field != expected_field
            ):
                raise ValueError(
                    "validator report input identity does not match "
                    f"{label} stat: field={field}, "
                    f"reported={reported_field!r}, "
                    f"expected={expected_field!r}"
                )

    require_identity(
        "start", input_info.get("identity_start"), input_identity_start
    )
    require_identity(
        "end", input_info.get("identity_end"), input_identity_end
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

    checks = report["checks"]
    derived_counts = {"pass": 0, "warn": 0, "fail": 0, "skip": 0}
    for index, check in enumerate(checks):
        if not isinstance(check, Mapping):
            raise ValueError(f"validator check {index} is not an object")
        status = normalize_status(check.get("status"))
        key = status.lower()
        if key not in derived_counts:
            raise ValueError(
                f"validator check {index} has unsupported status {status!r}"
            )
        derived_counts[key] += 1

    supplied_counts = report.get("counts")
    if not isinstance(supplied_counts, Mapping):
        raise ValueError("validator report is missing counts")
    for key, expected_count in derived_counts.items():
        supplied_count = supplied_counts.get(key)
        if (
            isinstance(supplied_count, bool)
            or not isinstance(supplied_count, int)
            or supplied_count != expected_count
        ):
            raise ValueError(
                "validator report count is inconsistent with checks[]: "
                f"{key}={supplied_count!r}, expected={expected_count}"
            )

    analysis = report.get("analysis")
    if not isinstance(analysis, Mapping):
        raise ValueError("validator report is missing analysis")
    completed = analysis.get("completed")
    cancelled = analysis.get("cancelled")
    if not isinstance(completed, bool) or not isinstance(cancelled, bool):
        raise ValueError(
            "validator analysis completed/cancelled flags must be boolean"
        )
    if completed == cancelled:
        raise ValueError(
            "validator analysis lifecycle flags are inconsistent"
        )
    expected_overall = (
        "CANCELLED"
        if cancelled
        else "FAIL"
        if derived_counts["fail"]
        else "WARN"
        if derived_counts["warn"]
        else "PASS"
    )
    actual_overall = normalize_status(report.get("overall_status"))
    if actual_overall != expected_overall:
        raise ValueError(
            "validator overall_status is inconsistent with lifecycle/checks: "
            f"reported={actual_overall}, expected={expected_overall}"
        )

    category_sets = {
        "data_integrity": {
            "integrity", "schema", "physics_sanity", "summary", "operation"
        },
        "provenance": {"provenance"},
        "trigger_and_quality": {"trigger", "data_quality", "operation"},
    }
    supplied_domains = report.get("domain_status")
    if not isinstance(supplied_domains, Mapping):
        raise ValueError("validator report is missing domain_status")
    severity = {"PASS": 0, "WARN": 1, "FAIL": 2}
    for domain, categories in category_sets.items():
        worst = -1
        for check in checks:
            if check.get("category") not in categories:
                continue
            status = normalize_status(check.get("status"))
            if status in severity:
                worst = max(worst, severity[status])
        expected_domain = (
            "FAIL" if worst == 2 else "WARN" if worst == 1
            else "PASS" if worst == 0 else "SKIP"
        )
        actual_domain = normalize_status(supplied_domains.get(domain))
        if actual_domain != expected_domain:
            raise ValueError(
                f"validator domain_status.{domain} is inconsistent: "
                f"reported={actual_domain}, expected={expected_domain}"
            )
