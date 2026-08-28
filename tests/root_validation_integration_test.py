"""End-to-end contract tests for the production ROOT validator.

CMake supplies the validator, production converter, DAQ configuration, and
ROOT executable.  Every fixture is created in a temporary directory.  The
validator is then required to leave each input byte-for-byte unchanged.
"""

import hashlib
import json
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


VALIDATOR = Path(sys.argv[1]).resolve()
PRODUCTION = Path(sys.argv[2]).resolve()
CONFIG = Path(sys.argv[3]).resolve()
ROOT = Path(sys.argv[4]).resolve()
# Keep unittest from interpreting CMake-provided paths as test names.
sys.argv = [sys.argv[0]]


REPORT_KEYS = {
    "schema_version",
    "overall_status",
    "summary",
    "checks",
    "channels",
}
OVERALL_STATUSES = {"PASS", "WARN", "FAIL"}
CHECK_STATUSES = OVERALL_STATUSES | {"SKIP"}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_only_identity(path: Path) -> tuple[int, int, int, int, int, int, str]:
    status = path.stat()
    return (
        status.st_dev,
        status.st_ino,
        status.st_mode,
        status.st_size,
        status.st_mtime_ns,
        status.st_ctime_ns,
        sha256(path),
    )


def metadata_for(
    raw: Path,
    metadata: Path,
    run_number: int,
    *,
    config: Path = CONFIG,
    polarity: str = "falling",
    pair_logic: str = "AND",
) -> dict:
    channels = []
    for channel, baseline in ((0, 16164.0), (1, 16255.0)):
        written = round(baseline) + (-8 if polarity == "falling" else 8)
        channels.append({
            "channel": channel,
            "trigger_enabled": True,
            "input_range_register": 0x1028 + 0x100 * channel,
            "input_range_readback": 0,
            "requested_dc_offset": 6554,
            "readback_dc_offset": 6554,
            "polarity_readback": polarity,
            "threshold_mode": "baseline_relative_mv",
            "measured_baseline_adc": baseline,
            "requested_threshold_mv": 1.0,
            "delta_adc": 8,
            "written_threshold_adc": written,
            "readback_threshold_adc": written,
            "effective_threshold_mv": (
                abs(baseline - written) * 2000.0 / 16384.0
            ),
        })
    for channel, baseline in ((2, 8192.0), (3, 8192.0)):
        channels.append({
            "channel": channel,
            "trigger_enabled": False,
            "input_range_register": 0x1028 + 0x100 * channel,
            "input_range_readback": 0,
            "requested_dc_offset": 6554,
            "readback_dc_offset": 6554,
            "polarity_readback": polarity,
            "threshold_mode": "not_used_record_only",
            "measured_baseline_adc": baseline,
            "requested_threshold_mv": None,
            "delta_adc": None,
            "written_threshold_adc": None,
            "readback_threshold_adc": None,
            "effective_threshold_mv": None,
        })
    return {
        "schema_version": 1,
        "run_number": run_number,
        "acquisition_status": "completed",
        "failure_reason": None,
        "created_unix_time": 1_788_000_000,
        "raw_output_path": str(raw),
        "raw_output_size_bytes": raw.stat().st_size,
        "raw_output_sha256": sha256(raw),
        "metadata_path": str(metadata),
        "config_path": str(config),
        "config_sha256": sha256(config),
        "source_config_path": str(config),
        "binary_path": str(PRODUCTION),
        "binary_sha256": sha256(PRODUCTION),
        "git_commit": "root-validator-integration-fixture",
        "build_timestamp": "root-validator-integration-fixture",
        "hardware": {
            "model": "FIXTURE-DT5730S",
            "serial_number": 5730,
            "roc_firmware": "fixture",
            "amc_firmware": "fixture",
            "input_range_mvpp": 2000,
            "adc_bits": 14,
            "dc_offset_dac_bits": 16,
            "clock_source": 0,
            "clock_source_readback": 0,
            "run_sync_mode": 0,
            "run_sync_mode_readback": 0,
            "trigger_polarity": polarity,
            "record_mask": 15,
            "record_mask_readback": 15,
            "record_length": 512,
            "post_trigger_percent": 60,
            "external_trigger_mode": 0,
            "self_trigger_mode": 1,
            "self_trigger_mask": 3,
            "pair_logic": pair_logic,
            "explicit_trigger_routing": True,
            "global_trigger_mask_readback": 1,
            "pair_logic_readback": [
                4 if pair_logic == "AND" else 7,
                0,
                0,
                0,
            ],
        },
        "channels": channels,
    }


