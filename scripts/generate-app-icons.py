#!/usr/bin/env python3
"""Generate ExoSnap application icons from the canonical brand-mark design.

The brand mark is the same 32x32 "aperture" design that app/ui/brand/BrandMarkWidget.cpp
paints in the running UI: a faint accent outer ring, a solid inner ring, and a centre dot.
The inner ring + dot change colour by state, exactly like the title-bar brand mark:

    idle      -> accent  #9BD9D2 (Studio Mint)   == ExoSnapPalette::kAccent
    recording -> coral   #E0786C                 == ExoSnapPalette::kErr
    paused    -> amber   #E6C57C                 == ExoSnapPalette::kWarn

The outer ring always stays accent at 0.45 opacity. Geometry and stroke weights are kept in
1:1 sync with BrandMarkWidget so the window/taskbar icon matches the in-app mark.

Rendering is supersampled on a large canvas and downsampled (LANCZOS) into a multi-resolution
.ico so the thin strokes stay crisp from 16 px to 256 px.

Usage:  python scripts/generate-app-icons.py
Output: app/assets/brand/exosnap-logo-{idle,recording,paused}.ico  (+ .png previews)
"""

from __future__ import annotations

import pathlib

from PIL import Image, ImageDraw

# --- design constants (32x32 grid, mirror of BrandMarkWidget) ---------------------------------
DESIGN = 32.0
CENTER = DESIGN / 2.0  # (16, 16)

ACCENT = (0x9B, 0xD9, 0xD2)   # kAccent  — idle
CORAL = (0xE0, 0x78, 0x6C)    # kErr     — recording
AMBER = (0xE6, 0xC5, 0x7C)    # kWarn    — paused

OUTER_R, OUTER_W, OUTER_ALPHA = 14.5, 1.5, 0.45
INNER_R, INNER_W = 6.2, 1.6
DOT_R = 2.4

# Inset the whole mark inside the icon canvas. BrandMarkWidget fills the 32x32 grid edge to
# edge (good inline in the UI), but as a standalone taskbar / alt-tab icon the mark needs a
# little breathing room. 0.88 keeps the design proportions while adding a modest margin.
CONTENT_SCALE = 0.88

MASTER = 1024  # supersample canvas
ICO_SIZES = [16, 24, 32, 48, 64, 128, 256]

OUT_DIR = pathlib.Path(__file__).resolve().parent.parent / "app" / "assets" / "brand"
VARIANTS = {
    "exosnap-logo-idle": ACCENT,
    "exosnap-logo-recording": CORAL,
    "exosnap-logo-paused": AMBER,
}


def _ring(draw: ImageDraw.ImageDraw, scale: float, radius: float, width: float, rgba) -> None:
    """Stroke a circle centred on `radius` (SVG semantics: stroke centred on the path)."""
    outer = radius + width / 2.0
    bbox = [
        (CENTER - outer) * scale,
        (CENTER - outer) * scale,
        (CENTER + outer) * scale,
        (CENTER + outer) * scale,
    ]
    draw.ellipse(bbox, outline=rgba, width=max(1, round(width * scale)))


def _disc(draw: ImageDraw.ImageDraw, scale: float, radius: float, rgba) -> None:
    bbox = [
        (CENTER - radius) * scale,
        (CENTER - radius) * scale,
        (CENTER + radius) * scale,
        (CENTER + radius) * scale,
    ]
    draw.ellipse(bbox, fill=rgba)


def render_master(inner_rgb) -> Image.Image:
    img = Image.new("RGBA", (MASTER, MASTER), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    scale = MASTER / DESIGN

    cs = CONTENT_SCALE
    _ring(draw, scale, OUTER_R * cs, OUTER_W * cs, ACCENT + (round(255 * OUTER_ALPHA),))
    _ring(draw, scale, INNER_R * cs, INNER_W * cs, inner_rgb + (255,))
    _disc(draw, scale, DOT_R * cs, inner_rgb + (255,))
    return img


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for stem, inner in VARIANTS.items():
        master = render_master(inner)
        frames = [master.resize((s, s), Image.Resampling.LANCZOS) for s in ICO_SIZES]
        ico_path = OUT_DIR / f"{stem}.ico"
        frames[-1].save(ico_path, format="ICO", sizes=[(s, s) for s in ICO_SIZES],
                        append_images=frames[:-1])
        print(f"wrote {ico_path.name} ({ico_path.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
