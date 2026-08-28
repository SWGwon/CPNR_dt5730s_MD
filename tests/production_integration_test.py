import hashlib
import json
import subprocess
import struct
import sys
import tempfile
import unittest
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


def metadata_for(raw: Path, metadata: Path, run_number: int) -> dict:
    channels = []
    for channel, baseline in ((0, 16164.0), (1, 16255.0)):
        written = round(baseline) - 8
        channels.append({
            "channel": channel,
            "trigger_enabled": True,
            "input_range_register": 0x1028 + 0x100 * channel,
            "input_range_readback": 0,
            "requested_dc_offset": 6554,
            "readback_dc_offset": 6554,
            "polarity_readback": "falling",
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
            "polarity_readback": "falling",
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
        "raw_output_path": str(raw),
        "raw_output_size_bytes": raw.stat().st_size,
        "raw_output_sha256": sha256(raw),
        "metadata_path": str(metadata),
        "config_path": str(CONFIG),
        "config_sha256": sha256(CONFIG),
        "source_config_path": str(CONFIG),
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
            "trigger_polarity": "falling",
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


def run_converter(raw: Path, metadata: Path, output: Path, run: int):
    return subprocess.run(
        [str(PRODUCTION), "-i", str(raw), "-c", str(CONFIG),
         "-m", str(metadata), "-r", str(run), "-o", str(output)],
        text=True, capture_output=True, check=False,
    )


class ProductionIntegrationTests(unittest.TestCase):
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
