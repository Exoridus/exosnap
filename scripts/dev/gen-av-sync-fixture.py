#!/usr/bin/env python3
"""gen-av-sync-fixture.py — regenerate the tiny golden clapper clip.

The A/V-sync analyzer (`av-sync-check.py`) must not regress silently, so CI runs it
against a committed golden fixture with a KNOWN, near-zero drift. This script bakes
that fixture with system ffmpeg: synchronized full-frame white FLASH + 1 kHz BEEP
markers spread evenly across the clip, and silence/black in between.

Because the flash and beep are emitted on the same synthetic timeline (no hardware
emission path), the fixture's drift is ~0 — exactly the regression anchor CI asserts.
The clip is deliberately tiny (320x180, low bitrate) to keep the repo small.

WHY MORE THAN TWO MARKERS. The analyzer refuses to issue a drift verdict from a
reference that cannot carry one, and two markers cannot: they define a line exactly,
so nonlinearity is invisible and the fit's uncertainty is at its worst. A two-marker
fixture therefore only ever exercises the path the qualification rejects. Five is the
smallest count that qualifies this clip's edge precision against a 25 ms budget; see
docs/dev/soak-and-recovery-drills.md for the count a given budget needs.

Run:  python scripts/dev/gen-av-sync-fixture.py
Output: tests/fixtures/av-sync/clapper-golden.mp4
"""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys

OUT = pathlib.Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "av-sync" / "clapper-golden.mp4"

FLASH_SECONDS = 0.1


def marker_times(duration: float, markers: int) -> list[float]:
    """Marker onsets, evenly spread with the last one ending inside the clip."""
    last = duration - FLASH_SECONDS
    if markers < 2 or last <= 0:
        raise ValueError("need at least two markers inside the clip")
    step = last / (markers - 1)
    return [round(index * step, 3) for index in range(markers)]


def build_filters(times: list[float]) -> tuple[str, str]:
    """The drawbox and volume enable expressions for a marker schedule.

    Both are built from the same list, which is what keeps the flash and the beep
    on one timeline: a fixture whose two enable expressions were written separately
    would bake in exactly the skew the analyzer exists to measure.
    """
    windows = [f"between(t,{start},{start + FLASH_SECONDS})" for start in times]
    enable = "+".join(windows)
    return (
        f"drawbox=t=fill:c=white:enable='{enable}'",
        # Silence everywhere the markers are not.
        f"volume=enable='not({enable})':volume=0",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duration", type=float, default=4.0, help="clip length in seconds")
    parser.add_argument("--markers", type=int, default=5, help="marker count (at least 2)")
    parser.add_argument("--out", type=pathlib.Path, default=OUT)
    args = parser.parse_args()

    if shutil.which("ffmpeg") is None:
        sys.stderr.write("gen-av-sync-fixture: ffmpeg not found on PATH.\n")
        return 3

    try:
        times = marker_times(args.duration, args.markers)
    except ValueError as error:
        sys.stderr.write(f"gen-av-sync-fixture: {error}\n")
        return 64

    video_filter, audio_filter = build_filters(times)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        "ffmpeg", "-y", "-hide_banner",
        "-f", "lavfi", "-i", f"color=c=black:s=320x180:r=60:d={args.duration}",
        "-f", "lavfi", "-i", f"sine=frequency=1000:sample_rate=48000:duration={args.duration}",
        "-vf", video_filter,
        "-af", audio_filter,
        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-b:v", "150k",
        "-c:a", "aac", "-b:a", "64k",
        "-shortest", str(args.out),
    ]
    proc = subprocess.run(cmd)
    if proc.returncode != 0:
        sys.stderr.write("gen-av-sync-fixture: ffmpeg failed.\n")
        return proc.returncode

    print(f"wrote {args.out} ({args.out.stat().st_size} bytes)")
    print(f"markers at {', '.join(f'{value:.3f}' for value in times)} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
