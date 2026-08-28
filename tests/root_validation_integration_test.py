"""End-to-end contract tests for the production ROOT validator.

CMake supplies the validator, production converter, DAQ configuration, and
ROOT executable.  Every fixture is created in a temporary directory.  The
validator is then required to leave each input byte-for-byte unchanged.
"""

import hashlib
import configparser
import json
import os
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
VALIDATOR_EXIT_CODES = {
    "PASS": 0,
    "WARN": 1,
    "FAIL": 2,
    "CANCELLED": 3,
}


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
    parsed_config = configparser.ConfigParser()
    parsed_config.optionxform = str
    if not parsed_config.read(config, encoding="utf-8"):
        raise ValueError(f"cannot read fixture config: {config}")
    expected_model = parsed_config.get(
        "Connection", "ExpectedModel", fallback="DT5730"
    )
    expected_serial = parsed_config.getint(
        "Connection", "ExpectedSerial", fallback=None
    )
    dsp_baseline = parsed_config.getint(
        "SoftwareDSP", "BaselineSamples", fallback=150
    )
    dsp_short = parsed_config.getint(
        "SoftwareDSP", "ShortGate", fallback=40
    )
    dsp_long = parsed_config.getint(
        "SoftwareDSP", "LongGate", fallback=200
    )
    dsp_threshold = parsed_config.getfloat(
        "SoftwareDSP", "PulseStartThresholdAdc", fallback=30.0
    )
    coincidence_window = parsed_config.getint(
        "SoftwareDSP", "CoincidenceWindow", fallback=20
    )
    event_bytes = 24 + 4 * 512 * 2
    raw_size = raw.stat().st_size
    if raw_size % event_bytes != 0:
        raise ValueError("metadata fixture RAW must contain complete events")
    recorded_events = raw_size // event_bytes
    first_ttt = None
    last_ttt = None
    lost_events = 0
    previous_counter = None
    with raw.open("rb") as stream:
        for event_index in range(recorded_events):
            stream.seek(event_index * event_bytes)
            extended_ttt, _, _, _, _, counter = struct.unpack(
                "<QIIHHI", stream.read(24)
            )
            if first_ttt is None:
                first_ttt = extended_ttt
            last_ttt = extended_ttt
            if previous_counter is not None:
                difference = (counter - previous_counter) & 0xFFFFFF
                if 1 < difference <= 0x800000:
                    lost_events += difference - 1
            previous_counter = counter
    elapsed = (
        (last_ttt - first_ttt) * 8.0e-9
        if first_ttt is not None and last_ttt is not None
        else None
    )
    window_sum = recorded_events * 512 * 2.0e-9
    window_ratio = (
        100.0 * window_sum / elapsed if elapsed is not None and elapsed > 0
        else None
    )
    average_rate = (
        recorded_events / elapsed if elapsed is not None and elapsed > 0
        else None
    )
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
        "schema_version": 2,
        "run_number": run_number,
        "acquisition_status": "completed",
        "termination_reason": "event_limit",
        "requested_max_events": recorded_events,
        "requested_run_time_sec": 0,
        "hardware_verified_unix_time": 1_788_000_000,
        "acquisition_start_unix_time": 1_788_000_001,
        "acquisition_end_unix_time": 1_788_000_002,
        "recorded_events": recorded_events,
        "lost_events": lost_events,
        "failure_reason": None,
        "created_unix_time": 1_788_000_003,
        "raw_output_path": str(raw),
        "requested_raw_output_path": str(raw),
        "raw_output_published": True,
        "raw_output_finalized": True,
        "raw_finalization_error": None,
        "raw_digest_method": (
            "streaming_sha256_verified_by_descriptor_sha256"
        ),
        "raw_recovery_performed": False,
        "raw_events_before_recovery": None,
        "lost_events_exact": True,
        "raw_format_version": 1,
        "raw_event_header_bytes": 24,
        "raw_event_bytes": event_bytes,
        "last_complete_offset": raw_size,
        "raw_output_size_bytes": raw_size,
        "raw_output_sha256": sha256(raw),
        "storage": {
            "free_bytes_at_start": 16 * 1024**3,
            "free_bytes_at_end": 15 * 1024**3,
            "expected_raw_bytes": recorded_events * event_bytes,
            "minimum_free_bytes": 1024 * 1024**2,
            "stop_free_bytes": 512 * 1024**2,
        },
        "runtime_counters": {
            "readout_errors": 0,
            "health_checks": 2,
            "health_read_errors": 0,
            "zmq_nonblocking_send_failures": 0,
            "zmq_send_errors": 0,
            "zmq_send_hwm_messages": 5000,
            "zmq_send_hwm_approx_bytes": 5000 * event_bytes,
            "runtime_configuration_checks": 2,
            "subscriber_delivery_evidence": (
                "unavailable_pub_socket_may_drop_silently"
            ),
            "max_temperature_c": [25, 25, 25, 25, None, None, None, None],
        },
        "timing_summary": {
            "first_extended_ttt": first_ttt,
            "last_extended_ttt": last_ttt,
            "elapsed_time_sec": elapsed,
            "recorded_window_sum_sec": window_sum,
            "recorded_window_to_elapsed_pct": window_ratio,
            "average_recorded_event_rate_hz": average_rate,
        },
        "metadata_path": str(metadata),
        "config_path": str(config),
        "config_sha256": sha256(config),
        "source_config_path": str(config),
        "binary_path": str(PRODUCTION),
        "binary_sha256": sha256(PRODUCTION),
        "git_commit": "root-validator-integration-fixture",
        "build_timestamp": "root-validator-integration-fixture",
        "hardware": {
            "connection_type": "USB",
            "connection_link": 0,
            "connection_node": 0,
            "connection_base_address": 0,
            "expected_model": expected_model,
            "expected_serial": expected_serial,
            "model": f"{expected_model}S-FIXTURE",
            "serial_number": expected_serial or 5730,
            "roc_firmware": "fixture",
            "amc_firmware": "fixture",
            "input_range_mvpp": 2000,
            "adc_bits": 14,
            "dc_offset_dac_bits": 16,
            "latest_acquisition_status_register": 384,
            "latest_board_failure_status_register": 0,
            "waveform_dsp_schema": 1,
            "dsp_baseline_samples": dsp_baseline,
            "dsp_short_gate_samples": dsp_short,
            "dsp_long_gate_samples": dsp_long,
            "dsp_pulse_start_threshold_adc": dsp_threshold,
            "software_coincidence_window_ns": coincidence_window,
            "trigger_time_tag_raw_lsb_ns": 8,
            "trigger_time_tag_observable_resolution_ns": 16,
            "adc_sample_period_ns": 2,
            "dead_time_measurement_available": False,
            "dead_time_method": (
                "unavailable_no_hardware_busy_or_livetime_scaler"
            ),
            "clock_source": 0,
            "clock_source_readback": 0,
            "run_sync_mode": 0,
            "run_sync_mode_readback": 0,
            "trigger_polarity": polarity,
            "record_mask": 15,
            "record_mask_readback": 15,
            "record_length": 512,
            "post_trigger_percent": 60,
            "post_trigger_readback_percent": 60,
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
    ttt_phase: int = 0,
) -> None:
    record_length = 512
    channel_mask = 0xF
    baselines = (16164, 16255, 8192, 8192)
    if board_counters is not None and len(board_counters) != event_count:
        raise ValueError("board_counters length must equal event_count")
    if ttt_phase not in (0, 1):
        raise ValueError("ttt_phase must be zero or one raw-count phase")
    with path.open("wb") as stream:
        for event_id in range(event_count):
            stream.write(struct.pack(
                "<QIIHHI",
                event_id * 1000 + ttt_phase,
                # 8 us between triggers, > 1024 ns record.  Absolute odd and
                # even raw-count phases are both valid at 16 ns resolution.
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


def run_validator(
    root_file: Path,
    *,
    max_events: int | None = None,
    raw_fidelity: bool = False,
    report_path: Path | None = None,
):
    command = [str(VALIDATOR), "-i", str(root_file)]
    if max_events is not None:
        command.extend(["--max-events", str(max_events)])
    if raw_fidelity:
        command.append("--raw-fidelity")
    if report_path is not None:
        command.extend(["--report", str(report_path)])
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


def rewrite_sync_times(
    source: Path, destination: Path, sync_times: list[int]
) -> None:
    """Clone a ROOT fixture while replacing only SyncTime_TTT."""

    rendered_times = ", ".join(f"{value}ULL" for value in sync_times)
    expression = f'''
TFile input({json.dumps(str(source))}, "READ");
auto *source_tree = dynamic_cast<TTree*>(input.Get("phys_tree"));
if (!source_tree || source_tree->GetEntries() != {len(sync_times)}) gSystem->Exit(1);
source_tree->SetBranchStatus("SyncTime_TTT", 0);
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
source_tree->SetBranchStatus("SyncTime_TTT", 0);
output.cd();
auto *destination_tree = source_tree->CloneTree(0);
if (!destination_tree) gSystem->Exit(3);
ULong64_t replacement_sync_time = 0;
destination_tree->Branch(
    "SyncTime_TTT", &replacement_sync_time, "SyncTime_TTT/l");
std::vector<ULong64_t> sync_times = {{{rendered_times}}};
for (Long64_t entry = 0; entry < source_tree->GetEntries(); ++entry) {{
  if (source_tree->GetEntry(entry) <= 0) gSystem->Exit(4);
  replacement_sync_time = sync_times.at(static_cast<std::size_t>(entry));
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


def copy_root_without_key(
    source: Path, destination: Path, omitted_key: str
) -> None:
    """Create a separate malformed fixture without mutating the source ROOT."""

    expression = f'''
TFile input({json.dumps(str(source))}, "READ");
if (input.IsZombie()) gSystem->Exit(1);
TFile output({json.dumps(str(destination))}, "RECREATE");
if (output.IsZombie()) gSystem->Exit(2);
TIter next_key(input.GetListOfKeys());
while (auto *key = dynamic_cast<TKey*>(next_key())) {{
  if (TString(key->GetName()) == {json.dumps(omitted_key)}) continue;
  auto *object = key->ReadObj();
  if (!object) gSystem->Exit(3);
  output.cd();
  object->Write(key->GetName());
  delete object;
}}
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


def decode_report_with_exit(result: subprocess.CompletedProcess) -> dict:
    """Decode stdout and enforce the validator's status/exit-code contract."""

    report = decode_report(result)
    status = report.get("overall_status")
    expected = VALIDATOR_EXIT_CODES.get(status)
    if expected is None:
        raise AssertionError(f"unknown validator status: {status!r}")
    if result.returncode != expected:
        raise AssertionError(
            "validator exit code does not match its JSON status:\n"
            f"status={status!r}, expected={expected}, "
            f"actual={result.returncode}\n"
            f"stdout={result.stdout!r}\nstderr={result.stderr!r}"
        )
    return report


def make_legacy_root(
    directory: Path,
    output: Path,
    *,
    malformed_event_id_array: bool = False,
    event_count: int = 64,
    transient_baseline_entry: int | None = None,
) -> None:
    macro = directory / "make_legacy.C"
    macro.write_text(
        r'''
#include <TFile.h>
#include <TParameter.h>
#include <TString.h>
#include <TTree.h>

void make_legacy(const char *path, bool malformed_event_id_array = false,
                 ULong64_t event_count = 64,
                 Long64_t transient_baseline_entry = -1) {
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

    for (event_id = 0; event_id < event_count; ++event_id) {
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
        if (static_cast<Long64_t>(event_id) == transient_baseline_entry) {
            baseline[0] += 100.0;
        }
        charge[0] = charge[1] = 128.0;
        pulse_height[0] = pulse_height[1] = 16.0;
        tree.Fill();
    }

    const double real_time = event_count > 1
        ? static_cast<double>(event_count - 1) * 1000.0 * 8.0e-9
        : 0.0;
    const double dead_time =
        static_cast<double>(event_count) * 512.0 * 2.0e-9;
    TParameter<int>("RunNumber", 0).Write();
    TParameter<double>("RealTime_sec", real_time).Write();
    TParameter<double>("LiveTime_sec", real_time - dead_time).Write();
    TParameter<double>("DeadTime_pct",
                       dead_time / real_time * 100.0).Write();
    TParameter<int>("LostEvents_count", 0).Write();
    TParameter<int>("RecordedEvents_count",
                    static_cast<int>(event_count)).Write();
    TParameter<double>("TriggerRate_Hz",
                       static_cast<double>(event_count) / real_time).Write();
    tree.Write();
    output.Close();
}
'''.lstrip(),
        encoding="utf-8",
    )
    malformed = "true" if malformed_event_id_array else "false"
    transient_entry = (
        -1 if transient_baseline_entry is None else transient_baseline_entry
    )
    invocation = (
        f'{macro}({json.dumps(str(output))},{malformed},'
        f'{event_count}ULL,{transient_entry}LL)'
    )
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

    def test_opt_in_raw_fidelity_detects_exact_and_altered_root_content(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_validator_fidelity_") as temp:
            directory = Path(temp)
            raw = directory / "fidelity_run071.dat"
            event_count = 64
            write_raw_fixture(raw, event_count=event_count)
            metadata = directory / "fidelity_run071.dat.run.json"
            metadata.write_text(
                json.dumps(metadata_for(raw, metadata, 71), indent=2) + "\n",
                encoding="utf-8",
            )

            waveform_root = directory / "fidelity_waveforms.root"
            conversion = run_converter(
                raw, metadata, waveform_root, 71, save_waveforms=True
            )
            self.assertEqual(
                conversion.returncode,
                0,
                conversion.stdout + conversion.stderr,
            )
            raw_identity = read_only_identity(raw)
            root_identity = read_only_identity(waveform_root)
            validation = run_validator(waveform_root, raw_fidelity=True)
            report = decode_report_with_exit(validation)
            self.assertIn(
                "authenticating RAW fidelity source", validation.stderr
            )
            self.assertIn(
                "cross-checking ROOT and RAW events", validation.stderr
            )
            fidelity = next(
                check for check in report["checks"]
                if check.get("name") == "raw_root_fidelity"
            )
            self.assertEqual(fidelity["status"], "PASS", fidelity)
            observed = fidelity.get("observed", {})
            self.assertEqual(observed.get("events_compared"), event_count)
            self.assertEqual(observed.get("header_field_mismatches"), 0)
            self.assertEqual(observed.get("scalar_field_mismatches"), 0)
            self.assertEqual(observed.get("waveform_sample_mismatches"), 0)
            self.assertTrue(observed.get("waveforms_compared"), observed)
            self.assertEqual(
                observed.get("waveform_samples_compared"),
                event_count * 4 * 512,
            )
            self.assertEqual(
                report.get("analysis", {}).get("raw_fidelity", {}).get(
                    "exact_match"
                ),
                True,
                report,
            )
            self.assert_validation_was_read_only(raw, raw_identity)
            self.assert_validation_was_read_only(waveform_root, root_identity)

            # The converter intentionally omitted Waveform_CH*. Fidelity must
            # still stream the authenticated payload and reproduce all four
            # scalar DSP fields with the converter's canonical algorithm.
            scalar_root = directory / "fidelity_scalars.root"
            scalar_conversion = run_converter(
                raw, metadata, scalar_root, 71, save_waveforms=False
            )
            self.assertEqual(
                scalar_conversion.returncode,
                0,
                scalar_conversion.stdout + scalar_conversion.stderr,
            )
            scalar_identity = read_only_identity(scalar_root)
            scalar_validation = run_validator(
                scalar_root, raw_fidelity=True
            )
            scalar_report = decode_report_with_exit(scalar_validation)
            scalar_fidelity = next(
                check for check in scalar_report["checks"]
                if check.get("name") == "raw_root_fidelity"
            )
            self.assertEqual(
                scalar_fidelity["status"], "PASS", scalar_fidelity
            )
            self.assertFalse(
                scalar_fidelity.get("observed", {}).get(
                    "waveforms_compared"
                ),
                scalar_fidelity,
            )
            self.assertEqual(
                scalar_fidelity.get("observed", {}).get(
                    "scalar_field_mismatches"
                ),
                0,
                scalar_fidelity,
            )
            self.assert_validation_was_read_only(scalar_root, scalar_identity)

            # Rewrite only the ROOT audit branch to a different but internally
            # monotonic sequence. Ordinary sequence checks still pass, while
            # the authenticated RAW header comparison must catch the change.
            altered_root = directory / "fidelity_altered.root"
            rewrite_board_counters(
                waveform_root,
                altered_root,
                [1000 + event for event in range(event_count)],
            )
            altered_identity = read_only_identity(altered_root)
            altered_validation = run_validator(
                altered_root, raw_fidelity=True
            )
            altered_report = decode_report_with_exit(altered_validation)
            altered_fidelity = next(
                check for check in altered_report["checks"]
                if check.get("name") == "raw_root_fidelity"
            )
            self.assertEqual(
                altered_fidelity["status"], "FAIL", altered_fidelity
            )
            self.assertGreater(
                altered_fidelity.get("observed", {}).get(
                    "header_field_mismatches", 0
                ),
                0,
                altered_fidelity,
            )
            self.assert_validation_was_read_only(altered_root, altered_identity)
            self.assert_validation_was_read_only(raw, raw_identity)

            prefix = run_validator(
                waveform_root, max_events=8, raw_fidelity=True
            )
            prefix_report = decode_report_with_exit(prefix)
            prefix_fidelity = next(
                check for check in prefix_report["checks"]
                if check.get("name") == "raw_root_fidelity"
            )
            self.assertEqual(prefix_fidelity["status"], "SKIP")
            self.assertEqual(
                prefix_fidelity.get("observed", {}).get("raw_bytes_read"),
                0,
                prefix_fidelity,
            )
            self.assertFalse(
                prefix_report.get("analysis", {}).get("raw_fidelity", {}).get(
                    "completed"
                ),
                prefix_report,
            )

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
            valid_report = decode_report_with_exit(valid_result)
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
                    report = decode_report_with_exit(result)
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

    def test_schema_v2_ttt_endpoints_must_match_authenticated_metadata(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_validator_ttt_bounds_") as temp:
            directory = Path(temp)
            event_count = 64
            raw = directory / "ttt_bounds_run072.dat"
            write_raw_fixture(raw, event_count=event_count)
            metadata = directory / "ttt_bounds_run072.dat.run.json"
            metadata.write_text(
                json.dumps(metadata_for(raw, metadata, 72), indent=2) + "\n",
                encoding="utf-8",
            )
            valid_root = directory / "ttt_bounds_valid.root"
            conversion = run_converter(
                raw, metadata, valid_root, 72, save_waveforms=False
            )
            self.assertEqual(
                conversion.returncode,
                0,
                conversion.stdout + conversion.stderr,
            )

            shifted_root = directory / "ttt_bounds_shifted.root"
            rewrite_sync_times(
                valid_root,
                shifted_root,
                [event * 1000 + 1 for event in range(event_count)],
            )
            identity = read_only_identity(shifted_root)
            validation = run_validator(shifted_root)
            report = decode_report_with_exit(validation)
            self.assertEqual(report["overall_status"], "FAIL", report)

            endpoint_check = next(
                check for check in report["checks"]
                if check.get("name") == "metadata_ttt_endpoints"
            )
            self.assertEqual(endpoint_check["status"], "FAIL", endpoint_check)
            self.assertEqual(
                endpoint_check["observed"],
                {
                    "first_extended_ttt": 1,
                    "last_extended_ttt": 63_001,
                    "events_scanned": event_count,
                },
                endpoint_check,
            )
            self.assertEqual(
                endpoint_check["expected"],
                {
                    "first_extended_ttt": 0,
                    "last_extended_ttt": 63_000,
                    "recorded_events": event_count,
                },
                endpoint_check,
            )
            monotonic = next(
                check for check in report["checks"]
                if check.get("name") == "trigger_time_monotonic"
            )
            quantization = next(
                check for check in report["checks"]
                if check.get("name") == "trigger_time_quantization"
            )
            timing = next(
                check for check in report["checks"]
                if check.get("name") == "timing_summary"
            )
            self.assertEqual(monotonic["status"], "PASS", monotonic)
            self.assertEqual(quantization["status"], "FAIL", quantization)
            self.assertEqual(
                quantization["observed"],
                {
                    "violations": event_count,
                    "expected_code_parity": 0,
                    "events_scanned": event_count,
                },
                quantization,
            )
            self.assertEqual(timing["status"], "PASS", timing)
            self.assert_validation_was_read_only(shifted_root, identity)

            # Observable 16 ns resolution means a fixed two-count phase, not
            # that every absolute raw code must be even.  An authenticated odd
            # phase is valid and occurs in real DT5730S data.
            odd_raw = directory / "ttt_odd_phase_run073.dat"
            write_raw_fixture(
                odd_raw, event_count=event_count, ttt_phase=1
            )
            odd_metadata = directory / "ttt_odd_phase_run073.dat.run.json"
            odd_metadata.write_text(
                json.dumps(
                    metadata_for(odd_raw, odd_metadata, 73), indent=2
                ) + "\n",
                encoding="utf-8",
            )
            odd_root = directory / "ttt_odd_phase_valid.root"
            odd_conversion = run_converter(
                odd_raw, odd_metadata, odd_root, 73, save_waveforms=True
            )
            self.assertEqual(
                odd_conversion.returncode,
                0,
                odd_conversion.stdout + odd_conversion.stderr,
            )
            odd_validation = run_validator(odd_root)
            odd_report = decode_report_with_exit(odd_validation)
            self.assertEqual(odd_report["overall_status"], "PASS", odd_report)
            odd_quantization = next(
                check for check in odd_report["checks"]
                if check.get("name") == "trigger_time_quantization"
            )
            self.assertEqual(odd_quantization["status"], "PASS")
            self.assertEqual(
                odd_quantization["observed"]["expected_code_parity"], 1
            )

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
            report = decode_report_with_exit(validation)
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
            sampled_report = decode_report_with_exit(sampled_validation)
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
                and check.get("name") not in {
                    "external_artifact_validation",
                    "external_provenance_coverage",
                }
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

    def test_report_file_is_atomic_no_clobber_and_never_aliases_input(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_validator_report_") as temp:
            directory = Path(temp)
            raw = directory / "fixture_run074.dat"
            write_raw_fixture(raw)
            metadata = directory / "fixture_run074.dat.run.json"
            metadata.write_text(
                json.dumps(metadata_for(raw, metadata, 74), indent=2) + "\n",
                encoding="utf-8",
            )
            output = directory / "fixture_run074_prod.root"
            conversion = run_converter(
                raw, metadata, output, 74, save_waveforms=True
            )
            self.assertEqual(
                conversion.returncode,
                0,
                conversion.stdout + conversion.stderr,
            )
            input_identity = read_only_identity(output)

            report_path = directory / "validation.json"
            validation = run_validator(output, report_path=report_path)
            self.assertEqual(
                validation.returncode,
                VALIDATOR_EXIT_CODES["PASS"],
                validation.stdout + validation.stderr,
            )
            self.assertEqual(validation.stdout, "")
            self.assertTrue(report_path.is_file())
            report = json.loads(report_path.read_text(encoding="utf-8"))
            self.assertEqual(report.get("overall_status"), "PASS", report)
            self.assertIn(str(report_path.resolve()), validation.stderr)
            self.assert_validation_was_read_only(output, input_identity)

            report_identity = read_only_identity(report_path)
            collision = run_validator(output, report_path=report_path)
            self.assertEqual(
                collision.returncode,
                73,
                collision.stdout + collision.stderr,
            )
            self.assertEqual(collision.stdout, "")
            self.assertIn("already exists", collision.stderr)
            self.assert_validation_was_read_only(report_path, report_identity)
            self.assert_validation_was_read_only(output, input_identity)

            same_path = run_validator(output, report_path=output)
            self.assertEqual(
                same_path.returncode,
                73,
                same_path.stdout + same_path.stderr,
            )
            self.assertIn("input ROOT path", same_path.stderr)
            self.assert_validation_was_read_only(output, input_identity)

            hard_link = directory / "input_hard_link.root"
            os.link(output, hard_link)
            hard_link_identity = read_only_identity(hard_link)
            input_after_link_identity = read_only_identity(output)
            hard_link_result = run_validator(output, report_path=hard_link)
            self.assertEqual(
                hard_link_result.returncode,
                73,
                hard_link_result.stdout + hard_link_result.stderr,
            )
            self.assertIn("hard link", hard_link_result.stderr)
            self.assert_validation_was_read_only(
                hard_link, hard_link_identity
            )
            self.assert_validation_was_read_only(
                output, input_after_link_identity
            )

            sentinel = directory / "existing_report.json"
            sentinel.write_text("do not replace\n", encoding="utf-8")
            sentinel_identity = read_only_identity(sentinel)
            existing = run_validator(output, report_path=sentinel)
            self.assertEqual(
                existing.returncode,
                73,
                existing.stdout + existing.stderr,
            )
            self.assert_validation_was_read_only(sentinel, sentinel_identity)
            self.assert_validation_was_read_only(
                output, input_after_link_identity
            )

            usage = subprocess.run(
                [str(VALIDATOR)],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(usage.returncode, 64, usage.stdout + usage.stderr)

    def test_schema_v2_cannot_downgrade_when_dsp_marker_is_missing(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_validator_dsp_marker_") as temp:
            directory = Path(temp)
            raw = directory / "fixture_run022.dat"
            write_raw_fixture(raw)
            metadata = directory / "fixture_run022.dat.run.json"
            metadata.write_text(
                json.dumps(metadata_for(raw, metadata, 22), indent=2) + "\n",
                encoding="utf-8",
            )
            valid_root = directory / "fixture_run022_prod.root"
            conversion = run_converter(
                raw, metadata, valid_root, 22, save_waveforms=True
            )
            self.assertEqual(
                conversion.returncode,
                0,
                conversion.stdout + conversion.stderr,
            )
            malformed = directory / "missing_dsp_schema.root"
            copy_root_without_key(valid_root, malformed, "WaveformDspSchema")
            identity = read_only_identity(malformed)

            validation = run_validator(malformed, raw_fidelity=True)
            report = decode_report_with_exit(validation)
            self.assertEqual(report["overall_status"], "FAIL", report)
            contract = next(
                check for check in report["checks"]
                if check.get("name") == "waveform_dsp_contract"
            )
            self.assertEqual(contract["status"], "FAIL", contract)
            fidelity = next(
                check for check in report["checks"]
                if check.get("name") == "raw_root_fidelity"
            )
            self.assertEqual(fidelity["status"], "FAIL", fidelity)
            self.assert_validation_was_read_only(malformed, identity)

    def test_missing_all_external_artifacts_is_partial_provenance(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_validator_external_") as temp:
            directory = Path(temp)
            bundle = directory / "ephemeral_bundle"
            bundle.mkdir()
            copied_converter = bundle / "production_dt5730"
            shutil.copy2(PRODUCTION, copied_converter)
            copied_config = bundle / "runtime.conf"
            shutil.copy2(CONFIG, copied_config)
            raw = bundle / "fixture_run023.dat"
            write_raw_fixture(raw)
            metadata = bundle / "fixture_run023.dat.run.json"
            metadata.write_text(
                json.dumps(metadata_for(
                    raw,
                    metadata,
                    23,
                    config=copied_config,
                ) | {
                    "binary_path": str(copied_converter),
                    "binary_sha256": sha256(copied_converter),
                }, indent=2) + "\n",
                encoding="utf-8",
            )
            produced = bundle / "fixture_run023_prod.root"
            conversion = subprocess.run(
                [
                    str(copied_converter),
                    "-i", str(raw),
                    "-c", str(copied_config),
                    "-m", str(metadata),
                    "-r", "23",
                    "-o", str(produced),
                    "-w",
                ],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(
                conversion.returncode,
                0,
                conversion.stdout + conversion.stderr,
            )
            isolated_root = directory / "isolated_run023_prod.root"
            shutil.copy2(produced, isolated_root)
            shutil.rmtree(bundle)
            identity = read_only_identity(isolated_root)

            validation = run_validator(isolated_root)
            report = decode_report_with_exit(validation)
            coverage = next(
                check for check in report["checks"]
                if check.get("name") == "external_provenance_coverage"
            )
            self.assertEqual(coverage["status"], "WARN", coverage)
            self.assertEqual(
                report.get("domain_status", {}).get("provenance"),
                "WARN",
                report,
            )
            self.assertEqual(report["overall_status"], "WARN", report)
            self.assert_validation_was_read_only(isolated_root, identity)

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
            report = decode_report_with_exit(validation)
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
            validation = run_validator(output, raw_fidelity=True)
            report = decode_report_with_exit(validation)
            self.assert_report_shape(report)
            self.assertEqual(report["overall_status"], "PASS", report)
            self.assertEqual(
                report.get("metadata", {}).get("hardware", {}).get(
                    "trigger_polarity"
                ),
                "rising",
                report,
            )
            fidelity = next(
                check for check in report["checks"]
                if check.get("name") == "raw_root_fidelity"
            )
            self.assertEqual(fidelity["status"], "PASS", fidelity)
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
            report = decode_report_with_exit(validation)
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
            sampled_report = decode_report_with_exit(sampled_validation)
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

    def test_large_full_scan_discloses_sampling_and_catches_interstitial_drift(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_validator_large_metrics_") as temp:
            directory = Path(temp)
            event_count = 500_001
            transient_entry = 250_001  # odd: stride-2 robust samples skip it
            legacy = directory / "large_interstitial_drift.root"
            make_legacy_root(
                directory,
                legacy,
                event_count=event_count,
                transient_baseline_entry=transient_entry,
            )
            identity = read_only_identity(legacy)

            validation = run_validator(legacy)
            report = decode_report_with_exit(validation)
            analysis = report["analysis"]
            self.assertEqual(analysis["events_scanned"], event_count, analysis)
            self.assertEqual(analysis["events_total"], event_count, analysis)
            self.assertFalse(analysis["sampled"], analysis)
            self.assertTrue(analysis["metrics_sampled"], analysis)
            self.assertEqual(analysis["quantile_sample_stride"], 2, analysis)
            self.assertEqual(
                analysis["metric_sampling"],
                {
                    "applied": True,
                    "sample_limit": 500_000,
                    "sample_stride": 2,
                    "baseline_quantiles": "stride_sampled",
                    "baseline_settling": "stride_sampled",
                    "legacy_trigger_extrema": "stride_sampled",
                    "baseline_stream_extrema": "all_scanned_events",
                },
                analysis,
            )
            coverage = next(
                check for check in report["checks"]
                if check.get("name") == "metric_sampling_coverage"
            )
            self.assertEqual(coverage["status"], "WARN", coverage)

            channel_zero = report["channels"][0]
            metrics = channel_zero["metrics"]
            self.assertTrue(metrics["baseline_metrics_sampled"], metrics)
            self.assertEqual(metrics["baseline_sample_stride"], 2, metrics)
            self.assertEqual(metrics["baseline_samples_collected"], 250_001)
            self.assertEqual(metrics["baseline_bin_span_adc"], 0.0, metrics)
            self.assertEqual(metrics["baseline_stream_span_adc"], 100.0, metrics)
            stability = next(
                check for check in report["checks"]
                if check.get("name") == "channel_0_baseline_stability"
            )
            self.assertEqual(stability["status"], "WARN", stability)
            self.assertEqual(
                stability["observed"]["stream_span_adc"], 100.0, stability
            )

            channel_one_stability = next(
                check for check in report["checks"]
                if check.get("name") == "channel_1_baseline_stability"
            )
            self.assertEqual(
                channel_one_stability["status"], "SKIP", channel_one_stability
            )
            legacy_inference = report["summary"]["legacy_trigger_inference"]
            self.assertTrue(
                legacy_inference["metric_sampling_applied"], legacy_inference
            )
            self.assertEqual(legacy_inference["sample_stride"], 2)
            self.assertEqual(
                legacy_inference["extrema_coverage"], "stride_sampled"
            )
            self.assertNotEqual(
                report["domain_status"]["trigger_and_quality"], "PASS", report
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
            report = decode_report_with_exit(validation)
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
            report = decode_report_with_exit(validation)
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
            report = decode_report_with_exit(validation)
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
            report = decode_report_with_exit(validation)
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
            self.assertEqual(
                validation.returncode,
                70,
                validation.stdout + validation.stderr,
            )
            self.assertTrue(
                validation.stdout.strip() or validation.stderr.strip(),
                "fatal validation failure must include a diagnostic",
            )
            self.assert_validation_was_read_only(corrupt, identity)


if __name__ == "__main__":
    unittest.main()
