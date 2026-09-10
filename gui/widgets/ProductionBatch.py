"""In-memory, sequential production queue. Never modifies input/output files."""

import os
from collections import Counter
from pathlib import Path

from PyQt6.QtCore import QTimer, pyqtSignal
from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QPushButton, QLabel, QCheckBox,
    QTableWidget, QTableWidgetItem, QAbstractItemView, QHeaderView,
    QFileDialog, QProgressBar,
)

from core.runtime_paths import default_production_output


class ProductionBatch(QWidget):
    jobRequested = pyqtSignal(dict)
    activeChanged = pyqtSignal(bool)

    def __init__(self, project_root, parent=None):
        super().__init__(parent)
        self.project_root = Path(project_root)
        self.jobs = []
        self._counts = Counter()
        self.active = False
        self.current_row = None
        self._next_row = 0
        self._stop_requested = False
        self._busy = False
        self._options = {}
        # An owned timer can be cancelled during Stop/close, including the
        # gap between children. Never recurse through failed/skipped entries.
        self._next_timer = QTimer(self)
        self._next_timer.setSingleShot(True)
        self._next_timer.timeout.connect(self._launch_next)

        layout = QVBoxLayout(self)
        buttons = QHBoxLayout()
        self.btn_add = QPushButton("Add DAT files…")
        self.btn_folder = QPushButton("Add folder…")
        self.btn_remove = QPushButton("Remove selected")
        self.btn_clear = QPushButton("Clear list")
        self.btn_start = QPushButton("Start Batch")
        self.chk_continue = QCheckBox("Continue after a failed file")
        self.chk_continue.setChecked(True)
        self.btn_add.clicked.connect(self.browse_files)
        self.btn_folder.clicked.connect(self.browse_folder)
        self.btn_remove.clicked.connect(self.remove_selected)
        self.btn_clear.clicked.connect(self.clear)
        for widget in (self.btn_add, self.btn_folder, self.btn_remove,
                       self.btn_clear, self.chk_continue, self.btn_start):
            buttons.addWidget(widget)
        layout.addLayout(buttons)
        note = QLabel(
            "One file at a time; ROOTs are saved beside each DAT. Existing outputs "
            "are skipped, NOT validated. Folder import includes only that folder's "
            ".dat files (no subfolders/partial files). Batch uses the analysis and "
            "waveform options above; run number is automatic per file. Verified "
            "mode uses each DAT's .config.conf + .run.json, not the single-file fields."
        )
        note.setWordWrap(True)
        layout.addWidget(note)
        self.table = QTableWidget(0, 4)
        self.table.setHorizontalHeaderLabels(["DAT input", "ROOT output", "Status", "Details"])
        self.table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self.table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self.table.setSelectionMode(QAbstractItemView.SelectionMode.ExtendedSelection)
        self.table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        self.table.setMinimumHeight(150)
        layout.addWidget(self.table)
        self.progress = QProgressBar()
        self.summary = QLabel("No files queued")
        layout.addWidget(self.progress)
        layout.addWidget(self.summary)
        self._update_summary()

    def set_busy(self, busy):
        self._busy = busy
        for widget in (self.btn_add, self.btn_folder, self.btn_remove,
                       self.btn_clear, self.chk_continue):
            widget.setEnabled(not busy)
        self.btn_start.setEnabled(not busy and bool(self.jobs))

    def add_paths(self, paths):
        if self._busy or self.active:
            return
        known = {job["raw"] for job in self.jobs}
        rejected = 0
        for value in sorted(paths, key=lambda path: str(path).casefold()):
            path = Path(value).expanduser()
            if not path.is_absolute():
                path = self.project_root / path
            try:
                path = path.resolve()
                if path.suffix.lower() != ".dat" or not path.is_file():
                    rejected += 1
                    continue
            except (OSError, RuntimeError):
                rejected += 1
                continue
            raw = str(path)
            if raw in known:
                continue
            known.add(raw)
            job = {"raw": raw, "output": str(default_production_output(path)),
                   "status": "Pending", "detail": ""}
            self.jobs.append(job)
            self._counts["Pending"] += 1
            row = self.table.rowCount()
            self.table.insertRow(row)
            for column, text in enumerate((job["raw"], job["output"], "Pending", "")):
                item = QTableWidgetItem(text)
                item.setToolTip(text)
                self.table.setItem(row, column, item)
        self._update_summary()
        if rejected:
            self.summary.setText(self.summary.text() + f" | Ignored {rejected} non-DAT/unreadable entries")
        self.set_busy(False)

    def browse_files(self):
        paths, _ = QFileDialog.getOpenFileNames(
            self, "Add DAT files to production queue", str(self.project_root / "data"),
            "DAT files (*.dat *.DAT)",
        )
        self.add_paths(paths)

    def add_folder(self, folder):
        if self._busy or self.active:
            return
        try:
            with os.scandir(folder) as entries:
                paths = [entry.path for entry in entries
                         if entry.name.lower().endswith(".dat") and entry.is_file()]
            self.add_paths(paths)
        except OSError as error:
            self.summary.setText(f"Cannot read folder: {error}")

    def browse_folder(self):
        folder = QFileDialog.getExistingDirectory(
            self, "Add folder's DAT files (not recursive)", str(self.project_root / "data")
        )
        if folder:
            self.add_folder(folder)

    def remove_selected(self):
        if self._busy or self.active:
            return
        for row in sorted({index.row() for index in self.table.selectedIndexes()}, reverse=True):
            self.table.removeRow(row)
            self._counts[self.jobs[row]["status"]] -= 1
            del self.jobs[row]
        self._update_summary()
        self.set_busy(False)

    def clear(self):
        if self._busy or self.active:
            return
        self.jobs.clear()
        self._counts.clear()
        self.table.setRowCount(0)
        self._update_summary()
        self.set_busy(False)

    def start(self, options):
        if self.active or self._busy or not self.jobs:
            return False
        self._options = dict(options)
        self._options["continue_after_failure"] = self.chk_continue.isChecked()
        self._next_row = 0
        self.current_row = None
        self._stop_requested = False
        for row in range(len(self.jobs)):
            self._set_status(row, "Pending", "")
        self.active = True
        self.set_busy(True)
        self.activeChanged.emit(True)
        self._next_timer.start(0)
        return True

    def _launch_next(self):
        if not self.active or self.current_row is not None:
            return
        if self._stop_requested or self._next_row >= len(self.jobs):
            self._finish()
            return
        row = self._next_row
        self._next_row += 1
        job = self.jobs[row]
        if os.path.lexists(job["output"]):
            self._set_status(row, "Skipped", "Output already exists; contents NOT validated or overwritten")
            self._next_timer.start(0)
            return
        self.current_row = row
        self._set_status(row, "Running", "")
        self.table.scrollToItem(self.table.item(row, 0))
        self.jobRequested.emit({**job, **self._options})

    def set_current_progress(self, percent):
        if self.current_row is not None:
            self.table.item(self.current_row, 2).setText(f"Running {percent:.1f}%")

    def complete_current(self, status, detail=""):
        if self.current_row is None:
            return
        self._set_status(self.current_row, status, detail)
        self.current_row = None
        if status == "Failed" and not self._options["continue_after_failure"]:
            self.stop("Not started: stopped after a failed file")
        elif self._stop_requested:
            self._finish()
        else:
            self._next_timer.start(0)

    def stop(self, reason="Not started: batch stopped"):
        if not self.active:
            return
        self._stop_requested = True
        self._next_timer.stop()
        for row in range(self._next_row, len(self.jobs)):
            self._set_status(row, "Cancelled", reason)
        if self.current_row is None:
            self._finish()

    def _finish(self):
        self._next_timer.stop()
        self.active = False
        self.set_busy(False)
        self._update_summary()
        self.activeChanged.emit(False)

    def _set_status(self, row, status, detail):
        self._counts[self.jobs[row]["status"]] -= 1
        self._counts[status] += 1
        self.jobs[row]["status"] = status
        self.jobs[row]["detail"] = detail
        self.table.item(row, 2).setText(status)
        item = self.table.item(row, 3)
        item.setText(detail)
        item.setToolTip(detail)
        self._update_summary()

    def _update_summary(self):
        counts = self._counts
        done = sum(counts[status] for status in ("Succeeded", "Failed", "Skipped", "Cancelled"))
        self.progress.setRange(0, max(1, len(self.jobs)))
        self.progress.setValue(done)
        self.progress.setFormat(f"{done}/{len(self.jobs)} files finished")
        self.summary.setText(" | ".join(
            f"{status}: {counts[status]}"
            for status in ("Pending", "Running", "Succeeded", "Failed", "Skipped", "Cancelled")
        ))
