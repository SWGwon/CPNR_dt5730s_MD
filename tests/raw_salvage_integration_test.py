#!/usr/bin/env python3
"""Hardware-free integration coverage for raw_salvage_dt5730."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys
import tempfile
import time


HEADER = struct.Struct("<QIIHHI")
RECORD_LENGTH = 256
CHANNEL_MASK = 1
ADC_BITS = 14


def fail(message: str) -> None:
    raise AssertionError(message)


def config_text(identity_marker: str = "A") -> str:
    return f"""# identity marker {identity_marker}
[Digitizer]
RecordLength={RECORD_LENGTH}
ChannelMask={CHANNEL_MASK}
SelfTriggerMask=1
PostTrigger=60
InputRangeMv=2000
ADCBits={ADC_BITS}
TriggerPolarity=1
ExtTriggerMode=0
SelfTriggerMode=1
[HardwareCoincidence]
PairLogic=OR
[TriggerCalibration]
SettlingTimeMs=0
SettlingTimeoutMs=1000
MeasurementEvents=1
StabilityToleranceAdc=2.0
StableMeasurements=2
[Synchronization]
ClockSource=0
RunSyncMode=0
[Channel_0]
DCOffset=3276
TriggerThresholdMv=1.0
"""


def event(event_id: int, *, sample: int = 100, record_length: int = RECORD_LENGTH,
          channel_mask: int = CHANNEL_MASK) -> bytes:
    channels = channel_mask.bit_count()
    header = HEADER.pack(1000 + event_id, event_id, record_length, channel_mask,
                         0x1234, event_id & 0xFFFFFF)
    return header + struct.pack(f"<{record_length * channels}H",
                                *([sample] * record_length * channels))


def run(executable: str, *arguments: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [executable, *(str(argument) for argument in arguments)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=20,
        check=False,
    )


def invoke(executable: str, source: Path, output: Path,
           config: Path) -> subprocess.CompletedProcess[str]:
    return run(executable, "-i", source, "-o", output, "-c", config)


def require_success(result: subprocess.CompletedProcess[str]) -> None:
    if result.returncode != 0:
        fail(f"salvage failed with {result.returncode}:\n{result.stdout}")


def require_failure(result: subprocess.CompletedProcess[str], text: str) -> None:
    if result.returncode == 0 or text not in result.stdout:
        fail(f"expected failure containing {text!r}, got {result.returncode}:\n"
             f"{result.stdout}")


def assert_no_outputs(output: Path) -> None:
    manifest = Path(f"{output}.recovery.json")
    if output.exists() or manifest.exists():
        fail(f"unexpected recovery artifacts: {output}, {manifest}")
    leftovers = list(output.parent.glob(f"{output.name}*.salvage.tmp.*"))
    if leftovers:
        fail(f"temporary recovery files were not removed: {leftovers}")


def write_large_source(path: Path, count: int = 100_000) -> None:
    with path.open("wb") as stream:
        for event_id in range(count):
            stream.write(event(event_id))


def stop_after_temporary_file(process: subprocess.Popen[str], output: Path) -> Path:
    deadline = time.monotonic() + 10
    pattern = f"{output.name}.salvage.tmp.*"
    while time.monotonic() < deadline:
        temporary_files = list(output.parent.glob(pattern))
        if temporary_files:
            os.kill(process.pid, signal.SIGSTOP)
            return temporary_files[0]
        if process.poll() is not None:
            stdout, _ = process.communicate()
            fail(f"salvage exited before the identity test could pause it:\n{stdout}")
        time.sleep(0.001)
    fail("salvage did not create its temporary output in time")


def finish_stopped_process(process: subprocess.Popen[str]) -> tuple[int, str]:
    os.kill(process.pid, signal.SIGCONT)
    stdout, _ = process.communicate(timeout=30)
    return process.returncode, stdout


def test_cli(executable: str, directory: Path) -> None:
    help_result = run(executable, "-h")
    if help_result.returncode != 0 or "Usage:" not in help_result.stdout:
        fail(f"-h did not print successful usage:\n{help_result.stdout}")
    require_failure(run(executable), "Options -i, -o, and -c are all required")
    require_failure(run(executable, "--not-an-option"), "Unknown option")
    require_failure(run(executable, "-i", "a", "-i", "b", "-o", "c",
                        "-c", "d"), "Option -i was specified twice")
    require_failure(run(executable, "-i", "a", "-o", "b", "-c", "c",
                        "extra"), "Positional arguments are not accepted")

    config = directory / "cli.conf"
    config.write_text(config_text(), encoding="utf-8")
    source = directory / "cli.dat.partial"
    source.write_bytes(event(0))
    output = directory / "cli-recovered.dat"
    require_success(invoke(executable, source, output, config))


def test_clean_and_provenance(executable: str, directory: Path) -> None:
    config = directory / "clean.conf"
    config.write_text(config_text(), encoding="utf-8")
    source = directory / "clean.dat.partial"
    original = event(0) + event(1, sample=(1 << ADC_BITS) - 1)
    source.write_bytes(original)
    before = source.stat()
    output = directory / "clean-recovered.dat"

    result = invoke(executable, source, output, config)
    require_success(result)
    if output.read_bytes() != original or source.read_bytes() != original:
        fail("clean recovery did not preserve source bytes exactly")
    after = source.stat()
    if (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) != (
            after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns):
        fail("salvage modified the source identity")

    manifest_path = Path(f"{output}.recovery.json")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    expected_source_hash = hashlib.sha256(original).hexdigest()
    expected_config_hash = hashlib.sha256(config.read_bytes()).hexdigest()
    expected_output_hash = hashlib.sha256(output.read_bytes()).hexdigest()
    expected = {
        "schema": "cpnr.raw-recovery/v1",
        "status": "verified_copy",
        "stop_reason": "clean_end",
        "source_sha256": expected_source_hash,
        "config_sha256": expected_config_hash,
        "output_sha256": expected_output_hash,
        "recovered_events": 2,
        "event_header_bytes": HEADER.size,
        "record_length": RECORD_LENGTH,
        "channel_mask": CHANNEL_MASK,
        "adc_bits": ADC_BITS,
    }
    for key, value in expected.items():
        if manifest.get(key) != value:
            fail(f"manifest {key} was {manifest.get(key)!r}, expected {value!r}")
    if manifest["binary_sha256"] != hashlib.sha256(Path(executable).read_bytes()).hexdigest():
        fail("manifest binary hash does not authenticate the running executable")
    if manifest["source_device"] != before.st_dev or manifest["source_inode"] != before.st_ino:
        fail("manifest source identity is incorrect")
    if not manifest.get("git_commit") or not manifest.get("build_timestamp"):
        fail("manifest omits build provenance")


def test_truncated_prefix(executable: str, directory: Path) -> None:
    config = directory / "truncated.conf"
    config.write_text(config_text(), encoding="utf-8")
    source = directory / "truncated.dat.partial"
    valid = event(0)
    incomplete = event(1)[:-17]
    source.write_bytes(valid + incomplete)
    output = directory / "truncated-recovered.dat"
    require_success(invoke(executable, source, output, config))
    if output.read_bytes() != valid:
        fail("truncated recovery did not stop at the last complete event")
    manifest = json.loads(Path(f"{output}.recovery.json").read_text())
    if manifest["stop_reason"] != "truncated_payload" or manifest["recovered_events"] != 1:
        fail(f"unexpected truncated manifest: {manifest}")
    if manifest["source_sha256"] != hashlib.sha256(source.read_bytes()).hexdigest():
        fail("truncated recovery did not hash the complete source")

    source2 = directory / "truncated-header.dat.partial"
    source2.write_bytes(valid + event(1)[:11])
    output2 = directory / "truncated-header-recovered.dat"
    require_success(invoke(executable, source2, output2, config))
    manifest2 = json.loads(Path(f"{output2}.recovery.json").read_text())
    if output2.read_bytes() != valid or manifest2["stop_reason"] != "truncated_header":
        fail("truncated legacy header was not rejected")


def test_corruption_boundaries(executable: str, directory: Path) -> None:
    config = directory / "corrupt.conf"
    config.write_text(config_text(), encoding="utf-8")
    valid = event(0)
    cases = {
        "event-id": (event(2), "noncontiguous_event_id"),
        "record-length": (event(1, record_length=RECORD_LENGTH * 2), "invalid_header"),
        "channel-mask": (event(1, channel_mask=3), "invalid_header"),
        "adc": (event(1, sample=1 << ADC_BITS), "adc_out_of_range"),
    }
    for name, (tail, reason) in cases.items():
        source = directory / f"corrupt-{name}.dat.partial"
        output = directory / f"corrupt-{name}-recovered.dat"
        source.write_bytes(valid + tail)
        require_success(invoke(executable, source, output, config))
        if output.read_bytes() != valid:
            fail(f"{name} corruption leaked into the recovered prefix")
        manifest = json.loads(Path(f"{output}.recovery.json").read_text())
        if manifest["stop_reason"] != reason or manifest["recovered_events"] != 1:
            fail(f"unexpected {name} corruption manifest: {manifest}")


def test_no_valid_event(executable: str, directory: Path) -> None:
    config = directory / "no-valid.conf"
    config.write_text(config_text(), encoding="utf-8")
    for name, contents in (("empty", b""), ("bad-first-id", event(1)),
                           ("bad-first-adc", event(0, sample=1 << ADC_BITS))):
        source = directory / f"{name}.dat.partial"
        output = directory / f"{name}-recovered.dat"
        source.write_bytes(contents)
        result = invoke(executable, source, output, config)
        require_failure(result, "no complete" if not contents else "no complete, valid event")
        assert_no_outputs(output)


def test_no_overwrite(executable: str, directory: Path) -> None:
    config = directory / "overwrite.conf"
    config.write_text(config_text(), encoding="utf-8")
    source = directory / "overwrite.dat.partial"
    source.write_bytes(event(0))

    output = directory / "existing-output.dat"
    output.write_bytes(b"do not replace data")
    result = invoke(executable, source, output, config)
    require_failure(result, "Refusing to overwrite")
    if output.read_bytes() != b"do not replace data":
        fail("existing recovered data was overwritten")
    if Path(f"{output}.recovery.json").exists():
        fail("a manifest was published beside pre-existing recovered data")

    output2 = directory / "existing-manifest.dat"
    manifest2 = Path(f"{output2}.recovery.json")
    manifest2.write_bytes(b"do not replace manifest")
    result2 = invoke(executable, source, output2, config)
    require_failure(result2, "Refusing to overwrite")
    if output2.exists() or manifest2.read_bytes() != b"do not replace manifest":
        fail("existing manifest was overwritten or paired with new data")


def test_input_identity(executable: str, directory: Path) -> None:
    config = directory / "identity-source.conf"
    config.write_text(config_text(), encoding="utf-8")
    source = directory / "identity-source.dat.partial"
    write_large_source(source)
    output = directory / "identity-source-recovered.dat"
    process = subprocess.Popen(
        [executable, "-i", str(source), "-o", str(output), "-c", str(config)],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    try:
        stop_after_temporary_file(process, output)
        moved = directory / "identity-source-original.dat.partial"
        source.rename(moved)
        source.write_bytes(b"replacement path identity")
        returncode, stdout = finish_stopped_process(process)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    if returncode == 0 or "Input changed" not in stdout:
        fail(f"source path replacement was not rejected ({returncode}):\n{stdout}")
    assert_no_outputs(output)


def test_config_identity(executable: str, directory: Path) -> None:
    config = directory / "identity-config.conf"
    config.write_text(config_text("A"), encoding="utf-8")
    source = directory / "identity-config.dat.partial"
    write_large_source(source)
    output = directory / "identity-config-recovered.dat"
    process = subprocess.Popen(
        [executable, "-i", str(source), "-o", str(output), "-c", str(config)],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    try:
        stop_after_temporary_file(process, output)
        old_stat = config.stat()
        replacement = config.read_text(encoding="utf-8").replace(
            "identity marker A", "identity marker B")
        config.write_text(replacement, encoding="utf-8")
        os.utime(config, ns=(old_stat.st_atime_ns, old_stat.st_mtime_ns))
        returncode, stdout = finish_stopped_process(process)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    if returncode == 0 or "Configuration file changed" not in stdout:
        fail(f"config identity change was not rejected ({returncode}):\n{stdout}")
    assert_no_outputs(output)


def test_cleanup_preserves_replaced_temporary(
        executable: str, directory: Path) -> None:
    config = directory / "cleanup-race.conf"
    config.write_text(config_text(), encoding="utf-8")
    source = directory / "cleanup-race.dat.partial"
    write_large_source(source)
    output = directory / "cleanup-race-recovered.dat"
    process = subprocess.Popen(
        [executable, "-i", str(source), "-o", str(output), "-c", str(config)],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    try:
        temporary = stop_after_temporary_file(process, output)
        displaced_temporary = directory / "cleanup-race-original.tmp"
        temporary.rename(displaced_temporary)
        sentinel = b"unrelated temporary sentinel"
        temporary.write_bytes(sentinel)
        moved_source = directory / "cleanup-race-original.dat.partial"
        source.rename(moved_source)
        source.write_bytes(b"replacement source forces rollback")
        returncode, stdout = finish_stopped_process(process)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    if returncode == 0 or "Input changed" not in stdout:
        fail(f"cleanup race did not reach rollback ({returncode}):\n{stdout}")
    if not displaced_temporary.exists() or temporary.read_bytes() != sentinel:
        fail("rollback deleted the acquired temporary inode or its replacement")
    if output.exists() or Path(f"{output}.recovery.json").exists():
        fail("cleanup race published a final recovery artifact")
    if list(directory.glob(".cpnr-cleanup-*")):
        fail("cleanup race stranded a private quarantine directory")


def main() -> int:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} /path/to/raw_salvage_dt5730", file=sys.stderr)
        return 2
    executable = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="cpnr_raw_salvage_") as temp:
        directory = Path(temp)
        test_cli(executable, directory)
        test_clean_and_provenance(executable, directory)
        test_truncated_prefix(executable, directory)
        test_corruption_boundaries(executable, directory)
        test_no_valid_event(executable, directory)
        test_no_overwrite(executable, directory)
        test_input_identity(executable, directory)
        test_config_identity(executable, directory)
        test_cleanup_preserves_replaced_temporary(executable, directory)
    print("raw salvage integration tests passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, subprocess.SubprocessError) as error:
        print(f"raw salvage integration test failed: {error}", file=sys.stderr)
        raise SystemExit(1)
