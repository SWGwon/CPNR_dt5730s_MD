import hashlib
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
        "git_commit": "integration-fixture",
        "build_timestamp": "integration-fixture",
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
            "pair_logic": "AND",
            "explicit_trigger_routing": True,
            "global_trigger_mask_readback": 1,
            "pair_logic_readback": [4, 0, 0, 0],
        },
        "channels": channels,
    }


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
    board_counters: tuple[int, int] = (100, 103),
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
    def test_board_counter_duplicate_backward_and_rollover(self):
        with tempfile.TemporaryDirectory(prefix="cpnr_counter_test_") as temp:
            directory = Path(temp)
            for label, counters, should_succeed in (
                ("duplicate", (100, 100), False),
                ("backward", (100, 99), False),
                ("rollover", (0xFFFFFE, 1), True),
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
if (!lost || lost->GetVal() != 2) gSystem->Exit(1);
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
Double_t pulse0 = 0.0, pulse1 = 0.0;
Double_t t0_0 = 0.0, t0_1 = 0.0;
Double_t baseline0 = 0.0, baseline1 = 0.0;
tree->SetBranchAddress("Pattern", &pattern);
tree->SetBranchAddress("BoardEventCounter", &counter);
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
if (charge0 != 512.0 || charge1 != 512.0) gSystem->Exit(6);
if (pulse0 != 64.0 || pulse1 != 64.0) gSystem->Exit(7);
if (t0_0 != 400.0 || t0_1 != 400.0) gSystem->Exit(8);
tree->GetEntry(1);
if (pattern != 0x5AA5 || counter != 103) gSystem->Exit(9);
auto *recorded = dynamic_cast<TParameter<Long64_t>*>(f.Get("RecordedEvents_count"));
auto *lost = dynamic_cast<TParameter<Long64_t>*>(f.Get("LostEvents_count"));
if (!recorded || recorded->GetVal() != 2) gSystem->Exit(10);
if (!lost || lost->GetVal() != 2) gSystem->Exit(11);
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
                        "ExecutableSha256", "phys_tree"):
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

            bad_raw = directory / "bad_run022.dat"
            bad_raw.write_bytes(b"x\n")
            bad_metadata = directory / "bad.run.json"
            bad_metadata.write_text(
                json.dumps(metadata_for(bad_raw, bad_metadata, 22), indent=2) + "\n",
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
                    metadata_for(changed_raw, changed_metadata, 23), indent=2
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
