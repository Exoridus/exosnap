#!/usr/bin/env python3
"""Generate ExoSnap aperture mark ICO files (idle + recording).

Design (Hybrid v3):
  - Background: #0E0E10 rounded-square tile on each size
  - Outer ring: Studio Mint #9BD9D2 at 45% opacity, 1.5px stroke on 32x32 grid
  - Inner ring: accent color (mint idle / coral recording), 1.6px stroke, r=6.2
  - Center dot: accent color filled, r=2.4

Output (relative to script location ../apps/exosnap/assets/brand/):
  exosnap-logo-idle.ico       -- 16, 32, 48, 64, 128, 256 px
  exosnap-logo-recording.ico  -- same sizes, coral inner ring + dot

Usage:
  python scripts/gen_app_icons.py
"""

import math
import os
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    print("Pillow not found. Install with: pip install Pillow", file=sys.stderr)
    sys.exit(1)

# ── Color constants ────────────────────────────────────────────────────────────
BG_COLOR       = (14,  14, 16, 255)   # #0E0E10
MINT_COLOR     = (155, 217, 210, 255) # #9BD9D2
MINT_DIM_COLOR = (155, 217, 210, int(0.45 * 255))  # outer ring
CORAL_COLOR    = (224, 120, 108, 255) # #E0786C

# ── Sizes to embed in the ICO ─────────────────────────────────────────────────
ICO_SIZES = [16, 32, 48, 64, 128, 256]

# Render at 4× and downscale for antialiasing.
SCALE = 4


def rounded_rect_mask(size: int, radius_frac: float = 0.18) -> Image.Image:
    """Creates a white-on-black rounded-square mask at `size`x`size`."""
    s = size * SCALE
    r = int(s * radius_frac)
    img = Image.new("L", (s, s), 0)
    draw = ImageDraw.Draw(img)
    draw.rounded_rectangle([(0, 0), (s - 1, s - 1)], radius=r, fill=255)
    return img


def draw_aperture(size: int, recording: bool) -> Image.Image:
    """Renders the aperture mark on a dark tile at `size`x`size` (hi-DPI, then downscaled)."""
    s = size * SCALE          # high-res canvas
    design = 32.0 * SCALE     # design grid 32px scaled up

    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    # -- Rounded dark background ------------------------------------------------
    r_bg = int(s * 0.18)
    draw.rounded_rectangle([(0, 0), (s - 1, s - 1)], radius=r_bg, fill=BG_COLOR)

    # Design coordinates (center on 32×32 design grid, scaled)
    cx = s / 2
    cy = s / 2
    # Scale factor from the design grid to actual pixels
    sf = s / design

    accent_color = CORAL_COLOR if recording else MINT_COLOR

    def design_to_px(d_val):
        return d_val * sf

    # -- Outer ring: accent dim, r=14.5, stroke=1.5 ----------------------------
    outer_r = design_to_px(14.5)
    outer_stroke = max(1, design_to_px(1.5))
    outer_color = (*MINT_COLOR[:3], int(0.45 * 255))
    # Draw as annulus: filled circle (outer_r) minus filled circle (outer_r - stroke)
    _draw_ring(draw, cx, cy, outer_r, outer_stroke, outer_color)

    # -- Inner ring: accent, r=6.2, stroke=1.6 ---------------------------------
    inner_r = design_to_px(6.2)
    inner_stroke = max(1, design_to_px(1.6))
    _draw_ring(draw, cx, cy, inner_r, inner_stroke, accent_color)

    # -- Center dot: accent, r=2.4 ---------------------------------------------
    dot_r = design_to_px(2.4)
    draw.ellipse(
        [cx - dot_r, cy - dot_r, cx + dot_r, cy + dot_r],
        fill=accent_color
    )

    # Downscale with Lanczos antialiasing
    return img.resize((size, size), Image.LANCZOS)


def _draw_ring(draw: ImageDraw.ImageDraw,
               cx: float, cy: float,
               radius: float, stroke: float,
               color: tuple) -> None:
    """Fills an annular ring by drawing the outer and inner circles in sequence."""
    outer_r = radius + stroke / 2
    inner_r = radius - stroke / 2
    # Outer disc
    draw.ellipse([cx - outer_r, cy - outer_r, cx + outer_r, cy + outer_r], fill=color)
    # Punch out inner disc with transparent
    if inner_r > 0:
        draw.ellipse([cx - inner_r, cy - inner_r, cx + inner_r, cy + inner_r],
                     fill=(0, 0, 0, 0))


def make_ico(recording: bool) -> list[Image.Image]:
    return [draw_aperture(s, recording) for s in ICO_SIZES]


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    brand_dir = os.path.join(script_dir, "..", "apps", "exosnap", "assets", "brand")
    os.makedirs(brand_dir, exist_ok=True)

    for recording, name in [(False, "exosnap-logo-idle.ico"), (True, "exosnap-logo-recording.ico")]:
        frames = make_ico(recording)
        out_path = os.path.join(brand_dir, name)
        # PIL saves ICO with all frames when given a list via `append_images`
        frames[0].save(
            out_path,
            format="ICO",
            sizes=[(s, s) for s in ICO_SIZES],
            append_images=frames[1:],
        )
        print(f"Wrote {out_path}  ({len(frames)} sizes: {ICO_SIZES})")


if __name__ == "__main__":
    main()
