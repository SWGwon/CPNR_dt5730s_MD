"""Pure helpers for reproducible GUI process launches.

This module deliberately has no Qt dependency so launch-path and freshness
checks can be regression-tested on machines without the DAQ GUI stack.
"""

from __future__ import annotations

import hashlib
import os
from datetime import datetime
from pathlib import Path
from typing import Iterable, Mapping, Sequence


class RuntimeValidationError(ValueError):
    """Raised when a process launch would not be reproducible or safe."""


def find_project_root(start: os.PathLike[str] | str) -> Path:
    """Return the nearest parent containing CMakeLists.txt."""

    candidate = Path(start).expanduser().resolve()
    if candidate.is_file():
        candidate = candidate.parent
    for directory in (candidate, *candidate.parents):
        if (directory / "CMakeLists.txt").is_file():
            return directory
    raise RuntimeValidationError(
        f"CMakeLists.txt를 기준으로 프로젝트 루트를 찾지 못했습니다: {start}"
    )


def resolve_path(project_root: os.PathLike[str] | str,
                 value: os.PathLike[str] | str) -> Path:
    """Resolve a user path relative to the source project, not the GUI cwd."""

    path = Path(value).expanduser()
    if not path.is_absolute():
        path = Path(project_root) / path
    return path.resolve()


def sha256_file(path: os.PathLike[str] | str) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def file_identity(path: os.PathLike[str] | str) -> dict[str, object]:
    resolved = Path(path).resolve()
    stat = resolved.stat()
    return {
        "path": str(resolved),
        "sha256": sha256_file(resolved),
        "size": stat.st_size,
        "mtime_ns": stat.st_mtime_ns,
        "mtime": datetime.fromtimestamp(stat.st_mtime).astimezone().isoformat(
            timespec="seconds"
        ),
    }


def identity_summary(identity: Mapping[str, object]) -> str:
    return (
        f"{identity['path']} | sha256={identity['sha256']} "
        f"| mtime={identity['mtime']}"
    )


def require_file(path: os.PathLike[str] | str, *, executable: bool = False,
                 description: str = "파일") -> Path:
    resolved = Path(path).resolve()
    if not resolved.is_file():
        raise RuntimeValidationError(f"{description}을 찾을 수 없습니다: {resolved}")
    if executable and not os.access(resolved, os.X_OK):
        raise RuntimeValidationError(f"{description}에 실행 권한이 없습니다: {resolved}")
    return resolved


def verify_binary_fresh(
    executable: os.PathLike[str] | str,
    source_paths: Iterable[os.PathLike[str] | str],
) -> dict[str, object]:
    """Reject a missing/non-executable binary or one older than its sources."""

    binary = require_file(executable, executable=True, description="실행 파일")
    binary_stat = binary.stat()
    newer_sources: list[Path] = []
    for source_value in source_paths:
        source = Path(source_value).resolve()
        if source.is_file() and source.stat().st_mtime_ns > binary_stat.st_mtime_ns:
            newer_sources.append(source)
    if newer_sources:
        newest = max(newer_sources, key=lambda item: item.stat().st_mtime_ns)
        raise RuntimeValidationError(
            "실행 파일보다 새 소스가 있어 stale binary 실행을 차단했습니다: "
            f"binary={binary}, newer_source={newest}"
        )
    return file_identity(binary)


def frontend_sources(project_root: os.PathLike[str] | str) -> list[Path]:
    root = Path(project_root).resolve()
    sources = [
        root / "CMakeLists.txt",
        root / "src" / "frontend_dt5730.cpp",
        root / "src" / "DAQManager.cpp",
        root / "src" / "DAQConfig.cpp",
        root / "src" / "TriggerCalibration.cpp",
        root / "src" / "Sha256.cpp",
    ]
    sources.extend(sorted((root / "include").glob("*.h")))
    return sources


def production_sources(project_root: os.PathLike[str] | str) -> list[Path]:
    root = Path(project_root).resolve()
    sources = [
        root / "CMakeLists.txt",
        root / "src" / "production_dt5730.cpp",
        root / "src" / "DAQConfig.cpp",
        root / "src" / "Sha256.cpp",
    ]
    sources.extend(sorted((root / "include").glob("*.h")))
    return sources


