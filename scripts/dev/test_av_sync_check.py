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


def events(*times: float, interval: float = 0.0) -> list:
    return [AV_SYNC.Event(time, interval) for time in times]


def markers(*pairs: tuple[float, float], flash_sigma: float = 0.0, beep_sigma: float = 0.0) -> list:
    """MarkerPairs from (flash_pts, beep_pts) tuples, with stated edge uncertainties."""
    return [
        AV_SYNC.MarkerPair(flash, beep, flash_sigma, beep_sigma)
        for flash, beep in pairs
    ]


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
        # Auto mode has no schedule to check against, so the only thing separating
        # four real markers from three plus a disturbance is the expected count.
        result = AV_SYNC.analyze_event_pairs(
            events(10.030, 30.020, 60.050, 110.040),
            events(10.000, 30.000, 60.000, 110.000),
        )
        self.assertFalse(result["measurable"])
        self.assertIn("auto mode", result["error"])
        self.assertIn("--expected-markers", result["error"])

    def test_a_longer_schedule_is_selected_when_it_is_declared(self) -> None:
        result = AV_SYNC.analyze_event_pairs(
            events(10.02, 35.02, 60.02, 85.02, 110.02, interval=0.002),
            events(10.00, 35.00, 60.00, 85.00, 110.00, interval=0.002),
            expected_markers=5,
        )
        self.assertTrue(result["measurable"], result.get("error"))
        self.assertEqual(result["marker_count"], 5)

    def test_the_evenly_spaced_candidate_is_preferred_over_a_disturbance(self) -> None:
        # Six pairs, five of them on a regular 25 s schedule. The odd one out is a
        # disturbance, and the set whose intervals vary least is the schedule.
        result = AV_SYNC.analyze_event_pairs(
            events(10.02, 22.02, 35.02, 60.02, 85.02, 110.02, interval=0.002),
            events(10.00, 22.00, 35.00, 60.00, 85.00, 110.00, interval=0.002),
            expected_markers=5,
        )
        self.assertTrue(result["measurable"], result.get("error"))
        self.assertEqual(
            [round(value, 2) for value in result["recognized_beep_pts"]],
            [10.0, 35.0, 60.0, 85.0, 110.0])

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


class EdgeLocationTest(unittest.TestCase):
    def test_the_crossing_is_interpolated_not_rounded_up_to_a_sample(self) -> None:
        # Taking the first sample above threshold puts every edge systematically
        # half a sampling gap late.
        series = AV_SYNC.Series(
            times=[0.0, 0.04, 0.08, 0.12],
            values=[0.0, 0.0, 1.0, 1.0],
        )
        found = AV_SYNC.detect_events(series, 0.5)
        self.assertEqual(len(found), 1)
        self.assertAlmostEqual(found[0].start_pts, 0.06, places=9)

    def test_a_noiseless_step_is_worth_the_grid_quantisation(self) -> None:
        # A step that happens entirely inside one gap carries no sub-sample
        # information. What is left is the grid itself.
        series = AV_SYNC.Series(
            times=[0.0, 0.04, 0.08, 0.12],
            values=[0.0, 0.0, 1.0, 1.0],
        )
        found = AV_SYNC.detect_events(series, 0.5)
        self.assertAlmostEqual(found[0].uncertainty_s, 0.04 / (12 ** 0.5) / 2, places=9)

    def test_an_edge_after_a_gap_is_located_less_precisely(self) -> None:
        # A marker that lands right after a dropped frame is genuinely less well
        # located, and the verdict has to know that rather than assume the
        # nominal frame interval everywhere.
        clean = AV_SYNC.detect_events(
            AV_SYNC.Series(times=[0.0, 0.04, 0.08, 0.12], values=[0.0, 0.0, 1.0, 1.0]), 0.5)
        after_gap = AV_SYNC.detect_events(
            AV_SYNC.Series(times=[0.0, 0.04, 0.28, 0.32], values=[0.0, 0.0, 1.0, 1.0]), 0.5)
        self.assertGreater(after_gap[0].uncertainty_s, clean[0].uncertainty_s)

    def test_a_noisy_baseline_widens_the_edge(self) -> None:
        # Jitter is noise over slope, floored by the grid. Driven directly, because
        # picking series values that produce a chosen baseline noise tests the
        # arithmetic of the fixture rather than the rule.
        _, quiet = AV_SYNC._interpolate_crossing(0.0, 0.0, 0.04, 1.0, 0.5, 0.04, noise=0.0)
        _, noisy = AV_SYNC._interpolate_crossing(0.0, 0.0, 0.04, 1.0, 0.5, 0.04, noise=0.4)
        self.assertGreater(noisy, quiet)

    def test_the_edge_uncertainty_is_capped_at_an_uninterpolated_crossing(self) -> None:
        # However bad the noise, interpolating cannot be worse than not having
        # interpolated at all.
        _, wild = AV_SYNC._interpolate_crossing(0.0, 0.0, 0.04, 1.0, 0.5, 0.04, noise=100.0)
        self.assertAlmostEqual(wild, 0.02, places=9)

    def test_baseline_noise_measures_the_quiet_samples(self) -> None:
        series = AV_SYNC.Series(
            times=[0.0, 0.04, 0.08, 0.12],
            values=[0.0, 0.2, 0.0, 1.0],
        )
        # Only the three samples below threshold: sd of (0, 0.2, 0).
        self.assertAlmostEqual(AV_SYNC.baseline_noise(series, 0.5), 0.11547005, places=6)


