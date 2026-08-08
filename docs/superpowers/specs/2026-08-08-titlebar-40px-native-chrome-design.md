# Title bar: 40 px, native window chrome, severity-coloured bell

Status: design, approved 2026-08-08
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
reports `HTCLIENT`, so move and resize are re-implemented on top of Qt events instead of being
delegated to Windows.

## Decisions

| Question | Decision |
| --- | --- |
| Title bar height | 40 px |
| Window-button hover | Fills the full 46×40 cell, no radius, no margin |
| Native integration | Answer `WM_NCHITTEST` for resize borders, button cells, and the drag area |
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

## Part 2 — Native window chrome

`MainWindow::nativeEvent()` gains a `WM_NCHITTEST` handler. This is the load-bearing change:
every zone below currently reports `HTCLIENT` and is emulated in Qt.

| Zone | Returns | Effect |
| --- | --- | --- |
| Window edges and corners | `HTLEFT` … `HTBOTTOMRIGHT` | Windows drives resize directly |
| Minimize / Close cells | `HTCLIENT` | Our buttons keep their own hover and click |
| Maximize cell | `HTMAXBUTTON` | Snap Layouts flyout on hover |
| Title bar drag area | `HTCAPTION` | Windows drives move, restore-on-drag, double-click, system menu |
| Everything else | `HTCLIENT` | Unchanged |

Edge width follows `GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER)`
instead of the hard-coded 8, so the grab band matches the rest of the system at any DPI.
The button cells are tested before the edge zones, so the top-right corner resolves to Close
rather than `HTTOPRIGHT`.

`hitTestFromResizeZone()` already returns exactly these `HT*` codes but is currently only used
to look up a cursor. It becomes the actual hit-test mapper.

`resizeZoneFromLocalPoint()` takes the border width as a parameter instead of hard-coding 8, and
the caller derives it from `SM_CXSIZEFRAME + SM_CXPADDEDBORDER` divided by the window's device
pixel ratio (those metrics report physical pixels; the zone maths is logical). At 100% that
resolves to 8, so nothing changes there — it stops drifting from the system at other scales.

`maximizeButtonRectInWindow()` (`OperationalTitleBar.cpp:358`) is deleted rather than finally
used: the hit test needs to name *which* button a point is over, and `hitTestWindowButton()`
answers that directly for all three cells. Keeping a second, rect-shaped path to the same fact
would be one more thing to keep in step.

### `HTMAXBUTTON` costs its own hover handling

Declaring the maximize cell as `HTMAXBUTTON` moves it into the non-client area as far as Windows
is concerned: Qt stops receiving `enterEvent` / `leaveEvent` / clicks for it, so the button would
sit inert while its two neighbours light up under the cursor. It needs:

- `WM_NCMOUSEMOVE` with `wParam == HTMAXBUTTON` → paint the hover state; any other `wParam`,
  plus `WM_NCMOUSELEAVE`, clears it.
- `WM_NCLBUTTONDOWN` with `wParam == HTMAXBUTTON` → paint the pressed state, consume the message.
- `WM_NCLBUTTONUP` with `wParam == HTMAXBUTTON` → toggle maximize/restore, clear the state.

`applyVisualWindowButtonHover()` already drives the painted hover state directly for the
visual-test harness (`OperationalTitleBar.cpp:238-255`); the same mechanism serves here, so no
new painting path is needed. It is renamed to `setForcedWindowButtonHover()` — with a second,
production caller it is no longer visual-test-only, and a name saying otherwise would invite
someone to strip it from a release build.

This is the one place where the change adds code rather than removing it. It buys the Snap
Layouts flyout, which cannot be obtained any other way.

### What this deletes

Emulation that Windows performs natively once the zones are declared:

- The resize branch of the app-wide event filter (`MainWindow.cpp:1051`, `2280-2313`), including
  its `startSystemResize()` call. Windows starts the modal loop from `DefWindowProc` instead —
  this is the latency that reads as "not quite snappy".
- The entire `WM_SETCURSOR` handler (`MainWindow.cpp:2175-2209`) and `resize_cursor_shown_`.
  Windows sets the cursor itself for declared edge zones.
