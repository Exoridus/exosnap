# Settings — Default/Expert rework (design)

Date: 2026-07-26 · Status: awaiting review
Visual reference: the interactive Settings mockup in the local design-canon
workspace (`suite-settings.jsx`), iterated and screenshot-verified in both
modes on this date.

## Goal

Re-draw the Default/Expert split on the Settings page around an explicit
criterion, instead of gating by "looks advanced". Prosumers who are not format
experts must find every safe, useful control in Default; Expert mode is
reserved for controls that carry real risk or require format expertise.

## The gating criterion

A row is **Expert-only** when at least one of these holds:

1. Misconfiguration can produce incompatible or broken files
   (bit depth, color range, chroma subsampling, rate control).
2. It is meaningless without format expertise
   (NVENC preset, keyframe interval, Opus frame duration/complexity).
3. It protects pipeline integrity and disabling it can ruin a long recording
   (audio clock slaving).

Everything else is Default-visible, even when it looks technical. The Expert
toolbar hint and warning banner already state exactly this ("reveals
lower-level controls that can produce incompatible files") — the criterion
makes the UI text literally true.

## Page structure (unchanged foundations)

- Slim preset toolbar: `Preset` label · combo (dirty state renders as
  "Name (changed)" inside the combo text) · `Save as new…` (visible only while
  dirty) · one `…` overflow menu (Rename/Reset/Delete/Export/Import) ·
  Expert-mode toggle with info hint. No search box.
- Built-in presets are the four shipped ones: Default, Quality, Efficiency,
  Compatibility. Rename/Delete stay disabled for built-ins.
- Amber warning banner while Expert mode is on.
- Two fixed columns; each card is hard-assigned to a column and Expert only
  reveals rows in place. Cards never move when toggling Expert.
  - **Column 1 (left):** Container & codecs · Quality & timing · Audio ·
    Hotkeys.
  - **Column 2 (right):** Output · Webcam · Notifications & overlays ·
    Updates · Appearance · Developer (Expert-only).
  - Updates sits right (matches ConfigPage's own column consolidation);
    Developer sits right because it only exists in Expert mode — it cannot
    "teleport" on toggle, and Expert inflates the left column far more than
    the right. Verified: both modes end within ~150 px per column.
- One shared control width (160 px) for all selects/spinboxes/sliders,
  right-aligned to a common edge (ADR 0031). 160 is derived from the longest
  fixed label, "CQ 30 · Efficient". The microphone device select is the one
  exception: it spans its own sub-row under the source rows and takes the
  available width there (device strings are unbounded).
- One row height everywhere: 46 px (border-box), regardless of control kind —
  select/spinbox rows, toggle rows, slider rows, two-line label+subtext rows
  and button rows all land on the same height (content is centered; the
  tallest control, the 34 px select chrome, exactly fills it).
- Sliders are plain tracks exactly 160 px wide in the shared control column —
  no permanent side read-out. The current value appears as a small bubble
  above the thumb while hovering or dragging; sliders may carry tick markers
  (e.g. the 0 dB center of Mic gain — `QSlider::setTickPosition` in the app).
- en-US spelling app-wide: "color", not "colour" (labels, options, hints).
- Buttons share one visual family (pill radius) everywhere, including the
  per-row Set/Change/Cancel buttons on Hotkey rows.
- Dependent rows follow one pattern everywhere: a toggle/select row, then its
  detail rows rendered only while applicable — as plain full-width rows with
  normal label alignment, never indented (limiter → ceiling, chroma key →
  key color/tolerance/softness/spill, split toggles → interval/size, audio
  codec → bit depth/FLAC compression, output resolution → custom size,
  mic post-processing stages → their numeric parameter).

## Card inventory

### Container & codecs (left)

Default: Container (MKV/WebM/MP4) · Video codec (AV1/H.264/HEVC) · Audio codec
(Opus/AAC/PCM/FLAC) · **HDR handling** (Tone-map to SDR / Native HDR10; row
only rendered while an HDR-active display is detected).
Expert adds: Bit depth (8/10-bit) · Color range (Full/Limited) · Encoder
preset (NVENC P1–P7) · Keyframe interval (2 s/1 s/0.5 s) · Chroma subsampling
(4:2:0/4:4:4).

HDR handling moves to Default: HDR displays are mainstream, the row is already
display-conditional, and its default (tone-map) is the safe choice. Label is
"HDR handling" with options "Tone-map to SDR" / "Native HDR10".

### Quality & timing (left)

Default: Quality (five-tier ladder, below) · Frame rate (fixed list) · Frame
timing ("CFR · Constant" / "VFR · Variable") · Capture cursor toggle. Footer:
current-format line.
Expert: Quality row is replaced by Rate control (CQ/VBR/CBR) + Quality (CQ)
spinbox (1–51, no suffix) or Bitrate spinbox; Frame rate combo is replaced by
a free-entry fps spinbox (1–240) in the same row; adds Frame pacing
(Phase-correct / Lowest latency).

**Quality ladder (product change):** five tiers, CQ-first labels —
`CQ 35 · Draft`, `CQ 30 · Efficient`, `CQ 24 · Balanced`, `CQ 19 · High`,
`CQ 16 · Ultra`. This is an engine change: `NvencQualityPreset` gains two
values with canonical CQs 35 and 16. CQ 16 equals the built-in "Quality"
preset's value, so that preset's quality now renders as an exact tier instead
of an approximation. The top tier deliberately avoids the words "Best"
(vague) and "Quality" (collides with the built-in preset name).

**Frame rate (product change):** the Default list becomes 15/30/60/120 fps —
24 fps (cinema) and 25 fps (PAL) are dropped; 120 stays present but disabled
("unavailable") until a hardware-proven path exists. Expert mode swaps the
combo for a free-entry spinbox (1–240 fps, not capability-checked at entry),
analogous to the Quality→CQ swap. No "Custom" list item. When Expert is left
with a rate that is not in the list, the configured value is kept and the
Default combo displays the nearest list entry — the same rule
`NearestQualityPreset` already applies to a custom CQ — and the
current-format footer always shows the true value.

### Audio (left)

Source rows: Application audio · Computer audio · Microphone, in that order,
each with enable checkbox, dB read-out, "Merge with above" + info, VU meter;
APP row only while a window is the capture target. The FIRST listed source
shows neither the merge cluster (there is no "above" to fold into) nor a dB
read-out while it has no signal. Mic device combo (full sub-row width) +
rescan below the Microphone row.

Default (re-gated): Mic gain (−12…+12 dB slider with 0 dB center tick) ·
Mic channel mode (Auto/Mono mix/Preserve stereo/L → Stereo/R → Stereo) ·
Audio bitrate (32–510 kbps) · Channels (Stereo/Mono) · Bit depth + FLAC
compression (codec-conditional: PCM/FLAC only) · Brickwall limiter toggle
(+ Limiter ceiling while on) · **Microphone post-processing as a flat,
always-visible labelled section** — High-pass filter, Noise gate, AGC and
Noise suppression (RNNoise) as ordinary toggle rows, each stage's numeric
parameter row (HPF cutoff, Gate threshold, AGC target level) rendered as a
plain full-width row only while that stage is on. This replaces today's
chevron disclosure + indented checkboxes deliberately.
Expert adds (in place): Opus frame duration · Opus complexity · Sample rate ·
Audio clock slaving.

### Hotkeys (left)

Unchanged from the shipped behavior: single-line rows with one fixed-position
state slot (chord chip with inline ×, "Press keys…", "Not set", amber
"In use" on rejected conflicts), per-row Set/Change/Cancel, "Reset all" in
the card header. Unset rows recede visually.

### Output (right)

Default: Output resolution (Native/4K/1440p/1080p/720p/Custom; Custom reveals
width×height) · **Automatic split** section (Split recording toggle →
interval row with exactly the shipped options Every 15/30/60 min; Split by
size toggle → size row in MB; whichever limit hits first) ·
Destination folder · Filename pattern editor (token chips, live "Saves as"
footer) · Open editor when finished.
Expert adds: nothing. Automatic split moves to Default: plain file
management, cannot hurt a recording.

### Webcam (right)

Never Expert-gated (unchanged). Hero row: Record-webcam toggle + inline live
preview (128×80) with a rescan affordance inside the preview corner. Camera ·
Resolution/FPS · Mirror image · Overlay opacity (slider) · Chroma key as a
plain toggle row; while on it reveals Key color (a picker-style chip showing
swatch + hex — always at full strength, never dimmed by the toggle state),
Tolerance, Softness and Spill reduction (sliders).

### Notifications & overlays (right)

Unchanged: Recording overlay · Diagnostics overlay · Notifications · Tray
behavior · Quick controls · Present/tearing/latency diagnostics (needs
administrator).

### Updates (right)

Unchanged content: auto-check toggle · Update channel (Stable/Preview) ·
update control (Check → Update in place) · verified-install footnote.

### Appearance (right)

Unchanged: four curated themes (two dark, two light), thumbnail grid.

### Developer (right, Expert-only card)

The single whole-card Expert exception: Developer logging level · NVTX /
profiling markers (planned, disabled) · Send crash reports automatically.

## Summary of behavior changes vs. shipped ConfigPage

1. Re-gate to Default: mic gain, mic channel mode, audio bitrate, channels,
   audio bit depth + FLAC compression (codec-conditional), brickwall limiter
   + ceiling, microphone post-processing, automatic split, HDR handling.
2. Frame-rate list becomes 15/30/60/120 (drop 24/25); Expert swaps the combo
   for a free fps spinbox (1–240) — new capability.
3. Quality ladder grows to five tiers with CQ-first labels (engine:
   two new `NvencQualityPreset` values, CQ 35 and CQ 16).
4. Frame timing options are relabelled "CFR · Constant" / "VFR · Variable".
5. Auto-split and mic post-processing detail controls render only while
   their parent toggle/stage is on.
6. Updates card moves to the right column; Developer card renders in the
   right column.
7. Shared control width 160 px and shared row height 46 px across the page.
8. Microphone post-processing flattens from a chevron disclosure with
   indented checkboxes into an always-visible labelled section of standard
   toggle rows.
9. Chroma key becomes a plain toggle row; the key color moves to its own
   picker-chip detail row (replacing the color-name badge strip).
10. Sliders lose their permanent side read-out; value shows as a hover/drag
    bubble above the thumb, with optional tick markers.
11. The first listed audio source row hides the "Merge with above" cluster
    and shows no dB read-out while signal-less.
12. en-US spelling app-wide: every "colour" label/hint becomes "color".

Each of these requires the matching `docs/product-spec.md` update and
ConfigPage/test changes; the quality ladder and free fps entry additionally
touch recorder_core.

## Out of scope

- Enabling 120 fps (stays a disabled item until proven).
- Webcam device auto-refresh (`QMediaDevices::videoInputsChanged` listener) —
  separate engine follow-up; the rescan affordance stays manual.
- A settings search box (evaluated and rejected — Default's inventory is
  small enough, Expert reveals in place).
- Any change to preset storage, import/export, or the preset toolbar's
  behavior.

## Testing notes

- ConfigPage widget tests: expert-gating visibility per row (new mapping),
  frame-rate option list, quality-tier labels/count, frame-timing labels,
  conditional detail rows (split, limiter, mic stages, codec-conditional
  audio rows), column/card placement, preset toolbar unchanged-behavior
  regression.
- recorder_core tests: new quality tiers map to canonical CQs; free fps value
  round-trips through preset save/load and is clamped 1–240.
- Visual: `--visual-test` render states for Settings Default and Expert after
  the port.
