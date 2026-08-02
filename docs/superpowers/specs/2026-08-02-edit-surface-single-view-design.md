# Edit surface: one view instead of a three-step stepper

Status: approved 2026-08-02. Supersedes the phase model in ADR 0022 §"Phases".

## Problem

The Edit/Output/Save surface steps through `Review → Edit → Output` (ADR 0022:132-152). In
practice the Review step reports post-flight numbers that rarely need attention, and continuing
from Review to Edit only swaps one panel corner for the timeline — a full blocking step for a
change that reads as a re-layout. The Output step is three controls behind another step. On top
of that the overlay covers the real title bar (ADR 0022:106-109), so the window cannot be moved
or minimised for the whole session.

The surface is one task — trim a clip and write it out. It should be one view.

## Target

```
┌─ ◉ ExoSnap   Record  Device  Settings  Diagnostics  Logs  About   READY 🔔  ─ □ ✕ ┐  real, clickable
├──────────────────────────────────────────────────────────────────────────────────┤
│  ←   Edit & export — 2026-08-02_14-03_Display 1.mkv                          ⓘ  │
│                                                                                   │
│  ┌─ Video ─────────────────────────────────┐   ┌─ Details ──────────────┐        │
│  │            [ Preview ]                  │   │ Duration      4:18     │        │
│  │            ▶  0:31 / 4:18               │   │ Size        812.4 MB   │        │
│  └─────────────────────────────────────────┘   │ …                      │        │
│  ┌─ Timeline ──────────────────────────────┐   └────────────────────────┘        │
│  │  ▓▓│━━━━━━━━━━━━●━━━━━━━━━━━│▓▓         │                                     │
│  └─────────────────────────────────────────┘                      [ Export… ]    │
└──────────────────────────────────────────────────────────────────────────────────┘
```

Removed outright: the phase stepper, the Review panel, the Output panel, the Exporting panel,
the Done/Failed result panel, the secondary action button, and the `EditExportPage::Phase` enum.
The player is no longer phase-gated; it is always present.

Visible strings stay English. Localisation is a 1.0 item and is out of scope here.

## Chrome and geometry

`EditExportOverlay` stays a child of `centralWidget()`. That parenting recipe exists for DXGI
preview HWND compositing (`MainWindow.cpp:5332-5336`) and is not up for revision.

What changes is the vertical extent: `syncGeometryToParent()` (`EditExportOverlay.cpp:129-132`)
applies a top inset equal to the title bar height, so the overlay spans `host->rect()` minus that
band. MainWindow owns the value and pushes it in — the overlay must not reach for
`parent()->findChild<OperationalTitleBar*>()`.

```cpp
void EditExportOverlay::setTopInset(int pixels);   // MainWindow: title_bar_->height()
```

The inset is re-applied on every parent resize, alongside the existing geometry sync.

The backdrop stays fully opaque (`kBackdropAlpha = 255`). Its original justification — window
chrome bleeding through the margin band — no longer applies, but a translucent backdrop would
show the Record page underneath, which is not wanted. The 20 px margin band and its
click-to-dismiss behaviour stay, now routed through the discard guard below.

Nav tabs remain visible and clickable during an edit session. Navigating away closes the overlay
as it does today (`MainWindow.cpp:2616-2619`), and now goes through the discard guard.

## Export overlay

A nested overlay inside the edit view, in one card with four states:

```
Options ──Export──> Running ──┬── ok ──> Done
                              └── err ─> Failed ──Retry──> Running
```

- **Options** — container combo, save-mode combo, static destination label, `Cancel` / `Export`.
- **Running** — status line, progress bar, `Cancel`.
- **Done** — check mark, output filename, `Open folder` / `Show in Explorer` / `Close`.
- **Failed** — error text from the remuxer, `Close` / `Retry`.

The card is presentation only. Export execution — thread, trim range, marker sidecar, atomic
rename (`EditExportPage::runExport()`, `.cpp:1321-1437`) — stays in the page and drives the card
through slots. The overwrite confirmation (`confirmOverwrite()`, `.cpp:1244-1265`) keeps its
current wording and its `Keep original` default button, and still runs before the export starts.

Escape and a backdrop click close the card, not the session. Both are blocked while `Running`;
`Cancel` is the only way out of a running export, exactly as the overlay's dismiss block works
today.

`EditExportOverlay::isDismissBlocked()` currently reads `page_->phase() == Phase::Exporting`
(`.cpp:82-84`). It becomes `page_->isExportRunning()`. The three MainWindow guards that depend on
it — close-event at `:2351` and `:2449`, nav-away at `:2617` — keep their present meaning.

### `app/ui/dialogs/ExportOverlay.h`

```cpp
namespace exosnap::ui::dialogs {

class ExportOverlay : public QWidget {
    Q_OBJECT
  public:
    enum class State { Options, Running, Done, Failed };

    explicit ExportOverlay(QWidget* parent = nullptr);

    void openCard();   // show in Options, progress reset to 0
    void closeCard();  // hide; ignored while Running
    [[nodiscard]] bool isCardOpen() const noexcept;
    [[nodiscard]] State state() const noexcept;

    // Selections, read by the page when it starts an export.
    [[nodiscard]] QString containerKey() const;  // "mkv" | "mp4"
    [[nodiscard]] QString saveModeKey() const;   // "new" | "overwrite"

    // Driven by the page while an export runs.
    void showRunning();
    void setProgress(int percent);
    void showDone(const QString& output_path);
    void showFailed(const QString& error_message);

    void applyThemeStyles();

  signals:
    void exportRequested();
    void cancelRequested();
    void retryRequested();
    void openFolderRequested();
    void revealFileRequested();
    void closeRequested();   // Close button, Escape, or backdrop click
};

} // namespace exosnap::ui::dialogs
```

