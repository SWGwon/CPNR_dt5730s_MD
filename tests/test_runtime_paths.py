import os
import stat
import sys
import tempfile
import time
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
GUI_DIR = PROJECT_ROOT / "gui"
if str(GUI_DIR) not in sys.path:
    sys.path.insert(0, str(GUI_DIR))

from core.runtime_paths import (  # noqa: E402
    RuntimeValidationError,
    build_frontend_command,
    build_production_arguments,
    build_root_validation_arguments,
    create_run_config_snapshot,
    default_production_output,
    file_identity,
    frontend_sources,
    metadata_status_paths,
    production_sources,
    root_validator_sources,
    sidecar_paths,
    verify_binary_fresh,
    verify_deployed_gui,
    verify_expected_hashes,
    verify_paths_absent,
)


class RuntimePathTests(unittest.TestCase):
    def test_binary_freshness_tracks_sha256_implementation(self):
        self.assertIn(PROJECT_ROOT / "src" / "Sha256.cpp",
                      frontend_sources(PROJECT_ROOT))
        self.assertIn(PROJECT_ROOT / "src" / "Sha256.cpp",
                      production_sources(PROJECT_ROOT))
        self.assertIn(PROJECT_ROOT / "include" / "Sha256.h",
                      production_sources(PROJECT_ROOT))
        self.assertIn(PROJECT_ROOT / "src" / "RootValidator.cpp",
                      root_validator_sources(PROJECT_ROOT))
        self.assertIn(PROJECT_ROOT / "src" / "DAQConfig.cpp",
                      root_validator_sources(PROJECT_ROOT))
        self.assertIn(PROJECT_ROOT / "src" / "Sha256.cpp",
                      root_validator_sources(PROJECT_ROOT))
        self.assertIn(PROJECT_ROOT / "include" / "DAQConfig.h",
                      root_validator_sources(PROJECT_ROOT))
        self.assertIn(PROJECT_ROOT / "include" / "EventHeader.h",
                      root_validator_sources(PROJECT_ROOT))
        self.assertIn(PROJECT_ROOT / ".git" / "index",
                      root_validator_sources(PROJECT_ROOT))

    def test_sidecar_names_are_bound_to_raw_file(self):
        raw = Path("/tmp/example_run007.dat")
        config, metadata = sidecar_paths(raw)
        self.assertEqual(config, Path("/tmp/example_run007.dat.config.conf"))
        self.assertEqual(metadata, Path("/tmp/example_run007.dat.run.json"))
        self.assertEqual(
            metadata_status_paths(metadata),
            (
                Path("/tmp/example_run007.dat.run.json.status."
                     "hardware_verified_not_started.json"),
                Path("/tmp/example_run007.dat.run.json.status.running.json"),
            ),
        )

    def test_config_snapshot_reserves_only_snapshot_without_overwrite(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            raw = Path(temp_dir) / "run007.dat"
            config, metadata = sidecar_paths(raw)
            contents = "[Digitizer]\nInputRangeMv=2000\n"

            identity = create_run_config_snapshot(raw, contents)

            self.assertFalse(raw.exists())
            self.assertFalse(metadata.exists())
            self.assertEqual(config.read_text(encoding="utf-8"), contents)
            self.assertEqual(identity["path"], str(config.resolve()))
            with self.assertRaises(RuntimeValidationError):
                create_run_config_snapshot(raw, "replacement")
            self.assertEqual(config.read_text(encoding="utf-8"), contents)

    def test_any_existing_run_artifact_blocks_before_snapshot_creation(self):
        for collision_name in (
            "raw", "config", "metadata", "hardware_status", "running_status"
        ):
            with self.subTest(collision=collision_name):
                with tempfile.TemporaryDirectory() as temp_dir:
                    raw = Path(temp_dir) / "run008.dat"
                    config, metadata = sidecar_paths(raw)
                    hardware_status, running_status = metadata_status_paths(
                        metadata
                    )
                    collision = {
                        "raw": raw,
                        "config": config,
                        "metadata": metadata,
                        "hardware_status": hardware_status,
                        "running_status": running_status,
                    }[collision_name]
                    collision.write_text("sentinel", encoding="utf-8")

                    with self.assertRaises(RuntimeValidationError):
                        create_run_config_snapshot(raw, "new config")

                    self.assertEqual(
                        collision.read_text(encoding="utf-8"), "sentinel"
                    )
                    for candidate in (
                        raw, config, metadata, hardware_status, running_status
                    ):
                        if candidate != collision:
                            self.assertFalse(os.path.lexists(candidate))

    def test_broken_symlink_is_a_collision_and_is_not_followed(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            raw = Path(temp_dir) / "run009.dat"
            config, metadata = sidecar_paths(raw)
            missing_target = Path(temp_dir) / "missing-target"
            raw.symlink_to(missing_target)

            with self.assertRaises(RuntimeValidationError):
                create_run_config_snapshot(raw, "new config")

            self.assertTrue(raw.is_symlink())
            self.assertEqual(os.readlink(raw), str(missing_target))
            self.assertFalse(os.path.lexists(config))
            self.assertFalse(os.path.lexists(metadata))

    def test_prelaunch_absence_check_catches_broken_symlink(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "metadata.run.json"
            path.symlink_to(Path(temp_dir) / "missing")
            with self.assertRaises(RuntimeValidationError):
                verify_paths_absent([path])

    def test_frontend_command_contains_absolute_provenance_paths(self):
        command = build_frontend_command(
            "/opt/cpnr/frontend_dt5730",
            "/data/run.dat.config.conf",
            "/data/run.dat",
            21,
            "/data/run.dat.run.json",
            max_events=200000,
        )
        self.assertEqual(command[0], "/opt/cpnr/frontend_dt5730")
        self.assertEqual(command[command.index("-c") + 1], "/data/run.dat.config.conf")
        self.assertEqual(command[command.index("-o") + 1], "/data/run.dat")
        self.assertEqual(command[command.index("-r") + 1], "21")
        self.assertEqual(command[command.index("-m") + 1], "/data/run.dat.run.json")
        self.assertEqual(command[command.index("-n") + 1], "200000")

    def test_production_arguments_always_carry_run_context(self):
        arguments = build_production_arguments(
            "/data/run.dat",
            "/data/run.dat.config.conf",
            21,
            "/data/run.dat.run.json",
            root_output="/data/run_prod.root",
            save_waveforms=True,
        )
        self.assertEqual(arguments[arguments.index("-c") + 1],
                         "/data/run.dat.config.conf")
        self.assertEqual(arguments[arguments.index("-r") + 1], "21")
        self.assertEqual(arguments[arguments.index("-m") + 1],
                         "/data/run.dat.run.json")
        self.assertIn("-w", arguments)

    def test_production_output_and_validation_arguments_match_cli(self):
        raw = Path("/data/take.with.dots/run021.dat")
        self.assertEqual(
            default_production_output(raw),
            Path("/data/take.with.dots/run021_prod.root"),
        )
        self.assertEqual(
            build_root_validation_arguments(
                "/data/take.with.dots/run021_prod.root", max_events=25000
            ),
            [
                "-i", "/data/take.with.dots/run021_prod.root",
                "--max-events", "25000",
            ],
        )
        with self.assertRaises(RuntimeValidationError):
            build_root_validation_arguments("/data/run.root", max_events=-1)

    def test_non_positive_run_number_is_rejected(self):
        with self.assertRaises(RuntimeValidationError):
            build_frontend_command("/bin/true", "/tmp/a", "/tmp/b", 0, "/tmp/c")
        with self.assertRaises(RuntimeValidationError):
            build_production_arguments("/tmp/a", "/tmp/b", 0, "/tmp/c")

    def test_stale_binary_and_hash_changes_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            directory = Path(temp_dir)
            executable = directory / "frontend"
            source = directory / "DAQManager.cpp"
            executable.write_text("old binary", encoding="utf-8")
            executable.chmod(executable.stat().st_mode | stat.S_IXUSR)
            source.write_text("source", encoding="utf-8")

            now_ns = time.time_ns()
            os.utime(executable, ns=(now_ns - 2_000_000_000,
                                     now_ns - 2_000_000_000))
            os.utime(source, ns=(now_ns, now_ns))
            with self.assertRaises(RuntimeValidationError):
                verify_binary_fresh(executable, [source])

            os.utime(executable, ns=(now_ns + 2_000_000_000,
                                     now_ns + 2_000_000_000))
            identity = verify_binary_fresh(executable, [source])
            expected = {str(executable): identity["sha256"]}
            verify_expected_hashes(expected)
            executable.write_text("changed binary", encoding="utf-8")
            with self.assertRaises(RuntimeValidationError):
                verify_expected_hashes(expected)

    def test_deployed_gui_mismatch_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            source_gui = root / "gui"
            runtime_gui = root / "bin" / "gui"
            (source_gui / "widgets").mkdir(parents=True)
            (runtime_gui / "widgets").mkdir(parents=True)
            (source_gui / "widgets" / "DaqTab.py").write_text(
                "SOURCE = 2\n", encoding="utf-8"
            )
            (runtime_gui / "widgets" / "DaqTab.py").write_text(
                "SOURCE = 1\n", encoding="utf-8"
            )
            with self.assertRaises(RuntimeValidationError):
                verify_deployed_gui(runtime_gui, root)

    def test_file_identity_has_full_hash_and_absolute_path(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "config.conf"
            path.write_text("value=1\n", encoding="utf-8")
            identity = file_identity(path)
            self.assertEqual(len(identity["sha256"]), 64)
            self.assertEqual(identity["path"], str(path.resolve()))


if __name__ == "__main__":
    unittest.main()
