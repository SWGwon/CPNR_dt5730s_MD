import hashlib
import os
import struct
import sys
import tempfile
import time
import unittest
from pathlib import Path

import numpy as np


PROJECT_ROOT = Path(__file__).resolve().parents[1]
GUI_DIR = PROJECT_ROOT / "gui"
if str(GUI_DIR) not in sys.path:
    sys.path.insert(0, str(GUI_DIR))

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from core.monitor_stream import (
    ADC_MAX_CODE,
    EVENT_HEADER_BYTES,
    EVENT_HEADER_FORMAT,
    MAX_MONITOR_FRAME_BYTES,
    EventSequenceTracker,
    MonitorConfigError,
    MonitorFrameError,
    RuntimeConfigReference,
    analyze_monitor_waveform,
    decode_monitor_frame,
    drain_latest_frames,
    load_runtime_polarity,
)

try:
    import zmq
    from PyQt6.QtCore import QTimer
    from PyQt6.QtWidgets import QApplication
except ImportError:  # pragma: no cover - optional outside GUI test runtime
    zmq = None
    QTimer = None
    QApplication = None


def make_frame(
    event_id=0,
    *,
    record_length=128,
    channel_mask=0x03,
    waveforms=None,
    board_event_counter=None,
):
    active = [channel for channel in range(8) if channel_mask & (1 << channel)]
    if waveforms is None:
        waveforms = {
            channel: [1000 + channel] * record_length for channel in active
        }
    payload = b"".join(
        struct.pack(f"<{record_length}H", *waveforms[channel])
        for channel in active
    )
    if board_event_counter is None:
        board_event_counter = event_id & 0xFFFFFF
    return struct.pack(
        EVENT_HEADER_FORMAT,
        123456789,
        event_id,
        record_length,
        channel_mask,
        0x1234,
        board_event_counter,
    ) + payload