def root_validator_sources(project_root: os.PathLike[str] | str) -> list[Path]:
    """Return every source that can change the deployed ROOT validator."""

    root = Path(project_root).resolve()
    sources = [
        root / "CMakeLists.txt",
        root / "src" / "root_validate_dt5730.cpp",
        root / "src" / "RootValidator.cpp",
        root / "src" / "DAQConfig.cpp",
        root / "src" / "Sha256.cpp",
        root / "include" / "RootValidator.h",
        root / "include" / "DAQConfig.h",
        root / "include" / "ConfigParser.h",
        root / "include" / "EventHeader.h",
        root / "include" / "Sha256.h",
        root / ".git" / "HEAD",
        root / ".git" / "index",
        root / ".git" / "refs" / "heads" / "main",
        root / ".git" / "packed-refs",
    ]
    return sources


def verify_deployed_gui(runtime_gui_dir: os.PathLike[str] | str,
                        project_root: os.PathLike[str] | str) -> None:
    """Reject an out-of-date bin/gui copy when the deployed GUI is in use."""

    runtime = Path(runtime_gui_dir).resolve()
    source = (Path(project_root).resolve() / "gui").resolve()
    if runtime == source:
        return

    mismatches: list[str] = []
    for source_file in sorted(source.rglob("*.py")):
        if "__pycache__" in source_file.parts:
            continue
        relative = source_file.relative_to(source)
        runtime_file = runtime / relative
        if not runtime_file.is_file():
            mismatches.append(f"missing:{relative}")
        elif sha256_file(source_file) != sha256_file(runtime_file):
            mismatches.append(f"changed:{relative}")
    if mismatches:
        preview = ", ".join(mismatches[:4])
        if len(mismatches) > 4:
            preview += f", ... (+{len(mismatches) - 4})"
        raise RuntimeValidationError(
            "배포된 bin/gui가 source gui와 다릅니다. GUI를 다시 배포한 뒤 "
            f"실행하세요: {preview}"
        )


def sidecar_paths(raw_output: os.PathLike[str] | str) -> tuple[Path, Path]:
    # Do not follow the final component: an existing/broken symlink at the
    # requested raw path is itself a collision and must not redirect sidecars.
    raw = Path(os.path.abspath(os.path.expanduser(os.fspath(raw_output))))
    return Path(f"{raw}.config.conf"), Path(f"{raw}.run.json")


def metadata_status_paths(
    metadata_output: os.PathLike[str] | str,
) -> tuple[Path, Path]:
    metadata = Path(
        os.path.abspath(os.path.expanduser(os.fspath(metadata_output)))
    )
    return (
        Path(f"{metadata}.status.hardware_verified_not_started.json"),
        Path(f"{metadata}.status.running.json"),
    )


def create_run_config_snapshot(
    raw_output: os.PathLike[str] | str,
    config_contents: str,
) -> dict[str, object]:
    """Create only the config snapshot after checking all run artifacts.

    Raw and metadata must remain absent because the frontend creates them with
    its own exclusive/no-overwrite checks. ``lexists`` also treats broken
    symlinks as collisions, while ``O_EXCL`` makes snapshot creation atomic.
    """

    raw = Path(os.path.abspath(os.path.expanduser(os.fspath(raw_output))))
    config, metadata = sidecar_paths(raw)
    status_paths = metadata_status_paths(metadata)
    paths = (raw, config, metadata, *status_paths)
    raw.parent.mkdir(parents=True, exist_ok=True)
    collisions = [path for path in paths if os.path.lexists(path)]
    if collisions:
        rendered = ", ".join(str(path) for path in collisions)
        raise RuntimeValidationError(
            "run output collision: 기존 raw/config/metadata를 덮어쓰지 "
            f"않습니다: {rendered}"
        )

    created_identity: tuple[int, int] | None = None
    try:
        descriptor = os.open(
            config,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL,
            0o666,
        )
        try:
            stat = os.fstat(descriptor)
            created_identity = (stat.st_dev, stat.st_ino)
            payload = config_contents.encode("utf-8")
            view = memoryview(payload)
            while view:
                written = os.write(descriptor, view)
                if written <= 0:
                    raise OSError("config snapshot write returned zero bytes")
                view = view[written:]
            os.fsync(descriptor)
        finally:
            os.close(descriptor)

        # A raw/metadata collision may have appeared while the snapshot was
        # being written. Preserve the colliding file and remove only our own
        # snapshot below.
        late_collisions = [
            path for path in (raw, metadata, *status_paths)
            if os.path.lexists(path)
        ]
        if late_collisions:
            rendered = ", ".join(str(path) for path in late_collisions)
            raise RuntimeValidationError(
                "run output collision이 snapshot 생성 중 발생해 실행을 "
                f"차단했습니다: {rendered}"
            )
    except Exception as exc:
        # Remove only the inode this function created. A concurrently replaced
        # path is somebody else's data and must be preserved.
        if created_identity is not None:
            try:
                current = os.lstat(config)
                if (current.st_dev, current.st_ino) == created_identity:
                    os.unlink(config)
            except FileNotFoundError:
                pass
        if isinstance(exc, FileExistsError):
            raise RuntimeValidationError(
                "run output collision이 검사 직후 발생해 실행을 차단했습니다."
            ) from exc
        raise

    return file_identity(config)


