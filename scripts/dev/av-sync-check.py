#!/usr/bin/env python3
"""av-sync-check.py — measure A/V clock drift of a recorded clapper file.

ExoSnap's `--clapper` capture emits a full-frame white FLASH and a loud BEEP at
two or three scheduled points. This script recovers, purely from the finished
file via system ffmpeg/ffprobe:

  * the video PTS of each flash (luma edge), and
  * the audio PTS of each beep (RMS edge).

From those it computes:

  offset_start = flash_start_pts - beep_start_pts
  offset_end   = flash_end_pts   - beep_end_pts
  drift        = offset_end - offset_start        (over the measured span)

Three-marker captures additionally report the middle offset and both segment
drifts. Flash/beep edges are paired by time instead of independently taking the
first and last event, so one-sided noise cannot silently become a marker.

WHAT THIS MEASURES — AND WHAT IT DOES NOT. The flash and the beep leave ExoSnap's
observation through *different, uncontrolled emission paths* (GPU present -> display
capture vs. WASAPI render -> SYS loopback capture), so `offset_start` carries a
device-dependent emission skew (~10-50 ms) that is NOT an ExoSnap A/V error. That
constant skew CANCELS in the drift (offset_end - offset_start). Therefore:

  * the DRIFT is pass/fail (this is the quantity `av-clock-slaving` cares about), and
  * the absolute OFFSET is reported ADVISORY only — never gated — unless a calibrated
    emission-skew subtraction is supplied.

THE DRIFT IS FITTED, NOT DIFFERENCED. An endpoint difference (offset_end - offset_start)
is the drift only if the drift is linear, and it cannot itself show whether it is: two
points always lie exactly on a line. Both edges also carry a real uncertainty -- a flash
is located to within one video frame, a beep to within one astats window -- and an
endpoint difference hides that a measured 8 ms may be +/- 20 ms. So all markers are fitted
by weighted least squares, and the fit's residuals are what says whether a straight line
describes the run at all.

A VERDICT NEEDS A QUALIFIED REFERENCE. Before any budget is applied, the clapper signal
has to be shown good enough to carry the judgement: at least three markers (two cannot
show nonlinearity), a slope uncertainty comfortably under the budget (otherwise "within
budget" is indistinguishable from noise), and residuals inside what the edge uncertainties
allow (otherwise the drift is not linear and no single rate describes it). An unqualified
reference produces "could not measure", never a pass -- the same rule the capture gates
follow: unavailable is not a pass.

Requires: system ffmpeg + ffprobe on PATH (a developer/CI dependency; the shipped
product bundles a mux-only FFmpeg without the signalstats/astats filters this uses).

Exit codes: 0 = drift within budget; 2 = drift over budget; 3 = could not measure.
"""

from __future__ import annotations

import argparse
import itertools
import json
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field


@dataclass
class Event:
    """A rising edge above threshold: the sample time where it first crossed."""

    start_pts: float
    # The edge lies somewhere between the last sample below threshold and this one,
    # so the sampling gap IS the localisation uncertainty -- one video frame for a
    # flash, one astats window for a beep. Measured from the series rather than
    # assumed from a declared frame rate, which a variable-frame-rate capture breaks.
    sample_interval_s: float = 0.0

    @property
    def uncertainty_s(self) -> float:
        """Half-width of the interval the true edge lies in."""
        return self.sample_interval_s / 2.0


@dataclass
class Series:
    times: list[float] = field(default_factory=list)
    values: list[float] = field(default_factory=list)


@dataclass(frozen=True)
class MarkerPair:
    flash_pts: float
    beep_pts: float
    flash_uncertainty_s: float = 0.0
    beep_uncertainty_s: float = 0.0

    @property
    def event_pts(self) -> float:
        return (self.flash_pts + self.beep_pts) / 2.0

    @property
    def offset_s(self) -> float:
        return self.flash_pts - self.beep_pts

    @property
    def offset_uncertainty_s(self) -> float:
        """Uncertainty of this marker's offset, from two independent edge locations."""
        return (self.flash_uncertainty_s ** 2 + self.beep_uncertainty_s ** 2) ** 0.5


