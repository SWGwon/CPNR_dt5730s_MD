import re
import os
import html
from PyQt6.QtWidgets import (QWidget, QVBoxLayout, QHBoxLayout, QGroupBox, 
                             QPushButton, QProgressBar, QLabel, QLineEdit, 
                             QTextEdit, QSpinBox, QFileDialog, QGridLayout, QCheckBox)
from PyQt6.QtCore import Qt, pyqtSignal, pyqtSlot, QSettings, QProcess
from core.DatabaseManager import DatabaseManager
from core.runtime_paths import (
    RuntimeValidationError,
    build_production_arguments,
    default_production_output,
    file_identity,
    find_project_root,
    identity_summary,
    production_sources,
    require_file,
    resolve_path,
    verify_binary_fresh,
    verify_deployed_gui,
    verify_expected_hashes,
)

class ProductionTab(QWidget):
    rootOutputReady = pyqtSignal(str)

    def __init__(self):
        super().__init__()
        
        self.proj_dir = str(find_project_root(__file__))
        self.runtime_gui_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        
        self.bin_dir = os.path.join(self.proj_dir, "bin")
        self.data_dir = os.path.join(self.proj_dir, "data")
        os.makedirs(self.data_dir, exist_ok=True)
        
        self.settings = QSettings("CPNR", "DT5730S_ProductionTab")
        self.db = DatabaseManager(os.path.join(self.data_dir, "run_history.db"))
        
        self.process = QProcess(self)
        self.process.setProcessChannelMode(
            QProcess.ProcessChannelMode.SeparateChannels
        )
        self.process.readyReadStandardOutput.connect(self.handle_stdout)
        self.process.readyReadStandardError.connect(self.handle_stderr)
        self.process.finished.connect(self.handle_finished)
        self.process.errorOccurred.connect(self.handle_process_error)
        
        self.last_stats = {}; self.current_raw_file = ""
        self.current_root_output = ""
        self.completed_run_context = None
        self._stderr_buffer = ""
        self._run_active = False
        self._cancel_requested = False
        self._active_db_run_id = None
        self._active_run_uuid = None
        self._db_production_begun = False
        self._db_terminal_recorded = False
        self.init_ui()
        self.load_settings()
        self.log_pattern = re.compile(r"\[Progress\]\s+([0-9.]+)%\s+\|\s+Events:\s+(\d+)\s+\|\s+Speed:\s+([0-9.]+)\s+MB/s\s+\|\s+ETA:\s+(\d+)")

    def init_ui(self):
        layout = QVBoxLayout()
        io_group = QGroupBox("Input / Output Selection")
        io_group.setStyleSheet("QGroupBox { font-weight: bold; color: #17a2b8; }")
        io_layout = QGridLayout()
        self.input_edit = QLineEdit()
        self.btn_browse_in = QPushButton("Browse Raw")
        self.btn_browse_in.clicked.connect(self.browse_input)
        self.output_edit = QLineEdit()
        self.output_edit.setPlaceholderText("Auto-generated if empty (*_prod.root)")
        self.btn_browse_out = QPushButton("Browse ROOT")
        self.btn_browse_out.clicked.connect(self.browse_output)
        io_layout.addWidget(QLabel("Input Raw (.dat):"), 0, 0); io_layout.addWidget(self.input_edit, 0, 1); io_layout.addWidget(self.btn_browse_in, 0, 2)
        io_layout.addWidget(QLabel("Output ROOT (.root):"), 1, 0); io_layout.addWidget(self.output_edit, 1, 1); io_layout.addWidget(self.btn_browse_out, 1, 2)

        self.config_edit = QLineEdit()
        self.config_edit.setPlaceholderText("Exact run snapshot (*.dat.config.conf)")
        self.btn_browse_config = QPushButton("Browse Config")
        self.btn_browse_config.clicked.connect(self.browse_config)
        io_layout.addWidget(QLabel("Run Config (.conf):"), 2, 0)
        io_layout.addWidget(self.config_edit, 2, 1)
        io_layout.addWidget(self.btn_browse_config, 2, 2)

        self.metadata_edit = QLineEdit()
        self.metadata_edit.setPlaceholderText("Exact runtime metadata (*.dat.run.json)")
        self.btn_browse_metadata = QPushButton("Browse Metadata")
        self.btn_browse_metadata.clicked.connect(self.browse_metadata)
        io_layout.addWidget(QLabel("Run Metadata (.json):"), 3, 0)
        io_layout.addWidget(self.metadata_edit, 3, 1)
        io_layout.addWidget(self.btn_browse_metadata, 3, 2)

        self.spin_run_number = QSpinBox()
        self.spin_run_number.setRange(1, 99999)
        io_layout.addWidget(QLabel("Run Number:"), 4, 0)
        io_layout.addWidget(self.spin_run_number, 4, 1)

        self.lbl_production_identity = QLabel()
        self.lbl_production_identity.setWordWrap(True)
        self.lbl_production_identity.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextSelectableByMouse
        )
        io_layout.addWidget(QLabel("Resolved production:"), 5, 0)
        io_layout.addWidget(self.lbl_production_identity, 5, 1, 1, 2)
        io_group.setLayout(io_layout); layout.addWidget(io_group)

        opt_group = QGroupBox("Conversion Options & Time-Machine Debugger")
        opt_layout = QHBoxLayout()
        self.chk_save_waveforms = QCheckBox("Save Waveforms (-w)")
        self.chk_save_waveforms.setStyleSheet("font-weight: bold;")
        self.chk_debug_mode = QCheckBox("Interactive Debug Mode (-d)")
        self.chk_debug_mode.setStyleSheet("font-weight: bold; color: #d9534f;")
        self.chk_debug_mode.stateChanged.connect(self.toggle_debug_ui)
        self.spin_debug_start = QSpinBox(); self.spin_debug_start.setRange(0, 9999999); self.spin_debug_start.setPrefix("Start Evt ID: "); self.spin_debug_start.setEnabled(False)

        self.btn_run = QPushButton("Run ROOT Conversion")
        self.btn_run.setStyleSheet("background-color: #5bc0de; color: white; font-weight: bold; padding: 8px;")
        self.btn_run.clicked.connect(self.run_conversion)
        self.btn_stop = QPushButton("Stop Conversion")
        self.btn_stop.setStyleSheet("background-color: #d9534f; color: white; font-weight: bold; padding: 8px;")
        self.btn_stop.clicked.connect(self.stop_all)
        
        self.btn_prev = QPushButton("Prev (p)"); self.btn_next = QPushButton("Next (n)"); self.btn_jump = QPushButton("Jump (j)")
        self.spin_jump = QSpinBox(); self.spin_jump.setRange(0, 9999999)
        self.btn_quit = QPushButton("Quit Debug (q)")

        self.btn_prev.clicked.connect(lambda: self.send_debug_command("p\n")); self.btn_next.clicked.connect(lambda: self.send_debug_command("n\n"))
        self.btn_jump.clicked.connect(lambda: self.send_debug_command(f"j {self.spin_jump.value()}\n")); self.btn_quit.clicked.connect(lambda: self.send_debug_command("q\n"))

        opt_layout.addWidget(self.chk_save_waveforms); opt_layout.addWidget(self.chk_debug_mode); opt_layout.addWidget(self.spin_debug_start)
        opt_layout.addSpacing(10); opt_layout.addWidget(self.btn_run); opt_layout.addWidget(self.btn_stop); opt_layout.addSpacing(20)
        opt_layout.addWidget(self.btn_prev); opt_layout.addWidget(self.btn_next); opt_layout.addWidget(self.spin_jump); opt_layout.addWidget(self.btn_jump); opt_layout.addWidget(self.btn_quit)
        opt_group.setLayout(opt_layout); layout.addWidget(opt_group)

        dash_group = QGroupBox("Conversion Status Dashboard")
        dash_layout = QVBoxLayout()
        self.progress_bar = QProgressBar(); self.progress_bar.setValue(0); self.progress_bar.setAlignment(Qt.AlignmentFlag.AlignCenter); self.progress_bar.setStyleSheet("QProgressBar::chunk { background-color: #5cb85c; }")
        stat_layout = QHBoxLayout()
        self.lbl_events = QLabel("Events: 0"); self.lbl_speed = QLabel("Speed: 0.0 MB/s"); self.lbl_eta = QLabel("ETA: 0 s")
        font = self.lbl_events.font(); font.setPointSize(11); font.setBold(True)
        for lbl in [self.lbl_events, self.lbl_speed, self.lbl_eta]: lbl.setFont(font); lbl.setAlignment(Qt.AlignmentFlag.AlignCenter); stat_layout.addWidget(lbl)
        dash_layout.addWidget(self.progress_bar); dash_layout.addLayout(stat_layout); dash_group.setLayout(dash_layout)
        layout.addWidget(dash_group)

        self.log_console = QTextEdit(); self.log_console.setReadOnly(True); self.log_console.setMaximumHeight(200)
        self.log_console.setStyleSheet("background-color: #f8f9fa; color: #333333; font-family: monospace; border: 1px solid #ced4da;")
        layout.addWidget(self.log_console)
        self.setLayout(layout)
        self.set_debug_controls_enabled(False)

    def toggle_debug_ui(self, state):
        self.spin_debug_start.setEnabled(self.chk_debug_mode.isChecked())

    def set_debug_controls_enabled(self, enabled):
        self.btn_prev.setEnabled(enabled); self.btn_next.setEnabled(enabled); self.spin_jump.setEnabled(enabled); self.btn_jump.setEnabled(enabled); self.btn_quit.setEnabled(enabled)

    def load_settings(self):
        self.input_edit.setText(self.settings.value("last_prod_input", ""))
        self.output_edit.setText(self.settings.value("last_prod_output", ""))
        self.config_edit.setText(self.settings.value("last_prod_config", ""))
        self.metadata_edit.setText(self.settings.value("last_prod_metadata", ""))
        self.spin_run_number.setValue(
            int(self.settings.value("last_prod_run_number", 1))
        )
        self.chk_save_waveforms.setChecked(self.settings.value("last_save_wave", False, type=bool))
        self.refresh_runtime_identity()

    def save_settings(self):
        self.settings.setValue("last_prod_input", self.input_edit.text())
        self.settings.setValue("last_prod_output", self.output_edit.text())
        self.settings.setValue("last_prod_config", self.config_edit.text())
        self.settings.setValue("last_prod_metadata", self.metadata_edit.text())
        self.settings.setValue("last_prod_run_number", self.spin_run_number.value())
        self.settings.setValue("last_save_wave", self.chk_save_waveforms.isChecked())

    def refresh_runtime_identity(self):
        executable = os.path.join(self.bin_dir, "production_dt5730")
        try:
            identity = file_identity(executable)
            self.lbl_production_identity.setText(identity_summary(identity))
            self.lbl_production_identity.setStyleSheet("color: #495057;")
        except OSError as exc:
            self.lbl_production_identity.setText(
                f"UNAVAILABLE: {executable} ({exc})"
            )
            self.lbl_production_identity.setStyleSheet(
                "color: #dc3545; font-weight: bold;"
            )

    def browse_input(self):
        last_dir = os.path.dirname(os.path.join(self.proj_dir, self.input_edit.text())) if self.input_edit.text() else self.data_dir
        fname, _ = QFileDialog.getOpenFileName(self, "Open Raw Data", last_dir, "Data Files (*.dat)")
        if fname: self.input_edit.setText(os.path.relpath(fname, self.proj_dir)); self.save_settings()

    def browse_output(self):
        last_dir = os.path.dirname(os.path.join(self.proj_dir, self.output_edit.text())) if self.output_edit.text() else self.data_dir
        fname, _ = QFileDialog.getSaveFileName(self, "Save ROOT Data", last_dir, "ROOT Files (*.root)")
        if fname: self.output_edit.setText(os.path.relpath(fname, self.proj_dir)); self.save_settings()

    def browse_config(self):
        last_dir = (
            os.path.dirname(str(resolve_path(self.proj_dir, self.config_edit.text())))
            if self.config_edit.text().strip() else self.data_dir
        )
        fname, _ = QFileDialog.getOpenFileName(
            self, "Open Exact Run Config", last_dir, "Config Files (*.conf);;All Files (*)"
        )
        if fname:
            self.config_edit.setText(os.path.abspath(fname))
            self.save_settings()

    def browse_metadata(self):
        last_dir = (
            os.path.dirname(str(resolve_path(self.proj_dir, self.metadata_edit.text())))
            if self.metadata_edit.text().strip() else self.data_dir
        )
        fname, _ = QFileDialog.getOpenFileName(
            self, "Open Runtime Metadata", last_dir, "JSON Files (*.json);;All Files (*)"
        )
        if fname:
            self.metadata_edit.setText(os.path.abspath(fname))
            self.save_settings()

    @pyqtSlot(dict)
    def set_run_context(self, context):
        """Populate conversion provenance from a successfully completed DAQ run."""

        self.completed_run_context = dict(context)
        self.input_edit.setText(context.get("raw_file", ""))
        self.config_edit.setText(context.get("config_path", ""))
        self.metadata_edit.setText(context.get("metadata_path", ""))
        run_number = int(context.get("run_number", 1))
        self.spin_run_number.setValue(max(1, run_number))
        self.output_edit.clear()
        self.save_settings()
        metadata_note = "ready" if context.get("metadata_exists") else "MISSING"
        self.log_console.append(
            "<b>[Run Context]</b> Completed DAQ context loaded: "
            f"run={run_number}, metadata={metadata_note}"
        )

    def run_conversion(self):
        if (
            self._run_active
            or self.process.state() != QProcess.ProcessState.NotRunning
        ):
            self.log_console.append(
                "<span style='color:#b45309;'>[Warning] Conversion이 이미 "
                "실행 중이므로 중복 실행하지 않았습니다.</span>"
            )
            return
        self.save_settings()
        raw_value = self.input_edit.text().strip()
        config_value = self.config_edit.text().strip()
        metadata_value = self.metadata_edit.text().strip()
        output_value = self.output_edit.text().strip()
        if not raw_value or not config_value or not metadata_value:
            self.log_console.append(
                "<span style='color:red;'>[Error] Raw input, exact run config, "
                "and runtime metadata are all required.</span>"
            )
            return

        try:
            verify_deployed_gui(self.runtime_gui_dir, self.proj_dir)
            raw_path = require_file(
                resolve_path(self.proj_dir, raw_value), description="raw 입력 파일"
            )
            config_path = require_file(
                resolve_path(self.proj_dir, config_value), description="run config snapshot"
            )
            metadata_path = require_file(
                resolve_path(self.proj_dir, metadata_value), description="runtime metadata"
            )
            executable = os.path.join(self.bin_dir, "production_dt5730")
            executable_identity = verify_binary_fresh(
                executable, production_sources(self.proj_dir)
            )
            config_identity = file_identity(config_path)
            metadata_identity = file_identity(metadata_path)
            context = self.completed_run_context or {}
            context_raw = context.get("raw_file")
            if context_raw and resolve_path(self.proj_dir, context_raw) == raw_path:
                expected_config = resolve_path(
                    self.proj_dir, context.get("config_path", "")
                )
                expected_metadata = resolve_path(
                    self.proj_dir, context.get("metadata_path", "")
                )
                if expected_config != config_path or expected_metadata != metadata_path:
                    raise RuntimeValidationError(
                        "완료된 DAQ context의 config/metadata 경로와 현재 선택이 "
                        "다릅니다."
                    )
                if (context.get("config_sha256") and
                        context["config_sha256"] != config_identity["sha256"]):
                    raise RuntimeValidationError(
                        "DAQ 완료 후 config snapshot 내용이 변경되었습니다."
                    )
                if (context.get("metadata_sha256") and
                        context["metadata_sha256"] != metadata_identity["sha256"]):
                    raise RuntimeValidationError(
                        "DAQ 완료 후 runtime metadata 내용이 변경되었습니다."
                    )
            output_path = (
                resolve_path(self.proj_dir, output_value)
                if output_value else None
            )
            if output_path:
                os.makedirs(output_path.parent, exist_ok=True)
            is_debug_mode = self.chk_debug_mode.isChecked()
            args = build_production_arguments(
                raw_path,
                config_path,
                self.spin_run_number.value(),
                metadata_path,
                root_output=output_path,
                save_waveforms=self.chk_save_waveforms.isChecked(),
                debug_event_id=(
                    self.spin_debug_start.value() if is_debug_mode else None
                ),
            )
            verify_expected_hashes({
                executable: executable_identity["sha256"],
                str(config_path): config_identity["sha256"],
                str(metadata_path): metadata_identity["sha256"],
            })
        except (OSError, RuntimeValidationError) as exc:
            self.log_console.append(
                f"<span style='color:red;'>[Error] Conversion launch blocked: "
                f"{exc}</span>"
            )
            return

        self.current_raw_file = str(raw_path)
        self.current_root_output = (
            "" if is_debug_mode else str(
                output_path or default_production_output(raw_path)
            )
        )
        self.progress_bar.setValue(0); self.progress_bar.setFormat("%p%")
        self.lbl_events.setText("Events: 0"); self.lbl_speed.setText("Speed: 0.0 MB/s"); self.lbl_eta.setText("ETA: 0 s")
        self.log_console.clear(); self.last_stats = {}
        self._stderr_buffer = ""
        self._run_active = True
        self._cancel_requested = False
        self._db_terminal_recorded = False
        self._active_db_run_id = None
        self._active_run_uuid = None
        self._db_production_begun = False
        context_db_identity_supplied = False
        if context_raw and resolve_path(self.proj_dir, context_raw) == raw_path:
            context_run_id = context.get("db_run_id")
            context_run_uuid = context.get("run_uuid")
            context_db_identity_supplied = context_run_id is not None
            if context_run_id is not None and context_run_uuid:
                try:
                    resolved_run = self.db.resolve_run_identity(
                        int(context_run_id),
                        run_uuid=context_run_uuid,
                        output_file=raw_path,
                    )
                    self._active_db_run_id = int(resolved_run["id"])
                    self._active_run_uuid = str(resolved_run["run_uuid"])
                except Exception as db_exc:
                    self.log_console.append(
                        "<span style='color:red;'>[DB Error] 전달된 run "
                        "id/UUID/raw identity가 현재 DB와 일치하지 않습니다. "
                        f"잘못된 행은 갱신하지 않습니다: "
                        f"{html.escape(str(db_exc))}</span>"
                    )
            elif context_run_id is not None:
                self.log_console.append(
                    "<span style='color:red;'>[DB Error] 전달된 DAQ context에 "
                    "run_uuid가 없어 정수 id만으로 DB 행을 갱신하지 "
                    "않습니다.</span>"
                )
        if self._active_db_run_id is None and not context_db_identity_supplied:
            try:
                self._active_db_run_id = self.db.find_run_id_by_output(raw_path)
                if self._active_db_run_id is not None:
                    self._active_run_uuid = self.db.get_run_uuid(
                        self._active_db_run_id
                    )
            except Exception as db_exc:
                self.log_console.append(
                    "<span style='color:red;'>[DB Error] Run identity lookup "
                    f"failed: {html.escape(str(db_exc))}</span>"
                )
        self.lbl_production_identity.setText(identity_summary(executable_identity))
        self.btn_run.setEnabled(False); self.set_debug_controls_enabled(is_debug_mode)
        self.log_console.append(
            f"<b>[Runtime] Production:</b> {identity_summary(executable_identity)}"
        )
        self.log_console.append(
            f"<b>[Runtime] Config:</b> {identity_summary(config_identity)}"
        )
        self.log_console.append(
            f"<b>[Runtime] Metadata:</b> {identity_summary(metadata_identity)}"
        )
        self.log_console.append(f"<b>[System] Starting:</b> {executable} {' '.join(args)}")
        if self._active_db_run_id is not None:
            try:
                self.db.begin_production(
                    self._active_db_run_id,
                    run_uuid=self._active_run_uuid,
                    output_file=self.current_raw_file,
                )
                self._db_production_begun = True
            except Exception as db_exc:
                self.log_console.append(
                    "<span style='color:red;'>[DB Error] Production launch "
                    f"status was not recorded: {html.escape(str(db_exc))}</span>"
                )
        self.process.setWorkingDirectory(self.proj_dir)
        self.process.start(str(executable), args)

    def stop_all(self):
        if self.process.state() != QProcess.ProcessState.NotRunning:
            self._cancel_requested = True
            self.process.terminate()
            self.log_console.append(
                "<span style='color:#b45309;'>[System] Graceful conversion "
                "stop requested; waiting for finalization.</span>"
            )

    def force_stop(self):
        """Explicit recovery action; normal Stop never blocks or kills."""

        if self.process.state() != QProcess.ProcessState.NotRunning:
            self._cancel_requested = True
            self.process.kill()
            self.log_console.append(
                "<span style='color:red;'>[System] Explicit force stop "
                "requested.</span>"
            )

    def send_debug_command(self, cmd_str):
        if self.process.state() == QProcess.ProcessState.Running: self.process.write(cmd_str.encode('utf-8'))

    @pyqtSlot()
    def handle_stdout(self):
        while self.process.canReadLine():
            line = self.process.readLine().data().decode('utf-8', errors='ignore').strip()
            if not line: continue
            clean_line = re.compile(r'\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])').sub('', line)
            match = self.log_pattern.search(clean_line)
            if match:
                self.progress_bar.setValue(int(float(match.group(1)))); self.lbl_events.setText(f"Events: {int(match.group(2)):,}")
                self.lbl_speed.setText(f"Speed: {match.group(3)} MB/s"); self.lbl_eta.setText(f"ETA: {match.group(4)} s")
                self.last_stats = {"events": match.group(2), "avg_speed": match.group(3)}
            else:
                self.log_console.append(clean_line)
                self.log_console.verticalScrollBar().setValue(self.log_console.verticalScrollBar().maximum())

    @pyqtSlot()
    def handle_stderr(self):
        payload = bytes(self.process.readAllStandardError()).decode(
            'utf-8', errors='replace'
        )
        if payload:
            self._stderr_buffer += payload
            self._consume_stderr(final=False)

    def _consume_stderr(self, final=False):
        lines = self._stderr_buffer.splitlines(keepends=True)
        remainder = ""
        if (
            lines
            and not final
            and not lines[-1].endswith(('\n', '\r'))
        ):
            remainder = lines.pop()
        self._stderr_buffer = remainder
        for raw_line in lines:
            line = raw_line.rstrip('\r\n')
            if line:
                self.log_console.append(
                    "<span style='color:red;'>"
                    f"{html.escape(line)}</span>"
                )

    def handle_process_error(self, process_error):
        self.handle_stderr()
        error_text = html.escape(self.process.errorString())
        self.log_console.append(
            "<span style='color:red;'><b>[Process Error]</b> "
            f"{error_text} ({process_error.name})</span>"
        )
        if process_error == QProcess.ProcessError.FailedToStart:
            self._run_active = False
            self.progress_bar.setValue(0)
            self.progress_bar.setFormat("Failed to start")
            self.btn_run.setEnabled(True)
            self.set_debug_controls_enabled(False)
            self._record_production_terminal(
                "production_launch_failed", -1,
                error_message=self.process.errorString(),
            )

    def _record_production_terminal(
        self, status, exit_code, *, root_file=None, error_message=None
    ):
        if self._db_terminal_recorded:
            return
        if self._active_db_run_id is None:
            self._db_terminal_recorded = True
            return
        try:
            if not self._db_production_begun:
                self.db.begin_production(
                    self._active_db_run_id,
                    run_uuid=self._active_run_uuid,
                    output_file=self.current_raw_file,
                )
                self._db_production_begun = True
            self.db.finalize_production_run(
                self._active_db_run_id,
                status=status,
                exit_code=exit_code,
                summary_dict=self.last_stats or None,
                root_file=root_file,
                error_message=error_message,
                run_uuid=self._active_run_uuid,
                output_file=self.current_raw_file,
            )
            self._db_terminal_recorded = True
            self.log_console.append(
                "<span style='color:#6f42c1;'><b>[DB]</b> "
                f"Production lifecycle recorded: {html.escape(status)}.</span>"
            )
        except Exception as db_exc:
            # Conversion completion and control recovery must not depend on DB
            # availability. The error remains operator-visible for later repair.
            self.log_console.append(
                "<span style='color:red;'>[DB Error] Production 종료 상태를 "
                f"기록하지 못했습니다: {html.escape(str(db_exc))}</span>"
            )

    @pyqtSlot(int, QProcess.ExitStatus)
    def handle_finished(self, exitCode, exitStatus):
        self.handle_stdout()
        self.handle_stderr()
        self._consume_stderr(final=True)
        self._run_active = False
        self.btn_run.setEnabled(True); self.set_debug_controls_enabled(False)
        process_succeeded = (
            exitStatus == QProcess.ExitStatus.NormalExit and exitCode == 0
        )
        expected_root_exists = bool(
            self.current_root_output
            and os.path.isfile(self.current_root_output)
        )
        if self._cancel_requested:
            db_status = "production_cancelled"
        elif process_succeeded and (
            not self.current_root_output or expected_root_exists
        ):
            db_status = "production_completed"
        else:
            db_status = "production_failed"
        self._record_production_terminal(
            db_status,
            exitCode,
            root_file=(self.current_root_output if expected_root_exists else None),
            error_message=(
                None if db_status == "production_completed"
                else f"production exit code {exitCode}"
            ),
        )

        conversion_succeeded = process_succeeded and (
            not self.current_root_output or expected_root_exists
        )
        if conversion_succeeded:
            self.progress_bar.setValue(100)
            self.progress_bar.setFormat("Completed")
            self.log_console.append(f"<span style='color:#5cb85c;'><b>[System] Conversion Successfully Finished!</b></span>")
            if self.current_root_output:
                if os.path.isfile(self.current_root_output):
                    self.rootOutputReady.emit(
                        os.path.abspath(self.current_root_output)
                    )
                    self.log_console.append(
                        "<span style='color:#17a2b8;'><b>[Validation]</b> "
                        "완성된 ROOT 파일을 검증 탭에 전달했습니다.</span>"
                    )
                else:
                    self.log_console.append(
                        "<span style='color:red;'><b>[Error]</b> Conversion은 "
                        "성공했지만 예상 ROOT 출력을 찾지 못했습니다: "
                        f"{self.current_root_output}</span>"
                    )
        else:
            self.progress_bar.setFormat(f"Failed (exit {exitCode})")
            self.log_console.append(f"<span style='color:red;'><b>[System] Conversion Exited with Code: {exitCode}</b></span>")
