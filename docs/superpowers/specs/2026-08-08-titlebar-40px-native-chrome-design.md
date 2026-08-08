# Title bar: 40 px, severity-coloured bell

Status: Parts 1 and 3 shipped 2026-08-08. **Part 2 (native window chrome) was withdrawn the
same day** — it cannot work in this application's window structure. See
"Part 2 — Native window chrome (withdrawn)" for the measurement that settled it.
Scope: `app/ui/chrome/OperationalTitleBar`, `app/MainWindow`, `app/ui/widgets/NotificationBell`,
`app/ui/chrome/NotificationHubPanel`, `app/ui/theme`

## Problem

Three separate complaints, one surface.

1. The title bar is 56 px tall. Every other window on the system — Explorer, Chrome, VS Code —
   sits between 32 and 40. The window reads as "an app with a header", not "a window with tabs".
2. The window-control buttons paint a 32×32 inset box with a 6 px radius. The click target is
   already the full 46×56 cell, but the top 8 px of the window are a resize border
   (`resizeZoneFromLocalPoint`, `MainWindow.cpp:267`), so throwing the mouse into the screen
   corner lands on `TopRight` resize instead of Close whenever the window is not maximized.
3. The notification badge is hard to read. Not a contrast problem — `#1A1206` on `caution` is
   roughly 11:1. A 9 px glyph inside a 14×13 px field (`NotificationBell.cpp:60-79`) puts the
   digit at about 5 px tall, below what IBM Plex Mono rasterises cleanly.

Underneath all three sits a structural point: the window is frameless
(`Qt::FramelessWindowHint`, `MainWindow.cpp:467`) but never answers `WM_NCHITTEST`. Every zone
reports `HTCLIENT`, so move and resize are re-implemented on top of Qt events.

That last observation is correct. The conclusion drawn from it — that answering `WM_NCHITTEST`
would therefore hand these behaviours to Windows — is not. See Part 2.

## Decisions

| Question | Decision |
| --- | --- |
| Title bar height | 40 px |
| Window-button hover | Fills the full 46×40 cell, no radius, no margin |
| Native integration | **Withdrawn** — not reachable in this window structure (Part 2) |
| Badge form | 8 px dot, no digit |
| Badge colour | Derived from the worst unread severity |

Rejected: `Qt::ExpandedClientAreaHint` (Qt 6.9, `qnamespace.h:242-243`). It would hand the
window buttons to the OS and therefore contradict the decision to keep their appearance ours.
Worth a standalone probe some day; not a route to this goal.

## Part 1 — Metrics

`ExoSnapMetrics::kTitlebarHeight` drops from 56 to 40. Children scale so the proportion holds:

| Element | Now | New |
| --- | --- | --- |
| Brand mark | 20 px | 18 px |
| Wordmark | 15 px | 14 px |
| Nav tab text | 13.5 px | 13 px |
| Nav tab padding | `0 15px` | `0 13px` |
| Brand slot left margin | 16 px | 14 px |
| Notification bell | 34 px | 28 px |
| Window button cell | 46 × 56 | 46 × 40 |
| Controls container width | 138 px | unchanged (3 × 46) |

The button cell keeps its 46 px width deliberately: that is the Windows 11 caption-button
metric, so the right edge lines up with every other title bar on the system.

In `exosnap_dark.qss`, `QPushButton#titlebarWindowButton` loses `margin` and `border-radius`.
Close keeps `#b8261d` / `#971e17`. The long comment above that rule explaining why `margin` does
not shrink the Fitts's-law target goes away with the margin.

Hover and pressed keep the existing `${bg3}` / `${bg4}` tokens rather than moving to a white
alpha. Despite the filename, this stylesheet is the only one — all four themes are served from
it through token substitution, so a literal `rgba(255, 255, 255, ...)` would be invisible on the
light ones. On the bar's dark ground the tokens produce the same result the white overlay would.

Complaint 2 from the Problem section — the thrown-into-the-corner click landing on the resize
border — is **not** fixed. It was to be fixed by Part 2's hit-test ordering. The corner behaves
as it did before this work.

## Part 2 — Native window chrome (withdrawn)

The plan was: answer `WM_NCHITTEST` on the top-level window, declaring the resize edges
(`HTLEFT` … `HTBOTTOMRIGHT`), the maximize cell (`HTMAXBUTTON`, for the Snap Layouts flyout) and
the drag area (`HTCAPTION`), then delete the Qt-side emulation of move, resize, edge cursors,
restore-on-drag and double-click-to-maximize.