Object names, relied on by tests: `exportOverlay` (the overlay), `exportOverlayCard` (the centred
card), `outputContainerCombo` and `outputSaveModeCombo` (unchanged from the current Output panel —
same controls, same combo item data), `editExportDestFolder`, `exportPrimaryBtn`,
`exportCancelBtn`, `exportProgressBar`, `exportStatusLabel`, `exportOpenFolderBtn`,
`exportRevealBtn`.

## Details rail

The right-hand Details card moves to `app/ui/widgets/EditDetailsRail.{h,cpp}` unchanged in
appearance: fixed 280 px column, seven fact rows, right-aligned monospace values, separators
between rows.

```cpp
namespace exosnap::ui::widgets {

class EditDetailsRail : public QFrame {
    Q_OBJECT
  public:
    struct Facts {
        QString duration, size, resolution, fps, video_codec, audio_codec, container;
    };

    explicit EditDetailsRail(QWidget* parent = nullptr);
    void setFacts(const Facts& facts);
    void applyThemeStyles();
};

} // namespace exosnap::ui::widgets
```

Object names: `editDetailsRail`, `editDetailsRailTitle`, and per-row value objects
`factDurationValue`, `factSizeValue`, `factResolutionValue`, `factFpsValue`, `factVideoValue`,
`factAudioValue`, `factContainerValue`.

## Post-flight report

The three numbers the Review panel used to show — real frame drops, peak A/V drift, pipeline
health — move to an icon at the right end of the overlay header, with the values in its tooltip.
The computation in `setEditContext()` (`.cpp:690-743`) is unchanged, including the em-dash
treatment for unavailable values and the real-versus-benign drop definition; only the display
target moves.

A hover-only tooltip would swallow a genuine finding, so the icon carries the severity:

| Pipeline health | Icon | Extra |
|---|---|---|
| Good / Unavailable | neutral `info` glyph, muted | none |
| Warning | warning triangle, amber | short label beside the icon |
| Critical | warning triangle, coral | short label beside the icon |

Quiet by default, visible when it matters.

## Discarding edits

Closing the surface with trim points or markers set currently drops that work silently. With nav
tabs and the title bar reachable, there are more ways to hit it.

```cpp
[[nodiscard]] bool EditExportPage::hasUnsavedEdits() const;  // trim range set, or markers present
```

When it returns true, closing asks first: `Discard edits?` with `Keep editing` as the default
button and `Discard` as the reject-role choice — same shape as the overwrite confirmation. It
guards the back arrow, Escape, a backdrop click, and nav-away. On `Keep editing` from nav-away the
navigation is cancelled, not merely deferred.

One exception: a recording start (`MainWindow.cpp:1996-1999`) closes the overlay without asking. A
modal that blocks a hotkey-triggered recording is worse than the lost trim.

## Code structure

`EditExportPage.cpp` is 1439 lines with a 410-line `buildUi()`, a monolithic `applyThemeStyles()`
and a `refreshPhase()` — every visual change has to be mirrored in three places. Pulling the
export card and the details rail out, and deleting `refreshPhase()` along with the phase model,
removes that mirroring. The page keeps the header, player, timeline, action bar, export
execution, and the report values.

Not in scope: extracting the player area, the timeline area, or the header. They are not being
rebuilt here.

## Tests

Roughly twenty cases in `test_edit_export_page.cpp` and `test_edit_export_overlay.cpp` assert
phase transitions. They are rewritten against the new model, not deleted — what they cover
(overwrite confirmation, report values, timeline behaviour, dismiss blocking) stays valid.

New coverage:

- `ExportOverlay` state machine: Options → Running → Done/Failed, progress updates, Escape and
  backdrop blocked while Running, `retryRequested` from Failed.
- `EditDetailsRail::setFacts()` fills all seven rows.
- `hasUnsavedEdits()` true after a trim range or markers, false on a fresh clip.
- The discard prompt fires on back/Escape/backdrop/nav-away and not on a recording start.
- The report icon reflects Good/Warning/Critical, and its tooltip carries all three values.

## Visual harness states

`edit-review` and `edit-output` disappear. New and retained:

| State | Shows |
|---|---|
| `edit-main` | The single view, untrimmed |
| `edit-trimmed` | Trim handles set, markers present |
| `edit-export-options` | Export card, Options |
| `edit-export-running` | Export card, Running at 62 % |
| `edit-export-done` | Export card, Done |
| `edit-report-warning` | Header icon in its warning state |

`edit-done` is retired in favour of `edit-export-done`. The scenario-ID list in
`test_visual_scenarios.cpp:142-149` is updated to match, and the `edit_export_phase` string field
in `VisualScenario` is replaced by what the new states actually need (an export-card state plus a
report-severity override).

## Documentation

- **ADR 0022** — amend: drop the phase/stepper sections (`:132-152`, `:272-280`), reverse the
  title-bar trade-off (`:106-109`), and record the export card and discard guard. The parenting
  recipe and the opaque backdrop keep their existing justification.
- **docs/product-spec.md** — `74-82` (IA paragraph), `692-695` (post-flight report location),
  `765-771` (three-phase overview), `799-806` (Output phase → export card).
