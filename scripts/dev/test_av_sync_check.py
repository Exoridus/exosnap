#!/ usr / bin / env python3
"""Focused unit tests for av-sync-check event pairing and marker selection."""

from __future__ import annotations

import importlib.util
import pathlib
import sys
import unittest


sys.dont_write_bytecode = True
MODULE_PATH = pathlib.Path(__file__).with_name("av-sync-check.py")
SPEC = importlib.util.spec_from_file_location("av_sync_check", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
AV_SYNC = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = AV_SYNC
SPEC.loader.exec_module(AV_SYNC)


def events(*times: float) -> list:
    return [AV_SYNC.Event(time) for time in times]


class AnalyzeEventPairsTest(unittest.TestCase):
    def test_two_markers(self) -> None:
        result = AV_SYNC.analyze_event_pairs(
            events(10.030, 110.042),
            events(10.000, 110.000),
            expected_markers=2,
        )
        self.assertTrue(result["measurable"])
        self.assertEqual(result["marker_count"], 2)
        self.assertAlmostEqual(result["drift_ms"], 12.0, places=6)

    def test_three_markers_reports_segment_and_total_drift(self) -> None:
        result = AV_SYNC.analyze_event_pairs(
            events(10.030, 60.050, 110.040),
            events(10.000, 60.000, 110.000),
            expected_markers=3,
        )
        self.assertTrue(result["measurable"])
        self.assertAlmostEqual(result["offset_middle_ms"], 50.0, places=6)
        self.assertAlmostEqual(result["drift_start_middle_ms"], 20.0, places=6)
        self.assertAlmostEqual(result["drift_middle_end_ms"], -10.0, places=6)
        self.assertAlmostEqual(result["drift_start_end_ms"], 10.0, places=6)

    def test_missing_marker_fails_closed(self) -> None:
        result = AV_SYNC.analyze_event_pairs(
            events(10.030, 110.040),
            events(10.000, 110.000),
            expected_markers=3,
        )
        self.assertFalse(result["measurable"])
        self.assertIn("need 3 paired markers", result["error"])

    def test_extra_disturbances_are_rejected_in_auto_mode(self) -> None:
        result = AV_SYNC.analyze_event_pairs(
            events(10.030, 30.020, 60.050, 110.040),
            events(10.000, 30.000, 60.000, 110.000),
        )
        self.assertFalse(result["measurable"])
        self.assertIn("auto mode", result["error"])

    def test_expected_schedule_selects_markers_around_extra_disturbances(self) -> None:
        result = AV_SYNC.analyze_event_pairs(
            events(10.030, 30.020, 60.050, 110.040),
            events(10.000, 30.000, 60.000, 110.000),
            expected_markers=3,
            expected_marker_times_s=[10.0, 60.0, 110.0],
        )
        self.assertTrue(result["measurable"])
        self.assertEqual(result["recognized_beep_pts"], [10.0, 60.0, 110.0])

    def test_wrong_schedule_order_fails_closed(self) -> None:
        result = AV_SYNC.analyze_event_pairs(
            events(10.030, 45.050, 110.040),
            events(10.000, 45.000, 110.000),
            expected_markers=3,
            expected_marker_times_s=[10.0, 60.0, 110.0],
            schedule_tolerance_s=2.0,
        )
        self.assertFalse(result["measurable"])
        self.assertIn("expected schedule", result["error"])


if __name__ == "__main__":
    unittest.main()
