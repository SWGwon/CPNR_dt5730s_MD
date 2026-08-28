import json
import sys
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
GUI_DIR = PROJECT_ROOT / "gui"
if str(GUI_DIR) not in sys.path:
    sys.path.insert(0, str(GUI_DIR))

from core.root_validation_output import (  # noqa: E402
    parse_validation_json_line,
    parse_validation_output,
    parse_validation_progress,
    status_counts,
    validate_report_envelope,
)


def sample_report():
    return {
        "schema_version": 1,
        "overall_status": "PASS",
        "file": "/data/run021_prod.root",
        "summary": {"entries": 200000},
        "checks": [
            {
                "id": "tree.entries",
                "status": "PASS",
                "message": "Tree and metadata event counts agree",
            }
        ],
        "channels": [
            {"channel": 0, "status": "PASS"},
            {"channel": 1, "status": "PASS"},
        ],
        "counts": {"pass": 1, "warn": 0, "fail": 0},
    }


INPUT_IDENTITY = {
    "device": 2049,
    "inode": 123456,
    "mode": 0o100440,
    "size_bytes": 987654,
    "mtime_seconds": 1700000000,
    "mtime_nanoseconds": 123456789,
    "ctime_seconds": 1700000001,
    "ctime_nanoseconds": 987654321,
}


def envelope_report(*, max_events=None):
    report = sample_report()
    report["input"] = {
        "path": "/data/run021_prod.root",
        "max_events": max_events,
        "identity_start": dict(INPUT_IDENTITY),
    }
    report["validator"] = {
        "executable_path": "/opt/cpnr/root_validate_dt5730",
        "executable_sha256": "a" * 64,
    }
    return report


class RootValidationOutputTests(unittest.TestCase):
    def test_progress_parser_accepts_ansi_spacing_and_fractional_values(self):
        self.assertEqual(
            parse_validation_progress(
                "\x1b[36m[ValidationProgress] 37.5% | "
                "Scanning event branches\x1b[0m"
            ),
            (37.5, "Scanning event branches"),
        )
        self.assertEqual(
            parse_validation_progress(
                "  [ValidationProgress]   8 % | Opening ROOT file  "
            ),
            (8.0, "Opening ROOT file"),
        )

    def test_progress_is_clamped_and_malformed_values_are_ignored(self):
        self.assertEqual(
            parse_validation_progress(
                "[ValidationProgress] 101.25% | Finalizing"
            ),
            (100.0, "Finalizing"),
        )
        for line in (
            "unrelated diagnostic",
            "[ValidationProgress] nan% | Scan",
            "[ValidationProgress] inf% | Scan",
            "[ValidationProgress] 50%",
            "[ValidationProgress] 50% |   ",
        ):
            with self.subTest(line=line):
                self.assertIsNone(parse_validation_progress(line))

    def test_json_line_parser_accepts_plain_and_documented_prefixes(self):
        report = sample_report()
        encoded = json.dumps(report)
        for line in (
            encoded,
            f"REPORT_JSON: {encoded}",
            f"[ValidationResult] {encoded}",
            f"\x1b[32mREPORT_JSON:\x1b[0m {encoded}",
        ):
            with self.subTest(line=line[:40]):
                self.assertEqual(parse_validation_json_line(line), report)

    def test_json_line_parser_rejects_malformed_or_non_object_payloads(self):
        for line in (
            "",
            "not json",
            "REPORT_JSON: {broken",
            "[]",
            '"a string"',
            "[ValidationResult] null",
        ):
            with self.subTest(line=line):
                self.assertIsNone(parse_validation_json_line(line))

    def test_full_output_parser_finds_report_among_diagnostics(self):
        report = sample_report()
        self.assertEqual(
            parse_validation_output(json.dumps(report, indent=2)), report
        )
        output = (
            "ROOT startup diagnostic\n"
            "[ValidationProgress] 50% | Scan\n"
            f"REPORT_JSON: {json.dumps(report)}\n"
        )
        self.assertEqual(parse_validation_output(output), report)

    def test_status_counts_make_skipped_checks_visible(self):
        report = sample_report()
        report.pop("counts")
        report["checks"].append({"status": "SKIP", "name": "partial"})
        self.assertEqual(
            status_counts(report),
            {"pass": 1, "warn": 0, "fail": 0, "skip": 1},
        )

    def test_report_envelope_authenticates_input_and_validator(self):
        report = envelope_report()
        validate_report_envelope(
            report,
            input_path="/data/run021_prod.root",
            max_events=0,
            input_identity_start=INPUT_IDENTITY,
            validator_path="/opt/cpnr/root_validate_dt5730",
            validator_sha256="a" * 64,
        )
        for field, replacement in (
            ("executable_path", "/tmp/swapped-validator"),
            ("executable_sha256", "b" * 64),
        ):
            with self.subTest(field=field):
                changed = json.loads(json.dumps(report))
                changed["validator"][field] = replacement
                with self.assertRaises(ValueError):
                    validate_report_envelope(
                        changed,
                        input_path="/data/run021_prod.root",
                        max_events=0,
                        input_identity_start=INPUT_IDENTITY,
                        validator_path="/opt/cpnr/root_validate_dt5730",
                        validator_sha256="a" * 64,
                    )

    def test_report_envelope_binds_exact_requested_max_events(self):
        report = envelope_report(max_events=25000)
        validate_report_envelope(
            report,
            input_path="/data/run021_prod.root",
            max_events=25000,
            input_identity_start=INPUT_IDENTITY,
            validator_path="/opt/cpnr/root_validate_dt5730",
            validator_sha256="a" * 64,
        )

        for reported_value in (None, 24999, False, 25000.0):
            with self.subTest(reported=reported_value):
                changed = json.loads(json.dumps(report))
                changed["input"]["max_events"] = reported_value
                with self.assertRaises(ValueError):
                    validate_report_envelope(
                        changed,
                        input_path="/data/run021_prod.root",
                        max_events=25000,
                        input_identity_start=INPUT_IDENTITY,
                        validator_path="/opt/cpnr/root_validate_dt5730",
                        validator_sha256="a" * 64,
                    )

        missing = json.loads(json.dumps(report))
        del missing["input"]["max_events"]
        with self.assertRaises(ValueError):
            validate_report_envelope(
                missing,
                input_path="/data/run021_prod.root",
                max_events=25000,
                input_identity_start=INPUT_IDENTITY,
                validator_path="/opt/cpnr/root_validate_dt5730",
                validator_sha256="a" * 64,
            )

    def test_report_envelope_binds_every_input_start_identity_field(self):
        report = envelope_report()
        for field in INPUT_IDENTITY:
            with self.subTest(field=field):
                changed = json.loads(json.dumps(report))
                changed["input"]["identity_start"][field] += 1
                with self.assertRaisesRegex(ValueError, field):
                    validate_report_envelope(
                        changed,
                        input_path="/data/run021_prod.root",
                        max_events=0,
                        input_identity_start=INPUT_IDENTITY,
                        validator_path="/opt/cpnr/root_validate_dt5730",
                        validator_sha256="a" * 64,
                    )

        missing = json.loads(json.dumps(report))
        del missing["input"]["identity_start"]
        with self.assertRaises(ValueError):
            validate_report_envelope(
                missing,
                input_path="/data/run021_prod.root",
                max_events=0,
                input_identity_start=INPUT_IDENTITY,
                validator_path="/opt/cpnr/root_validate_dt5730",
                validator_sha256="a" * 64,
            )


if __name__ == "__main__":
    unittest.main()
