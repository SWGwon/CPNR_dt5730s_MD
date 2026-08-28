"""Pure helpers for reproducible GUI process launches.

This module deliberately has no Qt dependency so launch-path and freshness
checks can be regression-tested on machines without the DAQ GUI stack.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import stat as stat_module
import sys
import tempfile
from datetime import datetime
from pathlib import Path
from typing import Iterable, Mapping, Sequence


RAW_EVENT_HEADER_BYTES = 24


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


def raw_event_size_bytes(record_length: int, channel_mask: int) -> int:
    """Return the exact on-disk bytes occupied by one frontend raw event."""

    if isinstance(record_length, bool) or not isinstance(record_length, int):
        raise RuntimeValidationError("RecordLength는 정수여야 합니다.")
    if record_length < 128 or record_length > 102400 or record_length % 8:
        raise RuntimeValidationError(
            "RecordLength는 128..102400 범위의 8의 배수여야 합니다."
        )
    if isinstance(channel_mask, bool) or not isinstance(channel_mask, int):
        raise RuntimeValidationError("ChannelMask는 정수여야 합니다.")
    if channel_mask < 1 or channel_mask > 0xFF:
        raise RuntimeValidationError("ChannelMask는 1..255여야 합니다.")
    active_channels = bin(channel_mask).count("1")
    return RAW_EVENT_HEADER_BYTES + 2 * record_length * active_channels


def expected_raw_size_bytes(
    record_length: int,
    channel_mask: int,
    max_events: int,
    *,
    segments: int = 1,
) -> int | None:
    """Estimate an event-limited run exactly; return ``None`` if unbounded."""

    event_bytes = raw_event_size_bytes(record_length, channel_mask)
    if isinstance(max_events, bool) or not isinstance(max_events, int):
        raise RuntimeValidationError("Max Events는 정수여야 합니다.")
    if max_events < 0:
        raise RuntimeValidationError("Max Events는 음수일 수 없습니다.")
    if isinstance(segments, bool) or not isinstance(segments, int):
        raise RuntimeValidationError("segment 수는 정수여야 합니다.")
    if segments < 1:
        raise RuntimeValidationError("segment 수는 1 이상이어야 합니다.")
    if max_events == 0:
        return None
    return event_bytes * max_events * segments


def _nearest_existing_directory(path: Path) -> Path:
    candidate = path
    while True:
        try:
            candidate_stat = candidate.stat()
        except FileNotFoundError as exc:
            if os.path.lexists(candidate):
                raise RuntimeValidationError(
                    f"출력 경로에 끊어진 symlink가 있습니다: {candidate}"
                ) from exc
        except OSError as exc:
            raise RuntimeValidationError(
                f"출력 경로를 확인할 수 없습니다: {candidate} ({exc})"
            ) from exc
        else:
            if not stat_module.S_ISDIR(candidate_stat.st_mode):
                raise RuntimeValidationError(
                    f"출력 파일의 상위 경로가 디렉터리가 아닙니다: {candidate}"
                )
            return candidate
        parent = candidate.parent
        if parent == candidate:
            raise RuntimeValidationError(
                f"출력 경로의 파일시스템을 확인할 수 없습니다: {path}"
            )
        candidate = parent


def _mount_point(path: Path) -> Path:
    """Best-effort mount point for an existing directory."""

    candidate = Path(os.path.realpath(path))
    try:
        device = candidate.stat().st_dev
    except OSError as exc:
        raise RuntimeValidationError(
            f"출력 파일시스템을 확인할 수 없습니다: {candidate} ({exc})"
        ) from exc
    while candidate.parent != candidate and not os.path.ismount(candidate):
        parent = candidate.parent
        try:
            if parent.stat().st_dev != device:
                break
        except OSError:
            break
        candidate = parent
    return candidate


def inspect_output_filesystem(
    project_root: os.PathLike[str] | str,
    output_value: os.PathLike[str] | str,
) -> dict[str, object]:
    """Inspect free space on the filesystem that will receive an output.

    The final component is intentionally not resolved: a symlink there must
    remain visible to the separate no-overwrite checks.  If the requested
    parent does not exist yet, its nearest existing ancestor is inspected.
    """

    rendered = os.path.expanduser(os.fspath(output_value)).strip()
    if not rendered:
        raise RuntimeValidationError("출력 파일 경로가 비어 있습니다.")
    output = Path(rendered)
    if not output.is_absolute():
        output = Path(project_root) / output
    output = Path(os.path.abspath(output))
    parent = output.parent
    inspected = _nearest_existing_directory(parent)
    if not os.access(inspected, os.W_OK | os.X_OK):
        raise RuntimeValidationError(
            f"출력 경로를 생성하거나 쓸 권한이 없습니다: {inspected}"
        )
    try:
        usage = shutil.disk_usage(inspected)
        stat = inspected.stat()
    except OSError as exc:
        raise RuntimeValidationError(
            f"출력 파일시스템의 여유 공간을 확인할 수 없습니다: "
            f"{inspected} ({exc})"
        ) from exc
    mount = _mount_point(inspected)
    if hasattr(os, "major") and hasattr(os, "minor"):
        device = f"{os.major(stat.st_dev)}:{os.minor(stat.st_dev)}"
    else:  # pragma: no cover - non-POSIX fallback
        device = str(stat.st_dev)
    return {
        "output_path": str(output),
        "output_parent": str(parent),
        "inspected_path": str(inspected),
        "mount_point": str(mount),
        "device": device,
        "total_bytes": int(usage.total),
        "free_bytes": int(usage.free),
    }


def validate_output_capacity(
    free_bytes: int,
    expected_raw_bytes: int | None,
    minimum_free_bytes: int,
) -> int:
    """Apply the frontend's capacity rule and return required start bytes."""

    values = (free_bytes, minimum_free_bytes)
    if any(isinstance(value, bool) or not isinstance(value, int)
           for value in values):
        raise RuntimeValidationError("저장공간 값은 정수 byte 단위여야 합니다.")
    if free_bytes < 0 or minimum_free_bytes < 0:
        raise RuntimeValidationError("저장공간 값은 음수일 수 없습니다.")
    if expected_raw_bytes is not None:
        if (isinstance(expected_raw_bytes, bool)
                or not isinstance(expected_raw_bytes, int)
                or expected_raw_bytes < 0):
            raise RuntimeValidationError("예상 RAW 크기가 올바르지 않습니다.")
    required = minimum_free_bytes + (expected_raw_bytes or 0)
    if free_bytes < required:
        raise RuntimeValidationError(
            "출력 파일시스템의 여유 공간이 부족합니다: "
            f"available={free_bytes} bytes, "
            f"expected_raw={expected_raw_bytes or 0} bytes, "
            f"required_reserve={minimum_free_bytes} bytes"
        )
    return required


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