def verify_paths_absent(paths: Iterable[os.PathLike[str] | str]) -> None:
    """Fail if any path exists, including directories or broken symlinks."""

    collisions = [
        Path(os.path.abspath(os.path.expanduser(os.fspath(path))))
        for path in paths
        if os.path.lexists(path)
    ]
    if collisions:
        rendered = ", ".join(str(path) for path in collisions)
        raise RuntimeValidationError(
            "run output collision: frontend 실행 직전에 기존 raw/metadata를 "
            f"발견했습니다: {rendered}"
        )


def _absolute_strings(paths: Sequence[os.PathLike[str] | str]) -> list[str]:
    return [str(Path(path).resolve()) for path in paths]


def build_frontend_command(
    executable: os.PathLike[str] | str,
    config_snapshot: os.PathLike[str] | str,
    raw_output: os.PathLike[str] | str,
    run_number: int,
    metadata_output: os.PathLike[str] | str,
    *,
    max_events: int = 0,
    run_time_sec: int = 0,
) -> list[str]:
    if run_number <= 0:
        raise RuntimeValidationError("run number는 양수여야 합니다.")
    if max_events > 0 and run_time_sec > 0:
        raise RuntimeValidationError("event/time stop condition은 동시에 지정할 수 없습니다.")
    exe, config, raw, metadata = _absolute_strings(
        [executable, config_snapshot, raw_output, metadata_output]
    )
    command = [
        exe, "-c", config, "-o", raw, "-r", str(run_number), "-m", metadata,
    ]
    if max_events > 0:
        command.extend(["-n", str(max_events)])
    elif run_time_sec > 0:
        command.extend(["-t", str(run_time_sec)])
    return command


def build_production_arguments(
    raw_input: os.PathLike[str] | str,
    config_snapshot: os.PathLike[str] | str,
    run_number: int,
    metadata_input: os.PathLike[str] | str,
    *,
    root_output: os.PathLike[str] | str | None = None,
    save_waveforms: bool = False,
    debug_event_id: int | None = None,
) -> list[str]:
    if run_number <= 0:
        raise RuntimeValidationError("run number는 양수여야 합니다.")
    raw, config, metadata = _absolute_strings(
        [raw_input, config_snapshot, metadata_input]
    )
    arguments = [
        "-i", raw, "-c", config, "-r", str(run_number), "-m", metadata,
    ]
    if root_output:
        arguments.extend(["-o", str(Path(root_output).resolve())])
    if save_waveforms:
        arguments.append("-w")
    if debug_event_id is not None:
        arguments.extend(["-d", str(debug_event_id)])
    return arguments


def default_production_output(
    raw_input: os.PathLike[str] | str,
) -> Path:
    """Mirror production_dt5730's default ``*_prod.root`` naming exactly."""

    raw = str(Path(raw_input).expanduser().resolve())
    last_dot = raw.rfind(".")
    last_slash = max(raw.rfind("/"), raw.rfind("\\"))
    if last_dot < 0 or last_dot < last_slash:
        return Path(raw + "_prod.root")
    return Path(raw[:last_dot] + "_prod.root")


def build_root_validation_arguments(
    root_input: os.PathLike[str] | str,
    *,
    max_events: int = 0,
) -> list[str]:
    """Build the argument vector for the read-only validation worker."""

    if max_events < 0:
        raise RuntimeValidationError("검증 event 수는 0 이상이어야 합니다.")
    arguments = ["-i", str(Path(root_input).expanduser().resolve())]
    if max_events > 0:
        arguments.extend(["--max-events", str(max_events)])
    return arguments


def verify_expected_hashes(expected: Mapping[os.PathLike[str] | str, str]) -> None:
    """Close the validation-to-Popen race by rechecking launch artifacts."""

    for path_value, expected_hash in expected.items():
        path = require_file(path_value, description="검증 대상 파일")
        actual_hash = sha256_file(path)
        if actual_hash != expected_hash:
            raise RuntimeValidationError(
                "검증 후 파일 내용이 변경되어 실행을 차단했습니다: "
                f"{path} (expected={expected_hash}, actual={actual_hash})"
            )