It was implemented, all 270 tests passed, and every screenshot looked right. In live use
nothing worked: the bar could not be dragged, double-click did nothing, a maximized window
could not be restored, and the maximize button showed no hover.

### Why it cannot work here

`WM_NCHITTEST` is only asked of the window that owns the pixel under the cursor. This
application has native child windows — `PreviewSurface` and `EditPlayerSurface` set
`Qt::WA_NativeWindow` and call `winId()` so DXGI has a real `HWND` to present into. Qt must give
a native widget native *ancestors*, and it promotes native *siblings* too unless
`Qt::AA_DontCreateNativeWidgetSiblings` is set, which it is not.

The result, read off the running process:

```
0x3810CE  MainWindow                1920x1032 @354
 └ 0x8308AA                         1920x1032 @354   spans the whole client area
    ├ 0x8E1008                      1920x 992 @394   content; the preview chain hangs below
    └ 0x4009EC                      1920x  40 @354   the title bar, its own HWND
```

`WindowFromPoint` — the same routing a real cursor takes — answers `0x4009EC` for every point
in the bar. `MainWindow` is never asked. Sending `WM_NCHITTEST` to `MainWindow` directly still
returned `HTCAPTION`, `HTMAXBUTTON` and `HTCLIENT` in exactly the right places, which is why the
implementation looked correct from the inside: the answers were right, nobody was listening.

Setting `AA_DontCreateNativeWidgetSiblings` would remove the ten promoted siblings but not fix
this. `0x8308AA` is an *ancestor* of the preview and spans the title-bar band; ancestors of a
native widget cannot be made non-native. The bar's pixels would move from `0x4009EC` to
`0x8308AA` and still not reach `MainWindow`.

### What would be needed

Top-level `WM_NCHITTEST` requires that no native window covers the title bar. Two routes exist:

- Lift the title bar out of the preview's ancestor chain — e.g. `QMainWindow::setMenuWidget()`,
  so the central widget starts below the bar — *and* set `AA_DontCreateNativeWidgetSiblings`.
  Cheaper, but it rests on an invariant nothing enforces: the moment someone wraps the bar in a
  container that also holds the preview, it silently breaks again. It would need a test that
  walks the `HWND` tree, and its own live-verification round for preview stacking, which
  `PreviewSurface::hideEvent()` already documents as fragile in this application.
- Remove the native child windows entirely by compositing the DXGI frame through Qt's own render
  pipeline. This is the same root constraint that forces the webcam PiP and the OSD stat rows to
  be composited inside `DxgiPreviewRenderer` instead of being ordinary Qt widgets. Larger, and it
  carries its own risks — preview latency, whole-window compositing cost, HDR output fidelity,
  device-lost blast radius — so it needs a measuring spike before it is decided, not a decision
  followed by measurement.

Neither belongs in this piece of work. What shipped is Parts 1 and 3; `MainWindow` and
`OperationalTitleBar` keep the Qt-side move/resize/double-click handling they had before.

### What was kept from the attempt

- `isInDragArea()` now excludes any `QAbstractButton` by type rather than listing the three
  window controls, and `mousePressEvent()` / `mouseDoubleClickEvent()` consult it instead of
  `hitTestWindowButton()`. Before, the function had no caller at all.
- `title_bar_` and `edit_export_overlay_` are `QPointer`s. `nativeEvent()` reaches for the bar
  from `WM_SETCURSOR`, which arrives on every mouse move over the window — including during
  teardown, after the child widgets are destroyed but before the `HWND` is. A raw pointer stays
  non-null there, so the `!= nullptr` guards sail into freed memory. This bug predates the
  attempt; `WM_SIZE` had the same exposure, just far more rarely.
- `maximizeButtonRectInWindow()` stays deleted. It had no caller before this work either.

## Part 3 — Bell and severity

### Severity already exists

Each hub advisory carries a `status` string — `info`, `success`, `caution`, `error` — and
`MainWindow.cpp:4094-4111` already derives it from `NotificationType`. What is missing is
aggregation: the hub exposes `unreadCount()` but not the worst status among unread entries.

### The derivation has gaps

The current `switch` handles four cases explicitly; everything else falls through `default` to
`info`. While only the hub consumed this, the cost was one wrong icon on an entry the user reads
anyway. Once the bell carries the signal, a failed settings write would leave the dot mint.