class FitDriftTest(unittest.TestCase):
    def test_a_linear_drift_is_recovered(self) -> None:
        # Offsets of 0, 10 and 20 ms at 0, 50 and 100 s: 0.2 ms/s.
        fitted = AV_SYNC.fit_drift(markers(
            (10.000, 10.000), (60.010, 60.000), (110.020, 110.000),
            flash_sigma=0.008, beep_sigma=0.005))
        self.assertTrue(fitted["fitted"])
        self.assertAlmostEqual(fitted["slope_s_per_s"], 0.0002, places=6)
        self.assertLess(fitted["max_abs_residual_s"], 1e-9)

    def test_a_less_certain_marker_pulls_the_fit_less(self) -> None:
        clean = AV_SYNC.fit_drift([
            AV_SYNC.MarkerPair(10.000, 10.000, 0.001, 0.001),
            AV_SYNC.MarkerPair(60.030, 60.000, 0.001, 0.001),
            AV_SYNC.MarkerPair(110.020, 110.000, 0.001, 0.001),
        ])
        downweighted = AV_SYNC.fit_drift([
            AV_SYNC.MarkerPair(10.000, 10.000, 0.001, 0.001),
            AV_SYNC.MarkerPair(60.030, 60.000, 0.100, 0.100),
            AV_SYNC.MarkerPair(110.020, 110.000, 0.001, 0.001),
        ])
        # The middle marker is off the line. Trusting it less moves the line
        # towards the two that are not, which LEAVES the outlier further out.
        self.assertGreater(
            abs(downweighted["residuals_s"][1]), abs(clean["residuals_s"][1]))
        self.assertNotAlmostEqual(
            clean["slope_s_per_s"], downweighted["slope_s_per_s"], places=9)

    def test_two_markers_are_fitted_but_leave_no_residual(self) -> None:
        fitted = AV_SYNC.fit_drift(markers(
            (10.000, 10.000), (110.020, 110.000), flash_sigma=0.008, beep_sigma=0.005))
        self.assertTrue(fitted["fitted"])
        self.assertLess(fitted["max_abs_residual_s"], 1e-12)


