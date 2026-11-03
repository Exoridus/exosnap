# Controller artwork sources

Each controller directory contains an editable `source.svg`, separately renderable
`elements/*.svg`, and a `mapping.json`. The catalog lists the available layouts.
The native widget and HTML viewer use the same SVG sources and JSON mappings.
Available artwork does not imply qualified support for a physical input device.
Steam Deck has full and compact views, with compact as the catalog default.
Steam Controller (2015) and Steam Controller (2026) are separate layouts.

Most artwork is adapted from AL2009man's Gamepad Asset Pack, under the MIT
license in [LICENSE.txt](LICENSE.txt), with ExoSnap contributors' revisions.
Generic uses a CC0 Xelu/Luna body with MIT controls. GameCube artwork retains
GPL-3.0-only terms. Steam Controller 2026 artwork uses CC BY-NC-SA 4.0 with
retained MIT component notices. Its noncommercial restriction applies to that
artwork; a free application download does not grant commercial artwork rights.
Original sources, attribution, changes and license terms for each exception
are documented in [SOURCE-NOTICE.txt](SOURCE-NOTICE.txt).
The application source code retains its own license.
Steam Controller 2026 remains release-blocked until suitable rights or documented
independent replacement geometry resolve its model-derived rear controls and panel seam.

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

## Logical controls

`mapping.json` is the artwork-to-control authority. SVG `id` values identify graphics only.
The 30 control IDs below follow the actual [SDL3 gamepad definitions](https://github.com/libsdl-org/SDL/blob/main/include/SDL3/SDL_gamepad.h).
The symbols specify correspondence, not numeric identity. No SDL library is required.

| Control IDs | SDL3 symbols |
| --- | --- |
| `south`, `east`, `west`, `north` | `SDL_GAMEPAD_BUTTON_SOUTH`, `SDL_GAMEPAD_BUTTON_EAST`, `SDL_GAMEPAD_BUTTON_WEST`, `SDL_GAMEPAD_BUTTON_NORTH` |
| `back`, `guide`, `start` | `SDL_GAMEPAD_BUTTON_BACK`, `SDL_GAMEPAD_BUTTON_GUIDE`, `SDL_GAMEPAD_BUTTON_START` |
| `left_shoulder`, `right_shoulder` | `SDL_GAMEPAD_BUTTON_LEFT_SHOULDER`, `SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER` |
| `left_stick_click`, `right_stick_click` | `SDL_GAMEPAD_BUTTON_LEFT_STICK`, `SDL_GAMEPAD_BUTTON_RIGHT_STICK` |
| `dpad_up`, `dpad_down`, `dpad_left`, `dpad_right` | `SDL_GAMEPAD_BUTTON_DPAD_UP`, `SDL_GAMEPAD_BUTTON_DPAD_DOWN`, `SDL_GAMEPAD_BUTTON_DPAD_LEFT`, `SDL_GAMEPAD_BUTTON_DPAD_RIGHT` |
| `misc1`, `misc2`, `misc3`, `misc4`, `misc5`, `misc6` | `SDL_GAMEPAD_BUTTON_MISC1`, `SDL_GAMEPAD_BUTTON_MISC2`, `SDL_GAMEPAD_BUTTON_MISC3`, `SDL_GAMEPAD_BUTTON_MISC4`, `SDL_GAMEPAD_BUTTON_MISC5`, `SDL_GAMEPAD_BUTTON_MISC6` |
| `left_paddle1`, `left_paddle2`, `right_paddle1`, `right_paddle2` | `SDL_GAMEPAD_BUTTON_LEFT_PADDLE1`, `SDL_GAMEPAD_BUTTON_LEFT_PADDLE2`, `SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1`, `SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2` |
| `touchpad` | `SDL_GAMEPAD_BUTTON_TOUCHPAD` |
| `left_stick` | Pair `SDL_GAMEPAD_AXIS_LEFTX`, `SDL_GAMEPAD_AXIS_LEFTY` |
| `right_stick` | Pair `SDL_GAMEPAD_AXIS_RIGHTX`, `SDL_GAMEPAD_AXIS_RIGHTY` |
| `left_trigger`, `right_trigger` | `SDL_GAMEPAD_AXIS_LEFT_TRIGGER`, `SDL_GAMEPAD_AXIS_RIGHT_TRIGGER` |

Paddle side means the player's hand, with 1 primary/upper and 2 secondary/lower.
DualSense Edge rear buttons use paddle1 and Fn buttons use paddle2. `misc1` includes device-specific
Share/Capture, Mute or Quick Access meanings. Pairing has no universal canonical button.
`touchpad` means the click, not contact or coordinates.

The Qt-free `GamepadState` and `ReadControl` expose these controls to every renderer.
`ControlValue` carries a status plus x/y. `Released` is a valid zero and `Active` a valid nonzero value.
`Unavailable` is missing usable data, whereas `Unsupported` means the provider/device has no such capability.
Only released/active values may drive graphics. Sticks are [-1, 1] with positive y up, triggers [0, 1],
and buttons 0/1. SDL stick Y increases downwards, so an SDL provider would invert Y while converting its integer
axis range. GameInput already uses positive y up. No SDL enum or browser index is a GameInput mask.

Schema version 1 adds optional `control` for a layer, `clickControl` for its independent click and
`controls` for the four keys `up`, `down`, `left`, `right` of a grouped directional pad.
For example, `btn-cross` can declare `control: "south"` while retaining its old GameInput `binding`.
A stick's `control` determines its movement, never its click mask. `clickControl: null` disables a click.
GameCube therefore uses A=south, X=east, B=west, Y=north, Z=right_shoulder, L/R=left/right_trigger,
with optional L/R clicks misc3/misc4 and no stick clicks.

A canonical field takes precedence over legacy binding interpretation. Explicit `binding: null` keeps a
layer inert even when a provider supplies the named control. `preview` does not change that rule.
Older mappings without canonical fields derive their controls from the central GameInput conversion.
For canonical mappings, omit a redundant binding rather than maintaining a second provider table.
This omission enables the canonical control, while explicit null remains the unsupported-input safeguard.
Device support is still determined by the input provider. Old GameInput masks remain compatibility metadata,
including where physical labels differ from canonical positions. A Joy-Con artwork pair does not aggregate devices.
The live GameInput provider does not qualify GameCube adapter wiring or separately reported trigger end clicks.

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
instead of adding a background fill. `pressColor` overrides the default blue pressure color.
`pressClip` further limits only the pressure fill and hit area, preserving fixed
contours that continue past the physical button.

Touch surfaces use normalized [0, 1] positions with Y increasing downwards. A
contact marker requires an actual contact state. Their null bindings remain
unqualified; the HTML review can simulate contact independently of stick values.
The old Steam Controller's pads show a contact marker only while contact is
reported or explicitly simulated. Stick output does not imply finger contact.
Steam Deck retains its two physical sticks and does not infer pad contact from
their values. Different Steam Input configurations can assign different pad
functions. Rear controls with null bindings remain unqualified.

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

The native atlas adapter normalizes these coordinates into its logical canvas,
prepares pressure variants and adapts clip paths to Qt-supported masks. It does
not modify these sources. The native widget uses the same atlas for preview and
recording.

`knockout` references an existing SVG geometry prepared as an immutable atlas tile.
Up to two masks follow their stick caps and subtract only body alpha in the GPU compositor.
The recording background, other overlays and normally rendered caps are preserved.
Rasterize the settled artwork resolution once and reuse the atlas while inputs change.
