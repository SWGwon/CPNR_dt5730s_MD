import hashlib
import json
import os
import stat as stat_module
from collections.abc import Mapping, Sequence
from pathlib import Path

from PyQt6.QtCore import (
    QProcess,
    QSettings,
    QThread,
    QTimer,
    Qt,
    pyqtSlot,
)
from PyQt6.QtGui import QColor, QFont
from PyQt6.QtWidgets import (
    QAbstractItemView,
    QCheckBox,
    QFileDialog,
    QGridLayout,
    QGroupBox,
    QHeaderView,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPlainTextEdit,
    QProgressBar,
    QPushButton,
    QSpinBox,
    QSplitter,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from core.root_validation_output import (
    display_value,
    flatten_mapping,
    normalize_status,
    parse_validation_output,
    parse_validation_progress,
    status_counts,
    strip_ansi,
    validate_report_envelope,
)
from core.runtime_paths import (
    RuntimeValidationError,
    atomic_write_bytes_no_clobber,
    build_root_validation_arguments,
    file_identity,
    find_project_root,
    identity_summary,
    pin_verified_executable,
    require_file,
    resolve_path,
    root_validator_sources,
    verify_deployed_gui,
    verify_expected_hashes,
)


_STATUS_COLOURS = {
    "PASS": ("#d1e7dd", "#0f5132"),
    "WARN": ("#fff3cd", "#664d03"),
    "FAIL": ("#f8d7da", "#842029"),
    "CANCELLED": ("#e2e3e5", "#41464b"),
    "INFO": ("#cff4fc", "#055160"),
    "SKIP": ("#e2e3e5", "#41464b"),
    "STALE": ("#f8d7da", "#842029"),
}

_VALIDATOR_EXIT_BY_STATUS = {
    "PASS": 0,
    "WARN": 1,
    "FAIL": 2,
    "CANCELLED": 3,
}
_INPUT_IDENTITY_FIELDS = (
    "device",
    "inode",
    "mode",
    "size_bytes",
    "mtime_seconds",
    "mtime_nanoseconds",
    "ctime_seconds",
    "ctime_nanoseconds",
)


def _identity_from_stat(status):
    return {
        "device": int(status.st_dev),
        "inode": int(status.st_ino),
        "mode": int(status.st_mode),
        "size_bytes": int(status.st_size),
        "mtime_seconds": status.st_mtime_ns // 1_000_000_000,
        "mtime_nanoseconds": status.st_mtime_ns % 1_000_000_000,
        "ctime_seconds": status.st_ctime_ns // 1_000_000_000,
        "ctime_nanoseconds": status.st_ctime_ns % 1_000_000_000,
    }


def _fingerprint_input_file(path, expected_identity, interrupted=lambda: False):
    """Hash one exact regular-file inode and reject every observed mutation."""

    if any(field not in expected_identity for field in _INPUT_IDENTITY_FIELDS):
        raise RuntimeValidationError("validated ROOT identity is incomplete")
    expected = {
        field: expected_identity[field] for field in _INPUT_IDENTITY_FIELDS
    }
    flags = os.O_RDONLY | getattr(os, "O_NONBLOCK", 0)
    flags |= getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    descriptor = -1
    try:
        descriptor = os.open(path, flags)
        before_status = os.fstat(descriptor)
        if not stat_module.S_ISREG(before_status.st_mode):
            raise RuntimeValidationError(
                f"validated ROOT input is no longer a regular file: {path}"
            )
        before = _identity_from_stat(before_status)
        if before != expected:
            raise RuntimeValidationError(
                "validated ROOT inode/size/timestamps changed"
            )

        digest = hashlib.sha256()
        offset = 0
        while True:
            if interrupted():
                raise InterruptedError("ROOT fingerprinting was interrupted")
            block = os.pread(descriptor, 1024 * 1024, offset)
            if not block:
                break
            digest.update(block)
            offset += len(block)

        after = _identity_from_stat(os.fstat(descriptor))
        path_identity = _identity_from_stat(
            os.stat(path, follow_symlinks=False)
        )
        if after != expected or path_identity != expected:
            raise RuntimeValidationError(
                "validated ROOT changed while its SHA-256 was recomputed"
            )
        return {**expected, "sha256": digest.hexdigest()}
    except OSError as error:
        if isinstance(error, InterruptedError):
            raise
        raise RuntimeValidationError(
            f"validated ROOT fingerprint cannot be read: {path} ({error})"
        ) from error
    finally:
        if descriptor >= 0:
            os.close(descriptor)


class _InputFingerprintThread(QThread):
    """Compute a missing post-validation hash without blocking the GUI."""

    def __init__(self, path, expected_identity, generation, parent=None):
        super().__init__(parent)
        self.path = str(path)
        self.expected_identity = dict(expected_identity)
        self.generation = generation
        self.fingerprint = None
        self.error = None
        self.cancelled = False

    def run(self):
        try:
            self.fingerprint = _fingerprint_input_file(
                self.path,
                self.expected_identity,
                self.isInterruptionRequested,
            )
        except InterruptedError:
            self.cancelled = True
        except (OSError, RuntimeValidationError, ValueError) as error:
            self.error = error


class RootValidationTab(QWidget):
    """Read-only controller and report viewer for production ROOT files."""

    # Kept as a class attribute so the real QProcess timeout path can be
    # exercised quickly in the offscreen lifecycle test without changing the
    # operator-facing grace period.
    CANCEL_GRACE_MSEC = 3000

    def __init__(
        self,
        parent=None,
        *,
        project_root=None,
        runtime_gui_dir=None,
        validator_path=None,
        validator_sources: Sequence[os.PathLike | str] | None = None,
    ):
        super().__init__(parent)

        self.proj_dir = str(
            Path(project_root).expanduser().resolve()
            if project_root is not None
            else find_project_root(__file__)
        )
        self.runtime_gui_dir = str(
            Path(runtime_gui_dir).expanduser().resolve()
            if runtime_gui_dir is not None
            else Path(__file__).resolve().parents[1]
        )
        self.validator_path = str(
            resolve_path(
                self.proj_dir,
                validator_path or os.path.join("bin", "root_validate_dt5730"),
            )
        )
        self.validator_sources = (
            [resolve_path(self.proj_dir, source) for source in validator_sources]
            if validator_sources is not None
            else root_validator_sources(self.proj_dir)
        )

        self.settings = QSettings("CPNR", "DT5730S_RootValidationTab")
        self.process = QProcess(self)
        self.process.setProcessChannelMode(
            QProcess.ProcessChannelMode.SeparateChannels
        )
        self.process.started.connect(self._handle_started)
        self.process.readyReadStandardOutput.connect(self._handle_stdout)
        self.process.readyReadStandardError.connect(self._handle_stderr)
        self.process.finished.connect(self._handle_finished)
        self.process.errorOccurred.connect(self._handle_process_error)
        self._cancel_kill_timer = QTimer(self)
        self._cancel_kill_timer.setSingleShot(True)
        self._cancel_kill_timer.timeout.connect(self._handle_cancel_timeout)
        self._cancel_kill_generation = 0
        self._result_identity_timer = QTimer(self)
        self._result_identity_timer.setInterval(1000)
        self._result_identity_timer.timeout.connect(
            self._handle_result_identity_timer
        )

        self._stdout_buffer = ""
        self._stderr_buffer = ""
        self._active_root_path = ""
        self._last_report = None
        self._run_active = False
        self._cancel_requested = False
        self._pinned_validator_fd = None
        self._pinned_validator_launch_path = None
        self._validated_input_identity = None
        self._validated_input_fingerprint = None
        self._result_generation = None
        self._result_stale = False
        self._pending_root_request = None
        self._fingerprint_threads = set()
        self._clear_launch_expectations()
        self._run_generation = 0

        self._setup_ui()
        self._load_settings()
        self.refresh_validator_identity()

    def _setup_ui(self):
        layout = QVBoxLayout(self)

        input_group = QGroupBox("Production ROOT Input (Read-only)")
        input_group.setStyleSheet(
            "QGroupBox { font-weight: bold; color: #17a2b8; }"
        )
        input_layout = QGridLayout(input_group)

        self.input_edit = QLineEdit()
        self.input_edit.setPlaceholderText("Select a production ROOT file")
        self.input_edit.setToolTip(
            "The validator opens this file read-only and never writes into it."
        )
        self.input_edit.textChanged.connect(self._handle_input_changed)
        self.btn_browse = QPushButton("Browse ROOT")
        self.btn_browse.clicked.connect(self._browse_root)
        input_layout.addWidget(QLabel("ROOT file:"), 0, 0)
        input_layout.addWidget(self.input_edit, 0, 1)
        input_layout.addWidget(self.btn_browse, 0, 2)

        self.spin_max_events = QSpinBox()
        self.spin_max_events.setRange(0, 2_000_000_000)
        self.spin_max_events.setSingleStep(10_000)
        self.spin_max_events.setSpecialValueText("All events")
        self.spin_max_events.setToolTip(
            "0 scans every event. A positive limit scans only the leading "
            "event prefix; the limit and partial coverage are recorded in "
            "the exported JSON."
        )
        self.spin_max_events.valueChanged.connect(self._handle_option_changed)
        input_layout.addWidget(QLabel("Scan limit:"), 1, 0)
        input_layout.addWidget(self.spin_max_events, 1, 1)

        self.check_raw_fidelity = QCheckBox(
            "Full RAW→ROOT conversion fidelity"
        )
        self.check_raw_fidelity.setToolTip(
            "Authenticates the recorded RAW file from RunMetadata and compares "
            "every event header, waveform sample, and derived DSP value with "
            "the corresponding archived ROOT content. If waveform branches "
            "were intentionally omitted, it proves header and scalar-DSP "
            "fidelity without claiming ROOT waveform-sample equivalence. This "
            "requires a full scan and valid v2 provenance."
        )
        self.check_raw_fidelity.toggled.connect(
            self._handle_raw_fidelity_toggled
        )
        input_layout.addWidget(self.check_raw_fidelity, 1, 2)

        self.lbl_validator_identity = QLabel()
        self.lbl_validator_identity.setWordWrap(True)
        self.lbl_validator_identity.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextSelectableByMouse
        )
        input_layout.addWidget(QLabel("Resolved validator:"), 2, 0)
        input_layout.addWidget(self.lbl_validator_identity, 2, 1, 1, 2)

        readonly_note = QLabel(
            "The selected ROOT file is never modified. Validation results are "
            "kept in memory until you explicitly export a separate JSON report."
        )
        readonly_note.setWordWrap(True)
        readonly_note.setStyleSheet("color: #495057; font-style: italic;")
        input_layout.addWidget(readonly_note, 3, 0, 1, 3)
        layout.addWidget(input_group)

        controls = QHBoxLayout()
        self.btn_validate = QPushButton("Validate ROOT")
        self.btn_validate.setStyleSheet(
            "background-color: #0d6efd; color: white; font-weight: bold; "
            "padding: 8px 18px;"
        )
        self.btn_validate.clicked.connect(self.start_validation)
        self.btn_cancel = QPushButton("Cancel")
        self.btn_cancel.setStyleSheet(
            "background-color: #dc3545; color: white; font-weight: bold; "
            "padding: 8px 18px;"
        )
        self.btn_cancel.setEnabled(False)
        self.btn_cancel.clicked.connect(self.cancel_validation)
        self.btn_export = QPushButton("Export JSON")
        self.btn_export.setStyleSheet("font-weight: bold; padding: 8px 18px;")
        self.btn_export.setEnabled(False)
        self.btn_export.clicked.connect(self.export_json)
        controls.addWidget(self.btn_validate)
        controls.addWidget(self.btn_cancel)
        controls.addWidget(self.btn_export)
        controls.addStretch()
        layout.addLayout(controls)

        self.progress_bar = QProgressBar()
        self.progress_bar.setRange(0, 100)
        self.progress_bar.setValue(0)
        self.progress_bar.setFormat("Idle")
        self.progress_bar.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(self.progress_bar)

        summary_group = QGroupBox("Validation Summary")
        summary_layout = QGridLayout(summary_group)
        self.lbl_overall = self._summary_value("NOT RUN")
        self.lbl_entries = self._summary_value("—")
        self.lbl_run = self._summary_value("—")
        self.lbl_counts = self._summary_value(
            "PASS 0 | WARN 0 | FAIL 0 | SKIP 0"
        )
        self.lbl_domains = self._summary_value(
            "Data — | Provenance — | Trigger/Quality —"
        )
        summary_layout.addWidget(QLabel("Overall:"), 0, 0)
        summary_layout.addWidget(self.lbl_overall, 0, 1)
        summary_layout.addWidget(QLabel("Entries:"), 0, 2)
        summary_layout.addWidget(self.lbl_entries, 0, 3)
        summary_layout.addWidget(QLabel("Run:"), 0, 4)
        summary_layout.addWidget(self.lbl_run, 0, 5)
        summary_layout.addWidget(QLabel("Checks:"), 0, 6)
        summary_layout.addWidget(self.lbl_counts, 0, 7)
        summary_layout.addWidget(QLabel("Domains:"), 1, 0)
        summary_layout.addWidget(self.lbl_domains, 1, 1, 1, 7)
        for column in (1, 3, 5, 7):
            summary_layout.setColumnStretch(column, 1)
        layout.addWidget(summary_group)

        self.checks_table = QTableWidget(0, 6)
        self.checks_table.setHorizontalHeaderLabels(
            ["Status", "Category", "Check", "Observed", "Expected", "Detail"]
        )
        self._configure_table(self.checks_table, stretch_column=5)

        self.channels_table = QTableWidget(0, 5)
        self.channels_table.setHorizontalHeaderLabels(
            ["Channel", "Active", "Trigger", "Metric", "Value"]
        )
        self._configure_table(self.channels_table, stretch_column=4)

        splitter = QSplitter(Qt.Orientation.Vertical)
        checks_container = self._table_container("Validation Checks", self.checks_table)
        channels_container = self._table_container(
            "Per-channel Threshold & Data Metrics", self.channels_table
        )
        splitter.addWidget(checks_container)
        splitter.addWidget(channels_container)
        splitter.setStretchFactor(0, 3)
        splitter.setStretchFactor(1, 2)
        layout.addWidget(splitter, 1)

        self.log_console = QPlainTextEdit()
        self.log_console.setReadOnly(True)
        self.log_console.setMaximumHeight(150)
        self.log_console.setMaximumBlockCount(1000)
        self.log_console.setFont(QFont("Monospace", 9))
        self.log_console.setStyleSheet(
            "background-color: #f8f9fa; color: #212529; "
            "border: 1px solid #ced4da;"
        )
        layout.addWidget(self.log_console)

    @staticmethod
    def _summary_value(text):
        label = QLabel(text)
        label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        label.setStyleSheet(
            "font-weight: bold; padding: 5px; background-color: #e9ecef; "
            "border: 1px solid #ced4da; border-radius: 4px;"
        )
        return label

    @staticmethod
    def _configure_table(table, *, stretch_column):
        table.setAlternatingRowColors(True)
        table.setSelectionBehavior(
            QAbstractItemView.SelectionBehavior.SelectRows
        )
        table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        table.setSortingEnabled(True)
        header = table.horizontalHeader()
        for column in range(table.columnCount()):
            header.setSectionResizeMode(
                column, QHeaderView.ResizeMode.ResizeToContents
            )
        header.setSectionResizeMode(
            stretch_column, QHeaderView.ResizeMode.Stretch
        )

    @staticmethod
    def _table_container(title, table):
        container = QGroupBox(title)
        container_layout = QVBoxLayout(container)
        container_layout.setContentsMargins(5, 8, 5, 5)
        container_layout.addWidget(table)
        return container

    def _load_settings(self):
        self.input_edit.setText(self.settings.value("last_root_input", ""))
        try:
            max_events = int(self.settings.value("last_max_events", 0))
        except (TypeError, ValueError):
            max_events = 0
        self.spin_max_events.setValue(
            min(self.spin_max_events.maximum(), max(0, max_events))
        )
        raw_fidelity = self.settings.value("raw_fidelity", False)
        if isinstance(raw_fidelity, str):
            raw_fidelity = raw_fidelity.strip().lower() in {
                "1", "true", "yes", "on"
            }
        self.check_raw_fidelity.setChecked(bool(raw_fidelity))

    def _save_settings(self):
        self.settings.setValue("last_root_input", self.input_edit.text().strip())
        self.settings.setValue("last_max_events", self.spin_max_events.value())
        self.settings.setValue(
            "raw_fidelity", self.check_raw_fidelity.isChecked()
        )

    def refresh_validator_identity(self):
        try:
            identity = file_identity(self.validator_path)
            self.lbl_validator_identity.setText(identity_summary(identity))
            self.lbl_validator_identity.setStyleSheet("color: #495057;")
        except OSError as error:
            self.lbl_validator_identity.setText(
                f"UNAVAILABLE: {self.validator_path} ({error})"
            )
            self.lbl_validator_identity.setStyleSheet(
                "color: #dc3545; font-weight: bold;"
            )

    def _browse_root(self):
        current = self.input_edit.text().strip()
        if current:
            initial = str(resolve_path(self.proj_dir, current).parent)
        else:
            initial = os.path.join(self.proj_dir, "data")
        selected, _ = QFileDialog.getOpenFileName(
            self,
            "Open Production ROOT File",
            initial,
            "ROOT Files (*.root);;All Files (*)",
        )
        if selected:
            self.set_root_file(selected)
            self._save_settings()

    def _handle_input_changed(self):
        if self._run_active:
            return
        self._invalidate_current_result(reset_view=True)

    def _handle_option_changed(self):
        if self._run_active:
            return
        self._invalidate_current_result(reset_view=True)

    def _handle_raw_fidelity_toggled(self, enabled):
        if self._run_active:
            return
        if enabled and self.spin_max_events.value() != 0:
            self.spin_max_events.setValue(0)
        self.spin_max_events.setEnabled(not enabled)
        self._handle_option_changed()
        self._save_settings()

    def set_root_file(self, path, auto_validate=False):
        """Select a ROOT file, optionally scheduling validation immediately."""

        value = str(path).strip() if path is not None else ""
        resolved = str(resolve_path(self.proj_dir, value)) if value else ""
        if self._run_active:
            self._pending_root_request = (resolved, bool(auto_validate))
            self._log(
                "[Validation] Queued next ROOT input until the active scan "
                f"finishes: {resolved or '<empty>'}"
            )
            return
        self._invalidate_current_result(reset_view=True)
        self.input_edit.setText(resolved)
        self._save_settings()
        if auto_validate and resolved:
            QTimer.singleShot(0, self.start_validation)

    @pyqtSlot()
    def start_validation(self):
        if self._run_active:
            return
        # A new attempt invalidates the prior result even when preflight is
        # rejected.  In particular, a previous PASS must never remain visible
        # or exportable after the operator asks to validate new state.
        self._invalidate_current_result(reset_view=True)
        self._active_root_path = ""
        self._release_pinned_validator()
        self._clear_launch_expectations()
        input_value = self.input_edit.text().strip()
        if not input_value:
            self._log("[Error] Select a production ROOT file first.")
            return

        requested_max_events = self.spin_max_events.value()
        requested_raw_fidelity = self.check_raw_fidelity.isChecked()
        try:
            verify_deployed_gui(self.runtime_gui_dir, self.proj_dir)
            root_path = require_file(
                resolve_path(self.proj_dir, input_value),
                description="production ROOT 파일",
            )
            if not os.access(root_path, os.R_OK):
                raise RuntimeValidationError(
                    f"production ROOT 파일을 읽을 권한이 없습니다: {root_path}"
                )
            input_status = os.stat(root_path, follow_symlinks=False)
            arguments = build_root_validation_arguments(
                root_path,
                max_events=requested_max_events,
                raw_fidelity=requested_raw_fidelity,
            )
            (
                validator_identity,
                pinned_validator_fd,
                pinned_validator_launch_path,
            ) = pin_verified_executable(
                self.validator_path, self.validator_sources
            )
            self._pinned_validator_fd = pinned_validator_fd
            self._pinned_validator_launch_path = pinned_validator_launch_path
        except (OSError, RuntimeValidationError) as error:
            self._release_pinned_validator()
            self._log(f"[Error] Validation launch blocked: {error}")
            self.refresh_validator_identity()
            return

        self._save_settings()
        self._stdout_buffer = ""
        self._stderr_buffer = ""
        self._active_root_path = str(root_path)
        self._last_report = None
        self._cancel_requested = False
        self._run_active = True
        self._run_generation += 1
        self._clear_tables_and_summary()
        self.progress_bar.setValue(0)
        self.progress_bar.setFormat("Starting validator…")
        self._set_running_ui(True)
        self.lbl_validator_identity.setText(identity_summary(validator_identity))
        self.lbl_validator_identity.setStyleSheet("color: #495057;")

        self._log(
            "[Validation] Read-only scan starting: "
            f"{self.validator_path} {' '.join(arguments)}"
        )
        self.process.setWorkingDirectory(self.proj_dir)
        self.process.setProgram(pinned_validator_launch_path)
        self.process.setArguments(arguments)
        self._expected_validator_identity = dict(validator_identity)
        self._expected_max_events = requested_max_events
        self._expected_raw_fidelity = requested_raw_fidelity
        self._expected_input_identity = {
            "device": int(input_status.st_dev),
            "inode": int(input_status.st_ino),
            "mode": int(input_status.st_mode),
            "size_bytes": int(input_status.st_size),
            "mtime_seconds": input_status.st_mtime_ns // 1_000_000_000,
            "mtime_nanoseconds": input_status.st_mtime_ns % 1_000_000_000,
            "ctime_seconds": input_status.st_ctime_ns // 1_000_000_000,
            "ctime_nanoseconds": input_status.st_ctime_ns % 1_000_000_000,
        }
        try:
            self.process.start()
        except Exception as error:
            self._release_pinned_validator()
            self._run_active = False
            self._clear_launch_expectations()
            self.progress_bar.setFormat("Failed to start")
            self.lbl_overall.setText("FAIL")
            self._apply_status_style(self.lbl_overall, "FAIL")
            self._set_running_ui(False)
            self._log(f"[Error] Validator start raised an exception: {error}")
            self._apply_pending_root_request()

    @pyqtSlot()
    def cancel_validation(self):
        if not self._run_active or self._cancel_requested:
            return
        self._cancel_requested = True
        self.btn_cancel.setEnabled(False)
        self.progress_bar.setFormat("Cancelling…")
        self._log("[Validation] Requesting graceful cancellation (SIGTERM).")
        generation = self._run_generation
        self._cancel_kill_generation = generation
        self.process.terminate()
        self._cancel_kill_timer.start(self.CANCEL_GRACE_MSEC)

    def stop_all(self, wait=False):
        """Stop validation, optionally waiting briefly during app shutdown."""

        if self._pending_root_request is not None:
            self._log("[Validation] Discarding queued ROOT handoff during shutdown.")
            self._pending_root_request = None
        self._result_identity_timer.stop()
        fingerprint_threads = tuple(self._fingerprint_threads)
        for thread in fingerprint_threads:
            thread.requestInterruption()
        self.cancel_validation()
        if wait and self.process.state() != QProcess.ProcessState.NotRunning:
            if not self.process.waitForFinished(1000):
                self.process.kill()
                self.process.waitForFinished(1000)
        if wait:
            for thread in fingerprint_threads:
                thread.wait(1000)
        if self.process.state() == QProcess.ProcessState.NotRunning:
            self._release_pinned_validator()

    def has_pending_work(self):
        return (
            self._run_active
            or self.process.state() != QProcess.ProcessState.NotRunning
            or any(thread.isRunning() for thread in self._fingerprint_threads)
        )

    def _kill_if_running(self, generation):
        if (
            self._run_active
            and generation == self._run_generation
            and self.process.state() != QProcess.ProcessState.NotRunning
        ):
            self._log(
                "[Validation] Graceful cancellation exceeded 3 s; killing validator."
            )
            self.process.kill()

    @pyqtSlot()
    def _handle_cancel_timeout(self):
        self._kill_if_running(self._cancel_kill_generation)

    @pyqtSlot()
    def _handle_started(self):
        self._log("[Validation] Validator process started.")

    @pyqtSlot()
    def _handle_stdout(self):
        payload = bytes(self.process.readAllStandardOutput()).decode(
            "utf-8", errors="replace"
        )
        self._stdout_buffer += payload

    @pyqtSlot()
    def _handle_stderr(self):
        payload = bytes(self.process.readAllStandardError()).decode(
            "utf-8", errors="replace"
        )
        self._stderr_buffer += payload
        self._consume_stderr_lines(final=False)

    def _consume_stderr_lines(self, *, final):
        # Progress writers commonly use either newline or carriage-return
        # updates. Preserve a partial final fragment until process completion.
        normalised = self._stderr_buffer.replace("\r", "\n")
        parts = normalised.split("\n")
        if final:
            complete, remainder = parts, ""
        else:
            complete, remainder = parts[:-1], parts[-1]
        self._stderr_buffer = remainder
        for raw_line in complete:
            line = strip_ansi(raw_line).strip()
            if not line:
                continue
            progress = parse_validation_progress(line)
            if progress is not None:
                percent, stage = progress
                self.progress_bar.setValue(int(round(percent)))
                self.progress_bar.setFormat(f"%p% — {stage}")
            else:
                self._log(line)

    @pyqtSlot(int, QProcess.ExitStatus)
    def _handle_finished(self, exit_code, exit_status):
        self._release_pinned_validator()
        self._cancel_kill_timer.stop()
        self._handle_stdout()
        self._handle_stderr()
        self._consume_stderr_lines(final=True)

        report = None
        parse_error = None
        try:
            input_end_status = os.stat(
                self._active_root_path, follow_symlinks=False
            )
            self._observed_input_end_identity = self._identity_from_stat(
                input_end_status
            )
        except OSError as error:
            self._observed_input_end_identity = None
            parse_error = RuntimeValidationError(
                f"cannot capture final ROOT input identity: {error}"
            )
        if self._stdout_buffer.strip():
            try:
                report = parse_validation_output(self._stdout_buffer)
                self._validate_report_envelope(report)
            except (
                OSError,
                RuntimeValidationError,
                TypeError,
                ValueError,
            ) as error:
                parse_error = error
                # Parsing alone does not authenticate a report.  Never retain
                # or render the object when any envelope/hash check failed.
                report = None

        if report is not None:
            reported_status = normalize_status(report.get("overall_status"))
            expected_exit = _VALIDATOR_EXIT_BY_STATUS.get(reported_status)
            if exit_status != QProcess.ExitStatus.NormalExit:
                parse_error = ValueError(
                    "validator crashed after emitting JSON "
                    f"(exit={exit_code}, status={exit_status})"
                )
                report = None
            elif expected_exit is None or exit_code != expected_exit:
                parse_error = ValueError(
                    "validator exit code does not match authenticated report "
                    f"status: status={reported_status}, exit={exit_code}, "
                    f"expected={expected_exit}"
                )
                report = None

        # A graceful validator returns an authenticated CANCELLED envelope with
        # partial diagnostics.  Any other result received after the operator
        # requested cancellation is not a completed result they asked for and
        # must never resurrect/export a stale PASS report.
        if (
            report is not None
            and self._cancel_requested
            and normalize_status(report.get("overall_status")) != "CANCELLED"
        ):
            parse_error = ValueError(
                "validator returned a non-CANCELLED report after cancellation"
            )
            report = None

        if report is not None:
            self._last_report = report
            self._render_report(report)
            analysis = report.get("analysis", {})
            completed = (
                bool(analysis.get("completed", False))
                if isinstance(analysis, Mapping)
                else False
            )
            cancelled = normalize_status(report.get("overall_status")) == "CANCELLED"
            if completed:
                self.progress_bar.setValue(100)
                self.progress_bar.setFormat("100% — Validation complete")
            elif cancelled:
                self.progress_bar.setFormat(
                    "Cancelled — authenticated partial report"
                )
            self._prepare_result_fingerprint(report)
            self._log(
                "[Validation] Report received: "
                f"status={normalize_status(report.get('overall_status'))}, "
                f"exit={exit_code}."
            )
        elif self._cancel_requested:
            self.progress_bar.setFormat("Cancelled")
            self.lbl_overall.setText("CANCELLED")
            self._apply_status_style(self.lbl_overall, "CANCELLED")
            self._log("[Validation] Validation cancelled; no report was exported.")
        else:
            self.progress_bar.setFormat("Validation failed")
            self.lbl_overall.setText("FAIL")
            self._apply_status_style(self.lbl_overall, "FAIL")
            detail = f": {parse_error}" if parse_error is not None else ""
            status_name = (
                "crashed" if exit_status == QProcess.ExitStatus.CrashExit
                else "exited"
            )
            self._log(
                f"[Error] Validator {status_name} with code {exit_code} "
                f"without a usable JSON report{detail}."
            )

        self._run_active = False
        self._clear_launch_expectations()
        self._set_running_ui(False)
        self._apply_pending_root_request()

    def _handle_process_error(self, process_error):
        self._release_pinned_validator()
        if not self._run_active:
            return
        self._log(
            "[Error] Validator process error: "
            f"{self.process.errorString()} ({process_error})"
        )
        if process_error == QProcess.ProcessError.FailedToStart:
            self._cancel_kill_timer.stop()
            self._run_active = False
            self._clear_launch_expectations()
            self.progress_bar.setFormat("Failed to start")
            self.lbl_overall.setText("FAIL")
            self._apply_status_style(self.lbl_overall, "FAIL")
            self._set_running_ui(False)
            self._apply_pending_root_request()

    def _validate_report_envelope(self, report):
        identity = self._expected_validator_identity
        if not isinstance(identity, Mapping):
            raise ValueError("validator launch identity is unavailable")
        input_identity = self._expected_input_identity
        if not isinstance(input_identity, Mapping):
            raise ValueError("input launch identity is unavailable")
        max_events = self._expected_max_events
        if isinstance(max_events, bool) or not isinstance(max_events, int):
            raise ValueError("validation launch max_events is unavailable")
        validate_report_envelope(
            report,
            input_path=self._active_root_path,
            max_events=max_events,
            input_identity_start=input_identity,
            input_identity_end=self._observed_input_end_identity,
            validator_path=str(identity["path"]),
            validator_sha256=str(identity["sha256"]),
            raw_fidelity_requested=self._expected_raw_fidelity,
        )
        verify_expected_hashes(
            {str(identity["path"]): str(identity["sha256"])}
        )

        input_info = report.get("input")
        if not isinstance(input_info, Mapping):
            raise ValueError("validator report input is unavailable")
        for field in ("sha256", "sha256_end"):
            value = input_info.get(field)
            if value is not None and not self._is_sha256(value):
                raise ValueError(
                    f"validator report input.{field} is not a SHA-256 digest"
                )

    @staticmethod
    def _is_sha256(value):
        return (
            isinstance(value, str)
            and len(value) == 64
            and all(character in "0123456789abcdefABCDEF" for character in value)
        )

    def _report_input_sha256(self, report):
        input_info = report.get("input")
        if not isinstance(input_info, Mapping):
            return None
        final_sha256 = input_info.get("sha256_end")
        if self._is_sha256(final_sha256):
            return final_sha256.lower()
        initial_sha256 = input_info.get("sha256")
        if (
            self._is_sha256(initial_sha256)
            and input_info.get("identity_start") == input_info.get("identity_end")
        ):
            return initial_sha256.lower()
        return None

    def _prepare_result_fingerprint(self, report):
        expected_identity = self._observed_input_end_identity
        if not isinstance(expected_identity, Mapping):
            self._mark_result_stale("validated ROOT end identity is unavailable")
            return
        expected_identity = dict(expected_identity)
        report_sha256 = self._report_input_sha256(report)
        self._result_generation = self._run_generation
        self._result_stale = False
        self._validated_input_identity = expected_identity
        self._result_identity_timer.start()
        if report_sha256 is not None:
            self._validated_input_fingerprint = {
                **expected_identity,
                "sha256": report_sha256,
            }
            self.btn_export.setEnabled(self._report_is_exportable(report))
            return

        self._validated_input_fingerprint = None
        self.btn_export.setEnabled(False)
        self.progress_bar.setFormat(
            "Validation complete — fingerprinting input for safe export…"
        )
        thread = _InputFingerprintThread(
            self._active_root_path,
            expected_identity,
            self._result_generation,
            self,
        )
        self._fingerprint_threads.add(thread)
        thread.finished.connect(
            lambda thread=thread: self._handle_fingerprint_finished(thread)
        )
        thread.start()

    def _handle_fingerprint_finished(self, thread):
        self._fingerprint_threads.discard(thread)
        is_current = (
            self._last_report is not None
            and self._result_generation == thread.generation
            and self._active_root_path == thread.path
            and not self._result_stale
        )
        if is_current and not thread.cancelled:
            if thread.error is not None or thread.fingerprint is None:
                self._mark_result_stale(
                    "post-validation SHA-256 could not be captured: "
                    f"{thread.error or 'unknown fingerprint failure'}"
                )
            else:
                self._validated_input_fingerprint = dict(thread.fingerprint)
                self.btn_export.setEnabled(
                    not self._run_active
                    and self._report_is_exportable(self._last_report)
                )
                status = normalize_status(
                    self._last_report.get("overall_status")
                )
                if status == "CANCELLED":
                    self.progress_bar.setFormat(
                        "Cancelled — authenticated report ready"
                    )
                else:
                    self.progress_bar.setValue(100)
                    self.progress_bar.setFormat(
                        "100% — Validation complete; report ready"
                    )
        thread.deleteLater()

    @staticmethod
    def _report_is_exportable(report):
        if not isinstance(report, Mapping):
            return False
        analysis = report.get("analysis")
        return (
            isinstance(analysis, Mapping)
            and analysis.get("completed") is True
            and normalize_status(report.get("overall_status")) != "CANCELLED"
        )

    @pyqtSlot()
    def _handle_result_identity_timer(self):
        expected = self._validated_input_identity
        if self._last_report is None or not isinstance(expected, Mapping):
            self._result_identity_timer.stop()
            return
        try:
            actual = _identity_from_stat(
                os.stat(self._active_root_path, follow_symlinks=False)
            )
        except OSError as error:
            self._mark_result_stale(
                f"validated ROOT path is unavailable: {error}"
            )
            return
        if actual != dict(expected):
            self._mark_result_stale(
                "validated ROOT inode/size/timestamps changed after validation"
            )

    def _cancel_fingerprint_workers(self):
        for thread in tuple(self._fingerprint_threads):
            thread.requestInterruption()

    def _invalidate_current_result(self, *, reset_view):
        self._cancel_fingerprint_workers()
        self._last_report = None
        self._validated_input_identity = None
        self._validated_input_fingerprint = None
        self._result_generation = None
        self._result_stale = False
        self._result_identity_timer.stop()
        if hasattr(self, "btn_export"):
            self.btn_export.setEnabled(False)
        if reset_view:
            self._reset_report_view()

    def _mark_result_stale(self, detail):
        already_stale = self._result_stale
        self._cancel_fingerprint_workers()
        self._result_stale = True
        self._result_identity_timer.stop()
        self._validated_input_identity = None
        self._validated_input_fingerprint = None
        self.btn_export.setEnabled(False)
        self.lbl_overall.setText("STALE — REVALIDATE")
        self._apply_status_style(self.lbl_overall, "STALE")
        self.progress_bar.setFormat("STALE — input changed; validate again")
        if not already_stale:
            self._log(f"[Error] Validation result is STALE: {detail}")

    def _input_is_fresh_for_export(self):
        expected = self._validated_input_fingerprint
        if not isinstance(expected, Mapping) or not self._is_sha256(
            expected.get("sha256")
        ):
            self._mark_result_stale(
                "validated input SHA-256 baseline is unavailable"
            )
            return False
        try:
            actual = _fingerprint_input_file(
                self._active_root_path,
                expected,
            )
        except (OSError, RuntimeValidationError, ValueError) as error:
            self._mark_result_stale(str(error))
            return False
        if actual["sha256"].lower() != str(expected["sha256"]).lower():
            self._mark_result_stale(
                "validated ROOT SHA-256 changed after validation"
            )
            return False
        return True

    def _apply_pending_root_request(self):
        pending = self._pending_root_request
        self._pending_root_request = None
        if pending is None:
            return
        path, auto_validate = pending
        self._log(
            "[Validation] Applying queued ROOT input after the prior scan: "
            f"{path or '<empty>'}"
        )
        self.set_root_file(path, auto_validate=auto_validate)

    def _clear_launch_expectations(self):
        self._expected_validator_identity = None
        self._expected_input_identity = None
        self._observed_input_end_identity = None
        self._expected_max_events = None
        self._expected_raw_fidelity = None

    def _release_pinned_validator(self):
        descriptor = self._pinned_validator_fd
        self._pinned_validator_fd = None
        self._pinned_validator_launch_path = None
        if descriptor is not None:
            try:
                os.close(descriptor)
            except OSError as error:
                self._log(
                    "[Warning] Could not close pinned validator descriptor "
                    f"{descriptor}: {error}"
                )

    @staticmethod
    def _identity_from_stat(status):
        return _identity_from_stat(status)

    def _render_report(self, report):
        self._render_summary(report)
        self._render_checks(report.get("checks", []))
        self._render_channels(report.get("channels", []))

    def _render_summary(self, report):
        overall = normalize_status(report.get("overall_status"))
        if report.get("legacy"):
            overall_text = f"{overall} (LEGACY)"
        else:
            overall_text = overall
        self.lbl_overall.setText(overall_text)
        self._apply_status_style(self.lbl_overall, overall)

        summary = report.get("summary")
        if not isinstance(summary, Mapping):
            summary = {}
        analysis = report.get("analysis")
        if not isinstance(analysis, Mapping):
            analysis = {}
        total = analysis.get("events_total", summary.get("entries"))
        scanned = analysis.get("events_scanned", total)
        prefix_limited = bool(analysis.get("sampled", False))
        if total is None and scanned is None:
            entries_text = "—"
        elif prefix_limited and total is not None:
            entries_text = (
                f"{display_value(scanned)} / {display_value(total)} prefix"
            )
        elif total is not None:
            entries_text = display_value(total)
        else:
            entries_text = display_value(scanned)
        self.lbl_entries.setText(entries_text)
        self.lbl_run.setText(display_value(summary.get("run_number")))

        counts = status_counts(report)
        self.lbl_counts.setText(
            f"PASS {counts['pass']} | WARN {counts['warn']} | "
            f"FAIL {counts['fail']} | SKIP {counts['skip']}"
        )
        domains = report.get("domain_status")
        if isinstance(domains, Mapping):
            self.lbl_domains.setText(
                "Data "
                f"{normalize_status(domains.get('data_integrity'))} | "
                "Provenance "
                f"{normalize_status(domains.get('provenance'))} | "
                "Trigger/Quality "
                f"{normalize_status(domains.get('trigger_and_quality'))}"
            )
        else:
            self.lbl_domains.setText(
                "Data — | Provenance — | Trigger/Quality —"
            )

    def _render_checks(self, checks):
        self.checks_table.setSortingEnabled(False)
        self.checks_table.setRowCount(0)
        for check in checks:
            if not isinstance(check, Mapping):
                continue
            row = self.checks_table.rowCount()
            self.checks_table.insertRow(row)
            status = normalize_status(check.get("status"))
            values = (
                status,
                check.get("category"),
                check.get("name"),
                check.get("observed"),
                check.get("expected"),
                check.get("detail"),
            )
            for column, value in enumerate(values):
                item = QTableWidgetItem(display_value(value))
                item.setToolTip(display_value(value))
                if column == 0:
                    self._apply_item_status(item, status)
                self.checks_table.setItem(row, column, item)
        self.checks_table.setSortingEnabled(True)

    def _render_channels(self, channels):
        self.channels_table.setSortingEnabled(False)
        self.channels_table.setRowCount(0)
        ordered = sorted(
            (channel for channel in channels if isinstance(channel, Mapping)),
            key=lambda value: self._channel_sort_key(value.get("channel")),
        )
        for channel in ordered:
            channel_id = channel.get("channel")
            active = channel.get("active")
            trigger = channel.get("trigger_enabled")
            metric_source = {}
            threshold = channel.get("threshold")
            metrics = channel.get("metrics")
            if isinstance(threshold, Mapping):
                metric_source["threshold"] = threshold
            if isinstance(metrics, Mapping):
                metric_source["metrics"] = metrics
            for key, value in channel.items():
                if key not in {
                    "channel", "active", "trigger_enabled", "threshold", "metrics"
                }:
                    metric_source[key] = value
            flattened = flatten_mapping(metric_source)
            if not flattened:
                flattened = [("metrics", None)]
            for metric_name, metric_value in flattened:
                row = self.channels_table.rowCount()
                self.channels_table.insertRow(row)
                values = (
                    channel_id,
                    active,
                    trigger,
                    metric_name,
                    metric_value,
                )
                for column, value in enumerate(values):
                    item = QTableWidgetItem(display_value(value))
                    item.setToolTip(display_value(value))
                    self.channels_table.setItem(row, column, item)
        self.channels_table.setSortingEnabled(True)

    @staticmethod
    def _channel_sort_key(value):
        try:
            return 0, int(value)
        except (TypeError, ValueError):
            return 1, str(value)

    @staticmethod
    def _apply_status_style(label, status):
        background, foreground = _STATUS_COLOURS.get(
            normalize_status(status), ("#e9ecef", "#212529")
        )
        label.setStyleSheet(
            "font-weight: bold; padding: 5px; border-radius: 4px; "
            f"background-color: {background}; color: {foreground};"
        )

    @staticmethod
    def _apply_item_status(item, status):
        background, foreground = _STATUS_COLOURS.get(
            normalize_status(status), ("#e9ecef", "#212529")
        )
        item.setBackground(QColor(background))
        item.setForeground(QColor(foreground))

    def _clear_tables_and_summary(self):
        self.checks_table.setRowCount(0)
        self.channels_table.setRowCount(0)
        self.lbl_overall.setText("RUNNING")
        self._apply_status_style(self.lbl_overall, "INFO")
        self.lbl_entries.setText("—")
        self.lbl_run.setText("—")
        self.lbl_counts.setText("PASS 0 | WARN 0 | FAIL 0 | SKIP 0")
        self.lbl_domains.setText(
            "Data — | Provenance — | Trigger/Quality —"
        )
        self.btn_export.setEnabled(False)

    def _reset_report_view(self):
        self.checks_table.setRowCount(0)
        self.channels_table.setRowCount(0)
        self.lbl_overall.setText("NOT RUN")
        self.lbl_overall.setStyleSheet(
            "font-weight: bold; padding: 5px; background-color: #e9ecef; "
            "border: 1px solid #ced4da; border-radius: 4px;"
        )
        self.lbl_entries.setText("—")
        self.lbl_run.setText("—")
        self.lbl_counts.setText("PASS 0 | WARN 0 | FAIL 0 | SKIP 0")
        self.lbl_domains.setText(
            "Data — | Provenance — | Trigger/Quality —"
        )
        self.progress_bar.setValue(0)
        self.progress_bar.setFormat("Idle")

    def _set_running_ui(self, running):
        self.btn_validate.setEnabled(not running)
        self.btn_browse.setEnabled(not running)
        self.input_edit.setEnabled(not running)
        self.spin_max_events.setEnabled(not running)
        self.check_raw_fidelity.setEnabled(not running)
        if not running and self.check_raw_fidelity.isChecked():
            self.spin_max_events.setEnabled(False)
        self.btn_cancel.setEnabled(running and not self._cancel_requested)
        if running:
            self.btn_export.setEnabled(False)

    @pyqtSlot()
    def export_json(self):
        if self._last_report is None or self._run_active:
            return
        if not self._report_is_exportable(self._last_report):
            return
        if self._result_stale:
            QMessageBox.warning(
                self,
                "Export Blocked — STALE Result",
                "The validated ROOT input changed after validation. Run "
                "validation again before exporting this report.",
            )
            return
        if self._validated_input_fingerprint is None:
            QMessageBox.warning(
                self,
                "Export Not Ready",
                "The post-validation input fingerprint is still being "
                "prepared. Wait for the report-ready message.",
            )
            return
        input_path = Path(self._active_root_path)
        default_path = Path(f"{input_path}.validation.json")
        last_dir = self.settings.value("last_export_dir", "")
        if last_dir:
            default_path = Path(str(last_dir)) / default_path.name
        selected, _ = QFileDialog.getSaveFileName(
            self,
            "Export ROOT Validation Report",
            str(default_path),
            "JSON Files (*.json);;All Files (*)",
        )
        if not selected:
            return
        destination = Path(os.path.abspath(os.path.expanduser(selected)))
        input_path = Path(self._active_root_path)
        same_input = destination == input_path
        if os.path.lexists(destination) and not same_input:
            try:
                same_input = os.path.samefile(destination, input_path)
            except OSError:
                same_input = False
        if same_input:
            QMessageBox.critical(
                self,
                "Export Blocked",
                "검증 대상 ROOT 파일 자체를 JSON 보고서로 덮어쓸 수 "
                f"없습니다:\n{input_path}",
            )
            return
        if os.path.lexists(destination):
            QMessageBox.warning(
                self,
                "Export Blocked",
                f"The report path already exists:\n{destination}\n\n"
                "Choose a new name; existing files are never replaced.",
            )
            return

        try:
            payload = (
                json.dumps(
                    self._last_report,
                    ensure_ascii=False,
                    indent=2,
                    sort_keys=True,
                    allow_nan=False,
                )
                + "\n"
            ).encode("utf-8")
        except (TypeError, ValueError) as error:
            QMessageBox.critical(
                self,
                "Export Error",
                f"The in-memory report is not valid strict JSON:\n{error}",
            )
            return
        if not self._input_is_fresh_for_export():
            QMessageBox.warning(
                self,
                "Export Blocked — STALE Result",
                "The ROOT input no longer matches the inode, size, "
                "timestamps, and SHA-256 captured when validation finished. "
                "Run validation again before exporting.",
            )
            return
        try:
            atomic_write_bytes_no_clobber(destination, payload)
        except FileExistsError:
            QMessageBox.warning(
                self,
                "Export Blocked",
                f"The report path appeared during export:\n{destination}\n\n"
                "Nothing was replaced. Choose a new name and try again.",
            )
            return
        except OSError as error:
            QMessageBox.critical(
                self,
                "Export Error",
                f"Could not atomically publish the report without replacing "
                f"an existing file:\n{destination}\n{error}",
            )
            return
        self.settings.setValue("last_export_dir", str(destination.parent))
        self._log(f"[Validation] JSON report exported: {destination}")

    def _log(self, message):
        self.log_console.appendPlainText(str(message))