def _sha256_descriptor(descriptor: int) -> str:
    """Hash an already-open file without changing its shared file offset."""

    digest = hashlib.sha256()
    offset = 0
    while True:
        block = os.pread(descriptor, 1024 * 1024, offset)
        if not block:
            return digest.hexdigest()
        digest.update(block)
        offset += len(block)


def pin_verified_executable(
    executable: os.PathLike[str] | str,
    source_paths: Iterable[os.PathLike[str] | str],
) -> tuple[dict[str, object], int, str]:
    """Open, authenticate, and pin the exact Linux executable inode.

    The returned tuple is ``(identity, descriptor, launch_path)``.  The caller
    owns ``descriptor`` and must keep it open until the child has finished (or
    failed to start), then close it.  Retaining it for the whole child lifetime
    also supports executable scripts whose interpreter opens ``argv[0]`` after
    QProcess has emitted ``started``.  ``launch_path`` deliberately names the
    GUI process' descriptor through procfs so a pathname replacement after
    this check cannot select a different executable.
    """

    if not sys.platform.startswith("linux"):
        raise RuntimeValidationError(
            "검증된 실행 파일 inode 고정은 Linux procfs가 필요합니다."
        )

    binary = require_file(executable, executable=True, description="실행 파일")
    flags = os.O_RDONLY | getattr(os, "O_NONBLOCK", 0)
    flags |= getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    descriptor = -1
    try:
        descriptor = os.open(binary, flags)
        before = os.fstat(descriptor)
        if not stat_module.S_ISREG(before.st_mode):
            raise RuntimeValidationError(
                f"실행 파일이 regular file이 아닙니다: {binary}"
            )
        if before.st_mode & 0o111 == 0:
            raise RuntimeValidationError(
                f"실행 파일에 실행 권한이 없습니다: {binary}"
            )

        newer_sources: list[Path] = []
        for source_value in source_paths:
            source = Path(source_value).resolve()
            if source.is_file() and source.stat().st_mtime_ns > before.st_mtime_ns:
                newer_sources.append(source)
        if newer_sources:
            newest = max(newer_sources, key=lambda item: item.stat().st_mtime_ns)
            raise RuntimeValidationError(
                "실행 파일보다 새 소스가 있어 stale binary 실행을 차단했습니다: "
                f"binary={binary}, newer_source={newest}"
            )

        executable_hash = _sha256_descriptor(descriptor)
        after = os.fstat(descriptor)
        stable_fields = (
            "st_dev", "st_ino", "st_mode", "st_size", "st_mtime_ns",
            "st_ctime_ns",
        )
        if any(getattr(before, field) != getattr(after, field)
               for field in stable_fields):
            raise RuntimeValidationError(
                f"검증 중 실행 파일 inode 또는 내용이 변경되었습니다: {binary}"
            )

        try:
            path_status = os.stat(binary, follow_symlinks=False)
        except OSError as error:
            raise RuntimeValidationError(
                f"검증 후 실행 파일 경로를 다시 확인할 수 없습니다: "
                f"{binary} ({error})"
            ) from error
        if (path_status.st_dev, path_status.st_ino) != (
            after.st_dev,
            after.st_ino,
        ):
            raise RuntimeValidationError(
                "검증 중 실행 파일 경로가 다른 inode로 변경되어 실행을 "
                f"차단했습니다: {binary}"
            )

        launch_path = f"/proc/{os.getpid()}/fd/{descriptor}"
        try:
            pinned_status = os.stat(launch_path)
        except OSError as error:
            raise RuntimeValidationError(
                "검증된 실행 파일을 고정할 Linux procfs 경로를 사용할 수 "
                f"없습니다: {launch_path} ({error})"
            ) from error
        if (pinned_status.st_dev, pinned_status.st_ino) != (
            after.st_dev,
            after.st_ino,
        ):
            raise RuntimeValidationError(
                f"procfs 실행 파일 inode 검증에 실패했습니다: {launch_path}"
            )

        identity = {
            "path": str(binary),
            "sha256": executable_hash,
            "size": after.st_size,
            "mtime_ns": after.st_mtime_ns,
            "mtime": datetime.fromtimestamp(after.st_mtime).astimezone().isoformat(
                timespec="seconds"
            ),
            "device": int(after.st_dev),
            "inode": int(after.st_ino),
        }
        owned_descriptor = descriptor
        descriptor = -1
        return identity, owned_descriptor, launch_path
    except OSError as error:
        raise RuntimeValidationError(
            f"실행 파일 inode를 열거나 검증할 수 없습니다: {binary} ({error})"
        ) from error
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def atomic_write_bytes_no_clobber(
    destination: os.PathLike[str] | str,
    payload: bytes,
) -> Path:
    """Publish a complete byte payload atomically without replacing anything.

    The payload is written and synced through an exclusive temporary file in
    the destination directory.  ``link`` is the atomic create-if-absent
    publication step: every existing directory entry, including a broken
    symlink or one created concurrently, makes it fail with
    ``FileExistsError`` instead of being replaced.
    """

    if not isinstance(payload, bytes):
        raise TypeError("payload must be bytes")
    rendered = os.path.expanduser(os.fspath(destination))
    if not rendered:
        raise ValueError("destination path is empty")
    output = Path(os.path.abspath(rendered))
    if not output.name:
        raise ValueError("destination must name a file")

    descriptor, temporary_name = tempfile.mkstemp(
        prefix=".cpnr-export-",
        suffix=".tmp",
        dir=output.parent,
    )
    temporary = Path(temporary_name)
    created = os.fstat(descriptor)
    created_identity = (created.st_dev, created.st_ino)
    try:
        view = memoryview(payload)
        while view:
            written = os.write(descriptor, view)
            if written <= 0:
                raise OSError("report export write returned zero bytes")
            view = view[written:]
        os.fsync(descriptor)
        os.close(descriptor)
        descriptor = -1

        # Unlike replace/rename, link fails atomically if output has appeared.
        os.link(temporary, output, follow_symlinks=False)
        return output
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        _remove_if_same_file(temporary, created_identity)


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
        root / "src" / "WaveformDsp.cpp",
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
        root / "src" / "RawRootFidelity.cpp",
        root / "src" / "WaveformDsp.cpp",
        root / "src" / "DAQConfig.cpp",
        root / "src" / "Sha256.cpp",
        root / ".git" / "HEAD",
        root / ".git" / "index",
        root / ".git" / "refs" / "heads" / "main",
        root / ".git" / "packed-refs",
    ]
    sources.extend(sorted((root / "include").glob("*.h")))
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


