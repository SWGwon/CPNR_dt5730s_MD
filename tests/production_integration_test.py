import hashlib
import configparser
import json
import math
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
    waveform_dsp_schema: int = 2,
    include_dc_offset_extension: bool = True,
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
    event_bytes = 24 + 4 * 520 * 2
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
    window_sum = recorded_events * 520 * 2.0e-9
    window_ratio = (
        100.0 * window_sum / elapsed if elapsed is not None and elapsed > 0
        else None
    )
    average_rate = (
        recorded_events / elapsed if elapsed is not None and elapsed > 0
        else None
    )
    calibration_values = {
        "dc_offset_target_tolerance_percent": parsed_config.getfloat(
            "DCOffsetCalibration", "TargetTolerancePercent", fallback=0.5
        ),
        "dc_offset_max_adjustment_iterations": parsed_config.getint(
            "DCOffsetCalibration", "MaxAdjustmentIterations", fallback=8
        ),
        "dc_offset_dac_busy_timeout_ms": parsed_config.getint(
            "DCOffsetCalibration", "DacBusyTimeoutMs", fallback=1000
        ),
        "dc_offset_step_settling_time_ms": parsed_config.getint(
            "DCOffsetCalibration", "StepSettlingTimeMs", fallback=3000
        ),
    }

    def offset_metadata(channel: int, raw_baseline: float) -> tuple[float, dict]:
        section = f"Channel_{channel}"
        mode = parsed_config.get(section, "DCOffsetMode", fallback="RawDac")
        if mode == "TargetBaseline":
            if not include_dc_offset_extension:
                raise ValueError(
                    "target-baseline fixture requires DC-offset extension"
                )
            percent = parsed_config.getfloat(section, "BaselineTargetPercent")
            target = math.floor(percent * 16383.0 / 100.0 + 0.5)
            initial = math.floor(
                (100.0 - percent) * 65535.0 / 100.0 + 0.5
            )
            # A channel-dependent final value proves that conversion validates
            # measured calibration rather than assuming one common DAC code.
            final = min(65535, initial + 1024 + channel)
            baseline = float(target)
            return baseline, {
                "dc_offset_mode": "target_baseline_percent",
                "requested_baseline_percent": percent,
                "target_baseline_adc": target,
                "initial_dc_offset_dac": initial,
                "final_dc_offset_dac": final,
                "requested_dc_offset": final,
                "readback_dc_offset": final,
                "baseline_error_adc": 0.0,
                "dc_offset_adjustment_iterations": 1,
                "dc_offset_converged": True,
            }
        raw = parsed_config.getint(section, "DCOffset")
        if not include_dc_offset_extension:
            return raw_baseline, {
                "requested_dc_offset": raw,
                "readback_dc_offset": raw,
            }
        return raw_baseline, {
            "dc_offset_mode": "raw_dac_code",
            "requested_baseline_percent": None,
            "target_baseline_adc": None,
            "initial_dc_offset_dac": raw,
            "final_dc_offset_dac": raw,
            "requested_dc_offset": raw,
            "readback_dc_offset": raw,
            "baseline_error_adc": None,
            "dc_offset_adjustment_iterations": 0,
            "dc_offset_converged": None,
        }

    channels = []
    for channel, raw_baseline in ((0, 16164.0), (1, 16255.0)):
        baseline, offset = offset_metadata(channel, raw_baseline)
        written = round(baseline) + (-8 if polarity == "falling" else 8)
        channels.append({
            "channel": channel,
            "trigger_enabled": True,
            "input_range_register": 0x1028 + 0x100 * channel,
            "input_range_readback": 0,
            **offset,
            "polarity_readback": polarity,
            "threshold_mode": "baseline_relative_mv",
            "measured_baseline_adc": baseline,
            "requested_threshold_mv": 1.0,
            "delta_adc": 8,
            "written_threshold_adc": written,
            "readback_threshold_adc": written,
            "effective_threshold_mv": abs(baseline - written) * 2000 / 16384,
        })
    for channel, raw_baseline in ((2, 8192.0), (3, 8192.0)):
        baseline, offset = offset_metadata(channel, raw_baseline)
        channels.append({
            "channel": channel,
            "trigger_enabled": False,
            "input_range_register": 0x1028 + 0x100 * channel,
            "input_range_readback": 0,
            **offset,
            "polarity_readback": polarity,
            "threshold_mode": "not_used_record_only",
            "measured_baseline_adc": baseline,
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
            **(calibration_values if include_dc_offset_extension else {}),
            "latest_acquisition_status_register": 384,
            "latest_board_failure_status_register": 0,
            "waveform_dsp_schema": waveform_dsp_schema,
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
            "record_length_granularity_samples": 10,
            "record_length": 520,
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
    if schema_version >= 2 and waveform_dsp_schema == 3:
        result["hardware"].update({
            "dsp_charge_anchor": "polarity_corrected_peak",
            "dsp_charge_window_pre_ns": 20,
            "dsp_charge_window_post_ns": 40,
            "dsp_charge_window_samples": 30,
            "dsp_short_charge_semantics": "alias_of_charge",
            "dsp_pulse_time_semantics": "polarity_corrected_peak_sample",
        })
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
            "record_length_granularity_samples",
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


def configured_fixture_baselines(config: Path = CONFIG) -> tuple[int, ...]:
    parsed = configparser.ConfigParser()
    parsed.optionxform = str
    if not parsed.read(config, encoding="utf-8"):
        raise ValueError(f"cannot read fixture config: {config}")
    legacy = (16164, 16255, 8192, 8192)
    result = []
    for channel, fallback in enumerate(legacy):
        section = f"Channel_{channel}"
        if parsed.get(section, "DCOffsetMode", fallback="RawDac") == \
                "TargetBaseline":
            percent = parsed.getfloat(section, "BaselineTargetPercent")
            result.append(math.floor(percent * 16383.0 / 100.0 + 0.5))
        else:
            result.append(fallback)
    return tuple(result)


def write_polarity_fixture(
    path: Path,
    polarity: str,
    *,
    board_counters: tuple[int, int] = (100, 101),
    opposite_record_only_pulse: bool = False,
) -> None:
    record_length = 520
    baselines = configured_fixture_baselines()
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
                elif channel == 2 and opposite_record_only_pulse:
                    waveform[200:208] = [baseline - pulse_delta] * 8
                stream.write(struct.pack(
                    f"<{record_length}H", *waveform
                ))


class ProductionIntegrationTests(unittest.TestCase):
    def test_legacy_raw_dc_offset_metadata_remains_convertible(self):
        with tempfile.TemporaryDirectory(
            prefix="cpnr_legacy_dc_offset_metadata_"
        ) as temp:
            directory = Path(temp)
            parsed = configparser.ConfigParser()
            parsed.optionxform = str
            self.assertTrue(parsed.read(CONFIG, encoding="utf-8"))
            parsed.remove_section("DCOffsetCalibration")
            for channel in range(4):
                section = f"Channel_{channel}"
                parsed[section].pop("DCOffsetMode", None)
                parsed[section].pop("BaselineTargetPercent", None)
                parsed[section]["DCOffset"] = "6554"
            config = directory / "legacy_raw.conf"
            with config.open("w", encoding="utf-8") as stream:
                parsed.write(stream)

            raw = directory / "legacy_raw.dat"
            write_polarity_fixture(raw, "falling")
            metadata = directory / "legacy_raw.run.json"
            document = metadata_for(
                raw,
                metadata,
                90,
                config=config,
                include_dc_offset_extension=False,
            )
            self.assertNotIn("dc_offset_mode", document["channels"][0])
            self.assertNotIn(
                "dc_offset_target_tolerance_percent", document["hardware"]
            )
            metadata.write_text(
                json.dumps(document, indent=2) + "\n", encoding="utf-8"
            )
            output = directory / "legacy_raw.root"
            result = run_converter(
                raw, metadata, output, 90, config=config
            )
            self.assertEqual(
                result.returncode, 0, result.stdout + result.stderr
            )
            self.assertTrue(output.is_file())

    def test_event_id_ttt_and_loss_policy_fail_closed(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_semantic_raw_test_") as temp:
            directory = Path(temp)
            event_bytes = struct.calcsize("<QIIHHI") + 4 * 520 * 2

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

    def test_charge_schema_preserves_signed_record_only_integrals(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_signed_charge_") as temp:
            directory = Path(temp)
            raw = directory / "opposite_record_only.dat"
            write_polarity_fixture(
                raw,
                "falling",
                opposite_record_only_pulse=True,
            )

            cases = (
                # With BaselineSamples=163 and no falling-pulse T0 on CH2,
                # ShortGate=40 sees three +64-ADC opposite-polarity samples;
                # LongGate=200 sees all eight.
                ("signed-v2", 2, 2, 2, -192.0, -512.0),
                ("clamped-v2", 2, 1, 1, 0.0, 0.0),
                # Runtime metadata v1 predates hardware.waveform_dsp_schema
                # and therefore retains the schema-1 clamped contract.
                ("legacy-runtime-v1", 1, 2, 1, 0.0, 0.0),
            )
            for (
                label,
                runtime_schema,
                requested_dsp_schema,
                expected_root_schema,
                expected_short,
                expected_long,
            ) in cases:
                with self.subTest(label=label):
                    run_number = {
                        "signed-v2": 81,
                        "clamped-v2": 82,
                        "legacy-runtime-v1": 83,
                    }[label]
                    metadata = directory / f"{label}.run.json"
                    metadata.write_text(
                        json.dumps(
                            metadata_for(
                                raw,
                                metadata,
                                run_number,
                                schema_version=runtime_schema,
                                waveform_dsp_schema=requested_dsp_schema,
                            ),
                            indent=2,
                        )
                        + "\n",
                        encoding="utf-8",
                    )
                    output = directory / f"{label}.root"
                    result = run_converter(
                        raw,
                        metadata,
                        output,
                        run_number,
                    )
                    self.assertEqual(
                        result.returncode,
                        0,
                        result.stdout + result.stderr,
                    )

                    expression = f'''
TFile f({json.dumps(str(output))});
auto *tree = dynamic_cast<TTree*>(f.Get("phys_tree"));
auto *schema = dynamic_cast<TParameter<int>*>(f.Get("WaveformDspSchema"));
Double_t short0 = 0.0, long0 = 0.0;
Double_t short2 = 0.0, long2 = 0.0;
Double_t pulse2 = 0.0, t0_2 = 0.0;
if (!tree || !schema || schema->GetVal() != {expected_root_schema}) gSystem->Exit(1);
if (tree->SetBranchAddress("ShortCharge_CH0", &short0) < 0 || tree->SetBranchAddress("Charge_CH0", &long0) < 0) gSystem->Exit(2);
if (tree->SetBranchAddress("ShortCharge_CH2", &short2) < 0 || tree->SetBranchAddress("Charge_CH2", &long2) < 0) gSystem->Exit(3);
if (tree->SetBranchAddress("PulseHeight_CH2", &pulse2) < 0 || tree->SetBranchAddress("PulseStart_T0_CH2", &t0_2) < 0) gSystem->Exit(4);
tree->GetEntry(0);
if (short0 != 512.0 || long0 != 512.0) gSystem->Exit(5);
if (short2 != {expected_short} || long2 != {expected_long}) gSystem->Exit(6);
if (pulse2 != 0.0 || t0_2 != -1.0) gSystem->Exit(7);
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

    def test_schema3_charge_uses_peak_centered_window_not_threshold_or_gates(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_peak_charge_") as temp:
            directory = Path(temp)
            config_text = CONFIG.read_text(encoding="utf-8")
            for key, value in (("ShortGate", 2000), ("LongGate", 1500)):
                pattern = rf"(?m)^(\s*{key}\s*=\s*)[^#\n]+?(\s*(?:#.*)?)$"
                config_text, replacements = re.subn(
                    pattern, rf"\g<1>{value}\2", config_text, count=1
                )
                self.assertEqual(replacements, 1)
            threshold_pattern = (
                r"(?m)^(\s*PulseStartThresholdAdc\s*=\s*)"
                r"[^#\n]+?(\s*(?:#.*)?)$"
            )
            config_text, replacements = re.subn(
                threshold_pattern, r"\g<1>0\2", config_text, count=1
            )
            if replacements == 0:
                # Keep the optional key inside [SoftwareDSP]; appending it to
                # the file would accidentally place it in the final channel
                # section of the INI-style config.
                config_text, replacements = re.subn(
                    r"(?m)^(\s*LongGate\s*=\s*1500\s*(?:#.*)?)$",
                    r"\1\nPulseStartThresholdAdc = 0",
                    config_text,
                    count=1,
                )
            self.assertEqual(replacements, 1)
            config = directory / "peak.conf"
            config.write_text(config_text, encoding="utf-8")

            record_length = 520
            baselines = configured_fixture_baselines(config)
            raw = directory / "peak.dat"
            with raw.open("wb") as stream:
                stream.write(struct.pack(
                    "<QIIHHI", 0, 0, record_length, 0xF, 0, 100
                ))
                for channel, baseline in enumerate(baselines):
                    waveform = [baseline] * record_length
                    if channel == 0:
                        waveform[189] = baseline - 10
                        waveform[190] = baseline - 5
                        waveform[200] = baseline - 20
                        waveform[201] = baseline + 4
                        waveform[219] = baseline - 7
                        waveform[220] = baseline - 15
                    stream.write(struct.pack(
                        f"<{record_length}H", *waveform
                    ))

            metadata = directory / "peak.run.json"
            metadata.write_text(
                json.dumps(
                    metadata_for(
                        raw, metadata, 84, config=config,
                        waveform_dsp_schema=3,
                    ),
                    indent=2,
                ) + "\n",
                encoding="utf-8",
            )
            output = directory / "peak.root"
            result = run_converter(raw, metadata, output, 84, config=config)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

            expression = f'''
TFile f({json.dumps(str(output))});
auto *tree = dynamic_cast<TTree*>(f.Get("phys_tree"));
auto *schema = dynamic_cast<TParameter<int>*>(f.Get("WaveformDspSchema"));
auto *pre = dynamic_cast<TParameter<int>*>(f.Get("DspChargeWindowPreNs"));
auto *post = dynamic_cast<TParameter<int>*>(f.Get("DspChargeWindowPostNs"));
auto *samples = dynamic_cast<TParameter<int>*>(f.Get("DspChargeWindowSamples"));
auto *anchor = dynamic_cast<TObjString*>(f.Get("DspChargeAnchor"));
Double_t qshort = 0.0, charge = 0.0, height = 0.0, peak_time = -1.0;
if (!tree || !schema || schema->GetVal() != 3) gSystem->Exit(1);
if (!pre || pre->GetVal() != 20 || !post || post->GetVal() != 40 || !samples || samples->GetVal() != 30) gSystem->Exit(2);
if (!anchor || TString(anchor->GetString()) != "polarity_corrected_peak") gSystem->Exit(3);
tree->SetBranchAddress("ShortCharge_CH0", &qshort);
tree->SetBranchAddress("Charge_CH0", &charge);
tree->SetBranchAddress("PulseHeight_CH0", &height);
tree->SetBranchAddress("PulseStart_T0_CH0", &peak_time);
tree->GetEntry(0);
if (qshort != 28.0 || charge != 28.0) gSystem->Exit(4);
if (height != 20.0 || peak_time != 400.0) gSystem->Exit(5);
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
                    expected_baselines = configured_fixture_baselines(config)
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
if (baseline0 != {expected_baselines[0]:.1f} || baseline1 != {expected_baselines[1]:.1f}) gSystem->Exit(5);
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
if (!window_sum || std::abs(window_sum->GetVal() - 2.08e-6) > 1.0e-15) gSystem->Exit(16);
if (!window_ratio || std::abs(window_ratio->GetVal() - 26.0) > 1.0e-12) gSystem->Exit(17);
if (!dead_available || dead_available->GetVal() != 0 || !dead_method || TString(dead_method->GetString()) != "unavailable_no_hardware_busy_or_livetime_scaler") gSystem->Exit(18);
if (f.Get("LiveTime_sec") || f.Get("DeadTime_pct")) gSystem->Exit(19);
auto *dsp_schema = dynamic_cast<TParameter<int>*>(f.Get("WaveformDspSchema"));
auto *dsp_baseline = dynamic_cast<TParameter<int>*>(f.Get("DspBaselineSamples"));
auto *dsp_short = dynamic_cast<TParameter<int>*>(f.Get("DspShortGateSamples"));
auto *dsp_long = dynamic_cast<TParameter<int>*>(f.Get("DspLongGateSamples"));
auto *dsp_threshold = dynamic_cast<TParameter<double>*>(f.Get("DspPulseStartThresholdAdc"));
if (!dsp_schema || dsp_schema->GetVal() != 2 || !dsp_baseline || dsp_baseline->GetVal() != 163 || !dsp_short || dsp_short->GetVal() != 40 || !dsp_long || dsp_long->GetVal() != 200 || !dsp_threshold || dsp_threshold->GetVal() != 30.0) gSystem->Exit(21);
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
                    "waveform_dsp_schema",
                    lambda value: value["hardware"].__setitem__(
                        "waveform_dsp_schema", 4
                    ),
                    "waveform_dsp_schema is out of range",
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
                (
                    "dc_offset_partial_extension",
                    lambda value: value["channels"][0].pop(
                        "baseline_error_adc"
                    ),
                    "DC-offset calibration extension is partially present",
                ),
                (
                    "dc_offset_final_alias",
                    lambda value: value["channels"][0].__setitem__(
                        "requested_dc_offset",
                        value["channels"][0]["requested_dc_offset"] + 1,
                    ),
                    "final write/readback aliases differ",
                ),
                (
                    "dc_offset_target_request",
                    lambda value: value["channels"][0].__setitem__(
                        "requested_baseline_percent", 89.0
                    ),
                    "target-baseline request did not converge",
                ),
                (
                    "dc_offset_not_converged",
                    lambda value: value["channels"][0].__setitem__(
                        "dc_offset_converged", False
                    ),
                    "target-baseline request did not converge",
                ),
                (
                    "dc_offset_error_outside_tolerance",
                    lambda value: value["channels"][0].__setitem__(
                        "baseline_error_adc", 100.0
                    ),
                    "target-baseline request did not converge",
                ),
                (
                    "dc_offset_calibration_settings",
                    lambda value: value["hardware"].__setitem__(
                        "dc_offset_target_tolerance_percent", 0.6
                    ),
                    "DC-offset calibration settings differ from config",
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
                struct.pack("<QIIHHI", 0, 0, 520, 3, 0, 0) +
                struct.pack("<1040H", *([16000] * 1040))
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