- `tracking_drag_from_max_`, `drag_press_global_pos_`, the 5 px threshold and the `rel_x`
  reposition maths (`OperationalTitleBar.cpp:285-309`). The comment there names it as a
  reimplementation of native restore-on-drag. Windows also preserves the cursor's relative
  vertical position, which the current code does not — it forces `y = current.y() - kHeight/2`.
- `mouseDoubleClickEvent()` (`OperationalTitleBar.cpp:329-340`).
- `QApplication::setOverrideCursor(Qt::SizeAllCursor)` (`OperationalTitleBar.cpp:262`) and its
  whole dependency chain: `move_cursor_active_`, `resetDragCursor()`, `mouseReleaseEvent()`,
  the `WM_EXITSIZEMOVE` handler (`MainWindow.cpp:2215-2218`), and the safety net inside
  `WM_SETCURSOR` (`MainWindow.cpp:2200-2208`) whose comment describes the override sticking
  "indefinitely".

The four-way move cursor is a deliberate, accepted regression: Windows shows a plain arrow when
dragging a caption. It was never native behaviour, and it is the root of the state that the two
safety nets exist to repair.

### What this gains

Right-click on the title bar opens the system menu (Move, Size, Minimize, Maximize, Close).
The application has no such menu today.

### The risk that needs care

Under `HTCAPTION`, Qt receives no mouse events for that area. Any interactive child not excluded
from the caption region becomes dead.

`isInDragArea()` (`OperationalTitleBar.cpp:342`) currently excludes only the three window
buttons. It becomes the single authority the `WM_NCHITTEST` handler consults, and excludes every
interactive child by walking up from `childAt()` and rejecting anything that is a
`QAbstractButton` — the six nav tabs, the bell, the three window controls.

A type test rather than a list of members, deliberately: a control added to the bar later is
excluded without anyone having to remember this function exists. The failure mode it guards
against is silent — a caption swallows mouse events entirely, so a forgotten control does not
misbehave, it stops responding.

The status pill stays draggable. It is a plain `QWidget` with no interaction, and native title
bars let their own inert content be dragged; excluding it would only shrink the grab area. Same
for the wordmark. Both are asserted as draggable so the intent is not mistaken for an oversight.

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
| `isInDragArea()` rejects each of: six nav tabs, bell, three window buttons; accepts the drag slot, the brand slot, and rejects points outside the bar | The `HTCAPTION` dead-child risk |
| `hitTestWindowButton()` resolves the top-right corner pixel to Close | Corner target |
| `hitTestWindowButton()` at both sides of every cell boundary | Only the maximize cell goes to Windows; a cell drifting one pixel would put a neighbour under `HTMAXBUTTON` and kill its click |

The `--visual-test` harness renders screenshots for a human to judge; it holds no stored
reference images, so there is nothing to re-bless. The 40 px bar is checked by eye there.

`WM_NCHITTEST` itself is not unit-testable — it needs a real window and a real cursor. It is
covered by the live checks below.

## Live verification

Only what cannot be judged from tests or the render harness. Per CLAUDE.md, each of these needs
coordination with the developer before driving the running application.

1. Throw the mouse into the top-right screen corner, windowed and maximized — Close is hit both
   times.
2. Hover the maximize button for about a second — the Snap Layouts flyout appears.
3. Drag each of the four edges and four corners — the cursor changes without lag and the resize
   starts on the first press.
4. Double-click the top edge — the window maximizes vertically.
5. Drag the title bar of a maximized window downwards — it restores under the cursor, and the
   cursor keeps its relative position in the bar.
6. Right-click the title bar — the system menu opens.
7. Click every nav tab, the bell, and the status pill — none is dead under `HTCAPTION`.

## Out of scope

- `Qt::ExpandedClientAreaHint` — rejected above.
- The exact unread count anywhere in the title bar. It lives in the hub.
- Any change to toast appearance or to `NotificationToastWindow`.
- Light-theme values. The QSS token structure carries them; no literal changes are needed
  beyond the shared hover alphas.

## Spec updates

`docs/product-spec.md` needs the notification bell's severity colours recorded as user-visible
behaviour. The title-bar height and button styling are implementation metrics and stay out of
the product spec.

An ADR is not warranted: this decides no architecture, it removes emulation in favour of the
platform.