def _require_tools() -> None:
    for tool in ("ffmpeg", "ffprobe"):
        if shutil.which(tool) is None:
            sys.stderr.write(
                f"av-sync-check: '{tool}' not found on PATH. Install a full system "
                f"ffmpeg (the app's bundled mux-only FFmpeg lacks the required filters).\n"
            )
            sys.exit(3)


_FRAME_RE = re.compile(r"pts_time:([-+0-9.eEnaif]+)")
_YAVG_RE = re.compile(r"lavfi\.signalstats\.YAVG=([-+0-9.eEnaif]+)")
_RMS_RE = re.compile(r"lavfi\.astats\.Overall\.RMS_level=([-+0-9.eEnaif]+)")


def _to_float(token: str, default: float) -> float:
    token = token.strip().lower()
    if token in ("-inf", "inf", "nan", "-nan"):
        return default
    try:
        return float(token)
    except ValueError:
        return default


def _run_metadata(args: list[str]) -> str:
    """Run ffmpeg and return the combined metadata-print output (stdout)."""
    proc = subprocess.run(args, capture_output=True, text=True)
    # The metadata `file=-` sink writes to stdout; ffmpeg's own logs go to stderr.
    return proc.stdout


def extract_luma_series(path: str) -> Series:
    out = _run_metadata(
        [
            "ffmpeg", "-hide_banner", "-nostats", "-i", path,
            "-vf", "signalstats,metadata=mode=print:file=-",
            "-an", "-f", "null", "-",
        ]
    )
    return _pair_series(out, _YAVG_RE, silence_default=0.0)


def extract_rms_series(path: str) -> Series:
    out = _run_metadata(
        [
            "ffmpeg", "-hide_banner", "-nostats", "-i", path,
            "-af", "astats=metadata=1:reset=1,ametadata=mode=print:file=-",
            "-vn", "-f", "null", "-",
        ]
    )
    # Silence reads as -inf dB; floor it well below any real beep.
    return _pair_series(out, _RMS_RE, silence_default=-120.0)


def _pair_series(text: str, value_re: re.Pattern[str], silence_default: float) -> Series:
    """metadata=print emits a `frame:.. pts_time:X` line then a `key=value` line."""
    s = Series()
    pending_time: float | None = None
    for line in text.splitlines():
        m = _FRAME_RE.search(line)
        if m:
            pending_time = _to_float(m.group(1), default=-1.0)
            continue
        mv = value_re.search(line)
        if mv and pending_time is not None and pending_time >= 0.0:
            s.times.append(pending_time)
            s.values.append(_to_float(mv.group(1), default=silence_default))
            pending_time = None
    return s


def detect_events(series: Series, threshold_frac: float) -> list[Event]:
    """Rising edges where the value crosses a fraction of its own dynamic range."""
    if len(series.values) < 2:
        return []
    lo = min(series.values)
    hi = max(series.values)
    if hi - lo < 1e-6:
        return []
    threshold = lo + threshold_frac * (hi - lo)
    events: list[Event] = []
    above = False
    previous_time: float | None = None
    previous_value: float | None = None
    nominal = median_sample_interval(series)
    noise = baseline_noise(series, threshold)
    for t, v in zip(series.times, series.values):
        if v >= threshold and not above:
            # The gap to the last sample below threshold, not the series median: a
            # marker that lands right after a dropped frame is located less precisely
            # than one in a clean stretch, and the verdict has to know that.
            gap = nominal if previous_time is None else max(t - previous_time, 0.0)
            crossing, residual_gap = _interpolate_crossing(
                previous_time, previous_value, t, v, threshold, gap, noise
            )
            events.append(Event(start_pts=crossing, sample_interval_s=residual_gap))
            above = True
        elif v < threshold:
            above = False
        previous_time = t
        previous_value = v
    return events


