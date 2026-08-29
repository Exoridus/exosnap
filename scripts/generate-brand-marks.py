#!/usr/bin/env python3
"""Generate the ExoSnap mark suite from the canonical brand parameters.

WHY THE SVGS ARE GENERATED RATHER THAN HAND-KEPT
------------------------------------------------
Every mark in ``app/assets/brand/marks`` draws the same aperture: an outer ring,
an inner ring and whatever the state puts inside it. The five numbers that define
that aperture therefore appear in fourteen files, and a hand-kept suite makes a
one-number design change a fourteen-file edit that drifts on the first miss.

``app/assets/brand/marks/parameters.json`` is the one place those numbers live.
This script writes the suite from them; the suite is checked in because the
runtime loads it out of Qt resources and a reviewer should be able to see the
shapes in the diff. ``brand_geometry_tests`` regenerates and compares, so a
checked-in file that stopped matching the parameters fails the build.

The per-state shapes below -- the check, the warning glyph, the pause bars, the
two animations -- are authored geometry, not derived: they are the designer cut's
own compositions, expressed against the canonical aperture so that moving the
inner ring moves what sits inside it.

The colours written into the files are the designer's reference palette. They are
NOT what ships: ``app/ui/brand/BrandMarkSvg.h`` substitutes the running theme's
accent and semantic colours for them at load. Keeping the reference values in the
files is what makes the assets readable on their own, and the substitution table
asserts that every colour it finds is one it knows.

Usage:  python scripts/generate-brand-marks.py [--check]
"""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
MARKS_DIR = REPO / "app" / "assets" / "brand" / "marks"
PARAMETERS = MARKS_DIR / "parameters.json"

# Frames per animated state. The shell renders them as whole-icon swaps;
# app/models/RecordingPulse.h holds the cadence.
PROCESSING_FRAME_COUNT = 4


def num(value: float, decimals: int = 2) -> str:
    """A number as the SVG carries it: rounded, then written without trailing
    zeroes, so a value that is exactly one reads as ``1``."""
    return f"{round(value, decimals):g}"


def fixed(value: float, decimals: int = 1) -> str:
    """A number that keeps its decimal place. Used where a coordinate would
    otherwise collapse to an integer and stop reading as a measurement."""
    return f"{value:.{decimals}f}"