class MonitorStreamPureTests(unittest.TestCase):
    def test_strict_little_endian_decoder_returns_exact_immutable_channels(self):
        ch0 = [1000] * 128
        ch3 = [2000] * 128
        ch0[42] = 900
        ch3[64] = ADC_MAX_CODE
        frame = make_frame(
            17,
            channel_mask=0x09,
            waveforms={0: ch0, 3: ch3},
            board_event_counter=0xABCDE,
        )

        decoded = decode_monitor_frame(frame)
        self.assertEqual(EVENT_HEADER_BYTES, 24)
        self.assertEqual(decoded.frame_bytes, 24 + 2 * 128 * 2)
        self.assertEqual(decoded.header.event_id, 17)
        self.assertEqual(decoded.header.channel_mask, 0x09)
        self.assertEqual(decoded.header.board_event_counter, 0xABCDE)
        self.assertEqual(tuple(decoded.waveforms), (0, 3))
        self.assertEqual(int(decoded.waveforms[0][42]), 900)
        self.assertEqual(int(decoded.waveforms[3][64]), ADC_MAX_CODE)
        self.assertFalse(decoded.waveforms[0].flags.writeable)
        with self.assertRaises(ValueError):
            decoded.waveforms[0][0] = 1

        # The explicit '<' contract must decode known byte order independently
        # of the host's native struct alignment/endian settings.
        self.assertEqual(frame[8:12], b"\x11\x00\x00\x00")

    def test_decoder_rejects_truncated_oversize_shape_mask_and_adc_corruption(self):
        valid = make_frame()
        cases = (
            (valid[: EVENT_HEADER_BYTES - 1], "truncated EventHeader"),
            (valid[:-1], "truncated waveform payload"),
            (valid + b"\x00", "oversize waveform payload"),
            (b"\x00" * (MAX_MONITOR_FRAME_BYTES + 1), "oversize monitor frame"),
            (make_frame(channel_mask=0), "at least one channel"),
            (make_frame(channel_mask=0x100), "unsupported bits"),
            (make_frame(record_length=120), "outside"),
            (make_frame(record_length=130), "multiple of 8"),
            (
                make_frame(board_event_counter=0x1000000),
                "24-bit range",
            ),
        )
        for frame, expected in cases:
            with self.subTest(expected=expected):
                with self.assertRaisesRegex(MonitorFrameError, expected):
                    decode_monitor_frame(frame)

        bad_samples = [1000] * 128
        bad_samples[91] = ADC_MAX_CODE + 1
        with self.assertRaisesRegex(MonitorFrameError, "14-bit ADC range"):
            decode_monitor_frame(
                make_frame(
                    channel_mask=1,
                    waveforms={0: bad_samples},
                )
            )
        with self.assertRaisesRegex(MonitorFrameError, "immutable bytes"):
            decode_monitor_frame(bytearray(valid))

    def test_sequence_gaps_are_subscriber_observations_with_wrap_and_resets(self):
        tracker = EventSequenceTracker()
        self.assertEqual(tracker.observe(10).classification, "first")
        self.assertEqual(tracker.observe(11).classification, "consecutive")
        gap = tracker.observe(15)
        self.assertEqual(gap.classification, "gap")
        self.assertEqual(gap.observed_subscriber_gap, 3)
        self.assertEqual(tracker.observed_subscriber_gaps, 3)
        self.assertEqual(tracker.observe(15).classification, "duplicate")
        self.assertEqual(tracker.observe(3).classification, "discontinuity")
        self.assertEqual(tracker.observed_subscriber_gaps, 3)
        self.assertEqual(tracker.duplicate_event_ids, 1)
        self.assertEqual(tracker.sequence_discontinuities, 1)

        tracker.reset()
        tracker.observe(0xFFFFFFFF)
        wrapped = tracker.observe(0)
        self.assertEqual(wrapped.classification, "consecutive")
        self.assertEqual(tracker.observed_subscriber_gaps, 0)

        tracker.observe(9)
        old_gap_total = tracker.observed_subscriber_gaps
        tracker.start_new_observation_window()
        self.assertEqual(tracker.observe(1000).classification, "first")
        self.assertEqual(tracker.observed_subscriber_gaps, old_gap_total)

    def test_bounded_drain_uses_message_and_time_budgets_and_latest_decimation(self):
        class InfiniteReceiver:
            def __init__(self):
                self.calls = 0

            def __call__(self):
                event_id = self.calls
                self.calls += 1
                return make_frame(event_id, channel_mask=1)

        receiver = InfiniteReceiver()
        tracker = EventSequenceTracker()
        batch = drain_latest_frames(
            receiver,
            tracker,
            max_messages=7,
            time_budget_ns=1_000_000_000,
            clock_ns=time.monotonic_ns,
        )
        self.assertEqual(receiver.calls, 7)
        self.assertEqual(batch.stop_reason, "message_budget")
        self.assertEqual(batch.received_frames, 7)
        self.assertEqual(batch.valid_frames, 7)
        self.assertEqual(batch.decimated_valid_frames, 6)
        self.assertEqual(batch.latest_frame.header.event_id, 6)
        self.assertEqual(tracker.observed_subscriber_gaps, 0)

        class StepClock:
            def __init__(self):
                self.value = 0

            def __call__(self):
                self.value += 1_000_000
                return self.value

        receiver = InfiniteReceiver()
        clock = StepClock()
        timed = drain_latest_frames(
            receiver,
            EventSequenceTracker(),
            max_messages=100,
            time_budget_ns=2_000_000,
            clock_ns=clock,
        )
        self.assertEqual(timed.stop_reason, "time_budget")
        self.assertLess(receiver.calls, 100)
        self.assertGreaterEqual(receiver.calls, 1)

    def test_drain_rejects_bad_frame_but_keeps_latest_trusted_frame(self):
        messages = iter((b"bad", make_frame(4, channel_mask=1)))

        def receive():
            try:
                return next(messages)
            except StopIteration as exc:
                raise BlockingIOError from exc

        batch = drain_latest_frames(
            receive,
            EventSequenceTracker(),
            max_messages=10,
            time_budget_ns=1_000_000_000,
            clock_ns=time.monotonic_ns,
        )
        self.assertEqual(batch.stop_reason, "empty")
        self.assertEqual(batch.received_frames, 2)
        self.assertEqual(batch.valid_frames, 1)
        self.assertEqual(batch.malformed_frames, 1)
        self.assertIn("truncated EventHeader", batch.last_decode_error)
        self.assertEqual(batch.latest_frame.header.event_id, 4)

    def test_runtime_config_polarity_is_strict_and_dsp_handles_both_directions(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "run.config.conf"
            path.write_text(
                "[Digitizer]\nRecordLength = 128\nTriggerPolarity = 0\n",
                encoding="utf-8",
            )
            rising = load_runtime_polarity(path)
            self.assertEqual(rising.polarity, "rising")
            self.assertEqual(rising.source_path, str(path.resolve()))
            expected_digest = hashlib.sha256(path.read_bytes()).hexdigest()
            authenticated = load_runtime_polarity(
                path, expected_sha256=expected_digest
            )
            self.assertTrue(authenticated.authenticated)
            self.assertEqual(authenticated.sha256, expected_digest)
            with self.assertRaisesRegex(MonitorConfigError, "does not match"):
                load_runtime_polarity(
                    path, expected_sha256="0" * 64
                )

            path.write_text(
                "[Digitizer]\nTriggerPolarity = 1\n", encoding="utf-8"
            )
            self.assertEqual(load_runtime_polarity(path).polarity, "falling")

            path.write_text(
                "[Digitizer]\nTriggerPolarity = falling\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(MonitorConfigError, "exactly 0 or 1"):
                load_runtime_polarity(path)

        falling_waveform = np.full(128, 1000, dtype=np.uint16)
        falling_waveform[64:72] = 900
        falling = analyze_monitor_waveform(falling_waveform, "falling")
        self.assertEqual(falling.baseline, 1000.0)
        self.assertEqual(falling.pulse_height, 100.0)
        self.assertEqual(falling.charge, 800.0)
        self.assertEqual(falling.pulse_index, 64)

        rising_waveform = np.full(128, 1000, dtype=np.uint16)
        rising_waveform[64:72] = 1100
        rising = analyze_monitor_waveform(rising_waveform, "rising")
        self.assertEqual(rising.baseline, 1000.0)
        self.assertEqual(rising.pulse_height, 100.0)
        self.assertEqual(rising.charge, 800.0)
        self.assertEqual(rising.pulse_index, 64)


@unittest.skipIf(
    QApplication is None or zmq is None,
    "PyQt6/pyzmq are not installed",
)
class MonitorTabQtSafetyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def test_real_subscriber_socket_has_bounded_queue_and_message_size(self):
        from widgets.MonitorTab import (
            MONITOR_RCVHWM_MESSAGES,
            MonitorTab,
        )

        tab = MonitorTab()
        tab.timer.stop()
        self.assertEqual(
            tab.sock.getsockopt(zmq.RCVHWM), MONITOR_RCVHWM_MESSAGES
        )
        self.assertEqual(
            tab.sock.getsockopt(zmq.MAXMSGSIZE), MAX_MONITOR_FRAME_BYTES
        )
        if hasattr(zmq, "CONFLATE"):
            self.assertEqual(tab.sock.getsockopt(zmq.CONFLATE), 1)
        tab.cleanup()
        tab.deleteLater()

    def test_sustained_flood_is_bounded_and_renders_only_latest_per_tick(self):
        from widgets.MonitorTab import MonitorTab

        class FloodSocket:
            def __init__(self):
                self.recv_calls = 0
                self.closed = False

            def recv(self, *, flags):
                self.assert_nonblocking(flags)
                event_id = self.recv_calls
                self.recv_calls += 1
                return make_frame(event_id, channel_mask=1)

            @staticmethod
            def assert_nonblocking(flags):
                if not flags & zmq.NOBLOCK:
                    raise AssertionError("monitor recv must be nonblocking")

            def close(self, linger=0):
                self.closed = True

        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "active.config.conf"
            config.write_text(
                "[Digitizer]\nTriggerPolarity = 1\n", encoding="utf-8"
            )
            socket = FloodSocket()
            tab = MonitorTab(
                monitor_socket=socket,
                config_path_provider=lambda: config,
                poll_message_budget=5,
                poll_time_budget_ms=50.0,
            )
            tab.timer.stop()
            rendered_ids = []
            tab._render_frame = (
                lambda frame, _now: rendered_ids.append(frame.header.event_id)
            )

            heartbeats = 0
            for _ in range(100):
                fired = []
                QTimer.singleShot(0, lambda: fired.append(True))
                tab.poll_zmq()
                self.app.processEvents()
                heartbeats += bool(fired)

            self.assertEqual(socket.recv_calls, 500)
            self.assertEqual(len(rendered_ids), 100)
            self.assertEqual(rendered_ids[-1], 499)
            self.assertEqual(tab.total_received_frames, 500)
            self.assertEqual(tab.total_rendered_frames, 100)
            self.assertEqual(tab.total_decimated_frames, 400)
            self.assertEqual(tab.budget_limited_ticks, 100)
            self.assertEqual(heartbeats, 100)
            self.assertIn(
                "Subscriber-observed EventID gaps (not DAQ loss)",
                tab.lbl_stream_stats.text(),
            )
            tab.cleanup()
            tab.cleanup()
            self.assertTrue(socket.closed)
            tab.deleteLater()

    def test_qt_monitor_tracks_exact_runtime_polarity_without_mixing_runs(self):
        from widgets.MonitorTab import MonitorTab

        rising_waveform = [1000] * 128
        rising_waveform[64:72] = [1100] * 8
        messages = [
            make_frame(
                0,
                channel_mask=1,
                waveforms={0: rising_waveform},
            )
        ]

        class QueueSocket:
            def recv(self, *, flags):
                if messages:
                    return messages.pop(0)
                raise zmq.Again()

            def close(self, linger=0):
                pass

        with tempfile.TemporaryDirectory() as directory:
            rising_config = Path(directory) / "rising.config.conf"
            rising_config.write_text(
                "[Digitizer]\nTriggerPolarity = 0\n", encoding="utf-8"
            )
            falling_config = Path(directory) / "falling.config.conf"
            falling_config.write_text(
                "[Digitizer]\nTriggerPolarity = 1\n", encoding="utf-8"
            )
            active_config = [
                RuntimeConfigReference(
                    rising_config,
                    hashlib.sha256(rising_config.read_bytes()).hexdigest(),
                )
            ]
            tab = MonitorTab(
                monitor_socket=QueueSocket(),
                config_path_provider=lambda: active_config[0],
            )
            tab.timer.stop()
            tab.cb_spec_mode.setCurrentIndex(1)
            tab.poll_zmq()
            self.assertEqual(tab._active_polarity, "rising")
            self.assertTrue(tab._polarity_authenticated)
            self.assertIn("rising", tab.lbl_polarity.text())
            self.assertEqual(list(tab.q_long_hists[0]), [100.0])

            falling_waveform = [1000] * 128
            falling_waveform[64:72] = [900] * 8
            messages.append(
                make_frame(
                    0,
                    channel_mask=1,
                    waveforms={0: falling_waveform},
                )
            )
            active_config[0] = RuntimeConfigReference(
                falling_config,
                hashlib.sha256(falling_config.read_bytes()).hexdigest(),
            )
            # The provider path is checked on every tick, even though the
            # periodic same-file refresh deadline is still in the future.
            tab.poll_zmq()
            self.assertEqual(tab._active_polarity, "falling")
            self.assertIn("falling", tab.lbl_polarity.text())
            self.assertEqual(list(tab.q_long_hists[0]), [100.0])
            self.assertEqual(
                tab.sequence_tracker.sequence_discontinuities, 0
            )
            tab.cleanup()
            tab.deleteLater()


if __name__ == "__main__":
    unittest.main(verbosity=2)
