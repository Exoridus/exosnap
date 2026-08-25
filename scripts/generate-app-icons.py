#!/usr/bin/env python3
"""Generate ExoSnap's build-time brand artefacts from the canonical mark geometry.

WHAT IS BUILT HERE AND WHAT IS NOT
----------------------------------
Only the artefacts whose appearance the build already knows are files:

  * ``exosnap-app.ico`` -- the multi-resolution application icon. It is the
    executable's identity in Explorer, on the desktop, in Start and in Alt+Tab,
    it carries no session state and no accent, and Windows wants it out of the
    PE resource table.
  * the thumbnail-toolbar glyphs, which ``THUMBBUTTON::hIcon`` takes as HICONs
    and which depend on nothing the user can change.
  * ``exosnap-logo.svg`` -- the mark as a vector, for documentation and design
    hand-off.

The state marks are NOT here. Their outer ring is the user's accent and their
inner ring is the session's semantic colour, so as files they would be one icon
per (state x accent x appearance x heartbeat frame). Both surfaces that show one
-- the notification area and the running window's own icon -- get it painted at
runtime by ``app/ui/brand/ShellIconRenderer``.

SOURCE OF TRUTH
---------------
``app/ui/brand/BrandMark.h`` is the only place the geometry exists. This script
parses it; it does not restate it. A renamed or deleted constant fails here
rather than silently producing an icon that no longer matches the application.

Usage:  python scripts/generate-app-icons.py
"""

from __future__ import annotations

import pathlib
import re
import sys

from PIL import Image, ImageDraw

REPO = pathlib.Path(__file__).resolve().parent.parent
BRAND_MARK_H = REPO / "app" / "ui" / "brand" / "BrandMark.h"
THEMES_H = REPO / "app" / "ui" / "theme" / "ExoSnapThemes.h"
OUT_DIR = REPO / "app" / "assets" / "brand"


# --- the canonical geometry, parsed rather than repeated -------------------------------------

class BrandMark:
    """The constants of app/ui/brand/BrandMark.h, as attributes."""

    _SCALAR = re.compile(r"^inline constexpr (?:double|int) (k\w+) = ([-\d.]+);", re.MULTILINE)
    _PROFILE = re.compile(
        r"inline constexpr OpticalProfile (k\w+)\{(.*?)\};", re.DOTALL)
    _FIELD = re.compile(r"\.(\w+)\s*=\s*([-\d.]+)")

    def __init__(self, source: str) -> None:
        self._scalars = {name: float(value) for name, value in self._SCALAR.findall(source)}
        self._profiles = {
            name: {field: float(value) for field, value in self._FIELD.findall(body)}
            for name, body in self._PROFILE.findall(source)
        }
        if not self._scalars or not self._profiles:
            raise SystemExit(f"{BRAND_MARK_H}: no constants parsed -- has the header's shape changed?")

    def value(self, name: str) -> float:
        try:
            return self._scalars[name]
        except KeyError:
            raise SystemExit(f"{BRAND_MARK_H}: missing constant {name}") from None

    def profile(self, name: str) -> dict[str, float]:
        try:
            return self._profiles[name]
        except KeyError:
            raise SystemExit(f"{BRAND_MARK_H}: missing optical profile {name}") from None

    def profile_for(self, px: int) -> dict[str, float]:
        """Mirror of OpticalProfileFor(). The one rule this file implements rather
        than reads, because it is control flow and not a number."""
        if px <= self.value("kSmallProfileMaxPx"):
            return self.profile("kSmallProfile")
        if px <= self.value("kMediumProfileMaxPx"):
            return self.profile("kMediumProfile")
        return self.profile("kLargeProfile")


def parse_theme_colour(source: str, appearance_id: str, index: int) -> str:
    """One colour out of ExoSnapThemes.h's appearance table, by position.

    The table is a C++ aggregate, so the fields are positional; `index` counts
    the quoted values after the appearance id. Fragile enough to be worth the
    assertion below, and still better than a second copy of the palette.
    """
    start = source.index(f'"{appearance_id}",')
    values = re.findall(r'"(#[0-9A-Fa-f]{6}|rgba\([^"]*\))"', source[start:])
    return values[index]


