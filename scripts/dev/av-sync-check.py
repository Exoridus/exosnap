#!/usr/bin/env python3
"""av-sync-check.py — measure A/V clock drift of a recorded clapper file.

ExoSnap's `--clapper` capture emits a full-frame white FLASH and a loud BEEP at
both recording start and recording end. This script recovers, purely from the
finished file via system ffmpeg/ffprobe:

  * the video PTS of the flash at the start and at the end (luma edge), and
  * the audio PTS of the beep at the start and at the end (RMS edge).

From those it computes:

  offset_start = flash_start_pts - beep_start_pts
  offset_end   = flash_end_pts   - beep_end_pts
  drift        = offset_end - offset_start        (over the measured span)

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


def measure(path: str, luma_frac: float, rms_frac: float) -> dict:
    luma = extract_luma_series(path)
    rms = extract_rms_series(path)
    flashes = detect_events(luma, luma_frac)
    beeps = detect_events(rms, rms_frac)

    result: dict = {
        "file": path,
        "flash_events": [e.start_pts for e in flashes],
        "beep_events": [e.start_pts for e in beeps],
        "measurable": False,
    }
    if len(flashes) < 2:
        result["error"] = f"need >=2 flash events, found {len(flashes)}"
        return result
    if len(beeps) < 2:
        result["error"] = f"need >=2 beep events, found {len(beeps)}"
        return result

    flash_start, flash_end = flashes[0].start_pts, flashes[-1].start_pts
    beep_start, beep_end = beeps[0].start_pts, beeps[-1].start_pts
    offset_start = flash_start - beep_start
    offset_end = flash_end - beep_end
    span = beep_end - beep_start
    drift = offset_end - offset_start
    drift_per_hour = (drift / span * 3600.0) if span > 1e-6 else 0.0

    result.update(
        {
            "measurable": True,
            "flash_start_s": flash_start,
            "flash_end_s": flash_end,
            "beep_start_s": beep_start,
            "beep_end_s": beep_end,
            "span_s": span,
            "offset_start_ms": offset_start * 1000.0,  # ADVISORY (emission skew)
            "offset_end_ms": offset_end * 1000.0,      # ADVISORY
            "drift_ms": drift * 1000.0,                # PASS/FAIL quantity
            "drift_ms_per_hour": drift_per_hour * 1000.0,
        }
    )
    return result


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
    p.add_argument("--json", action="store_true", help="emit the full measurement as JSON")
    args = p.parse_args()

    _require_tools()
    r = measure(args.file, args.luma_threshold_frac, args.rms_threshold_frac)

    if args.json:
        print(json.dumps(r, indent=2))
    if not r.get("measurable"):
        if not args.json:
            sys.stderr.write(f"av-sync-check: could not measure: {r.get('error')}\n")
            sys.stderr.write(f"  flash_events={r['flash_events']} beep_events={r['beep_events']}\n")
        return 3

    if not args.json:
        print(f"span:            {r['span_s']:.3f} s")
        print(f"offset_start:    {r['offset_start_ms']:+.2f} ms   (ADVISORY — emission skew, not gated)")
        print(f"offset_end:      {r['offset_end_ms']:+.2f} ms   (ADVISORY)")
        print(f"drift:           {r['drift_ms']:+.2f} ms over span")
        print(f"drift rate:      {r['drift_ms_per_hour']:+.2f} ms/hour")

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
    return 0


if __name__ == "__main__":
    sys.exit(main())
