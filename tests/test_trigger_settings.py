import sys
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
GUI_DIR = PROJECT_ROOT / "gui"
if str(GUI_DIR) not in sys.path:
    sys.path.insert(0, str(GUI_DIR))

from core.trigger_settings import (  # noqa: E402
    calculate_threshold_preview,
    millivolts_to_adc_delta,
)


class TriggerSettingsTests(unittest.TestCase):
    def test_one_mv_at_two_vpp_is_eight_adc(self):
        self.assertEqual(millivolts_to_adc_delta(1.0, 2000, 14), 8)

    def test_one_mv_at_half_vpp_is_thirty_three_adc(self):
        self.assertEqual(millivolts_to_adc_delta(1.0, 500, 14), 33)

    def test_falling_preview_subtracts_from_channel_baseline(self):
        ch0 = calculate_threshold_preview(16163.0, 1.0, 2000, 14, 1)
        ch1 = calculate_threshold_preview(16255.0, 1.0, 2000, 14, 1)
        self.assertEqual(ch0.absolute_threshold_adc, 16155)
        self.assertEqual(ch1.absolute_threshold_adc, 16247)
        self.assertNotEqual(ch0.absolute_threshold_adc,
                            ch1.absolute_threshold_adc)

    def test_rising_preview_adds_to_baseline(self):
        preview = calculate_threshold_preview(8192.0, 1.0, 2000, 14, 0)
        self.assertEqual(preview.absolute_threshold_adc, 8200)

    def test_effective_voltage_retains_fractional_baseline(self):
        preview = calculate_threshold_preview(16163.5, 1.0, 2000, 14, 1)
        lsb_mv = 2000.0 / (1 << 14)
        self.assertAlmostEqual(
            preview.effective_threshold_mv,
            abs(16163.5 - preview.absolute_threshold_adc) * lsb_mv,
        )

    def test_invalid_range_bits_polarity_and_values_are_rejected(self):
        invalid_calls = [
            lambda: millivolts_to_adc_delta(1.0, 1000, 14),
            lambda: millivolts_to_adc_delta(1.0, 2000, 12),
            lambda: millivolts_to_adc_delta(0.0, 2000, 14),
            lambda: millivolts_to_adc_delta(float("nan"), 2000, 14),
            lambda: millivolts_to_adc_delta(0.001, 2000, 14),
            lambda: calculate_threshold_preview(8192, 1.0, 2000, 14, 2),
            lambda: calculate_threshold_preview(3, 1.0, 2000, 14, 1),
            lambda: calculate_threshold_preview(16380, 1.0, 2000, 14, 0),
        ]
        for call in invalid_calls:
            with self.subTest(call=call):
                with self.assertRaises(ValueError):
                    call()


if __name__ == "__main__":
    unittest.main()