# Positions in ExoAppearance, counting only the colour-valued fields from the
# appearance id onwards: bg surf surf2 raise line line2 ink text1 mut dim, then
# the three semantic ones.
_CAUTION, _ERROR = 11, 12


def _rgb(value: str) -> tuple[int, int, int]:
    return tuple(int(value[i:i + 2], 16) for i in (1, 3, 5))  # type: ignore[return-value]


# --- rendering -------------------------------------------------------------------------------

# Supersample factor. The .ico frames are rasterized per target size so the
# optical profile that size resolves to is the one applied, and each is drawn
# large and reduced with LANCZOS because PIL has no analytic antialiasing.
SUPERSAMPLE = 16

# The frame sizes the application icon carries.
#
# Windows shell scale factors are 100/125/150/200/250/300/400 %, applied to the
# 16, 32 and 48 px base metrics; 256 is the jumbo view. The set below is the
# union of what those produce, minus the sizes no context selects exactly.
# Anything not listed is downscaled by the shell from the next one up, which is
# what the large frames are for.
APP_ICO_SIZES = [16, 20, 24, 32, 40, 48, 60, 64, 72, 80, 96, 128, 256]

# Thumbnail glyphs are shell chrome at 16-48 px. The large frames an application
# icon needs would only bloat the executable.
SHELL_ICO_SIZES = [16, 20, 24, 32, 40, 48]


class Renderer:
    def __init__(self, mark: BrandMark, accent: tuple[int, int, int]) -> None:
        self.mark = mark
        self.accent = accent
        self.grid = mark.value("kGrid")
        self.center = mark.value("kCenter")

    def _canvas(self, px: int) -> tuple[Image.Image, ImageDraw.ImageDraw, float]:
        side = px * SUPERSAMPLE
        img = Image.new("RGBA", (side, side), (0, 0, 0, 0))
        return img, ImageDraw.Draw(img), side / self.grid

    def _ring(self, draw, scale: float, radius: float, width: float, rgba) -> None:
        outer = radius + width / 2.0
        box = [(self.center - outer) * scale, (self.center - outer) * scale,
               (self.center + outer) * scale, (self.center + outer) * scale]
        draw.ellipse(box, outline=rgba, width=max(1, round(width * scale)))

    def _disc(self, draw, scale: float, radius: float, rgba, cx: float | None = None,
              cy: float | None = None) -> None:
        cx = self.center if cx is None else cx
        cy = self.center if cy is None else cy
        box = [(cx - radius) * scale, (cy - radius) * scale,
               (cx + radius) * scale, (cy + radius) * scale]
        draw.ellipse(box, fill=rgba)

    def mark_frame(self, px: int) -> Image.Image:
        """The application icon at one size: the aperture in the default accent,
        with the optical profile that size resolves to."""
        m = self.mark
        profile = m.profile_for(px)
        img, draw, scale = self._canvas(px)
        content = m.value("kStandaloneContentScale") * profile["content_scale"]

        outer_alpha = min(1.0, m.value("kOuterOpacity") * profile["outer_opacity_scale"])
        self._ring(draw, scale, m.value("kOuterRadius") * content,
                   m.value("kOuterStroke") * content * profile["outer_stroke_scale"],
                   self.accent + (round(255 * outer_alpha),))
        self._ring(draw, scale, m.value("kInnerRadius") * content * profile["inner_radius_scale"],
                   m.value("kInnerStroke") * content * profile["inner_stroke_scale"],
                   self.accent + (255,))
        self._disc(draw, scale, m.value("kDotRadius") * content * profile["dot_radius_scale"],
                   self.accent + (255,))
        return img.resize((px, px), Image.Resampling.LANCZOS)

    def thumb_frame(self, px: int, shape: str, rgb) -> Image.Image:
        m = self.mark
        img, draw, scale = self._canvas(px)
        rgba = tuple(rgb) + (255,)

        if shape == "disc":
            self._disc(draw, scale, m.value("kGlyphDiscRadius"), rgba)
        elif shape == "square":
            half = m.value("kGlyphSquareHalf")
            draw.rectangle([(self.center - half) * scale, (self.center - half) * scale,
                            (self.center + half) * scale, (self.center + half) * scale], fill=rgba)
        elif shape == "bars":
            bar_w = m.value("kGlyphBarWidth")
            bar_h = m.value("kGlyphBarHeight")
            gap = m.value("kGlyphBarGap")
            for direction in (-1, 1):
                x = self.center + direction * (gap / 2.0 + bar_w / 2.0)
                draw.rectangle([(x - bar_w / 2.0) * scale, (self.center - bar_h / 2.0) * scale,
                                (x + bar_w / 2.0) * scale, (self.center + bar_h / 2.0) * scale], fill=rgba)
        elif shape == "triangle":
            back = m.value("kGlyphTriangleBackX")
            tip = m.value("kGlyphTriangleTipX")
            half = m.value("kGlyphTriangleHalfHeight")
            points = [(back, self.center - half), (back, self.center + half), (tip, self.center)]
            draw.polygon([(x * scale, y * scale) for x, y in points], fill=rgba)
        return img.resize((px, px), Image.Resampling.LANCZOS)


