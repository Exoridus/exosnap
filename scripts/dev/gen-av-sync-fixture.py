#!/usr/bin/env python3
"""gen-av-sync-fixture.py — regenerate the tiny golden clapper clip.

The A/V-sync analyzer (`av-sync-check.py`) must not regress silently, so CI runs it
against a committed golden fixture with a KNOWN, near-zero drift. This script bakes
that fixture with system ffmpeg: a 4 s clip with a synchronized full-frame white
FLASH + 1 kHz BEEP at both the start and the end, and silence/black in between.

Because the flash and beep are emitted on the same synthetic timeline (no hardware
emission path), the fixture's drift is ~0 — exactly the regression anchor CI asserts.
The clip is deliberately tiny (320x180, low bitrate) to keep the repo small.

Run:  python scripts/dev/gen-av-sync-fixture.py
Output: tests/fixtures/av-sync/clapper-golden.mp4
"""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys

OUT = pathlib.Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "av-sync" / "clapper-golden.mp4"


def main() -> int:
    if shutil.which("ffmpeg") is None:
        sys.stderr.write("gen-av-sync-fixture: ffmpeg not found on PATH.\n")
        return 3
    OUT.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        "ffmpeg", "-y", "-hide_banner",
        "-f", "lavfi", "-i", "color=c=black:s=320x180:r=60:d=4",
        "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=48000:duration=4",
        # White full-frame flash for the first and last 0.1 s.
        "-vf", "drawbox=t=fill:c=white:enable='lt(t,0.1)+gt(t,3.9)'",
        # Mute the middle so only the start/end beeps survive — the unmuted
        # windows share the flash's edges (t<0.1 and t>3.9) so the baked-in
        # drift is bounded only by frame quantization, ~0.
        "-af", "volume=enable='between(t,0.1,3.9)':volume=0",
        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-b:v", "150k",
        "-c:a", "aac", "-b:a", "64k",
        "-shortest", str(OUT),
    ]
    proc = subprocess.run(cmd)
    if proc.returncode != 0:
        sys.stderr.write("gen-av-sync-fixture: ffmpeg failed.\n")
        return proc.returncode
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
