# Edit surface: export settings as a rail panel, and a layout that scales

Status: approved 2026-08-03. Amends ADR 0022 §"Export card" and §"Surface structure".

## Problem

Two things about the edit surface, both about space.

**The export card is a modal for two combo boxes.** `Export…` in the action bar opens
`ui::dialogs::ExportOverlay`, a 420 px card with a dimming backdrop over the whole view. Its
Options state is a container combo, a save-mode combo and a static destination line. That is not
a decision worth covering the clip the user is about to export — and the card also carries the
progress bar and the result, so the moment an export starts, the thing being exported is hidden
behind it. Meanwhile the right rail holds one Details card and then roughly 600 px of nothing on a
tall window.

**The surface does not scale.** `EditExportPage` has no `resizeEvent`. The rail is nailed to
280 px at construction and stays there at every window size, and the page never recomputes
anything on a resize. Being an overlay, it is also built long before it is first shown, so a
window resized while the surface was dismissed re-opens with a layout computed for a size that no
longer exists — the same hidden-page problem `RecordPage` solves by calling
`updateResponsiveLayout()` from `showEvent` as well as `resizeEvent`
(`RecordPage.cpp:653-657`, `:686-694`).

## Target

```
┌─ ◉ ExoSnap   Record  Device  Settings  …                          ─ □ ✕ ┐
├──────────────────────────────────────────────────────────────────────────┤
│  ←   Edit & export — 2026-08-02_14-03_Display 1.mkv                  ⓘ  │
│                                                                          │
│  ┌─ Video ──────────────────────────────┐   ┌─ Details ─────────────┐   │
│  │            [ Preview ]               │   │ Duration      4:18    │   │
│  │            ▶  0:31 / 4:18            │   │ …                     │   │
│  └──────────────────────────────────────┘   └───────────────────────┘   │
│  ┌─ Timeline ───────────────────────────┐   ┌─ Export ──────────────┐   │
│  │  ▓▓│━━━━━━━━●━━━━━━━━━━│▓▓           │   │ Container  [ MKV  ▾ ] │   │
│  └──────────────────────────────────────┘   │ Save   [ New file ▾ ] │   │
│                                             │ Stream-copy, lossless…│   │
│                                             │ ─────────────────────  │   │
│                                             │ Exporting…             │   │
│                                             │ ▓▓▓▓▓▓▓▓░░░░░░  62 %   │   │
│                                             │ [ Cancel ]             │   │
│                                             └───────────────────────┘   │
│                                                            [ Export ]   │
└──────────────────────────────────────────────────────────────────────────┘
```

The only modal left in this flow is the overwrite confirmation.

## Export panel

`app/ui/widgets/ExportPanel` replaces `app/ui/dialogs/ExportOverlay`, which is deleted outright
(header, source, CMake entries, `test_export_overlay.cpp`). It is a plain `QFrame` in the rail,
framed like the Details card above it — nothing about it is overlay-shaped any more: no backdrop
paint, no `syncGeometryToParent()`, no Escape/backdrop dismiss, no open/close.

```cpp
class ExportPanel : public QFrame {
    enum class State { Options, Running, Done, Failed };

    State state() const noexcept;
    QString containerKey() const;  // "mkv" | "mp4"
    QString saveModeKey() const;   // "new" | "overwrite"

    void reset();
    void showRunning();
    void setProgress(int percent);
    void showDone(const QString& output_path);
    void showFailed(const QString& error_message);

  signals:
    void cancelRequested();
    void retryRequested();
    void openFolderRequested();
    void revealFileRequested();
};
```

It stays presentation only, exactly as the card was: the thread, the trim range, the marker
sidecar and the atomic rename remain in `EditExportPage::runExport()`.

**The output rows do not swap out.** The card showed one of three mutually exclusive content
groups. The panel keeps container / save mode / destination visible in all four states and only
*disables* them while a run is in flight; the status area below a hairline separator is what
changes. Nothing moves under the pointer when an export starts, and after a result the settings
are already there for the next run — there is no "get back to the options" step to design.

**No Export button in the panel.** The trigger stays where the Record page keeps its primary
actions: bottom-right in the action bar. Two equal-weight Export buttons a hand's width apart
would be a coin flip, so the panel has none. `Retry` in the Failed state is the one exception, and
it repeats a run rather than starting a fresh one.