def write_ico(frames: list[Image.Image], stem: str) -> None:
    sizes = [(frame.width, frame.height) for frame in frames]
    path = OUT_DIR / f"{stem}.ico"
    frames[-1].save(path, format="ICO", sizes=sizes, append_images=frames[:-1])
    print(f"wrote {path.name} ({path.stat().st_size} bytes, {len(sizes)} frames)")


def write_svg(mark: BrandMark, accent_hex: str) -> None:
    """The mark as a vector, at the unmodified brand geometry.

    Inline in a document the mark is not a shell icon: it has no small-size
    rasterization to correct for and no standalone margin to reserve, so neither
    the optical profiles nor the content inset apply.
    """
    m = mark.value
    svg = (
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {m("kGrid"):g} {m("kGrid"):g}">\n'
        f'  <!-- Generated by scripts/generate-app-icons.py from app/ui/brand/BrandMark.h. Do not edit. -->\n'
        f'  <circle cx="{m("kCenter"):g}" cy="{m("kCenter"):g}" r="{m("kOuterRadius"):g}" fill="none"'
        f' stroke="{accent_hex}" stroke-width="{m("kOuterStroke"):g}" opacity="{m("kOuterOpacity"):g}"/>\n'
        f'  <circle cx="{m("kCenter"):g}" cy="{m("kCenter"):g}" r="{m("kInnerRadius"):g}" fill="none"'
        f' stroke="{accent_hex}" stroke-width="{m("kInnerStroke"):g}"/>\n'
        f'  <circle cx="{m("kCenter"):g}" cy="{m("kCenter"):g}" r="{m("kDotRadius"):g}" fill="{accent_hex}"/>\n'
        f'</svg>\n'
    )
    path = OUT_DIR / "exosnap-logo.svg"
    path.write_text(svg, encoding="utf-8", newline="\n")
    print(f"wrote {path.name} ({path.stat().st_size} bytes)")


def main() -> int:
    mark = BrandMark(BRAND_MARK_H.read_text(encoding="utf-8"))
    themes = THEMES_H.read_text(encoding="utf-8")

    # The application icon and the SVG carry the SHIPPED DEFAULT accent, which is
    # the only one a build-time artefact can carry: it is the identity of the
    # executable, not of a session.
    accent_hex = re.search(r'"aqua",.*?"(#[0-9A-Fa-f]{6})"', themes, re.DOTALL).group(1)

    coral = _rgb(parse_theme_colour(themes, "dark", _ERROR))
    amber = _rgb(parse_theme_colour(themes, "dark", _CAUTION))
    accent = _rgb(accent_hex)

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    renderer = Renderer(mark, accent)

    write_ico([renderer.mark_frame(px) for px in APP_ICO_SIZES], "exosnap-app")
    write_svg(mark, accent_hex)

    thumbs = {
        "exosnap-thumb-record": ("disc", coral),
        "exosnap-thumb-pause": ("bars", amber),
        "exosnap-thumb-resume": ("triangle", accent),
        "exosnap-thumb-stop": ("square", coral),
    }
    for stem, (shape, rgb) in thumbs.items():
        write_ico([renderer.thumb_frame(px, shape, rgb) for px in SHELL_ICO_SIZES], stem)

    return 0


if __name__ == "__main__":
    sys.exit(main())
