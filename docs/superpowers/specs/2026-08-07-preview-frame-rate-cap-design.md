# Preview frame rate cap

Status: approved 2026-08-07.

## Problem

The Record page's live preview (`DxgiPreviewRenderer`) always presents at a hardcoded 60 fps,
both idle (before recording starts, driven by its own WGC capture) and during recording (pushed
mode, fed by the engine's WYSIWYG source-tap — ADR 0040). The interval comes from
`RecordPage::primaryRecorderConfig()` (`RecordPage.cpp:295-306`), a fixed baseline stub that has
nothing to do with the user's actual configuration. There is no way to lower this, and no way to
turn the preview off, even though nothing about it is required for the recording itself to work —
found live while investigating why the idle app measurably uses CPU on the Record page.

## Target

A new "Preview frame rate" row in Settings → Quality & timing, directly under the existing
"Frame rate" row: a combo box, `Off / 15 / 30 / 60 / 120 fps`, default **60 fps**. Same
display-refresh gating the recording Frame rate row already has — 120 fps is listed but disabled
(with a tooltip) when no attached display can feed it.

The cap applies uniformly, idle and during recording:

- **15/30/60/120**: the preview render thread's present interval is derived from this value
  instead of the hardcoded 60. Lower values mean fewer Present() calls and less composition work
  per second — nothing else about the preview changes.
- **Off**: neither the idle WGC capture (`DxgiPreviewRenderer::StartCapture`) nor the pushed-only
  WYSIWYG render thread (`StartPushedOnly`) is started at all. `PreviewSurface` shows a calm
  static placeholder — "Preview off" — in the same visual language as the existing "Preview
  unavailable" state (4:4:4 Expert clips in the editor), instead of a live image.

## Storage

A global app setting, not part of the recording preset: `AppSettingsStore::preview_frame_rate`
(int, `0` = Off, else 15/30/60/120; default `60`), following the same pattern as
`open_editor_when_finished`. It is a UI/performance preference — it does not affect the recorded
file — so it stays fixed across preset switches rather than traveling with `presets.toml`.

## Data flow

```
ConfigPage (new combo + previewFrameRateChanged(int) signal)
  -> MainWindow (persists via settings_store_.Save(); holds the current value)
    -> RecordPage::setPreviewFrameRateCap(int fps)   // new setter, called from the ctor
                                                       // (persisted value) and on change
      -> tryStartHubPreview / tryStartDxgiPreview     // use this value instead of
                                                       // primaryRecorderConfig().frame_rate_num
```

`RecordPage` caches the cap in a new member (`preview_frame_rate_cap_`, default 60) the same way
it already caches `current_output_settings_` etc. Both preview-start call sites read from this
member. `primaryRecorderConfig()`'s `frame_rate_num`/`frame_rate_den` fields become dead at that
point and are removed from it — its only remaining fields are the format-baseline ones capability
validation needs elsewhere, if any are still live after this change (check at implementation
time; if none are, retire the function entirely rather than leave an empty stub, per the warning
already on it after the 2026-08-07 codec-fallback fix).

When the setting changes while a preview is already running (idle or pushed), the existing
`tryStartHubPreview`/`tryStartDxgiPreview` restart path is reused (stop, then start with the new
cap) — the same mechanism a target change already goes through. No new restart logic.

## Non-goals

- The engine still produces and publishes its shared pre-encode texture during recording
  (ADR 0040's tap) even when the preview is Off or capped low — nothing subscribes to it, but the
  encode-side publish cost is unchanged. Touching that is a change to the recording path itself,
  not the preview, and is out of scope here.
- No attempt to skip re-presenting an unchanged frame when the preview's cap happens to exceed the
  producer's ~30 Hz push rate during recording (a separate, smaller optimization noticed while
  investigating this, not requested).

## Known trade-off

With the cap set to Off, there is no way to visually confirm framing/webcam PiP placement before
pressing Record — the idle preview is the only thing that shows it today. This is accepted,
matching the explicit choice to make Off a real "nothing renders" state rather than a throttled
one.

## Testing

- `PreviewFrameIntervalMs`-style unit coverage for the new cap → interval mapping (reuse
  `PreviewHelpers.h`'s existing table-driven pattern, extended for the cap value in place of the
  recording frame rate).
- `RecordPage::setPreviewFrameRateCap` reaches both preview-start call sites: a widget-level test
  (or the existing `--visual-test` scenario machinery, if it already exercises preview start) that
  sets a non-default cap and asserts the value threading through, plus an Off case asserting
  neither preview path is started and the surface shows the placeholder.
- `ConfigPage` combo round-trip: selecting each entry emits the signal with the right value; the
  120 fps entry is disabled/enabled following the same display-refresh gating as the existing
  Frame rate row (existing helper, reused not reimplemented).
- Settings persistence: `AppSettingsStore` save/load round-trips `preview_frame_rate`, and the
  documented default (60) is asserted the same way `test_app_settings_store.cpp` already asserts
  other defaults.

## Docs

`docs/product-spec.md`'s Settings/Quality & timing section gets a short paragraph for the new row,
matching the existing "Frame rate" paragraph's tone (§ around line 303).
