#!/usr/bin/env python3
"""Hardware-free frontend CLI and signal-shutdown regression tests."""

from __future__ import annotations

import json
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time


def fail(message: str) -> None:
    raise AssertionError(message)


def check_invalid(executable: str, arguments: list[str], expected: str) -> None:
    completed = subprocess.run(
        [executable, *arguments],
        check=False,
        capture_output=True,
        text=True,
        timeout=5,
    )
    output = completed.stdout + completed.stderr
    if completed.returncode != 2:
        fail(
            f"{arguments!r}: expected exit 2, got {completed.returncode}\n{output}"
        )
    expected_line = f"Command-line error: {expected}"
    if expected_line not in output.splitlines():
        fail(f"{arguments!r}: missing exact line {expected_line!r}\n{output}")
    if "Usage:" not in output:
        fail(f"{arguments!r}: usage was not printed\n{output}")


def wait_for_path(path: Path, process: subprocess.Popen[str], timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        if process.poll() is not None:
            stdout, _ = process.communicate(timeout=1)
            fail(
                f"frontend exited before creating {path} "
                f"(exit={process.returncode})\n{stdout}"
            )
        time.sleep(0.01)
    fail(f"frontend did not create {path} within {timeout} seconds")


def check_signal_bridge(executable: str) -> None:
    with tempfile.TemporaryDirectory(prefix="cpnr_frontend_signal_") as temp:
        directory = Path(temp)
        config = directory / "run.conf"
        raw = directory / "run.dat"
        metadata = directory / "run.dat.run.json"
        config.write_text(
            """[Connection]
Type=USB
Link=0
Node=0
BaseAddress=0
ExpectedModel=MOCK-DT5730S
[Digitizer]
RecordLength=512
ChannelMask=1
SelfTriggerMask=1
PostTrigger=60
InputRangeMv=2000
ADCBits=14
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
""",
            encoding="utf-8",
        )

        process = subprocess.Popen(
            [
                executable,
                "-c",
                str(config),
                "-o",
                str(raw),
                "-r",
                "1",
                "-m",
                str(metadata),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        running_status = Path(f"{metadata}.status.running.json")
        try:
            wait_for_path(running_status, process, 5)
            process.send_signal(signal.SIGTERM)
            stdout, _ = process.communicate(timeout=10)
        except BaseException:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=5)
            raise

        if process.returncode != 0:
            fail(f"SIGTERM shutdown returned {process.returncode}\n{stdout}")
        if "[Interrupt] DAQ stopped" not in stdout:
            fail(f"signal bridge did not report graceful shutdown\n{stdout}")
        if not metadata.is_file():
            fail(f"signal shutdown did not publish terminal metadata\n{stdout}")
        document = json.loads(metadata.read_text(encoding="utf-8"))
        if document.get("acquisition_status") != "completed":
            fail(f"unexpected terminal metadata after SIGTERM: {document}")


def check_setup_cancellation(executable: str) -> None:
    with tempfile.TemporaryDirectory(prefix="cpnr_frontend_setup_cancel_") as temp:
        directory = Path(temp)
        config = directory / "run.conf"
        raw = directory / "run.dat"
        metadata = directory / "run.dat.run.json"
        config.write_text(
            """[Connection]
Type=USB
Link=0
Node=0
BaseAddress=0
ExpectedModel=MOCK-DT5730S
[Digitizer]
RecordLength=512
ChannelMask=1
SelfTriggerMask=1
PostTrigger=60
InputRangeMv=2000
ADCBits=14
TriggerPolarity=1
ExtTriggerMode=0
SelfTriggerMode=1
[HardwareCoincidence]
PairLogic=OR
[TriggerCalibration]
SettlingTimeMs=5000
SettlingTimeoutMs=6000
MeasurementEvents=1
StabilityToleranceAdc=2.0
StableMeasurements=2
[Synchronization]
ClockSource=0
RunSyncMode=0
[Channel_0]
DCOffset=3276
TriggerThresholdMv=1.0
""",
            encoding="utf-8",
        )
        process = subprocess.Popen(
            [
                executable,
                "-c",
                str(config),
                "-o",
                str(raw),
                "-r",
                "2",
                "-m",
                str(metadata),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            time.sleep(0.2)
            if process.poll() is not None:
                stdout, _ = process.communicate(timeout=1)
                fail(f"frontend exited before setup cancellation\n{stdout}")
            process.send_signal(signal.SIGTERM)
            stdout, _ = process.communicate(timeout=10)
        except BaseException:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=5)
            raise

        if process.returncode != 0:
            fail(f"setup cancellation returned {process.returncode}\n{stdout}")
        if "cancelled by user; terminal metadata was recorded" not in stdout:
            fail(f"setup cancellation was not reported as an interrupt\n{stdout}")
        document = json.loads(metadata.read_text(encoding="utf-8"))
        if document.get("acquisition_status") != "cancelled" or document.get(
            "termination_reason"
        ) != "cancelled_during_setup":
            fail(f"unexpected setup cancellation metadata: {document}")
        if document.get("failure_reason") is not None:
            fail(f"operator cancellation was recorded as a failure: {document}")
        if raw.exists() or Path(f"{raw}.partial").exists():
            fail("setup cancellation reserved a raw output")


def main() -> int:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} FRONTEND_EXECUTABLE", file=sys.stderr)
        return 2
    executable = str(Path(sys.argv[1]).resolve())

    cases = [
        ([], "Run number (-r) must be provided as a positive integer"),
        (["-r", "0"], "Option -r must be in the range 1..2147483647"),
        (["-r", "-1"], "Option -r must be in the range 1..2147483647"),
        (
            ["-r", "abc"],
            "Option -r requires a complete base-10 integer: abc",
        ),
        (
            ["-r", "12junk"],
            "Option -r requires a complete base-10 integer: 12junk",
        ),
        (["-r", ""], "Option -r requires a non-empty base-10 integer"),
        (
            ["-r", "999999999999999999999999"],
            "Option -r integer is outside the supported range: "
            "999999999999999999999999",
        ),
        (["-r", "1", "-n", "-1"], "Option -n must be nonnegative"),
        (
            ["-r", "1", "-n", ""],
            "Option -n requires a non-empty base-10 integer",
        ),
        (
            ["-r", "1", "-n", "12events"],
            "Option -n requires a complete base-10 integer: 12events",
        ),
        (
            ["-r", "1", "-n", "999999999999999999999999"],
            "Option -n integer is outside the supported range: "
            "999999999999999999999999",
        ),
        (
            ["-r", "1", "-t", "-1"],
            "Option -t must be in the range 0..2147483647",
        ),
        (
            ["-r", "1", "-t", ""],
            "Option -t requires a non-empty base-10 integer",
        ),
        (
            ["-r", "1", "-t", "12seconds"],
            "Option -t requires a complete base-10 integer: 12seconds",
        ),
        (
            ["-r", "1", "-t", "999999999999999999999999"],
            "Option -t integer is outside the supported range: "
            "999999999999999999999999",
        ),
        (
            ["-r", "1", "-n", "4294967296"],
            "Option -n exceeds the 32-bit EventHeader EventID capacity",
        ),
        (
            ["-r", "1", "-n", "3000000000"],
            "Option -n exceeds this build's signed DAQ event limit "
            "(2147483647)",
        ),
        (
            ["-r", "1", "-n", "1", "-t", "1"],
            "Options -n and -t cannot both be positive",
        ),
        (["-r", "1", "extra"], "Unexpected positional argument: extra"),
        (["-r", "1", "-n"], "Option -n requires a value"),
        (["-r", "1", "-x"], "Unknown option: -x"),
        (["-r", "1", "-c", ""], "Option -c requires a non-empty value"),
    ]
    for arguments, expected in cases:
        check_invalid(executable, arguments, expected)

    check_signal_bridge(executable)
    check_setup_cancellation(executable)
    print("All frontend CLI and signal tests passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