def _interpolate_crossing(
    previous_time: float | None,
    previous_value: float | None,
    time: float,
    value: float,
    threshold: float,
    gap: float,
    noise: float,
) -> tuple[float, float]:
    """Locate a threshold crossing between two samples, and say how well.

    Taking the first sample above threshold puts the edge anywhere in the preceding
    sampling gap, so a 60 fps capture locates a flash to +/-8 ms -- and with three
    markers that is +/-13 ms of drift over the whole span, which cannot carry a
    20 ms verdict. It is also biased: every edge lands systematically half a gap
    late. Interpolating removes the bias and, when the signal really does rise
    across the gap, most of the spread.

    The uncertainty returned is the standard edge-jitter estimate, the baseline
    noise divided by the slope at the crossing, floored by the quantisation of the
    sampling grid and capped at half the gap -- which is what an uninterpolated
    crossing is worth. A step that goes from dark to bright inside one sample
    carries no sub-sample information and lands at that cap.
    """
    if previous_time is None or previous_value is None or gap <= 0:
        return time, gap / 2 if gap > 0 else 0.0

    rise = value - previous_value
    if rise <= 0:
        return time, gap / 2

    fraction = (threshold - previous_value) / rise
    if not 0.0 <= fraction <= 1.0:
        return time, gap / 2

    crossing = previous_time + fraction * gap

    # Jitter = noise / slope. The slope here is rise per gap, so the time jitter
    # is (noise / rise) * gap.
    jitter = (noise / rise) * gap if rise > 0 else gap / 2
    # The floor is the grid's own quantisation: an interpolated crossing cannot be
    # known better than the uniform-distribution standard deviation of one gap.
    floor = gap / (12 ** 0.5)
    return crossing, min(max(jitter, floor), gap / 2)


def baseline_noise(series: Series, threshold: float) -> float:
    """Standard deviation of the samples below threshold.

    This is what sets how precisely an edge can be located: a clean flash against a
    still desktop is locatable to a fraction of a frame, the same flash over moving
    content is not, and the verdict has to carry the difference rather than assume
    the clean case.
    """
    quiet = [value for value in series.values if value < threshold]
    if len(quiet) < 2:
        return 0.0
    mean = sum(quiet) / len(quiet)
    variance = sum((value - mean) ** 2 for value in quiet) / (len(quiet) - 1)
    return variance ** 0.5