class QualifyReferenceTest(unittest.TestCase):
    def test_three_frame_accurate_markers_cannot_carry_a_20_ms_verdict(self) -> None:
        # The measured finding this qualification exists for. Three markers whose
        # edges are located to one video frame and one astats window give a total
        # drift uncertain to about +/-13 ms, so "8 ms, within a 20 ms budget" and
        # "we cannot tell" are the same measurement. The endpoint difference this
        # replaces reported the first of those.
        selected = markers(
            (10.000, 10.000), (60.010, 60.000), (110.020, 110.000),
            flash_sigma=0.008, beep_sigma=0.005)
        verdict = AV_SYNC.qualify_reference(
            selected, AV_SYNC.fit_drift(selected), span_s=100.0, budget_ms=20.0)
        self.assertFalse(verdict["qualified"])
        self.assertTrue(any("uncertain" in reason for reason in verdict["reasons"]))
        self.assertTrue(any("markers would reach it" in reason for reason in verdict["reasons"]))

    def test_enough_markers_at_the_same_edge_precision_do_qualify(self) -> None:
        # The remedy the verdict names, carried out: the same edges, more markers.
        needed = AV_SYNC.required_marker_count((0.008 ** 2 + 0.005 ** 2) ** 0.5, 20.0 / 3.0)
        self.assertIsNotNone(needed)
        selected = [
            AV_SYNC.MarkerPair(10.0 + index * 10.0, 10.0 + index * 10.0, 0.008, 0.005)
            for index in range(needed)
        ]
        span = selected[-1].event_pts - selected[0].event_pts
        verdict = AV_SYNC.qualify_reference(
            selected, AV_SYNC.fit_drift(selected), span_s=span, budget_ms=20.0)
        self.assertTrue(verdict["qualified"], verdict["reasons"])

    def test_sharper_edges_qualify_three_markers(self) -> None:
        selected = markers(
            (10.000, 10.000), (60.010, 60.000), (110.020, 110.000),
            flash_sigma=0.002, beep_sigma=0.001)
        verdict = AV_SYNC.qualify_reference(
            selected, AV_SYNC.fit_drift(selected), span_s=100.0, budget_ms=20.0)
        self.assertTrue(verdict["qualified"], verdict["reasons"])

    def test_two_markers_do_not_qualify(self) -> None:
        selected = markers(
            (10.000, 10.000), (110.020, 110.000), flash_sigma=0.008, beep_sigma=0.005)
        verdict = AV_SYNC.qualify_reference(
            selected, AV_SYNC.fit_drift(selected), span_s=100.0, budget_ms=20.0)
        self.assertFalse(verdict["qualified"])
        self.assertTrue(any("nonlinearity" in reason for reason in verdict["reasons"]))

    def test_a_budget_below_the_measurement_precision_does_not_qualify(self) -> None:
        # The same run that carries a 20 ms verdict cannot carry a 1 ms one.
        selected = markers(
            (10.000, 10.000), (60.010, 60.000), (110.020, 110.000),
            flash_sigma=0.002, beep_sigma=0.001)
        fitted = AV_SYNC.fit_drift(selected)
        self.assertTrue(
            AV_SYNC.qualify_reference(selected, fitted, 100.0, 20.0)["qualified"])
        tight = AV_SYNC.qualify_reference(selected, fitted, 100.0, 1.0)
        self.assertFalse(tight["qualified"])
        self.assertTrue(any("uncertain" in reason for reason in tight["reasons"]))

    def test_a_residual_is_judged_against_that_markers_own_edges(self) -> None:
        # The markers do not carry equal uncertainty. Judging every residual
        # against the median of all of them both accuses the well-measured markers
        # and excuses the poorly measured ones.
        selected = [
            AV_SYNC.MarkerPair(10.000, 10.000, 0.0005, 0.0005),
            AV_SYNC.MarkerPair(60.012, 60.000, 0.0300, 0.0300),
            AV_SYNC.MarkerPair(110.000, 110.000, 0.0005, 0.0005),
        ]
        fitted = AV_SYNC.fit_drift(selected)
        verdict = AV_SYNC.qualify_reference(selected, fitted, 100.0, 60.0)
        # The middle marker is far off the line, and its own edges say that is
        # exactly what a marker located that poorly does.
        self.assertTrue(verdict["qualified"], verdict["reasons"])

    def test_a_nonlinear_run_does_not_qualify(self) -> None:
        # Offsets +0, +60, +0 ms: the endpoints agree perfectly and the middle does
        # not. An endpoint difference reports zero drift for exactly this run.
        selected = markers(
            (10.000, 10.000), (60.060, 60.000), (110.000, 110.000),
            flash_sigma=0.008, beep_sigma=0.005)
        fitted = AV_SYNC.fit_drift(selected)
        verdict = AV_SYNC.qualify_reference(selected, fitted, 100.0, 20.0)
        self.assertFalse(verdict["qualified"])
        self.assertTrue(any("not linear" in reason for reason in verdict["reasons"]))

    def test_markers_without_stated_uncertainty_do_not_qualify(self) -> None:
        # An unmeasured edge is not an exact one. Treating a missing uncertainty as
        # zero would make every run qualify on arithmetic alone.
        selected = markers((10.0, 10.0), (60.01, 60.0), (110.02, 110.0))
        verdict = AV_SYNC.qualify_reference(
            selected, AV_SYNC.fit_drift(selected), 100.0, 20.0)
        self.assertFalse(verdict["qualified"])
        self.assertTrue(any("coincidence" in reason for reason in verdict["reasons"]))


