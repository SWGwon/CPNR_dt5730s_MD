import math
import os
import time

import numpy as np
import pyqtgraph as pg
import zmq
from PyQt6.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout, QComboBox, QLabel, QPushButton, QSpinBox, QProgressBar, QMessageBox
from PyQt6.QtCore import QTimer, pyqtSlot
from collections import deque

from core.monitor_stream import (
    EventSequenceTracker,
    MAX_MONITOR_FRAME_BYTES,
    MonitorConfigError,
    RuntimeConfigReference,
    analyze_monitor_waveform,
    drain_latest_frames,
    load_runtime_polarity,
)


DEFAULT_POLL_MESSAGE_BUDGET = 32
DEFAULT_POLL_TIME_BUDGET_MS = 5.0
MAX_POLL_MESSAGE_BUDGET = 256
MAX_POLL_TIME_BUDGET_MS = 50.0
POLARITY_REFRESH_NS = 1_000_000_000
HISTOGRAM_REFRESH_NS = 100_000_000
MONITOR_RCVHWM_MESSAGES = 32

class MonitorTab(QWidget):
    def __init__(
        self,
        parent=None,
        *,
        config_path_provider=None,
        monitor_socket=None,
        monitor_context=None,
        poll_message_budget=DEFAULT_POLL_MESSAGE_BUDGET,
        poll_time_budget_ms=DEFAULT_POLL_TIME_BUDGET_MS,
        clock_ns=time.monotonic_ns,
    ):
        super().__init__(parent)
        if (
            isinstance(poll_message_budget, bool)
            or not isinstance(poll_message_budget, int)
            or not 1 <= poll_message_budget <= MAX_POLL_MESSAGE_BUDGET
        ):
            raise ValueError(
                f"poll_message_budget must be 1..{MAX_POLL_MESSAGE_BUDGET}"
            )
        if (
            isinstance(poll_time_budget_ms, bool)
            or not isinstance(poll_time_budget_ms, (int, float))
            or not math.isfinite(float(poll_time_budget_ms))
            or not 0.1 <= float(poll_time_budget_ms) <= MAX_POLL_TIME_BUDGET_MS
        ):
            raise ValueError(
                "poll_time_budget_ms must be finite and in the 0.1..50 ms range"
            )
        if not callable(clock_ns):
            raise TypeError("clock_ns must be callable")

        self.current_mask = -1
        self.curves_wave = {}
        self.curves_qlong = {}
        self.q_long_hists = {}
        self.config_path_provider = config_path_provider
        self.poll_message_budget = poll_message_budget
        self.poll_time_budget_ns = int(float(poll_time_budget_ms) * 1_000_000)
        self._clock_ns = clock_ns
        self._cleaned = False
        self._owns_zmq_context = False
        self._active_polarity = None
        self._polarity_source = None
        self._polarity_identity = None
        self._polarity_sha256 = None
        self._polarity_authenticated = False
        self._polarity_provider_key = None
        self._polarity_error = "active runtime config has not been resolved"
        self._next_polarity_refresh_ns = 0
        self._next_histogram_refresh_ns = 0
        self.sequence_tracker = EventSequenceTracker()
        self.total_received_frames = 0
        self.total_valid_frames = 0
        self.total_malformed_frames = 0
        self.total_rendered_frames = 0
        self.total_decimated_frames = 0
        self.budget_limited_ticks = 0
        self.socket_errors = 0
        self.last_stream_error = None

        self.colors = [
            '#0d6efd', '#198754', '#dc3545', '#fd7e14', 
            '#6f42c1', '#0dcaf0', '#d63384', '#6c757d'
        ]

        self.warning_latched = False

        self.setup_zmq(monitor_socket, monitor_context)
        self.setup_ui()
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.poll_zmq)
        self.timer.start(33)

    def setup_zmq(self, monitor_socket=None, monitor_context=None):
        if monitor_socket is not None:
            self.ctx = monitor_context
            self.sock = monitor_socket
            return

        context = monitor_context or zmq.Context(io_threads=1)
        owns_context = monitor_context is None
        sock = context.socket(zmq.SUB)
        try:
            sock.setsockopt(zmq.LINGER, 0)
            sock.setsockopt(zmq.RCVHWM, MONITOR_RCVHWM_MESSAGES)
            sock.setsockopt(zmq.MAXMSGSIZE, MAX_MONITOR_FRAME_BYTES)
            # Keep at most the latest single-part event when libzmq supports
            # conflation.  The explicit poll budgets below remain mandatory.
            if hasattr(zmq, "CONFLATE"):
                sock.setsockopt(zmq.CONFLATE, 1)
            sock.setsockopt_string(zmq.SUBSCRIBE, "")
            sock.connect("tcp://127.0.0.1:5555")
        except Exception:
            sock.close(linger=0)
            if owns_context:
                context.term()
            raise
        self.ctx = context
        self.sock = sock
        self._owns_zmq_context = owns_context

    def setup_ui(self):
        layout = QVBoxLayout(self)
        
        ctrl_layout = QHBoxLayout()
        ctrl_layout.addWidget(QLabel("<b>Display Engine:</b>"))
        
        self.cb_monitor = QComboBox()
        self.cb_monitor.addItems(["🟢 Live Monitor: ON (Auto Multi-Channel)", "🔴 Live Monitor: OFF (Save CPU)"])
        self.cb_monitor.currentIndexChanged.connect(self.toggle_monitor)
        ctrl_layout.addWidget(self.cb_monitor)

        ctrl_layout.addWidget(QLabel("  |  <b>Analysis Mode:</b>"))
        self.cb_spec_mode = QComboBox()
        self.cb_spec_mode.addItems(["📊 Pulse Charge (Integral Area)", "📈 Pulse Height (Amplitude)"])
        self.cb_spec_mode.currentIndexChanged.connect(self.toggle_spec_mode)
        ctrl_layout.addWidget(self.cb_spec_mode)

        ctrl_layout.addWidget(QLabel("  |  <b>GUI Sample History:</b>"))
        self.spin_history = QSpinBox()
        self.spin_history.setRange(100, 100000)
        self.spin_history.setSingleStep(500)
        self.spin_history.setValue(2000)
        self.spin_history.setSuffix(" frames")
        self.spin_history.valueChanged.connect(self.update_history_size)
        ctrl_layout.addWidget(self.spin_history)

        self.btn_clear = QPushButton("🗑️ Clear All")
        self.btn_clear.setStyleSheet("font-weight: bold; padding: 4px 15px; margin-left: 10px;")
        self.btn_clear.clicked.connect(self.clear_data)
        ctrl_layout.addWidget(self.btn_clear)
        
        ctrl_layout.addWidget(QLabel("  |  <b>ADC Temp:</b>"))
        self.temp_bar = QProgressBar()
        self.temp_bar.setRange(0, 100)
        self.temp_bar.setFormat("%v °C")
        self.temp_bar.setFixedWidth(100)
        self.temp_bar.setStyleSheet("QProgressBar { text-align: center; } QProgressBar::chunk { background-color: #198754; }")
        ctrl_layout.addWidget(self.temp_bar)
        
        ctrl_layout.addStretch() 
        layout.addLayout(ctrl_layout)

        monitor_status_layout = QHBoxLayout()
        self.lbl_polarity = QLabel(
            "DSP polarity: unavailable (spectrum paused)"
        )
        self.lbl_polarity.setToolTip(
            "Charge/height DSP is enabled only when TriggerPolarity can be "
            "read from the active runtime config."
        )
        monitor_status_layout.addWidget(self.lbl_polarity)
        monitor_status_layout.addSpacing(20)
        self.lbl_stream_stats = QLabel()
        self.lbl_stream_stats.setWordWrap(True)
        self.lbl_stream_stats.setToolTip(
            "Subscriber-observed EventID gaps are gaps visible to this GUI "
            "SUB socket. They are not the DAQ hardware/publisher loss count."
        )
        monitor_status_layout.addWidget(self.lbl_stream_stats, 1)
        layout.addLayout(monitor_status_layout)
        self._update_stream_status()

        pg.setConfigOptions(antialias=True, background='#f8f9fa', foreground='#212529')
        self.glw = pg.GraphicsLayoutWidget()
        layout.addWidget(self.glw)

        self.plot_wave = self.glw.addPlot(title="Live Waveform (Auto Overlay)")
        self.plot_wave.setLabel('bottom', "Samples (2ns)")
        self.plot_wave.setLabel('left', "ADC Value (14-bit)")
        self.plot_wave.addLegend(offset=(10, 10))
        self.glw.nextRow()

        self.plot_qlong = self.glw.addPlot(
            title="Decimated Live-Monitor Charge Spectrum"
        )
        self.plot_qlong.setLogMode(y=True)
        self.plot_qlong.setLabel('bottom', "Integrated Charge (ADC Bins)")
        self.plot_qlong.setLabel('left', "Counts (Log)")
        self.plot_qlong.addLegend(offset=(10, 10))

    @pyqtSlot(int)
    def toggle_spec_mode(self, idx):
        """Update the monitor-only DSP view and discard mixed-mode history."""
        if idx == 0:
            self.plot_qlong.setTitle(
                "Decimated Live-Monitor Charge Spectrum"
            )
            self.plot_qlong.setLabel('bottom', "Integrated Charge (ADC Bins)")
        else:
            self.plot_qlong.setTitle(
                "Decimated Live-Monitor Pulse Height Spectrum"
            )
            self.plot_qlong.setLabel('bottom', "Pulse Height Amplitude (ADC Bins)")
        self._clear_plot_data(reset_stream_stats=False)

    @pyqtSlot(float)
    def update_temperature(self, temp: float):
        self.temp_bar.setValue(int(temp))
        if temp >= 80.0:
            self.temp_bar.setStyleSheet("QProgressBar { text-align: center; } QProgressBar::chunk { background-color: #dc3545; }")
            if not self.warning_latched:
                self.warning_latched = True
                QMessageBox.warning(
                    self, 
                    "Over-Temperature Warning", 
                    "ADC 내부 온도가 80°C를 초과했습니다.\n82°C 도달 시 하드웨어 보호를 위해 ADC가 강제 종료됩니다."
                )
        else:
            self.temp_bar.setStyleSheet("QProgressBar { text-align: center; } QProgressBar::chunk { background-color: #198754; }")
            if temp < 75.0:
                self.warning_latched = False

    def update_history_size(self, _value=None):
        new_size = self.spin_history.value()
        for ch in self.q_long_hists:
            current_data = list(self.q_long_hists[ch])
            self.q_long_hists[ch] = deque(current_data[-new_size:], maxlen=new_size)

    def rebuild_plots(self, mask):
        self.plot_wave.clear()
        self.plot_qlong.clear()
        if self.plot_wave.legend: self.plot_wave.legend.clear()
        if self.plot_qlong.legend: self.plot_qlong.legend.clear()
        
        self.curves_wave.clear()
        self.curves_qlong.clear()
        self.q_long_hists.clear()
        
        active_channels = [i for i in range(8) if (mask >> i) & 1]
        
        for ch in active_channels:
            color = self.colors[ch % len(self.colors)]
            
            pen = pg.mkPen(color, width=1.5)
            self.curves_wave[ch] = self.plot_wave.plot(name=f"CH {ch}", pen=pen)
            
            brush = pg.mkColor(color)
            brush.setAlpha(100)
            
            self.curves_qlong[ch] = self.plot_qlong.plot(
                name=f"CH {ch}",
                stepMode="center",
                fillLevel=0.1,
                brush=brush,
                pen=color,
            )
            
            self.q_long_hists[ch] = deque(maxlen=self.spin_history.value())

    def toggle_monitor(self, idx):
        if idx == 0:
            # A deliberate OFF interval is a new observation window.  Do not
            # report the intentionally unobserved interval as a DAQ loss.
            self.sequence_tracker.start_new_observation_window()
            self.timer.start(33)
        else:
            self.timer.stop()
            self.sequence_tracker.start_new_observation_window()

    def clear_data(self):
        self._clear_plot_data(reset_stream_stats=True)

    def _clear_plot_data(self, *, reset_stream_stats):
        for ch in self.q_long_hists:
            self.q_long_hists[ch].clear()
            if ch in self.curves_wave:
                self.curves_wave[ch].setData(np.array([], dtype=np.uint16))
            if ch in self.curves_qlong:
                self.curves_qlong[ch].setData(x=np.array([-0.5, 0.5]), y=np.array([0.1]))

        if reset_stream_stats:
            self.sequence_tracker.reset()
            self.total_received_frames = 0
            self.total_valid_frames = 0
            self.total_malformed_frames = 0
            self.total_rendered_frames = 0
            self.total_decimated_frames = 0
            self.budget_limited_ticks = 0
            self.socket_errors = 0
            self.last_stream_error = None
            self._update_stream_status()

    def _refresh_runtime_polarity(self, now_ns):
        if not callable(self.config_path_provider):
            state_changed = (
                self._active_polarity is not None
                or self._polarity_provider_key is not None
            )
            self._active_polarity = None
            self._polarity_source = None
            self._polarity_identity = None
            self._polarity_sha256 = None
            self._polarity_authenticated = False
            self._polarity_provider_key = None
            self._polarity_error = "active runtime config path is unavailable"
            if state_changed:
                self._start_new_runtime_observation()
            self._update_polarity_status()
            return

        try:
            config_reference = self.config_path_provider()
            if not config_reference:
                raise MonitorConfigError(
                    "active runtime config path is unavailable"
                )
            if isinstance(config_reference, RuntimeConfigReference):
                config_path = config_reference.path
                expected_sha256 = config_reference.expected_sha256
            else:
                config_path = config_reference
                expected_sha256 = None
            provider_key = os.fspath(config_path)
            if not isinstance(provider_key, str):
                raise MonitorConfigError(
                    "active runtime config path is not textual"
                )
            provider_key = (provider_key, expected_sha256)
        except (MonitorConfigError, OSError, TypeError, ValueError) as exc:
            state_changed = (
                self._active_polarity is not None
                or self._polarity_provider_key is not None
            )
            self._active_polarity = None
            self._polarity_source = None
            self._polarity_identity = None
            self._polarity_sha256 = None
            self._polarity_authenticated = False
            self._polarity_provider_key = None
            self._polarity_error = str(exc)
            self._next_polarity_refresh_ns = now_ns + POLARITY_REFRESH_NS
            if state_changed:
                self._start_new_runtime_observation()
            self._update_polarity_status()
            return

        # Check the provider on every tick (cheap) so a new run/config cannot
        # use the previous run's polarity for up to one refresh interval.
        provider_changed = provider_key != self._polarity_provider_key
        if not provider_changed and now_ns < self._next_polarity_refresh_ns:
            return
        self._next_polarity_refresh_ns = now_ns + POLARITY_REFRESH_NS
        previous_polarity = self._active_polarity
        if provider_changed:
            self._start_new_runtime_observation()

        try:
            selection = load_runtime_polarity(
                config_path, expected_sha256=expected_sha256
            )
        except (MonitorConfigError, OSError, TypeError, ValueError) as exc:
            # Fail closed: a stale previously parsed polarity is not safe for
            # a newly selected run. Waveforms remain visible, spectra pause.
            self._active_polarity = None
            self._polarity_source = None
            self._polarity_identity = None
            self._polarity_sha256 = None
            self._polarity_authenticated = False
            self._polarity_provider_key = provider_key
            self._polarity_error = str(exc)
            if not provider_changed and previous_polarity is not None:
                self._start_new_runtime_observation()
        else:
            self._active_polarity = selection.polarity
            self._polarity_source = selection.source_path
            self._polarity_identity = selection.identity
            self._polarity_sha256 = selection.sha256
            self._polarity_authenticated = selection.authenticated
            self._polarity_provider_key = provider_key
            self._polarity_error = None
            if (
                not provider_changed
                and previous_polarity is not None
                and previous_polarity != selection.polarity
            ):
                self._start_new_runtime_observation()
        self._update_polarity_status()

    def _start_new_runtime_observation(self):
        self.sequence_tracker.start_new_observation_window()
        self._clear_plot_data(reset_stream_stats=False)

    def _update_polarity_status(self):
        if self._active_polarity is None:
            self.lbl_polarity.setText(
                "DSP polarity: unavailable (spectrum paused)"
            )
            self.lbl_polarity.setStyleSheet("color: #b02a37; font-weight: bold;")
            self.lbl_polarity.setToolTip(
                "Monitor DSP is paused because the active runtime config "
                f"cannot be trusted: {self._polarity_error}"
            )
            return

        source_name = self._polarity_source.rsplit("/", 1)[-1]
        verification = "SHA-256 verified" if self._polarity_authenticated else "selected file"
        self.lbl_polarity.setText(
            f"DSP polarity: {self._active_polarity} "
            f"({source_name}, {verification})"
        )
        self.lbl_polarity.setStyleSheet("color: #146c43; font-weight: bold;")
        self.lbl_polarity.setToolTip(
            f"Polarity read from runtime config: {self._polarity_source}\n"
            f"SHA-256: {self._polarity_sha256}\n"
            f"Run-context authenticated: {self._polarity_authenticated}"
        )

    def _update_stream_status(self):
        tracker = self.sequence_tracker
        self.lbl_stream_stats.setText(
            "Frames recv/render/GUI-decimated/malformed: "
            f"{self.total_received_frames}/{self.total_rendered_frames}/"
            f"{self.total_decimated_frames}/{self.total_malformed_frames}  |  "
            "Subscriber-observed EventID gaps (not DAQ loss): "
            f"{tracker.observed_subscriber_gaps}  |  "
            f"duplicates/discontinuities: {tracker.duplicate_event_ids}/"
            f"{tracker.sequence_discontinuities}  |  "
            f"budget-limited ticks: {self.budget_limited_ticks}"
        )
        if self.last_stream_error:
            self.lbl_stream_stats.setToolTip(
                "Subscriber-observed EventID gaps are not the DAQ loss count. "
                f"Last monitor error: {self.last_stream_error}"
            )
        else:
            self.lbl_stream_stats.setToolTip(
                "Subscriber-observed EventID gaps can be caused by PUB/SUB "
                "conflation, HWM, transport, or a paused/decimated GUI. They "
                "do not measure hardware or raw-file DAQ loss."
            )

    def poll_zmq(self):
        if self._cleaned or self.sock is None:
            return

        now_ns = self._clock_ns()
        self._refresh_runtime_polarity(now_ns)
        try:
            batch = drain_latest_frames(
                lambda: self.sock.recv(flags=zmq.NOBLOCK),
                self.sequence_tracker,
                max_messages=self.poll_message_budget,
                time_budget_ns=self.poll_time_budget_ns,
                clock_ns=self._clock_ns,
                empty_exceptions=(zmq.Again,),
            )
        except zmq.ZMQError as exc:
            self.socket_errors += 1
            self.last_stream_error = f"ZeroMQ receive error: {exc}"
            self._update_stream_status()
            return

        self.total_received_frames += batch.received_frames
        self.total_valid_frames += batch.valid_frames
        self.total_malformed_frames += batch.malformed_frames
        self.total_decimated_frames += batch.decimated_valid_frames
        if batch.stop_reason in ("message_budget", "time_budget"):
            self.budget_limited_ticks += 1
        if batch.last_decode_error:
            self.last_stream_error = batch.last_decode_error

        if batch.latest_frame is not None:
            self._render_frame(batch.latest_frame, self._clock_ns())
            self.total_rendered_frames += 1
        self._update_stream_status()

    def _render_frame(self, frame, now_ns):
        mask = frame.header.channel_mask
        if mask != self.current_mask:
            self.current_mask = mask
            self.rebuild_plots(mask)

        spec_mode = self.cb_spec_mode.currentIndex()
        for channel, waveform in frame.waveforms.items():
            curve = self.curves_wave.get(channel)
            if curve is not None:
                curve.setData(waveform)

            if self._active_polarity is None:
                continue
            result = analyze_monitor_waveform(
                waveform, self._active_polarity
            )
            value = result.charge if spec_mode == 0 else result.pulse_height
            if value > 0.0:
                self.q_long_hists[channel].append(value)

        if now_ns < self._next_histogram_refresh_ns:
            return
        self._next_histogram_refresh_ns = now_ns + HISTOGRAM_REFRESH_NS
        for channel, curve in self.curves_qlong.items():
            hist_data = self.q_long_hists[channel]
            if len(hist_data) <= 5:
                continue
            y, x_edges = np.histogram(hist_data, bins=150)
            y = np.where(y == 0, 0.1, y)
            curve.setData(x=x_edges, y=y)

    def cleanup(self):
        if self._cleaned:
            return
        self._cleaned = True
        if self.timer.isActive():
            self.timer.stop()

        sock = self.sock
        self.sock = None
        if sock is not None:
            try:
                sock.close(linger=0)
            except TypeError:
                # Lightweight injected test sockets may expose close() only.
                sock.close()
            except zmq.ZMQError:
                pass

        context = self.ctx
        self.ctx = None
        if self._owns_zmq_context and context is not None:
            try:
                context.term()
            except zmq.ZMQError:
                pass