def median_sample_interval(series: Series) -> float:
    """The series' own sampling period, as the median gap between samples."""
    if len(series.times) < 2:
        return 0.0
    gaps = sorted(
        later - earlier
        for earlier, later in zip(series.times, series.times[1:])
        if later > earlier
    )
    if not gaps:
        return 0.0
    return gaps[len(gaps) // 2]


def pair_marker_events(
    flashes: list[Event], beeps: list[Event], max_pair_skew_s: float
) -> list[MarkerPair]:
    """Greedily pair the globally closest cross-stream edges within the skew limit."""
    candidates = sorted(
        (
            (abs(flash.start_pts - beep.start_pts), flash_index, beep_index)
            for flash_index, flash in enumerate(flashes)
            for beep_index, beep in enumerate(beeps)
            if abs(flash.start_pts - beep.start_pts) <= max_pair_skew_s
        ),
        key=lambda candidate: candidate[0],
    )
    used_flashes: set[int] = set()
    used_beeps: set[int] = set()
    pairs: list[MarkerPair] = []
    for _, flash_index, beep_index in candidates:
        if flash_index in used_flashes or beep_index in used_beeps:
            continue
        used_flashes.add(flash_index)
        used_beeps.add(beep_index)
        pairs.append(
            MarkerPair(
                flash_pts=flashes[flash_index].start_pts,
                beep_pts=beeps[beep_index].start_pts,
                flash_uncertainty_s=flashes[flash_index].uncertainty_s,
                beep_uncertainty_s=beeps[beep_index].uncertainty_s,
            )
        )
    return sorted(pairs, key=lambda pair: pair.event_pts)


def _interval_spread(candidate: tuple[MarkerPair, ...]) -> float:
    """How unevenly a candidate marker set is spaced, as max interval minus min."""
    intervals = [
        later.event_pts - earlier.event_pts
        for earlier, later in zip(candidate, candidate[1:])
    ]
    if not intervals:
        return 0.0
    return max(intervals) - min(intervals)


def select_marker_pairs(
    pairs: list[MarkerPair],
    expected_markers: int | None,
    expected_marker_times_s: list[float] | None,
    schedule_tolerance_s: float,
) -> tuple[list[MarkerPair], str | None]:
    """Select a complete ordered marker set without silently ignoring disturbances."""
    if expected_markers is None:
        # Auto mode stays at two or three. It has no schedule to check against, so
        # the only thing separating "five markers" from "three markers and two
        # disturbances" is the count it was told to expect -- and a run with more
        # markers always knows its own schedule, because the clapper prints it.
        if len(pairs) not in (2, 3):
            return [], (
                f"need exactly 2 or 3 paired markers in auto mode, found {len(pairs)}; "
                "pass --expected-markers for a longer schedule"
            )
        return pairs, None

    if len(pairs) < expected_markers:
        return [], f"need {expected_markers} paired markers, found {len(pairs)}"
    if expected_marker_times_s is not None and len(expected_marker_times_s) != expected_markers:
        return [], "marker schedule length does not match --expected-markers"

    combinations = list(itertools.combinations(pairs, expected_markers))
    if expected_marker_times_s is None:
        if expected_markers == 2:
            selected = max(
                combinations,
                key=lambda candidate: candidate[-1].event_pts - candidate[0].event_pts,
            )
        else:
            # The most evenly spaced candidate, then the widest. A clapper emits on
            # a regular schedule, so the set whose intervals vary least is the one
            # that is the schedule rather than the one that happens to include a
            # disturbance.
            selected = min(
                combinations,
                key=lambda candidate: (
                    _interval_spread(candidate),
                    -(candidate[-1].event_pts - candidate[0].event_pts),
                ),
            )
        return list(selected), None

    expected_intervals = [
        expected_marker_times_s[index] - expected_marker_times_s[index - 1]
        for index in range(1, expected_markers)
    ]
    if any(interval <= 0.0 for interval in expected_intervals):
        return [], "expected marker times must be strictly increasing"

    def schedule_error(candidate: tuple[MarkerPair, ...]) -> float:
        observed_intervals = [
            candidate[index].event_pts - candidate[index - 1].event_pts
            for index in range(1, expected_markers)
        ]
        return max(
            abs(observed - expected)
            for observed, expected in zip(observed_intervals, expected_intervals)
        )

    selected = min(combinations, key=schedule_error)
    selected_error = schedule_error(selected)
    if selected_error > schedule_tolerance_s:
        return [], (
            "paired markers do not match the expected schedule "
            f"(max interval error {selected_error:.3f}s > {schedule_tolerance_s:.3f}s)"
        )
    return list(selected), None



def fit_drift(markers: list[MarkerPair]) -> dict:
    """Weighted least-squares fit of offset against time.

    Returns the slope (the drift rate), the intercept (the constant emission skew
    that does not belong to ExoSnap), the standard error of the slope, and the
    residual of every marker.

    Weighted rather than ordinary: the markers do not carry equal uncertainty. A
    flash that lands after a dropped frame is located less precisely than one in a
    clean stretch, and an ordinary fit would give both the same say.
    """
    if len(markers) < 2:
        return {"fitted": False, "error": "a fit needs at least two markers"}

    weights = []
    for marker in markers:
        sigma = marker.offset_uncertainty_s
        # A marker with no stated uncertainty is not infinitely precise, it is
        # unmeasured. Weighting it as exact would silence every marker that did
        # state one, which is the opposite of what the weighting is for.
        weights.append(1.0 / (sigma ** 2) if sigma > 0 else 1.0)

    times = [marker.event_pts for marker in markers]
    offsets = [marker.offset_s for marker in markers]

    weight_sum = sum(weights)
    mean_time = sum(w * t for w, t in zip(weights, times)) / weight_sum
    mean_offset = sum(w * o for w, o in zip(weights, offsets)) / weight_sum

    covariance = sum(
        w * (t - mean_time) * (o - mean_offset) for w, t, o in zip(weights, times, offsets)
    )
    variance = sum(w * (t - mean_time) ** 2 for w, t in zip(weights, times))
    if variance <= 0:
        return {"fitted": False, "error": "every marker sits at the same time"}

    slope = covariance / variance
    intercept = mean_offset - slope * mean_time
    residuals = [o - (intercept + slope * t) for t, o in zip(times, offsets)]

    # Standard error from the stated uncertainties, not from the residual spread:
    # with three markers the residual-based estimate has one degree of freedom and
    # is worthless, while the edge uncertainties are known from the sampling rates.
    slope_standard_error = (1.0 / variance) ** 0.5

    return {
        "fitted": True,
        "slope_s_per_s": slope,
        "intercept_s": intercept,
        "slope_standard_error_s_per_s": slope_standard_error,
        "residuals_s": residuals,
        "max_abs_residual_s": max(abs(residual) for residual in residuals),
        "marker_uncertainties_s": [marker.offset_uncertainty_s for marker in markers],
    }



def required_marker_count(
    marker_sigma_s: float, allowed_uncertainty_ms: float, limit: int = 200
) -> int | None:
    """How many evenly spaced markers a budget needs at a given edge precision.

    The uncertainty of the TOTAL drift does not shrink with a longer run: the slope
    is determined better over a longer span, and the span it is multiplied by grows
    by the same factor. Only more markers (or sharper edges) help, which is worth
    saying in the verdict rather than leaving a campaign to lengthen a run that
    cannot get more precise that way.

    Returns None when no marker count within `limit` would do, which means the
    edges themselves have to get sharper.
    """
    if marker_sigma_s <= 0 or allowed_uncertainty_ms <= 0:
        return None
    for count in range(3, limit + 1):
        # For `count` markers spread evenly over a span, the slope's standard error
        # times that span reduces to sigma * sqrt(12(n-1) / (n(n+1))) -- the span
        # cancels, which is the point above.
        factor = (12.0 * (count - 1) / (count * (count + 1))) ** 0.5
        if marker_sigma_s * factor * 1000.0 <= allowed_uncertainty_ms:
            return count
    return None


def qualify_reference(
    markers: list[MarkerPair],
    fit: dict,
    span_s: float,
    budget_ms: float,
    uncertainty_fraction: float = 1.0 / 3.0,
    residual_sigmas: float = 3.0,
) -> dict:
    """Decide whether the clapper signal can carry a drift verdict at all.

    Three conditions, each of which a real run has failed:

    * At least three markers. Two define a line exactly, so their residuals are
      zero by construction and nonlinearity is invisible.
    * The drift the fit could be wrong by, over the measured span, is well under
      the budget. Otherwise "within budget" and "too noisy to tell" produce the
      same answer.
    * Every marker lies within its own edge uncertainty of the fitted line. A
      marker further out means the offset did not move linearly, and then no
      single rate -- fitted or differenced -- describes the run.
    """
    reasons: list[str] = []

    if not fit.get("fitted"):
        return {"qualified": False, "reasons": [fit.get("error", "the fit failed")]}

    if len(markers) < 3:
        reasons.append(
            f"{len(markers)} markers cannot show nonlinearity; two always lie exactly on a line"
        )

    drift_uncertainty_ms = fit["slope_standard_error_s_per_s"] * span_s * 1000.0
    allowed_uncertainty_ms = budget_ms * uncertainty_fraction
    if drift_uncertainty_ms > allowed_uncertainty_ms:
        typical_sigma = 0.0
        stated = [u for u in fit["marker_uncertainties_s"] if u > 0]
        if stated:
            typical_sigma = sorted(stated)[len(stated) // 2]
        needed = required_marker_count(typical_sigma, allowed_uncertainty_ms)
        remedy = (
            f"; {needed} evenly spaced markers would reach it at this edge precision"
            if needed else
            "; sharpen the marker edges or lengthen the run"
        )
        reasons.append(
            f"the fitted drift is uncertain to +/-{drift_uncertainty_ms:.2f} ms over the span, "
            f"more than the {allowed_uncertainty_ms:.2f} ms a {budget_ms:.2f} ms budget can be judged against"
            + remedy
        )

    uncertainties = fit["marker_uncertainties_s"]
    if any(sigma > 0 for sigma in uncertainties):
        # Each marker against ITS OWN edges, not against the median of all of
        # them: the markers do not carry equal uncertainty, and a median allowance
        # both accuses the well-measured markers and excuses the poorly measured
        # ones.
        worst_index = -1
        worst_excess = 0.0
        for index, (residual, sigma) in enumerate(zip(fit["residuals_s"], uncertainties)):
            if sigma <= 0:
                continue
            excess = abs(residual) - residual_sigmas * sigma
            if excess > worst_excess:
                worst_excess = excess
                worst_index = index
        if worst_index >= 0:
            allowed = residual_sigmas * uncertainties[worst_index]
            reasons.append(
                f"marker {worst_index + 1} sits {abs(fit['residuals_s'][worst_index]) * 1000:.2f} ms off the "
                f"fitted line, beyond the {allowed * 1000:.2f} ms its own edges allow; the drift is not linear"
            )
    else:
        reasons.append(
            "no marker carries an edge uncertainty, so the fit cannot be told from a coincidence"
        )

    return {
        "qualified": not reasons,
        "reasons": reasons,
        "drift_uncertainty_ms": drift_uncertainty_ms,
        "allowed_uncertainty_ms": allowed_uncertainty_ms,
    }


def analyze_event_pairs(
    flashes: list[Event],
    beeps: list[Event],
    expected_markers: int | None = None,
    expected_marker_times_s: list[float] | None = None,
    max_pair_skew_s: float = 0.250,
    schedule_tolerance_s: float = 2.0,
    budget_ms: float = 20.0,
) -> dict:
    pairs = pair_marker_events(flashes, beeps, max_pair_skew_s)
    selected, selection_error = select_marker_pairs(
        pairs, expected_markers, expected_marker_times_s, schedule_tolerance_s
    )
    result: dict = {
        "flash_events": [event.start_pts for event in flashes],
        "beep_events": [event.start_pts for event in beeps],
        "flash_event_count": len(flashes),
        "beep_event_count": len(beeps),
        "paired_event_count": len(pairs),
        "measurable": False,
    }
    if selection_error is not None:
        result["error"] = selection_error
        return result

    labels = ["start", "end"] if len(selected) == 2 else ["start", "middle", "end"]
    markers = [
        {
            "label": label,
            "flash_s": pair.flash_pts,
            "beep_s": pair.beep_pts,
            "offset_ms": pair.offset_s * 1000.0,
        }
        for label, pair in zip(labels, selected)
    ]
    offset_start = selected[0].offset_s
    offset_end = selected[-1].offset_s
    span = selected[-1].beep_pts - selected[0].beep_pts
    if span <= 1e-6:
        result["error"] = "marker PTS are not strictly ordered"
        return result
    drift = offset_end - offset_start
    drift_per_hour = drift / span * 3600.0

    result.update(
        {
            "measurable": True,
            "marker_count": len(selected),
            "markers": markers,
            "recognized_flash_pts": [pair.flash_pts for pair in selected],
            "recognized_beep_pts": [pair.beep_pts for pair in selected],
            "flash_start_s": selected[0].flash_pts,
            "flash_end_s": selected[-1].flash_pts,
            "beep_start_s": selected[0].beep_pts,
            "beep_end_s": selected[-1].beep_pts,
            "span_s": span,
            "offset_start_ms": offset_start * 1000.0,
            "offset_end_ms": offset_end * 1000.0,
            "drift_ms": drift * 1000.0,
            "drift_start_end_ms": drift * 1000.0,
            "drift_ms_per_hour": drift_per_hour * 1000.0,
        }
    )
    if len(selected) == 3:
        offset_middle = selected[1].offset_s
        result.update(
            {
                "offset_middle_ms": offset_middle * 1000.0,
                "drift_start_middle_ms": (offset_middle - offset_start) * 1000.0,
                "drift_middle_end_ms": (offset_end - offset_middle) * 1000.0,
            }
        )

    # The fitted drift is the verdict's quantity; the endpoint difference above stays
    # as a diagnostic, because a large disagreement between the two is itself the
    # finding that the run was not linear.
    fit = fit_drift(selected)
    reference = qualify_reference(selected, fit, span, budget_ms)
    result["fit"] = fit
    result["reference"] = reference
    if fit.get("fitted"):
        fitted_drift = fit["slope_s_per_s"] * span
        result.update(
            {
                "fitted_drift_ms": fitted_drift * 1000.0,
                "fitted_drift_ms_per_hour": fit["slope_s_per_s"] * 3600.0 * 1000.0,
                "fitted_emission_skew_ms": (fit["intercept_s"] + fit["slope_s_per_s"] * selected[0].event_pts) * 1000.0,
                "fitted_drift_uncertainty_ms": reference.get("drift_uncertainty_ms", 0.0),
                "max_residual_ms": fit["max_abs_residual_s"] * 1000.0,
            }
        )
    return result


def measure(
    path: str,
    luma_frac: float,
    rms_frac: float,
    budget_ms: float = 20.0,
    expected_markers: int | None = None,
    expected_marker_times_s: list[float] | None = None,
    max_pair_skew_s: float = 0.250,
    schedule_tolerance_s: float = 2.0,
) -> dict:
    luma = extract_luma_series(path)
    rms = extract_rms_series(path)
    flashes = detect_events(luma, luma_frac)
    beeps = detect_events(rms, rms_frac)
    result = analyze_event_pairs(
        flashes,
        beeps,
        expected_markers,
        expected_marker_times_s,
        max_pair_skew_s,
        schedule_tolerance_s,
        budget_ms,
    )
    result["file"] = path
    return result


def _parse_marker_times(value: str) -> list[float]:
    try:
        times = [float(token) for token in value.split(",")]
    except ValueError as error:
        raise argparse.ArgumentTypeError("marker times must be comma-separated seconds") from error
    if len(times) < 2 or any(time < 0.0 for time in times):
        raise argparse.ArgumentTypeError("marker times must contain 2 or 3 non-negative values")
    if any(times[index] <= times[index - 1] for index in range(1, len(times))):
        raise argparse.ArgumentTypeError("marker times must be strictly increasing")
    return times


def main() -> int:
    p = argparse.ArgumentParser(description="Measure A/V drift of an ExoSnap clapper recording.")
    p.add_argument("file", help="recorded clapper file (mkv/mp4/webm)")
    p.add_argument("--luma-threshold-frac", type=float, default=0.7,
                   help="flash detection threshold as a fraction of the luma range (default 0.7)")
    p.add_argument("--rms-threshold-frac", type=float, default=0.5,
                   help="beep detection threshold as a fraction of the RMS range (default 0.5)")
    p.add_argument("--max-drift-ms", type=float, default=20.0,
                   help="total drift budget over the measured span, ms (advisory default 20)")
    p.add_argument("--max-drift-ms-per-hour", type=float, default=None,
                   help="alternative drift budget as a rate (ms/hour); overrides --max-drift-ms when set")
    p.add_argument("--unqualified-reference", action="store_true",
                   help="report a verdict even when the clapper signal does not qualify to carry "
                        "one (diagnostic; the numbers are printed either way)")
    p.add_argument("--expected-markers", type=int, default=None,
                   help="require and select exactly this many scheduled marker pairs")
    p.add_argument("--marker-times-seconds", type=_parse_marker_times, default=None,
                   help="expected comma-separated marker schedule, e.g. 10,3600,7190")
    p.add_argument("--max-pair-skew-ms", type=float, default=250.0,
                   help="maximum flash/beep edge separation when pairing markers (default 250)")
    p.add_argument("--schedule-tolerance-seconds", type=float, default=2.0,
                   help="maximum interval error against --marker-times-seconds (default 2)")
    p.add_argument("--json", action="store_true", help="emit the full measurement as JSON")
    args = p.parse_args()

    if args.marker_times_seconds is not None:
        if args.expected_markers is None:
            args.expected_markers = len(args.marker_times_seconds)
        elif args.expected_markers != len(args.marker_times_seconds):
            p.error("--expected-markers must match --marker-times-seconds")
    if args.max_pair_skew_ms <= 0.0 or args.schedule_tolerance_seconds < 0.0:
        p.error("pair skew must be positive and schedule tolerance cannot be negative")

    _require_tools()
    # The budget reaches the analysis because reference qualification is relative
    # to it: a fit precise to +/-2 ms qualifies a 20 ms budget and does not qualify
    # a 3 ms one.
    budget_ms = args.max_drift_ms
    if args.max_drift_ms_per_hour is not None:
        budget_ms = args.max_drift_ms_per_hour

    r = measure(
        args.file,
        args.luma_threshold_frac,
        args.rms_threshold_frac,
        budget_ms,
        args.expected_markers,
        args.marker_times_seconds,
        args.max_pair_skew_ms / 1000.0,
        args.schedule_tolerance_seconds,
    )

    if not r.get("measurable"):
        if args.json:
            print(json.dumps(r, indent=2))
        else:
            sys.stderr.write(f"av-sync-check: could not measure: {r.get('error')}\n")
            sys.stderr.write(f"  flash_events={r['flash_events']} beep_events={r['beep_events']}\n")
        return 3

    segment_finding = False
    if r["marker_count"] == 3:
        segment_drifts = (r["drift_start_middle_ms"], r["drift_middle_end_ms"])
        segment_finding = (
            max(abs(value) for value in segment_drifts) > args.max_drift_ms
            and segment_drifts[0] * segment_drifts[1] < 0.0
        )
        r["segment_reliability_finding"] = segment_finding

    if args.json:
        print(json.dumps(r, indent=2))

    if not args.json:
        print(f"span:            {r['span_s']:.3f} s")
        print(f"offset_start:    {r['offset_start_ms']:+.2f} ms   (ADVISORY — emission skew, not gated)")
        if r["marker_count"] == 3:
            print(f"offset_middle:   {r['offset_middle_ms']:+.2f} ms   (ADVISORY)")
        print(f"offset_end:      {r['offset_end_ms']:+.2f} ms   (ADVISORY)")
        if r["marker_count"] == 3:
            print(f"drift start→mid: {r['drift_start_middle_ms']:+.2f} ms")
            print(f"drift mid→end:   {r['drift_middle_end_ms']:+.2f} ms")
        print(f"drift start→end: {r['drift_start_end_ms']:+.2f} ms over span   (endpoint, diagnostic)")
        print(f"drift rate:      {r['drift_ms_per_hour']:+.2f} ms/hour   (endpoint, diagnostic)")
        if r.get("fit", {}).get("fitted"):
            print(
                f"fitted drift:    {r['fitted_drift_ms']:+.2f} ms over span "
                f"+/-{r['fitted_drift_uncertainty_ms']:.2f} ms   (VERDICT)"
            )
            print(f"fitted rate:     {r['fitted_drift_ms_per_hour']:+.2f} ms/hour")
            print(f"max residual:    {r['max_residual_ms']:.2f} ms")
        reference_state = r.get("reference", {})
        print(f"reference:       {'QUALIFIED' if reference_state.get('qualified') else 'NOT QUALIFIED'}")
        for reason in reference_state.get("reasons", []):
            print(f"                 - {reason}")
        print(
            f"events:          flash={r['flash_event_count']} beep={r['beep_event_count']} "
            f"paired={r['paired_event_count']} recognized={r['marker_count']}"
        )
        print(f"flash PTS:       {r['recognized_flash_pts']}")
        print(f"beep PTS:        {r['recognized_beep_pts']}")

    reference = r.get("reference", {})
    if not reference.get("qualified") and not args.unqualified_reference:
        if not args.json:
            sys.stderr.write(
                "av-sync-check: could not measure: the reference signal does not qualify "
                "to carry a drift verdict\n"
            )
            for reason in reference.get("reasons", ["no reason was recorded"]):
                sys.stderr.write(f"  - {reason}\n")
            sys.stderr.write(
                "  Re-run with more markers or a longer span, or pass "
                "--unqualified-reference to read the numbers without a verdict.\n"
            )
        return 3

    # The fitted slope, not the endpoint difference: an endpoint difference is the
    # drift only if the drift is linear, which the qualification above is what
    # establishes.
    if args.max_drift_ms_per_hour is not None:
        rate = abs(r.get("fitted_drift_ms_per_hour", r["drift_ms_per_hour"]))
        over = rate > args.max_drift_ms_per_hour
        budget = f"{args.max_drift_ms_per_hour} ms/hour"
        measured = f"{rate:.2f} ms/hour"
    else:
        total = abs(r.get("fitted_drift_ms", r["drift_ms"]))
        over = total > args.max_drift_ms
        budget = f"{args.max_drift_ms} ms"
        measured = f"{total:.2f} ms"

    uncertainty_ms = r.get("fitted_drift_uncertainty_ms")
    if uncertainty_ms is not None:
        measured += f" +/-{uncertainty_ms:.2f} ms"

    if over:
        if not args.json:
            sys.stderr.write(f"av-sync-check: DRIFT OVER BUDGET — {measured} > {budget}\n")
        return 2
    if not args.json:
        print(f"OK: drift {measured} within budget {budget}")
        if segment_finding:
            sys.stderr.write(
                "av-sync-check: RELIABILITY FINDING — opposing segment drifts exceed "
                "the total-drift budget and cancel at the endpoint\n"
            )
    return 0


if __name__ == "__main__":
    sys.exit(main())