**Narrow-rail concessions.** The rail is 240 px at the minimum window, so: combo items are the
bare container/mode names (`MKV`, `New file`) with `AdjustToMinimumContentsLengthWithIcon` so
their size hint cannot push the column wider; "stream-copy, lossless" moves into the destination
line, which now also states what the selected mode does (`Written beside the source as
"<name>_edit.mkv"` / `Replaces the original recording in place`); the result badge is a 28 px
inline circle rather than the card's 56 px block; and `Open folder` / `Show in Explorer` stack
vertically instead of sharing a row.

**Action-bar button.** `Export…` becomes `Export` — no ellipsis, because it no longer opens
anything — and is disabled while a run is in flight. Without that, a second click reaches
`runExport()`, whose `join()` on the previous worker blocks the UI thread outright.

**Overwrite confirmation.** Unchanged, and now the only modal in the flow: overwrite-original
replaces the user's single copy of the recording with an atomic rename, so it is asked before the
export starts, names the file, and defaults to `Keep original`. A new-file export destroys nothing
and asks nothing.

## Responsive layout

`EditExportPage` gains the `RecordPage` pattern: `updateResponsiveLayout()` called from
`resizeEvent`, from `showEvent`, and once more deferred by a zero-timer from `showEvent` (the
overlay sizes the page from its own `showEvent`, so the page can still be carrying pre-show
geometry at that point). `updatePlayerHeight()` — which already existed and is already called
from the player/timeline resize filters — becomes the tail of it rather than a second entry point
callers must remember.

**The rail narrows; it never disappears.** A collapse-to-nothing breakpoint is the usual answer
for a sidebar, and it is the wrong one here: this rail carries the export controls, and the
surface has to stay fully usable at the enforced 860 × 700 minimum (`MainWindow.cpp:499`). Hiding
it would hide the way to export.

| Page width | Rail |
|---|---|
| ≥ 1180 px | 320 px |
| ≥ 960 px | 280 px |
| < 960 px | 240 px |

Measured against the page, which is the client area minus the edit overlay's 20 px margin band on
each side — an 860 px window reaches the page as 820 px and lands on the narrow rail, leaving the
player 580 px.

**The rail scrolls.** At 700 px window height the column has roughly 460 px of usable height, and
the Details card plus the export panel in its Done state need more than that. A clipped `Show in
Explorer` is unreachable; a scrolled one is not. The rail becomes a `QScrollArea`
(`editExportRail`, horizontal scrollbar off), the same treatment the left pane already has.

## Tests

`test_export_overlay.cpp` is replaced by `test_export_panel.cpp` (target `export_panel_tests`),
carrying over what still applies — the four states, progress clamping, the combo data keys, the
signal-only wiring — and adding what is new: the panel is an ordinary child widget rather than a
parent-sized overlay, the output rows stay visible-but-disabled while running, the destination
line follows the save mode and the container, the panel carries no Export button, and nothing in
it demands more than the 226 px the narrowest rail offers.

In `test_edit_export_page.cpp`: the panel is embedded in the rail under the Details card, no
`exportOverlay` widget is left on the surface, the action-bar button starts an export directly and
is disabled while one runs, plus four responsiveness cases (rail narrows across the breakpoints
and stays visible, the player keeps the bulk of the width at the minimum window, `showEvent`
re-applies the layout after a hidden resize, and the rail scrolls rather than clipping the panel).
The overwrite cases keep asserting that the modal fires for overwrite-original and not for a
new-file export.

## Visual harness states

`VisualScenario::edit_export_card_state` becomes `edit_export_panel_state` and gains `"failed"`.

| State | Shows |
|---|---|
| `edit-export-options` | Export panel at rest |
| `edit-export-running` | Export panel, Running at 62 % |
| `edit-export-done` | Export panel, Done |
| `edit-export-failed` | Export panel, Failed with a real error string |

Each is captured at the default harness size and at `860x700`, which is what proves the narrow
rail holds the panel without clipping.

## Documentation

- **ADR 0022** — amend §"Surface structure" (the rail now carries two cards and scrolls; the
  action bar starts the export) and replace §"Export card" with the panel. The overwrite
  confirmation and the export-execution split are unchanged.
- **docs/product-spec.md** — `766-771` (the surface's one action) and `816-822` (output options).