class FittedVerdictTest(unittest.TestCase):
    def test_the_fit_and_the_endpoint_difference_disagree_on_a_nonlinear_run(self) -> None:
        # This is why the endpoint difference is not the verdict: it reports zero
        # drift for a run whose offset moved 60 ms and came back.
        result = AV_SYNC.analyze_event_pairs(
            events(10.000, 60.060, 110.000, interval=0.016),
            events(10.000, 60.000, 110.000, interval=0.010),
            expected_markers=3,
        )
        self.assertTrue(result["measurable"])
        self.assertAlmostEqual(result["drift_start_end_ms"], 0.0, places=6)
        self.assertFalse(result["reference"]["qualified"])

    def test_a_clean_run_carries_the_fitted_drift(self) -> None:
        result = AV_SYNC.analyze_event_pairs(
            events(10.000, 60.010, 110.020, interval=0.002),
            events(10.000, 60.000, 110.000, interval=0.002),
            expected_markers=3,
        )
        self.assertTrue(result["reference"]["qualified"], result["reference"]["reasons"])
        self.assertAlmostEqual(result["fitted_drift_ms"], 20.0, places=2)
        self.assertGreater(result["fitted_drift_uncertainty_ms"], 0.0)

    def test_the_total_drift_uncertainty_does_not_shrink_with_a_longer_run(self) -> None:
        # A longer run determines the RATE better and is multiplied by a longer
        # span, so the total drift is known no better. A campaign that lengthens a
        # run to make a verdict possible is doing nothing; the verdict says so and
        # names the marker count instead.
        sigma = (0.008 ** 2 + 0.005 ** 2) ** 0.5
        short = [AV_SYNC.MarkerPair(t, t, 0.008, 0.005) for t in (0.0, 50.0, 100.0)]
        long_run = [AV_SYNC.MarkerPair(t, t, 0.008, 0.005) for t in (0.0, 500.0, 1000.0)]
        short_total = AV_SYNC.fit_drift(short)["slope_standard_error_s_per_s"] * 100.0
        long_total = AV_SYNC.fit_drift(long_run)["slope_standard_error_s_per_s"] * 1000.0
        self.assertAlmostEqual(short_total, long_total, places=9)
        self.assertAlmostEqual(short_total, sigma * (2 ** 0.5), places=9)


