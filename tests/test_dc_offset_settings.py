import sys
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
GUI_DIR = PROJECT_ROOT / "gui"
if str(GUI_DIR) not in sys.path:
    sys.path.insert(0, str(GUI_DIR))

from core.dc_offset_settings import (  # noqa: E402
    DC_OFFSET_MODE_RAW,
    DC_OFFSET_MODE_TARGET,
    calculate_dc_offset_preview,
    nominal_baseline_percent_from_dac,
    parse_dc_offset_settings,
)


class DCOffsetSettingsTests(unittest.TestCase):
    def test_ninety_percent_preview_uses_14_bit_adc_and_16_bit_dac(self):
        preview = calculate_dc_offset_preview(90.0, 2000)
        self.assertEqual(preview.target_adc, 14745)
        self.assertEqual(preview.nominal_dac, 6554)
        self.assertEqual(preview.falling_headroom_mv, 1800.0)
        self.assertAlmostEqual(preview.rising_headroom_mv, 200.0)

    def test_polarity_presets_have_expected_headroom(self):
        falling = calculate_dc_offset_preview(90.0, 500)
        centered = calculate_dc_offset_preview(50.0, 500)
        rising = calculate_dc_offset_preview(10.0, 500)
        self.assertEqual(falling.falling_headroom_mv, 450.0)
        self.assertAlmostEqual(falling.rising_headroom_mv, 50.0)
        self.assertEqual(centered.target_adc, 8192)
        self.assertEqual(centered.nominal_dac, 32768)
        self.assertEqual(rising.falling_headroom_mv, 50.0)
        self.assertEqual(rising.rising_headroom_mv, 450.0)

    def test_explicit_target_and_raw_modes_are_mutually_exclusive(self):
        target = parse_dc_offset_settings(
            DC_OFFSET_MODE_TARGET, "90", None
        )
        self.assertEqual(target.target_percent, 90.0)
        self.assertIsNone(target.raw_dac)

        raw = parse_dc_offset_settings(DC_OFFSET_MODE_RAW, None, "6554")
        self.assertEqual(raw.raw_dac, 6554)
        self.assertFalse(raw.legacy_raw)

        invalid = [
            (DC_OFFSET_MODE_TARGET, None, None),
            (DC_OFFSET_MODE_TARGET, "90", "6554"),
            (DC_OFFSET_MODE_RAW, "90", "6554"),
            (DC_OFFSET_MODE_RAW, None, None),
            ("target", "90", None),
        ]
        for values in invalid:
            with self.subTest(values=values):
                with self.assertRaises(ValueError):
                    parse_dc_offset_settings(*values)

    def test_legacy_raw_code_is_accepted_but_identified(self):
        settings = parse_dc_offset_settings(None, None, "6554")
        self.assertEqual(settings.mode, DC_OFFSET_MODE_RAW)
        self.assertEqual(settings.raw_dac, 6554)
        self.assertTrue(settings.legacy_raw)
        self.assertAlmostEqual(
            nominal_baseline_percent_from_dac(6554), 90.0, places=2
        )

    def test_target_and_raw_ranges_are_fail_closed(self):
        for value in ("4.999", "95.001", "nan", "inf", "not-a-number"):
            with self.subTest(target=value):
                with self.assertRaises(ValueError):
                    parse_dc_offset_settings(
                        DC_OFFSET_MODE_TARGET, value, None
                    )
        for value in ("-1", "65536", "1.5", "not-an-int"):
            with self.subTest(raw=value):
                with self.assertRaises(ValueError):
                    parse_dc_offset_settings(DC_OFFSET_MODE_RAW, None, value)


if __name__ == "__main__":
    unittest.main()