class Marks:
    def __init__(self, parameters: dict) -> None:
        self.grid = float(parameters["grid"])
        self.center = self.grid / 2.0
        geometry = parameters["geometry"]
        self.outer_r = float(geometry["outer_r"])
        self.outer_w = float(geometry["outer_w"])
        self.inner_r = float(geometry["inner_r"])
        self.inner_w = float(geometry["inner_w"])
        self.center_r = float(geometry["center_r"])
        # The suite is written at the dark opacity. Light differs in this one
        # value and nothing else, so it is a substitution at load rather than a
        # second set of files.
        self.outer_opacity = float(parameters["outer_opacity"]["dark"])
        colours = parameters["reference_colors"]
        self.accent = colours["accent"]
        self.recording = colours["recording"]
        self.caution = colours["caution"]
        self.success = colours["success"]
        # Not drawn by anything this script writes. `marks/wordmark.svg` is
        # authored outlines rather than derived geometry, and it is the only
        # asset in the suite that carries body ink -- but its colour belongs in
        # the one palette table, because the runtime substitutes all five from
        # the same list.
        self.ink = colours["ink"]

    # --- primitives ---------------------------------------------------------

    def circle(self, r: float, *, stroke: str | None = None, width: float | None = None,
               fill: str = "none", opacity: float | None = None, extra: str = "") -> str:
        parts = [f'<circle cx="{num(self.center)}" cy="{num(self.center)}" r="{num(r)}"']
        parts.append(f' fill="{fill}"')
        if stroke is not None:
            parts.append(f' stroke="{stroke}" stroke-width="{num(width or 0.0)}"')
        if extra:
            parts.append(" " + extra)
        if opacity is not None:
            parts.append(f' opacity="{num(opacity)}"')
        parts.append("/>")
        return "".join(parts)

    def disc(self, r: float, colour: str, opacity: float | None = None) -> str:
        return self.circle(r, fill=colour, opacity=opacity)

    def bar(self, cx: float, cy: float, w: float, h: float, colour: str, radius: float | None = None) -> str:
        """An upright bar, centred on (cx, cy). `radius` defaults to fully rounded
        ends; the transport bars take a softer corner instead, because a pause
        glyph drawn as two capsules reads as an equals sign at 16 px."""
        r = w / 2.0 if radius is None else radius
        return (f'<rect x="{num(cx - w / 2.0)}" y="{num(cy - h / 2.0)}" width="{num(w)}" height="{num(h)}"'
                f' rx="{num(r)}" ry="{num(r)}" fill="{colour}"/>')

    def outer_ring(self) -> str:
        return self.circle(self.outer_r, stroke=self.accent, width=self.outer_w, opacity=self.outer_opacity)

    def document(self, body: list[str]) -> str:
        grid = num(self.grid)
        return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{grid}" height="{grid}"'
                f' viewBox="0 0 {grid} {grid}">' + "".join(body) + "</svg>\n")

    # --- static states ------------------------------------------------------

    def brand(self) -> str:
        """The identity, and what Idle shows. The dot is the recording colour --
        the aperture is trained on something and the mark says so even at rest."""
        return self.document([
            self.outer_ring(),
            self.circle(self.inner_r, stroke=self.accent, width=self.inner_w),
            self.disc(self.center_r, self.recording),
        ])

    def paused(self) -> str:
        bar_w = self.inner_w * 1.107
        bar_h = self.inner_r * 1.284
        offset = self.inner_r * 0.294
        corner = self.inner_w * 0.321
        return self.document([
            self.outer_ring(),
            self.circle(self.inner_r, stroke=self.caution, width=self.inner_w),
            self.bar(self.center - offset, self.center, bar_w, bar_h, self.caution, corner),
            self.bar(self.center + offset, self.center, bar_w, bar_h, self.caution, corner),
        ])

    def saved(self) -> str:
        # A check, not a tick mark: the short arm is deliberately short, because
        # at 16 px two arms of similar length read as a V.
        #
        # Every coefficient is a fraction of the INNER radius, and the arms are
        # given as explicit deltas from the elbow rather than as one length times
        # two ratios. Measured against the outer ring instead, the stroke touched
        # the aperture it is meant to sit inside at the smallest rendered profile.
        # test_shell_icon_renderer pins the clearance at every shipped size.
        elbow_x = self.center - self.inner_r * 0.194595
        elbow_y = self.center + self.inner_r * 0.462162
        short = self.inner_r * 0.389189
        long_dx = self.inner_r * 0.778378
        long_dy = self.inner_r * 0.839189
        path = (f'M{num(elbow_x - short)} {num(elbow_y - short)}'
                f'L{num(elbow_x)} {num(elbow_y)}'
                f'L{num(elbow_x + long_dx)} {num(elbow_y - long_dy)}')
        return self.document([
            self.outer_ring(),
            self.circle(self.inner_r, stroke=self.success, width=self.inner_w),
            f'<path d="{path}" fill="none" stroke="{self.success}" stroke-width="{num(self.inner_w * 1.214286)}"'
            f' stroke-linecap="round" stroke-linejoin="round"/>',
        ])

    def warning(self) -> str:
        # Same correction as `saved`: the triangle, its stem and its dot are all
        # measured off the inner radius, so the glyph clears the aperture at the
        # smallest profile instead of resting on it.
        half = self.inner_r * 0.586486
        top = self.center - self.inner_r * 0.586486
        bottom = self.center + self.inner_r * 0.429730
        return self.document([
            self.outer_ring(),
            self.circle(self.inner_r, stroke=self.caution, width=self.inner_w),
            f'<path d="M{num(self.center)} {num(top)}L{num(self.center + half)} {num(bottom)}'
            f'H{num(self.center - half)}Z" fill="none" stroke="{self.caution}"'
            f' stroke-width="{num(self.inner_w * 0.893)}" stroke-linejoin="round"/>',
            f'<path d="M{num(self.center)} {num(self.center - self.inner_r * 0.266216)}'
            f'V{num(self.center + self.inner_r * 0.121622)}" fill="none" stroke="{self.caution}"'
            f' stroke-width="{num(self.inner_w * 0.964)}" stroke-linecap="round" stroke-linejoin="round"/>',
            f'<circle cx="{num(self.center)}" cy="{num(self.center + self.inner_r * 0.314865)}"'
            f' r="{num(self.center_r * 0.221818)}" fill="{self.caution}"/>',
        ])

    def error(self) -> str:
        arm = self.inner_r * 0.493
        lo, hi = self.center - arm, self.center + arm
        path = f'M{num(lo)} {num(lo)}L{num(hi)} {num(hi)} M{num(hi)} {num(lo)}L{num(lo)} {num(hi)}'
        return self.document([
            self.outer_ring(),
            self.circle(self.inner_r, stroke=self.recording, width=self.inner_w),
            f'<path d="{path}" fill="none" stroke="{self.recording}" stroke-width="{num(self.inner_w * 1.036)}"'
            f' stroke-linecap="round" stroke-linejoin="round"/>',
        ])

    # --- animations ---------------------------------------------------------

    # The recording beat: the canonical aperture at three brightness levels, with
    # the light travelling outwards from the dot to the ring and back.
    #
    # ALPHA ONLY, and deliberately so. The designer cut's recording candidate
    # modulated the radii as well, and at 16 px two adjacent frames of that differ
    # by well under a device pixel -- it read as a flicker in the corner of the
    # screen rather than as a heartbeat. Brightness has no sub-pixel problem at
    # any size.
    #
    # The first and last frames are the same on purpose: the loop rests at the
    # bottom for two ticks, which is what makes it a heartbeat rather than a
    # metronome.
    DIM, MID, BRIGHT = 0.42, 0.70, 1.0
    RECORDING_LEVELS = [
        # (dot, ring)
        (DIM, DIM),
        (MID, DIM),
        (BRIGHT, MID),
        (MID, BRIGHT),
        (DIM, MID),
        (DIM, DIM),
    ]

    # The processing animation: the inner ring becomes a rotating dashed arc and
    # three bars pass a wave along. Six dashes, so the arc reads as motion rather
    # than as a broken ring, and one quarter segment of travel per frame, which is
    # what makes the loop seamless.
    PROCESSING_DASH_COUNT = 6
    PROCESSING_DASH_FRACTION = 0.42
    PROCESSING_BAR_HEIGHTS = [
        [0.878, 0.703, 0.514],
        [0.703, 0.878, 0.703],
        [0.514, 0.703, 0.878],
        [0.703, 0.514, 0.703],
    ]

    def recording_frame(self, index: int) -> str:
        dot, ring = self.RECORDING_LEVELS[index]
        return self.document([
            self.outer_ring(),
            self.circle(self.inner_r, stroke=self.recording, width=self.inner_w, opacity=ring),
            self.disc(self.center_r, self.recording, opacity=dot),
        ])

    def processing_frame(self, index: int) -> str:
        segment = 2.0 * math.pi * self.inner_r / self.PROCESSING_DASH_COUNT
        dash = segment * self.PROCESSING_DASH_FRACTION
        gap = segment - dash
        offset = -index * segment / PROCESSING_FRAME_COUNT
        arc = self.circle(
            self.inner_r, stroke=self.accent, width=self.inner_w,
            extra=(f'stroke-linecap="round" stroke-dasharray="{num(dash, 5)} {num(gap, 5)}"'
                   f' stroke-dashoffset="{num(offset, 5)}" transform="rotate(-90 {num(self.center)} {num(self.center)})"'))
        bar_w = self.inner_w * 0.986
        pitch = self.inner_r * 0.466
        bars = [
            self.bar(self.center + (column - 1) * pitch, self.center, bar_w,
                     self.inner_r * height, self.accent)
            for column, height in enumerate(self.PROCESSING_BAR_HEIGHTS[index])
        ]
        return self.document([self.outer_ring(), arc, *bars])

    # --- the suite ----------------------------------------------------------

    def suite(self) -> dict[str, str]:
        files = {
            "brand.svg": self.brand(),
            # Idle and the brand mark are deliberately the same drawing. Two
            # names because they are two ideas: one is the product's identity and
            # the other is a session state that happens to look like it.
            "idle.svg": self.brand(),
            "paused.svg": self.paused(),
            "saved.svg": self.saved(),
            "warning.svg": self.warning(),
            "error.svg": self.error(),
        }
        for index in range(len(self.RECORDING_LEVELS)):
            files[f"recording-f{index}.svg"] = self.recording_frame(index)
        for index in range(PROCESSING_FRAME_COUNT):
            files[f"processing-f{index}.svg"] = self.processing_frame(index)
        return files


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="report drift instead of writing; exits non-zero if the suite is stale")
    args = parser.parse_args()

    marks = Marks(json.loads(PARAMETERS.read_text(encoding="utf-8")))
    stale: list[str] = []
    for name, content in marks.suite().items():
        path = MARKS_DIR / name
        current = path.read_text(encoding="utf-8") if path.exists() else ""
        if current == content:
            continue
        if args.check:
            stale.append(name)
            continue
        path.write_text(content, encoding="utf-8", newline="\n")
        print(f"wrote {path.name}")

    if stale:
        print(f"stale: {', '.join(stale)}", file=sys.stderr)
        print(f"run: python {pathlib.Path(__file__).relative_to(REPO).as_posix()}", file=sys.stderr)
        return 1
    if args.check:
        print(f"{len(marks.suite())} marks match {PARAMETERS.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