def write_raw_fixture(
    path: Path,
    event_count: int = 256,
    *,
    overlapping_trigger_pulses: bool = True,
    polarity: str = "falling",
    pulse_amplitude: int = 16,
    board_counters: list[int] | tuple[int, ...] | None = None,
) -> None:
    record_length = 512
    channel_mask = 0xF
    baselines = (16164, 16255, 8192, 8192)
    if board_counters is not None and len(board_counters) != event_count:
        raise ValueError("board_counters length must equal event_count")
    with path.open("wb") as stream:
        for event_id in range(event_count):
            stream.write(struct.pack(
                "<QIIHHI",
                event_id * 1000,  # 8 us between triggers, > 1024 ns record.
                event_id,
                record_length,
                channel_mask,
                0,
                event_id if board_counters is None
                else board_counters[event_id],
            ))
            for channel, baseline in enumerate(baselines):
                waveform = [baseline] * record_length
                if channel in (0, 1):
                    # Place a clean falling pulse after the 128-sample baseline
                    # region.  It exceeds the calibrated 8-ADC discriminator
                    # distance on both sides of the configured AND pair.
                    pulse_start = (
                        240 if overlapping_trigger_pulses
                        else 220 + channel * 40
                    )
                    pulse_value = baseline + (
                        -pulse_amplitude
                        if polarity == "falling"
                        else pulse_amplitude
                    )
                    waveform[pulse_start:pulse_start + 8] = [pulse_value] * 8
                stream.write(struct.pack(
                    f"<{record_length}H", *waveform
                ))


def run_converter(
    raw: Path,
    metadata: Path,
    output: Path,
    run: int,
    *,
    config: Path = CONFIG,
    save_waveforms: bool = False,
):
    command = [
        str(PRODUCTION),
        "-i", str(raw),
        "-c", str(config),
        "-m", str(metadata),
        "-r", str(run),
        "-o", str(output),
    ]
    if save_waveforms:
        command.append("-w")
    return subprocess.run(
        command,
        text=True,
        capture_output=True,
        check=False,
    )


def run_validator(root_file: Path, *, max_events: int | None = None):
    command = [str(VALIDATOR), "-i", str(root_file)]
    if max_events is not None:
        command.extend(["--max-events", str(max_events)])
    return subprocess.run(
        command,
        text=True,
        capture_output=True,
        check=False,
    )


