# Controller artwork sources

Each controller directory contains an editable `source.svg`, separately renderable
`elements/*.svg`, and a `mapping.json`. The catalog lists the available layouts.
These assets are artwork previews. They do not expand the native application's
qualified device support. Steam Deck uses its full device view. The Steam Controller artwork represents
the 2015 model, not the separate 2026 hardware.

The source artwork is adapted from AL2009man's Gamepad Asset Pack, under the MIT
license in [LICENSE.txt](LICENSE.txt). The pack's additional credits are retained
in [SOURCE-NOTICE.txt](SOURCE-NOTICE.txt). All catalogued layouts use this pack.
Generic derives from its Xbox Series geometry with blank button surfaces and no
vendor logo. Other artwork outside this catalog has separate provenance.

## Coordinates and layers

All element SVGs retain their controller's full `viewBox`. Compose them at the
same origin in mapping order. For controls, `bounds` describes the untransformed visible
element in these shared SVG coordinates. It excludes antialiasing and shadow
padding. The body bounds cover the full canvas. A renderer that crops an element must keep that offset when placing it.

`body.svg` contains static geometry only. Movable stick caps are separate files,
not copies baked into the body. Fixed wells and surrounds may remain in the
body. Stick `motion.center` and `travelRadius` use the same SVG coordinate units.
Input Y is positive upwards, while SVG Y is positive downwards. Apply the radial
display deadzone without changing captured input values.

Fixed internal contours and stick wells use the `detail` class in the editable
source. Their strokes are 50% of the regular width and 40% opaque. Filled contour
rings retain their geometry at 40% opacity. Outer silhouettes and movable
controls retain their stronger contrast.

Button masks use `GameInputGamepadButtons`. Face-button bindings follow physical
position, including Nintendo's different letter arrangement. Auxiliary fields
are device-dependent. A null binding means no qualified input mapping, not a
permanently released input. A Joy-Con artwork pair does not imply automatic
device aggregation. Grouped directional pads name their four independent masks.

Horizontal single Joy-Cons have their own mappings. SL/SR map to the normalized
left/right shoulder buttons, face inputs follow their rotated physical positions,
and the single stick supplies the normalized left-stick axes. Original L/R/ZL/ZR
controls remain separate and unqualified. `controlLabels` retains the physical
letter or symbol when it differs from the usual layout. This mapping is not proof
that a particular driver exposes a Joy-Con as a standard gamepad.

`visible: false` hides an unsupported control while retaining its editable source
and mapping. Renderers must not offer a hidden control as an active input.

`pressElement` identifies an explicit pressure surface when a symbol alone is
not enough to define the whole button. `clip` isolates a component in shared SVG
coordinates. The element export includes the same clip. A renderer must apply
that region to its highlight and hit area as well as to the idle artwork.
`pressMode: "tint"` recolors the complete `pressElement` itself while pressed,
instead of adding a background fill. PlayStation logo buttons use this mode.
`pressClip` further limits only the pressure fill and hit area, preserving fixed
contours that continue past the physical button.

Touch surfaces use normalized [0, 1] positions with Y increasing downwards. A
contact marker requires an actual contact state. Their null bindings remain
unqualified; the HTML review can simulate contact independently of stick values.
The old Steam Controller's pads show a contact marker only while contact is
reported or explicitly simulated. Stick output does not imply finger contact.
Steam Deck retains its two physical sticks and does not infer pad contact from
their values. Different Steam Input configurations can assign different pad
functions. Rear controls not shown by these front views are not qualified.

## Interactive artwork preview

Serve the repository over HTTP and open
[the artwork viewer](../preview/index.html). It reads these sources and mappings,
supports simulated controls and the browser Gamepad API, and can show the static
body alone. Browser input is not a qualification of native GameInput support.

## Editing and optimization

Edit `source.svg` and retain its semantic element IDs. Export the static body
with every mapped element removed, and export each element with its ancestor
transforms intact. Recalculate bounds and stick movement geometry after changing
positions or proportions. Preserve drawing order where parts overlap.

Keep element exports unoptimized while editing geometry. Optimize settled
exports with SVGO 4.1.0 and
[svgo.config.mjs](svgo.config.mjs). The configuration retains semantic IDs and
uses conservative path merging. Keep the editable source separate from the
optimized exports. Multiple paths remain appropriate when fill rules, clipping
or strokes differ. Do not force all artwork into one path or merge moving
controls into the body. Compare rendered source and exports after optimization.

Generate all mapped layers from the unoptimized sources with:

```powershell
cargo exo-dev generate-gamepad-assets
cargo exo-dev generate-gamepad-assets --check
```

For optimized exports, install SVGO 4.1.0 in a development tool directory and pass
its `node_modules/svgo/bin/svgo.js` path with `--svgo`. Node.js must be available.
Use the same option with `--check` to check optimized output. The command stages
optimization in a temporary directory, verifies the mapped IDs, and leaves
`source.svg`, JSON mappings and licensing files unchanged. It does not download
tools or optimize during application startup or frame rendering.

The contrast shadow is a composition option. It is not baked into these sources
or the transparent body. Rasterize the settled artwork resolution once and
reuse the atlas while moving sticks or changing input states.
