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
The mark's geometry lives in ``app/assets/brand/marks/brand.svg``, written by
``scripts/generate-brand-marks.py`` from ``marks/parameters.json``. The optical
corrections live in ``app/ui/brand/BrandMark.h``, because they are a property of
the raster rather than of the drawing, and the transport glyphs live there too --
they are shell chrome and the designer suite has no composition for them.

This script parses both; it restates neither. A renamed constant or a changed
asset shape fails here rather than silently producing an icon that no longer
matches the application.

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
MARKS_DIR = OUT_DIR / "marks"
BRAND_SVG = MARKS_DIR / "brand.svg"


# --- the canonical geometry, parsed rather than repeated -------------------------------------

class Circle:
    """One ``<circle>`` of the canonical mark, as the asset states it."""

    def __init__(self, attributes: dict[str, str]) -> None:
        self.r = float(attributes["r"])
        self.stroke = attributes.get("stroke")
        self.stroke_width = float(attributes.get("stroke-width", 0.0))
        self.fill = attributes.get("fill", "none")
        self.opacity = float(attributes.get("opacity", 1.0))


def parse_mark_circles(source: str) -> list[Circle]:
    """The brand mark's three circles, in paint order.

    A parse rather than a copy: the radii and weights are the asset's, and this
    script is the one place that has to agree with it. The assertion is the
    point -- a mark that stopped being three circles is a design change that has
    to be looked at here, not one that silently produces a wrong .ico.
    """
    circles = [Circle(dict(re.findall(r'([\w-]+)="([^"]+)"', body)))
               for body in re.findall(r"<circle ([^/]*)/>", source)]
    if len(circles) != 3:
        raise SystemExit(f"{BRAND_SVG}: expected 3 circles, found {len(circles)}")
    return circles


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
    def __init__(self, mark: BrandMark, circles: list[Circle]) -> None:
        self.mark = mark
        self.circles = circles
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
        """The application icon at one size: the canonical mark, with the optical
        profile that size resolves to.

        The colours are the asset's own. This icon is the identity of the FILE
        and carries no session and no user accent, so there is nothing here to
        resolve against a theme."""
        m = self.mark
        profile = m.profile_for(px)
        img, draw, scale = self._canvas(px)
        content = m.value("kStandaloneContentScale") * profile["content_scale"]

        for circle in self.circles:
            alpha = round(255 * min(1.0, circle.opacity * (
                profile["outer_opacity_scale"] if circle.opacity < 1.0 else 1.0)))
            if circle.stroke is not None:
                self._ring(draw, scale, circle.r * content,
                           circle.stroke_width * content * profile["stroke_scale"],
                           _rgb(circle.stroke) + (alpha,))
            else:
                self._disc(draw, scale, circle.r * content, _rgb(circle.fill) + (alpha,))
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


def write_svg(brand_svg: str) -> None:
    """The mark as a vector, for documentation, design hand-off and the package
    manifests that point at a raw URL.

    A copy of the canonical asset rather than a second drawing of it: what it is
    FOR is a stable path, and the shapes are already written down once.
    """
    path = OUT_DIR / "exosnap-logo.svg"
    path.write_text(brand_svg, encoding="utf-8", newline="\n")
    print(f"wrote {path.name} ({path.stat().st_size} bytes)")


def main() -> int:
    mark = BrandMark(BRAND_MARK_H.read_text(encoding="utf-8"))
    themes = THEMES_H.read_text(encoding="utf-8")
    brand_svg = BRAND_SVG.read_text(encoding="utf-8")

    # The asset is authored in the shipped default accent, which is the only one
    # a build-time artefact can carry: this is the identity of the executable,
    # not of a session. If the two ever part company that is a decision, so it is
    # asserted rather than papered over.
    accent_hex = re.search(r'"aqua",.*?"(#[0-9A-Fa-f]{6})"', themes, re.DOTALL).group(1)
    if accent_hex.upper() not in brand_svg.upper():
        raise SystemExit(f"{BRAND_SVG}: authored accent is not the shipped default {accent_hex}")

    coral = _rgb(parse_theme_colour(themes, "dark", _ERROR))
    amber = _rgb(parse_theme_colour(themes, "dark", _CAUTION))
    accent = _rgb(accent_hex)

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    renderer = Renderer(mark, parse_mark_circles(brand_svg))

    write_ico([renderer.mark_frame(px) for px in APP_ICO_SIZES], "exosnap-app")
    write_svg(brand_svg)

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