| Type | Now | New | Reason |
| --- | --- | --- | --- |
| `SettingsSaveFailed` | `info` | `error` | The change may be lost |
| `CaptureActionFailed` | `info` | `error` | The action failed |
| `HotkeyConflict` | `info` | `caution` | A bound hotkey is dead |
| `SettingsRepaired` | `info` | `caution` | The file was damaged |
| `LowStorage` | `caution` | `error` | Crosses the hard-stop threshold and ends the recording |
| `RecoveryAvailable` | `error` | `caution` | A chance to recover work, not an alarm; otherwise the bell is coral immediately after launch |
| `UpdateAvailable`, `PresetSwitched` | `info` | `info` | Unchanged |
| `Saved` | `success` | `success` | Unchanged |
| `UnexpectedStop` | `error` | `error` | Unchanged |
| `FramesDropped`, `OverlayOmitted`, `AudioSourceDegraded` | `caution` | `caution` | Unchanged |

The `default` branch stays as a safety net but no longer carries real cases: every one of the 13
`NotificationType` values is listed explicitly, so adding a type forces a decision at compile
time rather than defaulting to "harmless".

The derivation moves out of `MainWindow` into a free function in the `notifications` namespace —
`AdvisoryStatusForType(NotificationType) -> QString` — so it is testable without constructing a
window (CLAUDE.md: prefer pure resolver logic).

### Aggregation and rendering

`NotificationHubPanel::worstUnreadStatus()` returns the highest-ranking status among entries
with `unread == true`, ranked `error > caution > info == success`, and an empty string when
nothing is unread. `AdvisoryEntry` already tracks `unread` per entry, so no new state is needed.

`NotificationBell` replaces `setUnreadCount(int)` with `setUnreadStatus(const QString&)`. The
badge becomes an 8 px circle at the icon's top-right corner with a 2 px ring in `bg`, filled
from the status: `accent` for `info` / `success`, `warn` for `caution`, `err` for `error`.
Empty status paints nothing. The `QFont` / `drawText` block disappears entirely.

`OperationalTitleBar::setBellUnreadCount(int)` becomes `setBellUnreadStatus(const QString&)`.
Two call sites follow: `MainWindow::refreshHubUnreadBell()` (`MainWindow.cpp:4456-4460`) passes
`worstUnreadStatus()` through, and the visual-test seed (`MainWindow.cpp:726`) switches from
`2` to a literal status so the harness keeps rendering a visible dot. The exact unread count
remains available in the hub itself, one click away.

Colour meaning stays consistent with the rest of the application: coral is recording and error,
amber is caution, mint is a neutral hint.

## Tests

| Test | Covers |
| --- | --- |
| `AdvisoryStatusForType` over all 13 `NotificationType` values | The mapping table above, including the six corrections |
| `worstUnreadStatus()`: empty, all-read, single unread, mixed severities, ranking order | Aggregation |
| `NotificationBell` renders the dot in the three colours and nothing when the status is empty | Rendering |
| `isInDragArea()` rejects each of: six nav tabs, bell, three window buttons; accepts the drag slot, the brand slot, and rejects points outside the bar | Which presses start a window drag |
| `hitTestWindowButton()` resolves the top-right corner pixel to Close | Corner target |
| `hitTestWindowButton()` at both sides of every cell boundary | A cell drifting one pixel would hand a click to its neighbour |

The `--visual-test` harness renders screenshots for a human to judge; it holds no stored
reference images, so there is nothing to re-bless. The 40 px bar is checked by eye there.

Note what this suite could not catch. Every one of these tests exercises widgets, and every
screenshot renders widgets. Which `HWND` owns a pixel is invisible to both, which is why Part 2
passed 270 tests and a visual review while being inert in the user's hands. A test that walks
the `HWND` tree would have caught it; none exists.

## Live verification

Nothing here needs live verification beyond ordinary use: the move/resize/double-click paths are
the ones that were already shipping before this work. The severity dot is judged in the
`--visual-test` harness (`notifications-open`, `record-ready`).

## Out of scope

- `Qt::ExpandedClientAreaHint` — rejected above.
- The exact unread count anywhere in the title bar. It lives in the hub.
- Any change to toast appearance or to `NotificationToastWindow`.
- Light-theme values. The QSS token structure carries them; no literal changes are needed
  beyond the shared hover alphas.
- Native window chrome, in either of the two forms named in Part 2. Each needs its own spec.

## Spec updates

`docs/product-spec.md` needs the notification bell's severity colours recorded as user-visible
behaviour. The title-bar height and button styling are implementation metrics and stay out of
the product spec.

An ADR is not warranted: this decides no architecture. The Part 2 finding is a constraint
discovered, not a decision taken — the decision it forces (native child windows vs. Qt-composited
preview) is still open.
