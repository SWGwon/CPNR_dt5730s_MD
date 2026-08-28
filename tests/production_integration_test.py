import hashlib
import configparser
import json
import re
import shutil
import subprocess
import struct
import sys
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


PRODUCTION = Path(sys.argv[1]).resolve()
CONFIG = Path(sys.argv[2]).resolve()
ROOT = Path(sys.argv[3]).resolve()
ROOTLS = Path(sys.argv[4]).resolve()
# Keep unittest from treating CMake-provided paths as test names.
sys.argv = [sys.argv[0]]


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
    schema_version: int = 2,
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
    structurally_complete = raw_size % event_bytes == 0
    recorded_events = raw_size // event_bytes if structurally_complete else 0
    first_ttt = None
    last_ttt = None
    lost_events = 0
    if recorded_events:
        previous_counter = None
        with raw.open("rb") as stream:
            for event_index in range(recorded_events):
                stream.seek(event_index * event_bytes)
                header = stream.read(24)
                extended_ttt, _, _, _, _, counter = struct.unpack(
                    "<QIIHHI", header
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
            "effective_threshold_mv": abs(baseline - written) * 2000 / 16384,
        })
    for channel in (2, 3):
        channels.append({
            "channel": channel,
            "trigger_enabled": False,
            "input_range_register": 0x1028 + 0x100 * channel,
            "input_range_readback": 0,
            "requested_dc_offset": 6554,
            "readback_dc_offset": 6554,
            "polarity_readback": polarity,
            "threshold_mode": "not_used_record_only",
            "measured_baseline_adc": 8192.0,
            "requested_threshold_mv": None,
            "delta_adc": None,
            "written_threshold_adc": None,
            "readback_threshold_adc": None,
            "effective_threshold_mv": None,
        })
    result = {
        "schema_version": schema_version,
        "run_number": run_number,
        "acquisition_status": "completed",
        "termination_reason": (
            "event_limit" if recorded_events else "completed"
        ),
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
        "git_commit": "integration-fixture",
        "build_timestamp": "integration-fixture",
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
            "pair_logic": "AND",
            "explicit_trigger_routing": True,
            "global_trigger_mask_readback": 1,
            "pair_logic_readback": [4, 0, 0, 0],
        },
        "channels": channels,
    }
    if schema_version == 1:
        for key in (
            "termination_reason",
            "requested_max_events",
            "requested_run_time_sec",
            "hardware_verified_unix_time",
            "acquisition_start_unix_time",
            "acquisition_end_unix_time",
            "recorded_events",
            "lost_events",
            "requested_raw_output_path",
            "raw_output_published",
            "raw_output_finalized",
            "raw_finalization_error",
            "raw_digest_method",
            "raw_recovery_performed",
            "raw_events_before_recovery",
            "lost_events_exact",
            "raw_format_version",
            "raw_event_header_bytes",
            "raw_event_bytes",
            "last_complete_offset",
            "storage",
            "runtime_counters",
            "timing_summary",
        ):
            result.pop(key)
        for key in (
            "trigger_time_tag_raw_lsb_ns",
            "trigger_time_tag_observable_resolution_ns",
            "adc_sample_period_ns",
            "dead_time_measurement_available",
            "dead_time_method",
            "post_trigger_readback_percent",
            "connection_type",
            "connection_link",
            "connection_node",
            "connection_base_address",
            "expected_model",
            "expected_serial",
            "latest_acquisition_status_register",
            "latest_board_failure_status_register",
            "waveform_dsp_schema",
            "dsp_baseline_samples",
            "dsp_short_gate_samples",
            "dsp_long_gate_samples",
            "dsp_pulse_start_threshold_adc",
            "software_coincidence_window_ns",
        ):
            result["hardware"].pop(key)
    return result


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
        str(PRODUCTION), "-i", str(raw), "-c", str(config),
        "-m", str(metadata), "-r", str(run), "-o", str(output),
    ]
    if save_waveforms:
        command.append("-w")
    return subprocess.run(
        command,
        text=True, capture_output=True, check=False,
    )


