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


@dataclass
class Series:
    times: list[float] = field(default_factory=list)
    values: list[float] = field(default_factory=list)


@dataclass(frozen=True)
class MarkerPair:
    flash_pts: float
    beep_pts: float

    @property
    def event_pts(self) -> float:
        return (self.flash_pts + self.beep_pts) / 2.0

    @property
    def offset_s(self) -> float:
        return self.flash_pts - self.beep_pts


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
    for t, v in zip(series.times, series.values):
        if v >= threshold and not above:
            events.append(Event(start_pts=t))
            above = True
        elif v < threshold:
            above = False
    return events


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
            )
        )
    return sorted(pairs, key=lambda pair: pair.event_pts)


def select_marker_pairs(
    pairs: list[MarkerPair],
    expected_markers: int | None,
    expected_marker_times_s: list[float] | None,
    schedule_tolerance_s: float,
) -> tuple[list[MarkerPair], str | None]:
    """Select a complete ordered marker set without silently ignoring disturbances."""
    if expected_markers is None:
        if len(pairs) not in (2, 3):
            return [], f"need exactly 2 or 3 paired markers in auto mode, found {len(pairs)}"
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
            selected = min(
                combinations,
                key=lambda candidate: (
                    abs(
                        (candidate[1].event_pts - candidate[0].event_pts)
                        - (candidate[2].event_pts - candidate[1].event_pts)
                    ),
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


def analyze_event_pairs(
    flashes: list[Event],
    beeps: list[Event],
    expected_markers: int | None = None,
    expected_marker_times_s: list[float] | None = None,
    max_pair_skew_s: float = 0.250,
    schedule_tolerance_s: float = 2.0,
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
    return result


def measure(
    path: str,
    luma_frac: float,
    rms_frac: float,
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
    )
    result["file"] = path
    return result


def _parse_marker_times(value: str) -> list[float]:
    try:
        times = [float(token) for token in value.split(",")]
    except ValueError as error:
        raise argparse.ArgumentTypeError("marker times must be comma-separated seconds") from error
    if len(times) not in (2, 3) or any(time < 0.0 for time in times):
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
    p.add_argument("--expected-markers", type=int, choices=(2, 3), default=None,
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
    r = measure(
        args.file,
        args.luma_threshold_frac,
        args.rms_threshold_frac,
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
        print(f"drift start→end: {r['drift_start_end_ms']:+.2f} ms over span")
        print(f"drift rate:      {r['drift_ms_per_hour']:+.2f} ms/hour")
        print(
            f"events:          flash={r['flash_event_count']} beep={r['beep_event_count']} "
            f"paired={r['paired_event_count']} recognized={r['marker_count']}"
        )
        print(f"flash PTS:       {r['recognized_flash_pts']}")
        print(f"beep PTS:        {r['recognized_beep_pts']}")

    drift_ms = abs(r["drift_ms"])
    if args.max_drift_ms_per_hour is not None:
        over = abs(r["drift_ms_per_hour"]) > args.max_drift_ms_per_hour
        budget = f"{args.max_drift_ms_per_hour} ms/hour"
        measured = f"{abs(r['drift_ms_per_hour']):.2f} ms/hour"
    else:
        over = drift_ms > args.max_drift_ms
        budget = f"{args.max_drift_ms} ms"
        measured = f"{drift_ms:.2f} ms"

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