def rewrite_board_counters(
    source: Path, destination: Path, counters: list[int]
) -> None:
    rendered_counters = ", ".join(str(value) for value in counters)
    expression = f'''
TFile input({json.dumps(str(source))}, "READ");
auto *source_tree = dynamic_cast<TTree*>(input.Get("phys_tree"));
if (!source_tree || source_tree->GetEntries() != {len(counters)}) gSystem->Exit(1);
source_tree->SetBranchStatus("BoardEventCounter", 0);
TFile output({json.dumps(str(destination))}, "RECREATE");
TIter next_key(input.GetListOfKeys());
while (auto *key = dynamic_cast<TKey*>(next_key())) {{
  if (TString(key->GetName()) == "phys_tree") continue;
  auto *object = key->ReadObj();
  if (!object) gSystem->Exit(2);
  output.cd();
  object->Write(key->GetName());
  delete object;
}}
input.cd();
source_tree->SetBranchStatus("BoardEventCounter", 0);
output.cd();
auto *destination_tree = source_tree->CloneTree(0);
if (!destination_tree) gSystem->Exit(3);
UInt_t replacement_counter = 0;
destination_tree->Branch(
    "BoardEventCounter", &replacement_counter, "BoardEventCounter/i");
std::vector<UInt_t> counters = {{{rendered_counters}}};
for (Long64_t entry = 0; entry < source_tree->GetEntries(); ++entry) {{
  if (source_tree->GetEntry(entry) <= 0) gSystem->Exit(4);
  replacement_counter = counters.at(static_cast<std::size_t>(entry));
  destination_tree->Fill();
}}
destination_tree->Write("phys_tree", TObject::kOverwrite);
output.Close();
input.Close();
'''
    result = subprocess.run(
        [str(ROOT), "-l", "-b", "-q", "-e", expression],
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(result.stdout + result.stderr)


def decode_report(result: subprocess.CompletedProcess) -> dict:
    try:
        report = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise AssertionError(
            "validator stdout is not a single JSON document:\n"
            f"stdout={result.stdout!r}\nstderr={result.stderr!r}"
        ) from error
    if not isinstance(report, dict):
        raise AssertionError(f"validator report is not an object: {report!r}")
    return report


def make_legacy_root(
    directory: Path,
    output: Path,
    *,
    malformed_event_id_array: bool = False,
) -> None:
    macro = directory / "make_legacy.C"
    macro.write_text(
        r'''
#include <TFile.h>
#include <TParameter.h>
#include <TString.h>
#include <TTree.h>

void make_legacy(const char *path, bool malformed_event_id_array = false) {
    TFile output(path, "RECREATE");
    UInt_t event_id = 0;
    UInt_t event_id_array[4] = {};
    ULong64_t sync_time = 0;
    UShort_t channel_mask = 15;
    UInt_t record_length = 512;
    double charge[8] = {};
    double pulse_height[8] = {};
    double pulse_start[8] = {};
    double baseline[8] = {};

    TTree tree("phys_tree", "DT5730 Physics Data");
    if (malformed_event_id_array) {
        tree.Branch("EventID", event_id_array, "EventID[4]/i");
    } else {
        tree.Branch("EventID", &event_id, "EventID/i");
    }
    tree.Branch("SyncTime_TTT", &sync_time, "SyncTime_TTT/l");
    tree.Branch("ChannelMask", &channel_mask, "ChannelMask/s");
    tree.Branch("RecordLength", &record_length, "RecordLength/i");
    for (int channel = 0; channel < 8; ++channel) {
        tree.Branch(Form("Charge_CH%d", channel), &charge[channel],
                    Form("Charge_CH%d/D", channel));
        tree.Branch(Form("PulseHeight_CH%d", channel),
                    &pulse_height[channel],
                    Form("PulseHeight_CH%d/D", channel));
        tree.Branch(Form("PulseStart_T0_CH%d", channel),
                    &pulse_start[channel],
                    Form("PulseStart_T0_CH%d/D", channel));
        tree.Branch(Form("Baseline_CH%d", channel), &baseline[channel],
                    Form("Baseline_CH%d/D", channel));
    }

    for (event_id = 0; event_id < 64; ++event_id) {
        event_id_array[0] = event_id;
        event_id_array[1] = event_id + 1000;
        event_id_array[2] = event_id + 2000;
        event_id_array[3] = event_id + 3000;
        sync_time = static_cast<ULong64_t>(event_id) * 1000ULL;
        for (int channel = 0; channel < 8; ++channel) {
            charge[channel] = 0.0;
            pulse_height[channel] = 0.0;
            pulse_start[channel] = -1.0;
            baseline[channel] = 0.0;
        }
        baseline[0] = 16164.0;
        baseline[1] = 16255.0;
        baseline[2] = 8192.0;
        baseline[3] = 8192.0;
        charge[0] = charge[1] = 128.0;
        pulse_height[0] = pulse_height[1] = 16.0;
        tree.Fill();
    }

    const double real_time = 63.0 * 1000.0 * 8.0e-9;
    const double dead_time = 64.0 * 512.0 * 2.0e-9;
    TParameter<int>("RunNumber", 0).Write();
    TParameter<double>("RealTime_sec", real_time).Write();
    TParameter<double>("LiveTime_sec", real_time - dead_time).Write();
    TParameter<double>("DeadTime_pct",
                       dead_time / real_time * 100.0).Write();
    TParameter<int>("LostEvents_count", 0).Write();
    TParameter<int>("RecordedEvents_count", 64).Write();
    TParameter<double>("TriggerRate_Hz", 64.0 / real_time).Write();
    tree.Write();
    output.Close();
}
'''.lstrip(),
        encoding="utf-8",
    )
    malformed = "true" if malformed_event_id_array else "false"
    invocation = f'{macro}({json.dumps(str(output))},{malformed})'
    result = subprocess.run(
        [str(ROOT), "-l", "-b", "-q", invocation],
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0 or not output.is_file():
        raise AssertionError(
            "failed to create legacy ROOT fixture:\n"
            f"stdout={result.stdout}\nstderr={result.stderr}"
        )


class RootValidationIntegrationTests(unittest.TestCase):
    def assert_report_shape(self, report: dict) -> None:
        self.assertTrue(REPORT_KEYS.issubset(report), report)
        self.assertEqual(report["schema_version"], 1)
        self.assertIn(report["overall_status"], OVERALL_STATUSES)
        self.assertIsInstance(report["summary"], dict)
        self.assertIsInstance(report["checks"], list)
        self.assertIsInstance(report["channels"], list)
        self.assertTrue(report["checks"], "validator returned no checks")
        for check in report["checks"]:
            self.assertIsInstance(check, dict)
            self.assertIn(check.get("status"), CHECK_STATUSES)
        counts = report.get("counts")
        self.assertIsInstance(counts, dict)
        self.assertEqual(
            sum(counts.get(status, 0) for status in ("pass", "warn", "fail", "skip")),
            len(report["checks"]),
            report,
        )

    def assert_validation_was_read_only(
        self,
        path: Path,
        identity: tuple[int, int, int, int, int, int, str],
    ) -> None:
        self.assertEqual(read_only_identity(path), identity)

    def test_board_counter_rollover_duplicate_backward_and_range(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_validator_counter_") as temp:
            directory = Path(temp)
            counters = [
                0xFFFFFC,
                0xFFFFFD,
                0xFFFFFE,
                0xFFFFFF,
                0,
                1,
                2,
                3,
            ]
            raw = directory / "rollover.dat"
            write_raw_fixture(
                raw, event_count=len(counters), board_counters=counters
            )
            metadata = directory / "rollover.dat.run.json"
            metadata.write_text(
                json.dumps(metadata_for(raw, metadata, 61), indent=2) + "\n",
                encoding="utf-8",
            )
            valid_root = directory / "rollover.root"
            conversion = run_converter(
                raw, metadata, valid_root, 61, save_waveforms=True
            )
            self.assertEqual(
                conversion.returncode,
                0,
                conversion.stdout + conversion.stderr,
            )
            valid_identity = read_only_identity(valid_root)
            valid_result = run_validator(valid_root)
            self.assertEqual(
                valid_result.returncode,
                0,
                valid_result.stdout + valid_result.stderr,
            )
            valid_report = decode_report(valid_result)
            sequence_check = next(
                item for item in valid_report["checks"]
                if item.get("name") == "board_event_counter_sequence"
            )
            self.assertEqual(sequence_check["status"], "PASS", sequence_check)
            self.assertEqual(
                valid_report["summary"].get(
                    "board_counter_reconstructed_lost_events"
                ),
                0,
                valid_report,
            )
            self.assert_validation_was_read_only(valid_root, valid_identity)

            anomaly_sequences = {
                "duplicate": counters[:6] + [1, 2],
                "backward": counters[:6] + [0, 1],
                "range": counters[:6] + [0x1000000, 2],
            }
            for label, anomaly in anomaly_sequences.items():
                with self.subTest(label=label):
                    malformed = directory / f"{label}.root"
                    rewrite_board_counters(valid_root, malformed, anomaly)
                    identity = read_only_identity(malformed)
                    result = run_validator(malformed)
                    self.assertEqual(
                        result.returncode,
                        0,
                        result.stdout + result.stderr,
                    )
                    report = decode_report(result)
                    self.assertEqual(report["overall_status"], "FAIL", report)
                    failed_names = {
                        item.get("name")
                        for item in report["checks"]
                        if item.get("status") == "FAIL"
                    }
                    expected_name = (
                        "board_event_counter_range"
                        if label == "range"
                        else "board_event_counter_sequence"
                    )
                    self.assertIn(expected_name, failed_names, report)
                    self.assert_validation_was_read_only(malformed, identity)

    def test_valid_production_file_passes_and_is_not_modified(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_root_validator_valid_") as temp:
            directory = Path(temp)
            raw = directory / "fixture_run021.dat"
            write_raw_fixture(raw)
            metadata = directory / "fixture_run021.dat.run.json"
            metadata.write_text(
                json.dumps(metadata_for(raw, metadata, 21), indent=2) + "\n",
                encoding="utf-8",
            )
            output = directory / "fixture_run021_prod.root"
            conversion = run_converter(
                raw, metadata, output, 21, save_waveforms=True
            )
            self.assertEqual(
                conversion.returncode,
                0,
                conversion.stdout + conversion.stderr,
            )
            self.assertTrue(output.is_file())

            identity = read_only_identity(output)
            validation = run_validator(output)
            self.assertEqual(
                validation.returncode,
                0,
                validation.stdout + validation.stderr,
            )
            report = decode_report(validation)
            self.assert_report_shape(report)
            self.assertEqual(report["overall_status"], "PASS", report)
            self.assertEqual(
                report.get("domain_status"),
                {
                    "data_integrity": "PASS",
                    "provenance": "PASS",
                    "trigger_and_quality": "PASS",
                },
                report,
            )
            self.assertTrue(report["summary"].get("waveforms_saved"), report)
            self.assertTrue(
                report["summary"].get("audit_branches_saved"), report
            )
            self.assertEqual(
                report["summary"].get(
                    "board_counter_reconstructed_lost_events"
                ),
                0,
                report,
            )
            audit_check = next(
                check for check in report["checks"]
                if check.get("name") == "audit_branches"
            )
            self.assertEqual(audit_check["status"], "PASS", audit_check)
            lost_check = next(
                check for check in report["checks"]
                if check.get("name") == "board_counter_lost_event_count"
            )
            self.assertEqual(lost_check["status"], "PASS", lost_check)
            for channel in report["channels"]:
                self.assertEqual(
                    channel.get("metrics", {}).get("waveform_dsp_mismatches"),
                    0,
                    channel,
                )
            self.assertNotIn(
                "FAIL", {check.get("status") for check in report["checks"]}
            )
            self.assertIn("[ValidationProgress]", validation.stderr)
            self.assertRegex(
                validation.stderr,
                re.compile(r"\[ValidationProgress\]\s+100(?:\.0+)?%"),
            )
            self.assert_validation_was_read_only(output, identity)

            sampled_validation = run_validator(output, max_events=32)
            self.assertEqual(
                sampled_validation.returncode,
                0,
                sampled_validation.stdout + sampled_validation.stderr,
            )
            sampled_report = decode_report(sampled_validation)
            self.assertEqual(sampled_report["overall_status"], "WARN")
            self.assertTrue(sampled_report["analysis"]["sampled"])
            self.assertEqual(
                sampled_report.get("input", {}).get("validation_mode"),
                "prefix",
                sampled_report,
            )
            self.assertIsNone(
                sampled_report.get("input", {}).get("sha256"),
                sampled_report,
            )
            self.assertIsNone(
                sampled_report.get("input", {}).get("sha256_end"),
                sampled_report,
            )
            sha_check = next(
                check for check in sampled_report["checks"]
                if check.get("name") == "file_sha256_stable"
            )
            self.assertEqual(sha_check["status"], "SKIP", sha_check)
            external_hash_checks = [
                check for check in sampled_report["checks"]
                if str(check.get("name", "")).startswith("external_")
                and check.get("name") != "external_artifact_validation"
            ]
            self.assertTrue(external_hash_checks, sampled_report)
            self.assertTrue(
                all(check.get("status") == "SKIP"
                    for check in external_hash_checks),
                external_hash_checks,
            )
            self.assertEqual(
                sampled_report.get("domain_status", {}).get("data_integrity"),
                "WARN",
                sampled_report,
            )
            self.assertEqual(
                sampled_report.get("domain_status", {}).get(
                    "trigger_and_quality"
                ),
                "WARN",
                sampled_report,
            )
            coverage = next(
                check for check in sampled_report["checks"]
                if check.get("name") == "analysis_coverage"
            )
            self.assertEqual(coverage["status"], "WARN", coverage)
            self.assert_validation_was_read_only(output, identity)

    def test_relocated_bundle_provenance_is_authenticated(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_validator_relocation_") as temp:
            directory = Path(temp)
            original = directory / "original"
            relocated = directory / "relocated"
            original.mkdir()
            relocated.mkdir()

            raw_name = "fixture_run026.dat"
            config_name = raw_name + ".config.conf"
            metadata_name = raw_name + ".run.json"
            original_raw = original / raw_name
            original_config = original / config_name
            original_metadata = original / metadata_name
            write_raw_fixture(original_raw)
            shutil.copy2(CONFIG, original_config)
            original_metadata.write_text(
                json.dumps(metadata_for(
                    original_raw,
                    original_metadata,
                    26,
                    config=original_config,
                ), indent=2) + "\n",
                encoding="utf-8",
            )
            original_files = (
                original_raw,
                original_config,
                original_metadata,
            )
            original_identities = {
                path: read_only_identity(path) for path in original_files
            }

            relocated_raw = relocated / raw_name
            relocated_config = relocated / config_name
            relocated_metadata = relocated / metadata_name
            for source, destination in zip(
                original_files,
                (relocated_raw, relocated_config, relocated_metadata),
            ):
                shutil.copy2(source, destination)
            relocated_files = (
                relocated_raw,
                relocated_config,
                relocated_metadata,
            )
            relocated_identities = {
                path: read_only_identity(path) for path in relocated_files
            }

            output = relocated / "fixture_run026_prod.root"
            conversion = run_converter(
                relocated_raw,
                relocated_metadata,
                output,
                26,
                config=relocated_config,
                save_waveforms=True,
            )
            self.assertEqual(
                conversion.returncode,
                0,
                conversion.stdout + conversion.stderr,
            )
            output_identity = read_only_identity(output)
            validation = run_validator(output)
            self.assertEqual(
                validation.returncode,
                0,
                validation.stdout + validation.stderr,
            )
            report = decode_report(validation)
            self.assert_report_shape(report)
            self.assertEqual(report["overall_status"], "PASS", report)
            paths = next(
                check for check in report["checks"]
                if check.get("name") == "embedded_artifact_paths"
            )
            self.assertEqual(paths["status"], "PASS", paths)
            self.assertEqual(
                paths.get("observed", {}).get("relocated"),
                {"raw": True, "config": True, "metadata": True},
                paths,
            )
            for name in (
                "external_resolved_raw_output",
                "external_resolved_runtime_config",
                "external_resolved_runtime_metadata",
            ):
                check = next(
                    item for item in report["checks"]
                    if item.get("name") == name
                )
                self.assertEqual(check["status"], "PASS", check)
            self.assert_validation_was_read_only(output, output_identity)
            for path, identity in original_identities.items():
                self.assertEqual(read_only_identity(path), identity)
            for path, identity in relocated_identities.items():
                self.assertEqual(read_only_identity(path), identity)

    def test_rising_polarity_dsp_is_recomputed_and_validated(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_root_validator_rising_") as temp:
            directory = Path(temp)
            rising_text, replacements = re.subn(
                r"(?m)^(\s*TriggerPolarity\s*=\s*)1(\s*(?:#.*)?)$",
                r"\g<1>0\2",
                CONFIG.read_text(encoding="utf-8"),
                count=1,
            )
            self.assertEqual(replacements, 1)
            rising_config = directory / "rising.conf"
            rising_config.write_text(rising_text, encoding="utf-8")

            raw = directory / "rising_run025.dat"
            write_raw_fixture(raw, polarity="rising", pulse_amplitude=64)
            metadata = directory / "rising_run025.dat.run.json"
            metadata.write_text(
                json.dumps(metadata_for(
                    raw,
                    metadata,
                    25,
                    config=rising_config,
                    polarity="rising",
                ), indent=2) + "\n",
                encoding="utf-8",
            )
            output = directory / "rising_run025_prod.root"
            conversion = run_converter(
                raw,
                metadata,
                output,
                25,
                config=rising_config,
                save_waveforms=True,
            )
            self.assertEqual(
                conversion.returncode,
                0,
                conversion.stdout + conversion.stderr,
            )
            identity = read_only_identity(output)
            validation = run_validator(output)
            self.assertEqual(
                validation.returncode,
                0,
                validation.stdout + validation.stderr,
            )
            report = decode_report(validation)
            self.assert_report_shape(report)
            self.assertEqual(report["overall_status"], "PASS", report)
            self.assertEqual(
                report.get("metadata", {}).get("hardware", {}).get(
                    "trigger_polarity"
                ),
                "rising",
                report,
            )
            for channel in report["channels"][:2]:
                metrics = channel.get("metrics", {})
                self.assertEqual(metrics.get("waveform_dsp_mismatches"), 0)
                self.assertEqual(metrics.get("pulse_height_min_adc"), 64.0)
                self.assertEqual(metrics.get("pulse_height_max_adc"), 64.0)
                self.assertEqual(metrics.get("charge_min_adc_samples"), 512.0)
                self.assertEqual(metrics.get("charge_max_adc_samples"), 512.0)
                self.assertEqual(metrics.get("t0_found_fraction"), 1.0)
            self.assert_validation_was_read_only(output, identity)

    def test_legacy_file_is_fully_scanned_but_fails_provenance(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_root_validator_legacy_") as temp:
            directory = Path(temp)
            legacy = directory / "legacy_run042_prod.root"
            make_legacy_root(directory, legacy)
            identity = read_only_identity(legacy)

            validation = run_validator(legacy)
            self.assertEqual(
                validation.returncode,
                0,
                "analysis failures belong in JSON, not the process exit code:\n"
                + validation.stdout
                + validation.stderr,
            )
            report = decode_report(validation)
            self.assert_report_shape(report)
            self.assertEqual(report["overall_status"], "FAIL", report)
            self.assertEqual(
                report.get("domain_status", {}).get("data_integrity"),
                "PASS",
                report,
            )
            self.assertEqual(
                report.get("domain_status", {}).get("provenance"),
                "FAIL",
                report,
            )
            failed_text = " ".join(
                " ".join(
                    str(check.get(field, ""))
                    for field in (
                        "id", "category", "name", "message", "detail",
                        "details",
                    )
                )
                for check in report["checks"]
                if check.get("status") == "FAIL"
            ).lower()
            self.assertIn("provenance", failed_text, report)
            self.assert_validation_was_read_only(legacy, identity)

            sampled_validation = run_validator(legacy, max_events=16)
            self.assertEqual(
                sampled_validation.returncode,
                0,
                sampled_validation.stdout + sampled_validation.stderr,
            )
            sampled_report = decode_report(sampled_validation)
            inference = next(
                check for check in sampled_report["checks"]
                if check.get("name") == "legacy_trigger_inference"
            )
            self.assertEqual(inference["status"], "SKIP", inference)
            self.assertNotIn(
                "legacy_trigger_inference",
                sampled_report["summary"],
                sampled_report,
            )
            for channel in sampled_report["channels"][:2]:
                self.assertNotIn(
                    "legacy_inferred_absolute_cutoff_adc",
                    channel.get("metrics", {}),
                    channel,
                )
            self.assert_validation_was_read_only(legacy, identity)

    def test_waveform_and_requires_simultaneous_comparator_overlap(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_root_validator_and_") as temp:
            directory = Path(temp)
            raw = directory / "fixture_run022.dat"
            write_raw_fixture(
                raw, event_count=128, overlapping_trigger_pulses=False
            )
            metadata = directory / "fixture_run022.dat.run.json"
            metadata.write_text(
                json.dumps(metadata_for(raw, metadata, 22), indent=2) + "\n",
                encoding="utf-8",
            )
            output = directory / "fixture_run022_prod.root"
            conversion = run_converter(
                raw, metadata, output, 22, save_waveforms=True
            )
            self.assertEqual(
                conversion.returncode,
                0,
                conversion.stdout + conversion.stderr,
            )
            identity = read_only_identity(output)
            validation = run_validator(output)
            self.assertEqual(
                validation.returncode,
                0,
                validation.stdout + validation.stderr,
            )
            report = decode_report(validation)
            self.assert_report_shape(report)
            self.assertEqual(report["overall_status"], "FAIL", report)
            routing = next(
                check for check in report["checks"]
                if check.get("name") == "routing_event_evidence"
            )
            self.assertEqual(routing["status"], "FAIL", routing)
            self.assertEqual(
                report["summary"].get("routing_evidence_mode"),
                "simultaneous_waveform_comparator_overlap",
            )
            self.assert_validation_was_read_only(output, identity)

    def test_waveform_or_accepts_nonoverlapping_channel_crossings(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_root_validator_or_") as temp:
            directory = Path(temp)
            or_text, replacements = re.subn(
                r"(?m)^(\s*PairLogic\s*=\s*)AND(\s*(?:#.*)?)$",
                r"\g<1>OR\2",
                CONFIG.read_text(encoding="utf-8"),
                count=1,
            )
            self.assertEqual(replacements, 1)
            or_config = directory / "or.conf"
            or_config.write_text(or_text, encoding="utf-8")
            raw = directory / "fixture_run023.dat"
            write_raw_fixture(
                raw, event_count=128, overlapping_trigger_pulses=False
            )
            metadata = directory / "fixture_run023.dat.run.json"
            metadata.write_text(
                json.dumps(
                    metadata_for(
                        raw,
                        metadata,
                        23,
                        config=or_config,
                        pair_logic="OR",
                    ),
                    indent=2,
                ) + "\n",
                encoding="utf-8",
            )
            output = directory / "fixture_run023_prod.root"
            conversion = run_converter(
                raw,
                metadata,
                output,
                23,
                config=or_config,
                save_waveforms=True,
            )
            self.assertEqual(
                conversion.returncode,
                0,
                conversion.stdout + conversion.stderr,
            )
            identity = read_only_identity(output)
            validation = run_validator(output)
            self.assertEqual(
                validation.returncode,
                0,
                validation.stdout + validation.stderr,
            )
            report = decode_report(validation)
            self.assert_report_shape(report)
            self.assertEqual(report["overall_status"], "PASS", report)
            routing = next(
                check for check in report["checks"]
                if check.get("name") == "routing_event_evidence"
            )
            self.assertEqual(routing["status"], "PASS", routing)
            self.assertEqual(
                report["summary"].get("routing_evidence_mode"),
                "waveform_comparator_crossing",
            )
            self.assert_validation_was_read_only(output, identity)

    def test_post_calibration_baseline_drift_fails_effective_threshold(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_threshold_drift_") as temp:
            directory = Path(temp)
            raw = directory / "fixture_run024.dat"
            write_raw_fixture(raw, event_count=64)
            metadata = directory / "fixture_run024.dat.run.json"
            document = metadata_for(raw, metadata, 24)
            for channel in document["channels"][:2]:
                stale_baseline = channel["measured_baseline_adc"] - 100.0
                stale_threshold = round(stale_baseline) - 8
                channel["measured_baseline_adc"] = stale_baseline
                channel["written_threshold_adc"] = stale_threshold
                channel["readback_threshold_adc"] = stale_threshold
                channel["effective_threshold_mv"] = (
                    abs(stale_baseline - stale_threshold) * 2000.0 / 16384.0
                )
            metadata.write_text(
                json.dumps(document, indent=2) + "\n", encoding="utf-8"
            )
            output = directory / "fixture_run024_prod.root"
            conversion = run_converter(
                raw, metadata, output, 24, save_waveforms=True
            )
            self.assertEqual(
                conversion.returncode,
                0,
                conversion.stdout + conversion.stderr,
            )
            identity = read_only_identity(output)
            validation = run_validator(output)
            self.assertEqual(
                validation.returncode,
                0,
                validation.stdout + validation.stderr,
            )
            report = decode_report(validation)
            self.assertEqual(report["overall_status"], "FAIL", report)
            for channel in (0, 1):
                threshold_check = next(
                    check for check in report["checks"]
                    if check.get("name")
                    == f"channel_{channel}_effective_threshold"
                )
                self.assertEqual(
                    threshold_check["status"], "FAIL", threshold_check
                )
                self.assertGreater(
                    threshold_check["observed"]["calibration_drift_adc"],
                    90.0,
                    threshold_check,
                )
            self.assert_validation_was_read_only(output, identity)

    def test_array_disguised_as_scalar_is_rejected_before_binding(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_root_validator_array_") as temp:
            directory = Path(temp)
            malformed = directory / "array_event_id_prod.root"
            make_legacy_root(
                directory,
                malformed,
                malformed_event_id_array=True,
            )
            identity = read_only_identity(malformed)

            validation = run_validator(malformed)
            self.assertEqual(
                validation.returncode,
                0,
                validation.stdout + validation.stderr,
            )
            report = decode_report(validation)
            self.assert_report_shape(report)
            scalar_schema = next(
                check for check in report["checks"]
                if check.get("name") == "scalar_branches"
            )
            self.assertEqual(scalar_schema["status"], "FAIL", scalar_schema)
            event_scan = next(
                check for check in report["checks"]
                if check.get("name") == "event_scan"
            )
            self.assertEqual(event_scan["status"], "SKIP", event_scan)
            self.assertFalse(
                any(
                    check.get("name") == "branch_binding" and
                    check.get("status") == "PASS"
                    for check in report["checks"]
                ),
                report,
            )
            self.assert_validation_was_read_only(malformed, identity)

    def test_unopenable_corrupt_input_is_fatal_and_not_modified(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_root_validator_corrupt_") as temp:
            corrupt = Path(temp) / "corrupt.root"
            # This is a separate test fixture, never a mutation or truncation
            # of a valid/raw measurement ROOT file.
            corrupt.write_bytes(b"not a ROOT file\n")
            identity = read_only_identity(corrupt)

            validation = run_validator(corrupt)
            self.assertNotEqual(validation.returncode, 0)
            self.assertTrue(
                validation.stdout.strip() or validation.stderr.strip(),
                "fatal validation failure must include a diagnostic",
            )
            self.assert_validation_was_read_only(corrupt, identity)


if __name__ == "__main__":
    unittest.main()