def write_polarity_fixture(
    path: Path,
    polarity: str,
    *,
    board_counters: tuple[int, int] = (100, 101),
) -> None:
    record_length = 512
    baselines = (16164, 16255, 8192, 8192)
    pulse_delta = -64 if polarity == "falling" else 64
    patterns = (0xA55A, 0x5AA5)
    with path.open("wb") as stream:
        for event_id in range(2):
            stream.write(struct.pack(
                "<QIIHHI",
                event_id * 1000,
                event_id,
                record_length,
                0xF,
                patterns[event_id],
                board_counters[event_id],
            ))
            for channel, baseline in enumerate(baselines):
                waveform = [baseline] * record_length
                if channel in (0, 1):
                    waveform[200:208] = [baseline + pulse_delta] * 8
                stream.write(struct.pack(
                    f"<{record_length}H", *waveform
                ))


class ProductionIntegrationTests(unittest.TestCase):
    def test_event_id_ttt_and_loss_policy_fail_closed(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_semantic_raw_test_") as temp:
            directory = Path(temp)
            event_bytes = struct.calcsize("<QIIHHI") + 4 * 512 * 2

            for label, mutate, expected in (
                (
                    "event_id",
                    lambda payload: struct.pack_into(
                        "<I", payload, event_bytes + 8, 7
                    ),
                    "Raw EventID is not the exact zero-based stream index",
                ),
                (
                    "ttt",
                    lambda payload: struct.pack_into(
                        "<Q", payload, event_bytes, 0
                    ),
                    "ExtendedTTT is not strictly increasing",
                ),
            ):
                with self.subTest(label=label):
                    raw = directory / f"{label}.dat"
                    write_polarity_fixture(raw, "falling")
                    payload = bytearray(raw.read_bytes())
                    mutate(payload)
                    raw.write_bytes(payload)
                    metadata = directory / f"{label}.run.json"
                    metadata.write_text(
                        json.dumps(metadata_for(raw, metadata, 34), indent=2)
                        + "\n",
                        encoding="utf-8",
                    )
                    output = directory / f"{label}.root"
                    result = run_converter(raw, metadata, output, 34)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(expected, result.stdout + result.stderr)
                    self.assertFalse(output.exists())

            loss_raw = directory / "loss.dat"
            write_polarity_fixture(
                loss_raw, "falling", board_counters=(100, 103)
            )
            strict_metadata = directory / "loss_strict.run.json"
            strict_metadata.write_text(
                json.dumps(
                    metadata_for(loss_raw, strict_metadata, 35), indent=2
                )
                + "\n",
                encoding="utf-8",
            )
            strict_output = directory / "loss_strict.root"
            strict_result = run_converter(
                loss_raw, strict_metadata, strict_output, 35
            )
            self.assertNotEqual(strict_result.returncode, 0)
            self.assertIn(
                "exceeds the configured accepted-trigger loss policy",
                strict_result.stdout + strict_result.stderr,
            )
            self.assertFalse(strict_output.exists())

            bounded_text = CONFIG.read_text(encoding="utf-8")
            bounded_text = re.sub(
                r"(?m)^(\s*MaxLostEvents\s*=\s*)0(\s*(?:#.*)?)$",
                r"\g<1>2\2",
                bounded_text,
                count=1,
            )
            bounded_text = re.sub(
                r"(?m)^(\s*MaxLostFraction\s*=\s*)0\.0(\s*(?:#.*)?)$",
                r"\g<1>0.5\2",
                bounded_text,
                count=1,
            )
            bounded_config = directory / "loss_bounded.conf"
            bounded_config.write_text(bounded_text, encoding="utf-8")
            bounded_metadata = directory / "loss_bounded.run.json"
            bounded_metadata.write_text(
                json.dumps(
                    metadata_for(
                        loss_raw,
                        bounded_metadata,
                        36,
                        config=bounded_config,
                    ),
                    indent=2,
                )
                + "\n",
                encoding="utf-8",
            )
            bounded_output = directory / "loss_bounded.root"
            bounded_result = run_converter(
                loss_raw,
                bounded_metadata,
                bounded_output,
                36,
                config=bounded_config,
            )
            self.assertEqual(
                bounded_result.returncode,
                0,
                bounded_result.stdout + bounded_result.stderr,
            )
            self.assertTrue(bounded_output.is_file())

    def test_software_dsp_gate_config_controls_root_scalars(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_dsp_gate_test_") as temp:
            directory = Path(temp)
            config_text = CONFIG.read_text(encoding="utf-8")
            for key, value in (
                ("BaselineSamples", 100),
                ("ShortGate", 4),
                ("LongGate", 6),
            ):
                config_text, replacements = re.subn(
                    rf"(?m)^(\s*{key}\s*=\s*)\d+(\s*(?:#.*)?)$",
                    rf"\g<1>{value}\2",
                    config_text,
                    count=1,
                )
                self.assertEqual(replacements, 1)
            config = directory / "dsp.conf"
            config.write_text(config_text, encoding="utf-8")
            raw = directory / "dsp.dat"
            write_polarity_fixture(raw, "falling")
            metadata = directory / "dsp.run.json"
            metadata.write_text(
                json.dumps(
                    metadata_for(
                        raw, metadata, 33, config=config, polarity="falling"
                    ),
                    indent=2,
                )
                + "\n",
                encoding="utf-8",
            )
            output = directory / "dsp.root"
            result = run_converter(raw, metadata, output, 33, config=config)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            expression = f'''
TFile f({json.dumps(str(output))});
auto *tree = dynamic_cast<TTree*>(f.Get("phys_tree"));
Double_t qshort = 0.0, qlong = 0.0;
if (!tree || tree->SetBranchAddress("ShortCharge_CH0", &qshort) < 0 || tree->SetBranchAddress("Charge_CH0", &qlong) < 0) gSystem->Exit(1);
tree->GetEntry(0);
if (qshort != 256.0 || qlong != 384.0) gSystem->Exit(2);
auto *baseline = dynamic_cast<TParameter<int>*>(f.Get("DspBaselineSamples"));
auto *short_gate = dynamic_cast<TParameter<int>*>(f.Get("DspShortGateSamples"));
auto *long_gate = dynamic_cast<TParameter<int>*>(f.Get("DspLongGateSamples"));
if (!baseline || baseline->GetVal() != 100 || !short_gate || short_gate->GetVal() != 4 || !long_gate || long_gate->GetVal() != 6) gSystem->Exit(3);
'''
            root_check = subprocess.run(
                [str(ROOT), "-l", "-b", "-q", "-e", expression],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(
                root_check.returncode,
                0,
                root_check.stdout + root_check.stderr,
            )

    def test_board_counter_duplicate_backward_and_rollover(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_counter_test_") as temp:
            directory = Path(temp)
            for label, counters, should_succeed in (
                ("duplicate", (100, 100), False),
                ("backward", (100, 99), False),
                ("rollover", (0xFFFFFF, 0), True),
            ):
                with self.subTest(label=label):
                    raw = directory / f"{label}.dat"
                    write_polarity_fixture(
                        raw, "falling", board_counters=counters
                    )
                    metadata = directory / f"{label}.run.json"
                    metadata.write_text(
                        json.dumps(
                            metadata_for(raw, metadata, 71), indent=2
                        ) + "\n",
                        encoding="utf-8",
                    )
                    output = directory / f"{label}.root"
                    raw_identity = read_only_identity(raw)
                    metadata_identity = read_only_identity(metadata)
                    result = run_converter(raw, metadata, output, 71)
                    diagnostics = result.stdout + result.stderr
                    self.assertEqual(
                        result.returncode == 0,
                        should_succeed,
                        diagnostics,
                    )
                    self.assertEqual(output.exists(), should_succeed)
                    self.assertEqual(read_only_identity(raw), raw_identity)
                    self.assertEqual(
                        read_only_identity(metadata), metadata_identity
                    )
                    self.assertEqual(
                        list(directory.glob(f"{label}.root.partial.*")), []
                    )
                    if should_succeed:
                        expression = f'''
TFile f({json.dumps(str(output))});
auto *lost = dynamic_cast<TParameter<Long64_t>*>(f.Get("LostEvents_count"));
if (!lost || lost->GetVal() != 0) gSystem->Exit(1);
'''
                        check = subprocess.run(
                            [str(ROOT), "-l", "-b", "-q", "-e", expression],
                            text=True,
                            capture_output=True,
                            check=False,
                        )
                        self.assertEqual(
                            check.returncode,
                            0,
                            check.stdout + check.stderr,
                        )
                    elif label == "duplicate":
                        self.assertIn("Duplicate/stale", diagnostics)
                    else:
                        self.assertIn("Backward/reset", diagnostics)

    def test_relocated_bundle_is_content_authenticated_and_read_only(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_relocation_test_") as temp:
            directory = Path(temp)
            original = directory / "original"
            relocated = directory / "relocated"
            tampered = directory / "tampered"
            original.mkdir()
            relocated.mkdir()
            tampered.mkdir()

            raw_name = "fixture_run041.dat"
            config_name = raw_name + ".config.conf"
            metadata_name = raw_name + ".run.json"
            original_raw = original / raw_name
            original_config = original / config_name
            original_metadata = original / metadata_name
            write_polarity_fixture(original_raw, "falling")
            shutil.copy2(CONFIG, original_config)
            original_metadata.write_text(
                json.dumps(metadata_for(
                    original_raw,
                    original_metadata,
                    41,
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

            output = relocated / "fixture_run041_prod.root"
            result = run_converter(
                relocated_raw,
                relocated_metadata,
                output,
                41,
                config=relocated_config,
            )
            self.assertEqual(
                result.returncode,
                0,
                result.stdout + result.stderr,
            )
            diagnostics = result.stdout + result.stderr
            for warning in (
                "Raw input relocated",
                "Runtime config relocated",
                "Runtime metadata sidecar relocated",
            ):
                self.assertIn(warning, diagnostics)
            for path, identity in original_identities.items():
                self.assertEqual(read_only_identity(path), identity)
            for path, identity in relocated_identities.items():
                self.assertEqual(read_only_identity(path), identity)

            root_path = json.dumps(str(output))
            recorded_raw = json.dumps(str(original_raw))
            resolved_raw = json.dumps(str(relocated_raw.resolve()))
            recorded_config = json.dumps(str(original_config))
            resolved_config = json.dumps(str(relocated_config.resolve()))
            recorded_metadata = json.dumps(str(original_metadata))
            resolved_metadata = json.dumps(str(relocated_metadata.resolve()))
            metadata_digest = json.dumps(sha256(relocated_metadata))
            expression = f'''
TFile f({root_path});
auto read_string = [&](const char *name) {{
  auto *value = dynamic_cast<TObjString*>(f.Get(name));
  return value ? TString(value->GetString()) : TString();
}};
if (read_string("RecordedRawOutputPath") != {recorded_raw}) gSystem->Exit(1);
if (read_string("ResolvedRawInputPath") != {resolved_raw}) gSystem->Exit(2);
if (read_string("RecordedConfigPath") != {recorded_config}) gSystem->Exit(3);
if (read_string("ResolvedConfigPath") != {resolved_config}) gSystem->Exit(4);
if (read_string("RecordedMetadataPath") != {recorded_metadata}) gSystem->Exit(5);
if (read_string("ResolvedMetadataPath") != {resolved_metadata}) gSystem->Exit(6);
if (read_string("RunMetadataSha256") != {metadata_digest}) gSystem->Exit(7);
'''
            root_check = subprocess.run(
                [str(ROOT), "-l", "-b", "-q", "-e", expression],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(
                root_check.returncode,
                0,
                root_check.stdout + root_check.stderr,
            )

            tampered_raw = tampered / raw_name
            tampered_config = tampered / config_name
            tampered_metadata = tampered / metadata_name
            for source, destination in zip(
                original_files,
                (tampered_raw, tampered_config, tampered_metadata),
            ):
                shutil.copy2(source, destination)
            changed = bytearray(tampered_raw.read_bytes())
            changed[-1] ^= 1
            tampered_raw.write_bytes(changed)
            tampered_files = (
                tampered_raw,
                tampered_config,
                tampered_metadata,
            )
            tampered_identities = {
                path: read_only_identity(path) for path in tampered_files
            }
            tampered_output = tampered / "fixture_run041_prod.root"
            tampered_result = run_converter(
                tampered_raw,
                tampered_metadata,
                tampered_output,
                41,
                config=tampered_config,
            )
            self.assertNotEqual(tampered_result.returncode, 0)
            self.assertIn(
                "Raw input SHA-256 does not match RunMetadata",
                tampered_result.stdout + tampered_result.stderr,
            )
            self.assertFalse(tampered_output.exists())
            for path, identity in original_identities.items():
                self.assertEqual(read_only_identity(path), identity)
            for path, identity in tampered_identities.items():
                self.assertEqual(read_only_identity(path), identity)

    def test_parallel_converters_keep_reserved_partial_inode(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_parallel_root_test_") as temp:
            directory = Path(temp)
            jobs = []
            for index in range(12):
                run_number = 100 + index
                raw = directory / f"parallel_run{run_number}.dat"
                raw.write_bytes(b"")
                metadata = directory / f"parallel_run{run_number}.run.json"
                metadata.write_text(
                    json.dumps(metadata_for(
                        raw, metadata, run_number
                    ), indent=2) + "\n",
                    encoding="utf-8",
                )
                output = directory / f"parallel_run{run_number}.root"
                jobs.append((raw, metadata, output, run_number))

            def convert(job):
                raw, metadata, output, run_number = job
                return output, run_converter(
                    raw, metadata, output, run_number
                )

            with ThreadPoolExecutor(max_workers=len(jobs)) as executor:
                results = list(executor.map(convert, jobs))
            for output, result in results:
                self.assertEqual(
                    result.returncode,
                    0,
                    result.stdout + result.stderr,
                )
                self.assertTrue(output.is_file())
            self.assertEqual(list(directory.glob("*.partial.*")), [])

    def test_polarity_dsp_audit_branches_and_64bit_summary(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_polarity_test_") as temp:
            directory = Path(temp)
            config_text = CONFIG.read_text(encoding="utf-8")
            rising_text, replacements = re.subn(
                r"(?m)^(\s*TriggerPolarity\s*=\s*)1(\s*(?:#.*)?)$",
                r"\g<1>0\2",
                config_text,
                count=1,
            )
            self.assertEqual(replacements, 1)
            rising_config = directory / "rising.conf"
            rising_config.write_text(rising_text, encoding="utf-8")

            for polarity, config, run_number in (
                ("falling", CONFIG, 31),
                ("rising", rising_config, 32),
            ):
                with self.subTest(polarity=polarity):
                    raw = directory / f"{polarity}.dat"
                    write_polarity_fixture(raw, polarity)
                    metadata = directory / f"{polarity}.run.json"
                    metadata.write_text(
                        json.dumps(metadata_for(
                            raw,
                            metadata,
                            run_number,
                            config=config,
                            polarity=polarity,
                        ), indent=2) + "\n",
                        encoding="utf-8",
                    )
                    output = directory / f"{polarity}.root"
                    result = run_converter(
                        raw,
                        metadata,
                        output,
                        run_number,
                        config=config,
                        save_waveforms=True,
                    )
                    self.assertEqual(
                        result.returncode,
                        0,
                        result.stdout + result.stderr,
                    )
                    root_path = json.dumps(str(output))
                    expression = f'''
TFile f({root_path});
auto *tree = dynamic_cast<TTree*>(f.Get("phys_tree"));
if (!tree || tree->GetEntries() != 2) gSystem->Exit(1);
auto *pattern_leaf = tree->GetLeaf("Pattern");
auto *counter_leaf = tree->GetLeaf("BoardEventCounter");
if (!pattern_leaf || TString(pattern_leaf->GetTypeName()) != "UShort_t") gSystem->Exit(2);
if (!counter_leaf || TString(counter_leaf->GetTypeName()) != "UInt_t") gSystem->Exit(3);
UShort_t pattern = 0;
UInt_t counter = 0;
Double_t charge0 = 0.0, charge1 = 0.0;
Double_t short0 = 0.0, short1 = 0.0;
Double_t pulse0 = 0.0, pulse1 = 0.0;
Double_t t0_0 = 0.0, t0_1 = 0.0;
Double_t baseline0 = 0.0, baseline1 = 0.0;
tree->SetBranchAddress("Pattern", &pattern);
tree->SetBranchAddress("BoardEventCounter", &counter);
tree->SetBranchAddress("ShortCharge_CH0", &short0);
tree->SetBranchAddress("ShortCharge_CH1", &short1);
tree->SetBranchAddress("Charge_CH0", &charge0);
tree->SetBranchAddress("Charge_CH1", &charge1);
tree->SetBranchAddress("PulseHeight_CH0", &pulse0);
tree->SetBranchAddress("PulseHeight_CH1", &pulse1);
tree->SetBranchAddress("PulseStart_T0_CH0", &t0_0);
tree->SetBranchAddress("PulseStart_T0_CH1", &t0_1);
tree->SetBranchAddress("Baseline_CH0", &baseline0);
tree->SetBranchAddress("Baseline_CH1", &baseline1);
tree->GetEntry(0);
if (pattern != 0xA55A || counter != 100) gSystem->Exit(4);
if (baseline0 != 16164.0 || baseline1 != 16255.0) gSystem->Exit(5);
if (short0 != 512.0 || short1 != 512.0) gSystem->Exit(20);
if (charge0 != 512.0 || charge1 != 512.0) gSystem->Exit(6);
if (pulse0 != 64.0 || pulse1 != 64.0) gSystem->Exit(7);
if (t0_0 != 400.0 || t0_1 != 400.0) gSystem->Exit(8);
tree->GetEntry(1);
if (pattern != 0x5AA5 || counter != 101) gSystem->Exit(9);
auto *recorded = dynamic_cast<TParameter<Long64_t>*>(f.Get("RecordedEvents_count"));
auto *lost = dynamic_cast<TParameter<Long64_t>*>(f.Get("LostEvents_count"));
if (!recorded || recorded->GetVal() != 2) gSystem->Exit(10);
if (!lost || lost->GetVal() != 0) gSystem->Exit(11);
auto *timing_schema = dynamic_cast<TParameter<int>*>(f.Get("TimingSummarySchema"));
auto *ttt_lsb = dynamic_cast<TParameter<double>*>(f.Get("TriggerTimeTagRawLsb_ns"));
auto *ttt_resolution = dynamic_cast<TParameter<double>*>(f.Get("TriggerTimeTagObservableResolution_ns"));
auto *real_time = dynamic_cast<TParameter<double>*>(f.Get("RealTime_sec"));
auto *window_sum = dynamic_cast<TParameter<double>*>(f.Get("RecordedWindowSum_sec"));
auto *window_ratio = dynamic_cast<TParameter<double>*>(f.Get("RecordedWindowToElapsed_pct"));
auto *dead_available = dynamic_cast<TParameter<int>*>(f.Get("DeadTimeMeasurementAvailable"));
auto *dead_method = dynamic_cast<TObjString*>(f.Get("DeadTimeMethod"));
if (!timing_schema || timing_schema->GetVal() != 2) gSystem->Exit(12);
if (!ttt_lsb || ttt_lsb->GetVal() != 8.0) gSystem->Exit(13);
if (!ttt_resolution || ttt_resolution->GetVal() != 16.0) gSystem->Exit(14);
if (!real_time || std::abs(real_time->GetVal() - 8.0e-6) > 1.0e-15) gSystem->Exit(15);
if (!window_sum || std::abs(window_sum->GetVal() - 2.048e-6) > 1.0e-15) gSystem->Exit(16);
if (!window_ratio || std::abs(window_ratio->GetVal() - 25.6) > 1.0e-12) gSystem->Exit(17);
if (!dead_available || dead_available->GetVal() != 0 || !dead_method || TString(dead_method->GetString()) != "unavailable_no_hardware_busy_or_livetime_scaler") gSystem->Exit(18);
if (f.Get("LiveTime_sec") || f.Get("DeadTime_pct")) gSystem->Exit(19);
auto *dsp_schema = dynamic_cast<TParameter<int>*>(f.Get("WaveformDspSchema"));
auto *dsp_baseline = dynamic_cast<TParameter<int>*>(f.Get("DspBaselineSamples"));
auto *dsp_short = dynamic_cast<TParameter<int>*>(f.Get("DspShortGateSamples"));
auto *dsp_long = dynamic_cast<TParameter<int>*>(f.Get("DspLongGateSamples"));
auto *dsp_threshold = dynamic_cast<TParameter<double>*>(f.Get("DspPulseStartThresholdAdc"));
if (!dsp_schema || dsp_schema->GetVal() != 1 || !dsp_baseline || dsp_baseline->GetVal() != 163 || !dsp_short || dsp_short->GetVal() != 40 || !dsp_long || dsp_long->GetVal() != 200 || !dsp_threshold || dsp_threshold->GetVal() != 30.0) gSystem->Exit(21);
'''
                    root_check = subprocess.run(
                        [str(ROOT), "-l", "-b", "-q", "-e", expression],
                        text=True,
                        capture_output=True,
                        check=False,
                    )
                    self.assertEqual(
                        root_check.returncode,
                        0,
                        root_check.stdout + root_check.stderr,
                    )

    def test_provenance_success_and_fail_closed_inputs(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_production_test_") as temp:
            directory = Path(temp)
            raw = directory / "fixture_run021.dat"
            raw.write_bytes(b"")
            metadata = directory / "fixture.run.json"
            metadata.write_text(
                json.dumps(metadata_for(raw, metadata, 21), indent=2) + "\n",
                encoding="utf-8",
            )
            output = directory / "fixture.root"
            result = run_converter(raw, metadata, output, 21)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(output.is_file())
            listing = subprocess.run(
                [str(ROOTLS), "-l", str(output)], text=True,
                capture_output=True, check=False,
            )
            self.assertEqual(listing.returncode, 0,
                             listing.stdout + listing.stderr)
            for key in ("RunConfig", "RunConfigExact", "RunMetadata",
                        "RunMetadataSha256", "RunNumber",
                        "ExecutableSha256", "TimingSummarySchema",
                        "TriggerTimeTagRawLsb_ns",
                        "TriggerTimeTagObservableResolution_ns",
                        "RecordedWindowSum_sec",
                        "DeadTimeMeasurementAvailable", "DeadTimeMethod",
                        "WaveformDspSchema", "DspBaselineSamples",
                        "DspShortGateSamples", "DspLongGateSamples",
                        "DspPulseStartThresholdAdc",
                        "phys_tree"):
                self.assertIn(key, listing.stdout)
            root_check = subprocess.run(
                [str(ROOT), "-l", "-b", "-q", "-e",
                 f'TFile f("{output}"); auto *run = '
                 'dynamic_cast<TParameter<int>*>(f.Get("RunNumber")); '
                 'if (!run || run->GetVal() != 21) gSystem->Exit(1);'],
                text=True, capture_output=True, check=False,
            )
            self.assertEqual(root_check.returncode, 0,
                             root_check.stdout + root_check.stderr)

            duplicate_metadata = directory / "duplicate.run.json"
            duplicate = metadata_for(raw, duplicate_metadata, 21)
            duplicate_text = json.dumps(duplicate, indent=2)
            duplicate_text = duplicate_text.replace(
                '"acquisition_status": "completed",',
                '"acquisition_status": "completed",\n'
                '  "acquisition_status": "completed",',
                1,
            )
            duplicate_metadata.write_text(duplicate_text + "\n", encoding="utf-8")
            duplicate_output = directory / "duplicate.root"
            duplicate_result = run_converter(
                raw, duplicate_metadata, duplicate_output, 21
            )
            self.assertNotEqual(duplicate_result.returncode, 0)
            self.assertIn("duplicate JSON key",
                          duplicate_result.stdout + duplicate_result.stderr)
            self.assertFalse(duplicate_output.exists())

            incomplete_metadata = directory / "incomplete.run.json"
            incomplete = metadata_for(raw, incomplete_metadata, 21)
            del incomplete["channels"][0]["readback_threshold_adc"]
            incomplete_metadata.write_text(
                json.dumps(incomplete, indent=2) + "\n", encoding="utf-8"
            )
            incomplete_output = directory / "incomplete.root"
            incomplete_result = run_converter(
                raw, incomplete_metadata, incomplete_output, 21
            )
            self.assertNotEqual(incomplete_result.returncode, 0)
            self.assertIn("missing readback_threshold_adc",
                          incomplete_result.stdout + incomplete_result.stderr)
            self.assertFalse(incomplete_output.exists())

            v2_tamper_cases = (
                (
                    "unpublished",
                    lambda value: value.__setitem__(
                        "raw_output_published", False
                    ),
                    "not finalized and published",
                ),
                (
                    "ttt_unit",
                    lambda value: value["hardware"].__setitem__(
                        "trigger_time_tag_raw_lsb_ns", 16
                    ),
                    "timing/dead-time semantics are invalid",
                ),
                (
                    "post_trigger_readback",
                    lambda value: value["hardware"].__setitem__(
                        "post_trigger_readback_percent", 59
                    ),
                    "post-trigger readback differs",
                ),
                (
                    "zmq_watermark",
                    lambda value: value["runtime_counters"].__setitem__(
                        "zmq_send_hwm_approx_bytes",
                        value["runtime_counters"][
                            "zmq_send_hwm_approx_bytes"
                        ] + 1,
                    ),
                    "ZeroMQ watermark accounting is inconsistent",
                ),
                (
                    "timing_window",
                    lambda value: value["timing_summary"].__setitem__(
                        "recorded_window_sum_sec", 1.0
                    ),
                    "recorded-window sum is inconsistent",
                ),
                (
                    "timestamp_order",
                    lambda value: value.__setitem__(
                        "acquisition_start_unix_time",
                        value["acquisition_end_unix_time"] + 1,
                    ),
                    "timestamps are not ordered",
                ),
            )
            for label, mutate, expected_error in v2_tamper_cases:
                with self.subTest(metadata_v2_tamper=label):
                    tampered_metadata = directory / f"{label}.run.json"
                    tampered_document = metadata_for(
                        raw, tampered_metadata, 21
                    )
                    mutate(tampered_document)
                    tampered_metadata.write_text(
                        json.dumps(tampered_document, indent=2) + "\n",
                        encoding="utf-8",
                    )
                    tampered_output = directory / f"{label}.root"
                    raw_identity = read_only_identity(raw)
                    metadata_identity = read_only_identity(tampered_metadata)
                    tampered_result = run_converter(
                        raw, tampered_metadata, tampered_output, 21
                    )
                    diagnostics = (
                        tampered_result.stdout + tampered_result.stderr
                    )
                    self.assertNotEqual(tampered_result.returncode, 0)
                    self.assertIn(expected_error, diagnostics)
                    self.assertFalse(tampered_output.exists())
                    self.assertEqual(read_only_identity(raw), raw_identity)
                    self.assertEqual(
                        read_only_identity(tampered_metadata),
                        metadata_identity,
                    )
                    self.assertEqual(
                        list(directory.glob(f"{label}.root.partial.*")), []
                    )

            bad_raw = directory / "bad_run022.dat"
            bad_raw.write_bytes(b"x\n")
            bad_metadata = directory / "bad.run.json"
            bad_metadata.write_text(
                json.dumps(
                    metadata_for(
                        bad_raw, bad_metadata, 22, schema_version=1
                    ),
                    indent=2,
                ) + "\n",
                encoding="utf-8",
            )
            bad_output = directory / "bad.root"
            bad_result = run_converter(bad_raw, bad_metadata, bad_output, 22)
            self.assertNotEqual(bad_result.returncode, 0)
            self.assertIn("partial event header",
                          bad_result.stdout + bad_result.stderr)
            self.assertFalse(bad_output.exists())
            self.assertEqual(list(directory.glob("bad.root.partial.*")), [])

            changed_raw = directory / "changed_run023.dat"
            changed_raw.write_bytes(
                struct.pack("<QIIHHI", 0, 0, 512, 3, 0, 0) +
                struct.pack("<1024H", *([16000] * 1024))
            )
            changed_metadata = directory / "changed.run.json"
            changed_metadata.write_text(
                json.dumps(
                    metadata_for(
                        changed_raw,
                        changed_metadata,
                        23,
                        schema_version=1,
                    ),
                    indent=2,
                ) + "\n",
                encoding="utf-8",
            )
            changed_bytes = bytearray(changed_raw.read_bytes())
            changed_bytes[-1] ^= 1
            changed_raw.write_bytes(changed_bytes)
            changed_output = directory / "changed.root"
            changed_result = run_converter(
                changed_raw, changed_metadata, changed_output, 23
            )
            self.assertNotEqual(changed_result.returncode, 0)
            self.assertIn("SHA-256 does not match",
                          changed_result.stdout + changed_result.stderr)
            self.assertFalse(changed_output.exists())


if __name__ == "__main__":
    unittest.main()
