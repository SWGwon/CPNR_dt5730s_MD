import sys
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
GUI_DIR = PROJECT_ROOT / "gui"
if str(GUI_DIR) not in sys.path:
    sys.path.insert(0, str(GUI_DIR))

from core.process_output import (  # noqa: E402
    parse_drop_count,
    parse_led_status,
    parse_live_daq_stats,
    parse_temperature,
)


class ProcessOutputTests(unittest.TestCase):
    def test_mixed_live_temperature_and_led_line_is_fully_parsed(self):
        line = (
            "[LIVE DAQ] Time: 00:03 | RealTime: 2.50 s | Live: 2.25 s | "
            "DT: 10.0000 % | Rate: 42.5 Hz | Events: 123 | "
            "Speed: 1.50 MB/s | Drops: 0"
            "[STATUS] TEMP: 31.0"
            "[STATUS] LED: LOCK=1, BYPS=0, RUN=1, TRG=1, DRDY=1, BUSY=0"
        )

        self.assertEqual(
            parse_live_daq_stats(line),
            {
                "live_time": "2.25 s",
                "dead_time": "10.0000 %",
                "events": "123",
                "rate": "42.5 Hz",
                "speed": "1.50 MB/s",
                "drops": "0",
            },
        )
        self.assertEqual(parse_temperature(line), 31.0)
        self.assertEqual(
            parse_led_status(line),
            {
                "PLL LOCK": 1,
                "PLL BYPS": 0,
                "RUN": 1,
                "TRG": 1,
                "DRDY": 1,
                "BUSY": 0,
            },
        )

    def test_live_parser_ignores_malformed_or_unrelated_fields(self):
        self.assertEqual(parse_live_daq_stats("Live: 1.0 s"), {})
        self.assertEqual(
            parse_live_daq_stats("[LIVE DAQ] Live: broken | Events: 7"),
            {"events": "7"},
        )
        self.assertEqual(parse_live_daq_stats("[LIVE DAQ] unavailable"), {})
        self.assertIsNone(parse_temperature("[STATUS] TEMP: unavailable"))
        self.assertIsNone(parse_led_status("[STATUS] LED: LOCK=1"))

    def test_drop_count_defensively_extracts_first_integer(self):
        cases = {
            "0[STATUS] LED: LOCK=1": 0,
            "12 trailing text": 12,
            7: 7,
            "unavailable": 0,
            "garbage[STATUS] LED: LOCK=1": 0,
            None: 0,
        }
        for value, expected in cases.items():
            with self.subTest(value=value):
                self.assertEqual(parse_drop_count(value), expected)


if __name__ == "__main__":
    unittest.main()