def raw_partial_path(raw_output: os.PathLike[str] | str) -> Path:
    """Return the failure-safe working name used by the DAQ frontend."""

    raw = Path(os.path.abspath(os.path.expanduser(os.fspath(raw_output))))
    return Path(f"{raw}.partial")


def _remove_if_same_file(path: Path, expected_identity: tuple[int, int]) -> bool:
    """Delete only the expected inode without an lstat-to-unlink name race.

    POSIX has no conditional unlink-by-inode.  Move the public name atomically
    into a private same-directory quarantine, inspect that moved entry, and
    unlink it only when it is still the file this process created.  A
    replacement is restored with a no-clobber hard link where possible; if its
    public name was reused concurrently, both entries are preserved.
    """

    parent = path.parent if path.parent != Path("") else Path(".")
    quarantine = Path(tempfile.mkdtemp(prefix=".cpnr-cleanup-", dir=parent))
    candidate = quarantine / "candidate"
    quarantine_empty = True
    try:
        try:
            os.rename(path, candidate)
            quarantine_empty = False
        except FileNotFoundError:
            return True

        moved = os.lstat(candidate)
        if (moved.st_dev, moved.st_ino) == expected_identity:
            os.unlink(candidate)
            quarantine_empty = True
            return True

        try:
            os.link(candidate, path, follow_symlinks=False)
            os.unlink(candidate)
            quarantine_empty = True
        except OSError:
            # The public name may have been reused, or the moved entry may not
            # support hard links.  Leave it in the private quarantine rather
            # than risking deletion or overwrite of unrelated data.
            pass
        return False
    except OSError:
        return False
    finally:
        if quarantine_empty:
            try:
                os.rmdir(quarantine)
            except OSError:
                pass


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