class VerdictGateTest(unittest.TestCase):
    """The exit code, not the analysis.

    Every rule below is already tested against the analysis functions. These
    exist because the analysis and the gate are two layers, and a correct
    analysis whose verdict the gate ignores looks exactly like a passing run --
    which is the failure mode that got past a first attempt twice in this work.
    """

    def run_main(self, measured: dict, argv: list[str]) -> tuple[int, str, str]:
        import contextlib
        import io as _io

        original_measure = AV_SYNC.measure
        original_argv = sys.argv
        AV_SYNC.measure = lambda *args, **kwargs: dict(measured)
        sys.argv = ["av-sync-check.py", *argv]
        out, err = _io.StringIO(), _io.StringIO()
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = AV_SYNC.main()
        finally:
            AV_SYNC.measure = original_measure
            sys.argv = original_argv
        return code, out.getvalue(), err.getvalue()

    @staticmethod
    def measured(qualified: bool, fitted_drift_ms: float) -> dict:
        return {
            "measurable": True,
            "marker_count": 3,
            "span_s": 100.0,
            "offset_start_ms": 30.0,
            "offset_middle_ms": 35.0,
            "offset_end_ms": 40.0,
            "drift_start_middle_ms": 5.0,
            "drift_middle_end_ms": 5.0,
            "drift_start_end_ms": 10.0,
            "drift_ms": 10.0,
            "drift_ms_per_hour": 360.0,
            "flash_event_count": 3,
            "beep_event_count": 3,
            "paired_event_count": 3,
            "recognized_flash_pts": [10.0, 60.0, 110.0],
            "recognized_beep_pts": [10.0, 60.0, 110.0],
            "fit": {"fitted": True},
            "fitted_drift_ms": fitted_drift_ms,
            "fitted_drift_ms_per_hour": fitted_drift_ms * 36.0,
            "fitted_drift_uncertainty_ms": 2.0,
            "max_residual_ms": 1.0,
            "reference": {
                "qualified": qualified,
                "reasons": [] if qualified else ["the fitted drift is uncertain to +/-13.34 ms"],
            },
        }

    def test_an_unqualified_reference_is_not_a_pass(self) -> None:
        code, _, err = self.run_main(self.measured(False, 4.0), ["recording.mkv"])
        self.assertEqual(code, 3)
        self.assertIn("does not qualify", err)

    def test_an_unqualified_reference_is_not_a_failure_either(self) -> None:
        # 3 is "could not measure". Reporting a drift the reference cannot carry
        # as a product failure is the same error in the other direction.
        code, _, _ = self.run_main(self.measured(False, 400.0), ["recording.mkv"])
        self.assertEqual(code, 3)

    def test_a_qualified_reference_within_budget_passes(self) -> None:
        code, out, _ = self.run_main(self.measured(True, 4.0), ["recording.mkv"])
        self.assertEqual(code, 0)
        self.assertIn("within budget", out)

    def test_a_qualified_reference_over_budget_fails(self) -> None:
        code, _, err = self.run_main(self.measured(True, 40.0), ["recording.mkv"])
        self.assertEqual(code, 2)
        self.assertIn("OVER BUDGET", err)

    def test_the_verdict_reads_the_fitted_drift_not_the_endpoint_difference(self) -> None:
        # The fixture's endpoint difference is 10 ms, inside the budget; its fitted
        # drift is 40 ms, outside it. A gate still reading the endpoint passes.
        code, _, _ = self.run_main(self.measured(True, 40.0), ["recording.mkv"])
        self.assertEqual(code, 2)

    def test_the_override_reports_a_verdict_without_a_qualified_reference(self) -> None:
        code, out, _ = self.run_main(
            self.measured(False, 4.0), ["recording.mkv", "--unqualified-reference"])
        self.assertEqual(code, 0)
        self.assertIn("NOT QUALIFIED", out)


if __name__ == "__main__":
    unittest.main()
