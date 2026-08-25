# The ExoSnap mark suite

Every drawing the product shows for itself: the brand mark, the five session
states, and the two animations -- a six-frame recording beat and a four-frame
processing loop.

The recording frames modulate BRIGHTNESS only. The designer cut's candidate
moved the radii as well, and at 16 px two adjacent frames of that differ by
well under a device pixel: it read as a flicker rather than as a heartbeat,
which is why the shell used to play it twice and then stop. Brightness has no
sub-pixel problem, so the beat now runs for as long as the recording does.

## What is authoritative

`parameters.json` holds the aperture — five numbers, the two themes' outer-ring
opacities, and the reference palette. It is the only place those exist.

The `.svg` files are **generated** from it by `scripts/generate-brand-marks.py`
and checked in, because the runtime loads them out of Qt resources and a shape
change should be visible in a diff. Do not hand-edit one: the next run of the
script overwrites it, and `brand_geometry_tests` fails in the meantime.

```
python scripts/generate-brand-marks.py            # rewrite the suite
python scripts/generate-brand-marks.py --check     # report drift, change nothing
python scripts/generate-app-icons.py               # rebuild exosnap-app.ico and the logo
```

Changing a radius, a stroke weight or the spacing is therefore an edit to
`parameters.json` and two commands. The per-state compositions — the check, the
warning glyph, the pause bars, the dashed processing arc — are authored in the
generator, expressed against the aperture, so moving the inner ring moves what
sits inside it.

## Colours

The files carry the designer's reference palette:

| Value     | Role                                         |
| --------- | -------------------------------------------- |
| `#9BD9D2` | accent — the user's, whatever they picked     |
| `#E0786C` | recording / error                             |
| `#E7C875` | caution — paused, warning                     |
| `#8FD0AF` | success — saved                               |

None of them ships. `app/ui/brand/BrandMarkSvg.cpp` substitutes the running
theme's accent and semantic colours for each, and the outer ring's `0.64` for
the appearance's own value, before anything rasterizes the file. A colour that
is not in this table would ship unrecoloured, so the drift guard refuses one.

## What is NOT here

- The multi-resolution application icon and the thumbnail-toolbar glyphs, which
  are `.ico` files beside this directory: Windows reads them out of the PE
  resource table, and they carry no session and no accent.
- The optical corrections small rasters need, which are a property of the raster
  rather than of the drawing and live in `app/ui/brand/BrandMark.h`.
- Wordmarks, which are a separate asset and unrelated to this suite.
