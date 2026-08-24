#!/usr/bin/env python3
"""Generate ExoSnap application icons from the canonical brand-mark design.

The brand mark is the same 32x32 "aperture" design that app/ui/brand/BrandMarkWidget.cpp
paints in the running UI: a faint accent outer ring, a solid inner ring, and a centre dot.
The inner ring + dot change colour by state, exactly like the title-bar brand mark:

    idle      -> accent  #9BD9D2 (Studio Mint)   == ExoSnapPalette::kAccent
    recording -> coral   #E0786C                 == ExoSnapPalette::kErr
    paused    -> amber   #E6C57C                 == ExoSnapPalette::kWarn
    saved     -> green   #84CBA2                 == the dark theme's `success`

Three further families come out of the same geometry, so the shell surfaces cannot
drift from the mark:

  * recording pulse frames -- the recording mark with the inner ring and dot at a
    lower opacity. Windows has no animated-icon API, so the tray heartbeat is a
    timer swapping these static frames. The peak frame IS exosnap-logo-recording.
  * taskbar overlay badges -- a filled disc, not the aperture. The badge is drawn
    into the corner of the taskbar button at roughly a third of its size, where
    the mark's thin rings are mush; a disc in the state colour is not.
  * thumbnail toolbar glyphs -- the transport controls Windows draws under the
    taskbar thumbnail.

The outer ring always stays accent at 0.45 opacity. Geometry and stroke weights are kept in
1:1 sync with BrandMarkWidget so the window/taskbar icon matches the in-app mark.

Rendering is supersampled on a large canvas and downsampled (LANCZOS) into a multi-resolution
.ico so the thin strokes stay crisp from 16 px to 256 px.

Usage:  python scripts/generate-app-icons.py
Output: app/assets/brand/exosnap-{logo,badge,thumb}-*.ico
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
GREEN = (0x84, 0xCB, 0xA2)    # success  — saved
INK = (0x0E, 0x0E, 0x10)      # the page ground, used as a knockout on the badges

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
    "exosnap-logo-saved": GREEN,
}

# The recording heartbeat, as opacity of the inner ring and dot. The peak is the
# plain recording mark, so only the two lower steps are separate files; the frame
# order the shell plays is trough, mid, peak, mid (models/RecordingPulse.h).
#
# Opacity rather than scale: at 16 px a scale pulse of the 2.4-unit dot moves it by
# well under a pixel and reads as noise. The outer ring is deliberately left alone
# so the mark never looks like it is disappearing.
PULSE_FRAMES = {
    "exosnap-logo-recording-p0": 0.40,
    "exosnap-logo-recording-p1": 0.72,
}

# The badges and the thumbnail glyphs are shell chrome at 16-48 px. The big layers
# an application icon needs would only bloat the executable.
SHELL_ICO_SIZES = [16, 20, 24, 32, 40, 48]

BADGE_R = 13.0        # a nearly full-bleed disc: the badge IS the shape
BADGE_RIM_W = 2.0     # dark rim, so the badge reads on a light taskbar too
# The dimmed recording badge. Darkened rather than made transparent: the badge sits
# on top of the application icon, and a translucent one would show it through.
BADGE_DIM = 0.62


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


def render_master(inner_rgb, inner_alpha: float = 1.0) -> Image.Image:
    img = Image.new("RGBA", (MASTER, MASTER), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    scale = MASTER / DESIGN

    cs = CONTENT_SCALE
    inner = inner_rgb + (round(255 * inner_alpha),)
    _ring(draw, scale, OUTER_R * cs, OUTER_W * cs, ACCENT + (round(255 * OUTER_ALPHA),))
    _ring(draw, scale, INNER_R * cs, INNER_W * cs, inner)
    _disc(draw, scale, DOT_R * cs, inner)
    return img


def _darken(rgb, factor: float):
    return tuple(round(channel * factor) for channel in rgb)


def render_badge_master(rgb, glyph: str) -> Image.Image:
    """The taskbar overlay badge: a filled disc with a dark rim and a knockout glyph."""
    img = Image.new("RGBA", (MASTER, MASTER), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    scale = MASTER / DESIGN

    _disc(draw, scale, BADGE_R, rgb + (255,))
    _ring(draw, scale, BADGE_R - BADGE_RIM_W / 2.0, BADGE_RIM_W, INK + (255,))

    knockout = INK + (255,)
    if glyph == "pause":
        bar_w, bar_h, gap = 2.2, 10.0, 2.6
        for direction in (-1, 1):
            x = CENTER + direction * (gap / 2.0 + bar_w / 2.0)
            draw.rectangle(
                [(x - bar_w / 2.0) * scale, (CENTER - bar_h / 2.0) * scale,
                 (x + bar_w / 2.0) * scale, (CENTER + bar_h / 2.0) * scale],
                fill=knockout,
            )
    elif glyph == "check":
        # Two strokes rather than a font glyph: a font would have to be present on
        # whichever machine runs this script.
        points = [(10.6, 16.4), (14.4, 20.2), (21.6, 12.2)]
        draw.line([(x * scale, y * scale) for x, y in points],
                  fill=knockout, width=round(3.0 * scale), joint="curve")
    return img


def render_thumb_master(shape: str, rgb) -> Image.Image:
    """A thumbnail-toolbar transport glyph, drawn on the same 32-unit grid."""
    img = Image.new("RGBA", (MASTER, MASTER), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    scale = MASTER / DESIGN
    rgba = rgb + (255,)

    if shape == "disc":
        _disc(draw, scale, 8.0, rgba)
    elif shape == "square":
        half = 7.0
        draw.rectangle([(CENTER - half) * scale, (CENTER - half) * scale,
                        (CENTER + half) * scale, (CENTER + half) * scale], fill=rgba)
    elif shape == "bars":
        bar_w, bar_h, gap = 3.6, 15.0, 3.4
        for direction in (-1, 1):
            x = CENTER + direction * (gap / 2.0 + bar_w / 2.0)
            draw.rectangle([(x - bar_w / 2.0) * scale, (CENTER - bar_h / 2.0) * scale,
                            (x + bar_w / 2.0) * scale, (CENTER + bar_h / 2.0) * scale], fill=rgba)
    elif shape == "triangle":
        points = [(11.5, 8.0), (11.5, 24.0), (23.5, 16.0)]
        draw.polygon([(x * scale, y * scale) for x, y in points], fill=rgba)
    return img


def write_ico(master: Image.Image, stem: str, sizes) -> None:
    frames = [master.resize((s, s), Image.Resampling.LANCZOS) for s in sizes]
    ico_path = OUT_DIR / f"{stem}.ico"
    frames[-1].save(ico_path, format="ICO", sizes=[(s, s) for s in sizes],
                    append_images=frames[:-1])
    print(f"wrote {ico_path.name} ({ico_path.stat().st_size} bytes)")


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for stem, inner in VARIANTS.items():
        write_ico(render_master(inner), stem, ICO_SIZES)

    for stem, opacity in PULSE_FRAMES.items():
        write_ico(render_master(CORAL, inner_alpha=opacity), stem, SHELL_ICO_SIZES)

    badges = {
        "exosnap-badge-recording": (CORAL, ""),
        "exosnap-badge-recording-dim": (_darken(CORAL, BADGE_DIM), ""),
        "exosnap-badge-paused": (AMBER, "pause"),
        "exosnap-badge-saved": (GREEN, "check"),
    }
    for stem, (rgb, glyph) in badges.items():
        write_ico(render_badge_master(rgb, glyph), stem, SHELL_ICO_SIZES)

    thumbs = {
        "exosnap-thumb-record": ("disc", CORAL),
        "exosnap-thumb-pause": ("bars", AMBER),
        "exosnap-thumb-resume": ("triangle", ACCENT),
        "exosnap-thumb-stop": ("square", CORAL),
    }
    for stem, (shape, rgb) in thumbs.items():
        write_ico(render_thumb_master(shape, rgb), stem, SHELL_ICO_SIZES)


if __name__ == "__main__":
    main()