def frontend_expected_absent_paths(
    raw_output: os.PathLike[str] | str,
) -> tuple[Path, ...]:
    """Artifacts that must still be absent immediately before frontend start."""

    raw = Path(os.path.abspath(os.path.expanduser(os.fspath(raw_output))))
    _config, metadata = sidecar_paths(raw)
    return (
        raw,
        raw_partial_path(raw),
        metadata,
        *metadata_status_paths(metadata),
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
    partial = raw_partial_path(raw)
    paths = (raw, partial, config, metadata, *status_paths)
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
            path for path in (raw, partial, metadata, *status_paths)
            if os.path.lexists(path)
        ]
        if late_collisions:
            rendered = ", ".join(str(path) for path in late_collisions)
            raise RuntimeValidationError(
                "run output collision이 snapshot 생성 중 발생해 실행을 "
                f"차단했습니다: {rendered}"
            )
    except Exception as exc:
        # Remove only the inode this function created.  The quarantine helper
        # avoids the lstat-then-unlink race that could otherwise delete a
        # concurrently installed replacement.
        if created_identity is not None:
            _remove_if_same_file(config, created_identity)
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
    raw_fidelity: bool = False,
) -> list[str]:
    """Build the argument vector for the read-only validation worker."""

    if max_events < 0:
        raise RuntimeValidationError("검증 event 수는 0 이상이어야 합니다.")
    if not isinstance(raw_fidelity, bool):
        raise RuntimeValidationError("RAW 충실도 옵션은 boolean이어야 합니다.")
    if raw_fidelity and max_events != 0:
        raise RuntimeValidationError(
            "RAW↔ROOT 충실도 검증은 전체 event scan에서만 사용할 수 있습니다."
        )
    arguments = ["-i", str(Path(root_input).expanduser().resolve())]
    if max_events > 0:
        arguments.extend(["--max-events", str(max_events)])
    if raw_fidelity:
        arguments.append("--raw-fidelity")
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
