# Harness modes and tracing

Diagnostic switches on `exosnap.exe`, and the AddressSanitizer build. Each one
exists because the defect it was written for is invisible to every other
instrument the project has — a screenshot cannot show which window owns a pixel,
and a test cannot show that frames stopped arriving.

None of these modes synthesizes input or automates a window. They are argv- or
environment-configured, and they observe.

## AddressSanitizer

`windows-x64-asan` (MSBuild) and `windows-x64-ninja-asan` (Ninja, needs a VS
Developer shell) build the whole tree with `/fsanitize=address`. Use them when
chasing a crash whose stack makes no sense — a use-after-free surfaces as an
unrelated crash somewhere else entirely, and ASan turns that into a report at
the first invalid access with the allocation and free stacks attached.

- Build + test: `cmake --preset windows-x64-asan && cmake --build --preset windows-x64-asan`,
  then `pwsh scripts/run-tests.ps1 -BuildDir build/windows-x64-asan -Config Debug`.
- The sanitizer runtime (`clang_rt.asan*dynamic-*.dll`) ships next to `cl.exe`
  and is never on PATH; the build stages it beside every binary. A missing
  'C++ AddressSanitizer' VS component fails configure with an explicit message
  rather than at first launch with 0xC0000135.
- `/RTC1` is stripped from the Debug flags — MSVC rejects it alongside
  `/fsanitize=address`. ASan subsumes what it checked.
- Expect the suite to run noticeably slower than a plain Debug run. This is a
  diagnostic preset, not a replacement for the normal test gate.

## Window ownership and chrome — `--hwnd-audit`

`exosnap.exe --hwnd-audit` reports three things about the real top-level window
and exits 0 only when all three hold:

```
quick-hwnd-audit: child_hwnds=0
quick-hwnd-audit: style=0x96040000 exstyle=0x00000100 caption=0 thickframe=1 border=0
quick-hwnd-audit: nonclient_inset=0,0,0,0 native_titlebar=0
```

1. **`child_hwnds=0`** — no native child windows. Tests and `--visual-test` are
   both blind to this: they see objects and pixels, never which WINDOW owns a
   pixel — and a native child never lets the top-level window see a
   `WM_NCHITTEST`, so it silently breaks drag, resize and Snap over whatever it
   covers.
2. **No non-client area** — the 40 px title band is the product's own, so Windows
   must reserve nothing outside the client rect. A non-zero top inset is a native
   caption drawn ABOVE ours, i.e. two title bars.
3. **`WS_THICKFRAME` present** — a frameless window has no caption to offer the
   system, so this bit is the only thing keeping the native resize drag, Aero
   Snap and Win+Arrow alive. Qt drops it when it makes the window visible unless
   `QuickWindowChrome` re-asserts it; nothing about the window LOOKS wrong when
   it is missing.

Run it after any work on window chrome, hit-testing or overlays. The window is
never activated.

Zero is the expected result and is the point of the Qt Quick migration: the
preview and the editor player are scene-graph items, not child windows. A
non-zero count is the whole signal, because in a Quick build any native child at
all is the regression.

## Startup window geometry — `--window-trace`

`exosnap.exe --window-trace` (or `EXOSNAP_WINDOW_TRACE=1`, for a launch that
cannot take extra argv) writes one line per startup geometry milestone to the
application log:

```
window-trace: persisted 400,120 1280x820 maximized=0
window-trace: resolved  400,120 1280x820 maximized=0
window-trace: pre-show  qt=400,120 1280x820 ... native_window=400,120 1280x820 native_visible=0 ...
window-trace: post-show qt=400,120 1280x820 ... native_window=400,120 1280x820 native_visible=1 ...
window-trace: first-frame ...
```

Each line carries both spaces at once — Qt's logical geometry and believed frame
margins next to the native window and client rects — because the whole class of
defect here is the two disagreeing about what a rect means.

The property to check is **not** "it ends up in the right place". It is that
`pre-show` already holds the final rect while `native_visible=0`, and that
`post-show`, `first-expose` and `first-frame` never change it. A window that
reaches the right rect a frame late reached it visibly.

`--window-trace` also logs the Win32 messages that decide the rect
(`window-msg: WINDOWPOSCHANGING/NCCALCSIZE/GETMINMAXINFO/STYLECHANGED`) until the
first frame. Those are *sent*, not posted, so they are invisible to any log
written from Qt signals: by the time `xChanged` arrives the decision is made and
its cause is gone.

Combine with `--hwnd-audit` for a run that measures all of this and exits without
ever activating the window.

## Pointing hand — `--cursor-audit`

`exosnap.exe --cursor-audit` visits every navigation destination, reports the
pointer over the centre of each control that declares
`HoverHandler { cursorShape: Qt.PointingHandCursor }`, and compares two answers:
the shape Qt believes the window carries and the cursor `GetCursor()` says the OS
is actually showing. Only disagreements are named; a clean page is one line.

```
cursor-audit: page=record probed=13 failed=0
cursor-audit: page=settings probed=47 failed=0
cursor-audit: page=diagnostics probed=9 failed=0
cursor-audit: page=logs probed=13 failed=0
cursor-audit: page=about probed=11 failed=0
cursor-audit: probes=93 agreed=86 disabled=7 clipped=1 os_lost_it=0 never_fired=0
```

Two verdicts, and they point at different layers:

- `never-fired` — Qt's own window cursor never became a pointing hand, so no
  handler was reached at that point. The declaration exists and nothing delivers
  hover to it.
- `os-lost-it` — Qt set the cursor and the desktop shows something else. That is
  a platform-layer problem, not a QML one.

`disabled` and `clipped` are neither: Qt delivers no hover to a disabled item, and
a row scrolled past the end of its view is not on screen at the position it
reports. Both are counted so a page that went entirely disabled cannot pass as a
page that was audited.

The pointer is never moved. `WM_MOUSEMOVE` is sent to the application's own
window, which takes no focus and synthesizes no input, and the desktop cursor is
restored to whatever it was carrying before the run. `GetCursor()` does read
state the machine's real pointer also writes, so a disagreement is re-probed once
before it is reported.

The lasting reason this mode exists: a QML test can only prove the declaration is
there, and `QCoreApplication::sendEvent()` never teaches Qt's platform layer that
the pointer is inside the window — so `SetCursor()` never runs and the assertion
passes on a build where no user sees a cursor. `app/quick/tests/test_hover_cursor_native.cpp`
covers the same question on synthetic windows; this mode covers the shipping one.

## Maximize and restore — `--window-maximize-cycle`

`exosnap.exe --window-maximize-cycle` drives the shell's own `toggleMaximized()`
once in each direction and checks the result against Windows, then exits:

```
window-cycle: baseline     zoomed=0 iconic=0 show_cmd=1 ws_maximize=0 restore_to_max=0 window=120,80 1920x1050 restore=120,80 1920x1050
window-cycle: windowed resize edges=6/6
window-cycle: maximized    zoomed=1 iconic=0 show_cmd=3 ws_maximize=1 restore_to_max=1 window=0,0 3840x2088   restore=120,80 1920x1050
window-cycle: maximized resize edges=0/6
window-cycle: minimized    zoomed=0 iconic=1 show_cmd=2 ws_maximize=0 restore_to_max=1 window=-32000,-32000 160x28
window-cycle: un-minimized zoomed=1 iconic=0 show_cmd=3 ws_maximize=1 restore_to_max=1 window=0,0 3840x2088
window-cycle: restored     zoomed=0 iconic=0 show_cmd=1 ws_maximize=0 restore_to_max=0 window=120,80 1920x1050 restore=120,80 1920x1050
window-cycle: PASS maximize and restore round-tripped
```

The run places the window on a rect that is deliberately not the work area before
it starts. A window persisted at work-area size would otherwise satisfy the
round-trip assertion whatever the restore did.

Five assertions, none of which a unit test can reach — `QT_QPA_PLATFORM=offscreen`
has no HWND, so `IsZoomed`, `WINDOWPLACEMENT` and `WS_MAXIMIZE` do not exist there:

1. **The window is really zoomed** while maximized. `Window.visibility` is not
   evidence: setting it to `Window.Maximized` on this frameless window produces a
   single `SetWindowPos` onto the work area and leaves Windows in
   `SW_SHOWNORMAL`, which looks maximized and behaves like a normal window.
2. **`rcNormalPosition` survives the maximize.** A window that resizes into the
   maximized state instead of entering it overwrites the rect it must return to,
   and the damage is only visible one restore later.
3. **The resize edges answer in exactly one state.** Six `WM_NCHITTEST` probes,
   sent straight to the window rather than performed with a cursor: all six must
   resolve to a resize code while windowed and none of them while maximized.
4. **A window minimized while maximized comes back maximized.** The taskbar
   button sends the same `SC_RESTORE` the harness sends, and Windows decides
   where to return from `WPF_RESTORETOMAXIMIZED` in the placement — a flag only a
   native minimize sets. `restore_to_max` on the `minimized` line is the value to
   read; it going to 0 there is the whole defect.
5. **The restore rect round-trips** back to the baseline exactly.

Exit codes: `1` state, `2` geometry, `3` no window, `4` normalisation, `5` hit
test, `6` minimize/restore. The run isolates its config directory, stays out of the single-instance
guard, and drives no input — the toggle is the same QML function the Maximize
button calls.

Dragging a maximized window's title bar downward is the one contract point this
cannot reach: that is Windows' own move loop and needs a real drag.

## Preview presentation — `EXOSNAP_PREVIEW_TRACE=1`

Writes one `preview-trace:` line per Record-preview presentation lifecycle
transition (window expose, screen change, scene-graph re-initialisation):

```
preview-trace: screen-changed screen=\\.\DISPLAY2 dpr=1.00 exposed=1 visible=1 loop=1 owed=1 reissued=1 publishes=412 wakeups=409 updates=410 renders=409
```

`owed=1` means a producer published a frame that no render pass has followed —
i.e. the newest frame is sitting in the transport and the screen has not shown
it. `reissued=1` is this transition asking for the render that frame is owed.
The pair is the whole contract: a transition that finds `owed=0` does nothing,
and a frame that is owed one is never left waiting for an unrelated redraw.

Off by default and read once, because the point of the preview's redraw gate is
that a quiet desktop costs nothing. It exists because the defect it was written
for — the preview freezing when the window crosses a monitor boundary, until the
mouse moves — is invisible to every other instrument: a screenshot cannot show
that frames stopped arriving, and the metrics overlay reports rates rather than
the transition that broke them.

`preview.snapshot` on the Live Verify control channel reports the same counters
(`publishSignals`, `wakeups`, `renderPasses`, `owed`) as structured state; prefer
it over parsing these lines, which stay useful as secondary evidence. See
[live-verify.md](live-verify.md).

## Deterministic captures — `--visual-test`

`--visual-test <path>` renders one screenshot and exits. The process runs in its
own scratch config directory, so a capture never reads or writes the developer's
settings, and every capture-excluded overlay on screen is grabbed into its own
`<path>.quickOverlay<Name>.png` beside it.

A capture is only evidence if it is reproducible, which is what the rest of these
options are for: without them the picture shows whatever this machine happened to
be doing, and two captures a week apart are not comparable. Each one seeds a
stated state instead. None of them synthesizes input.

| Option | Selects |
|---|---|
| `--visual-test-size WxH` | Window size. The review baseline is `1440x1000` |
| `--visual-delay-ms N` | Delay before the shutter. Raised automatically for the options that need a built page |
| `--visual-appearance dark\|light`, `--visual-accent <id>` | Theme. Pinned in both directions — omitting them means the product default, never the previous run's |
| `--visual-page N` | Nav destination, in product order: Record, Settings, Diagnostics, Logs, About |
| `--visual-expert` | Expert mode, one switch for both surfaces that have two arrangements |
| `--visual-scroll F` | Scroll position as a fraction of the page's own scrollable height |
| `--visual-popup source-picker\|notification-hub` | A popup that is built on first use |
| `--visual-dialog <name>` | A modal surface nothing but an interaction raises: `close-guard`, `preset-delete`, `preset-rename`, `preset-save-as` |
| `--record-visual-state <name>` | A Record-page state. One name per product state, no aliases |
| `--overlay-visual-state <name>` | A runtime overlay: recovery, recording error, crash report |
| `--desktop-pattern` | A synthetic window behind the preview, so the preview frame's content is fixed too |

Content is seeded through the environment, in the same spirit:

| Variable | Seeds |
|---|---|
| `EXOSNAP_VISUAL_EDIT_SCENARIO` | The Edit surface: `edit-default`, `edit-trimmed`, `edit-timeline-multitrack`, `edit-timeline-loading`, `edit-timeline-unavailable`, `edit-export-running`, `edit-export-done`, `edit-export-failed`, `edit-report-warning`, `edit-long-filename` |
| `EXOSNAP_VISUAL_LOG_SCENARIO`, `EXOSNAP_VISUAL_DIAG_SCENARIO`, `EXOSNAP_VISUAL_DIAG_LIVE` | Logs and Diagnostics content |
| `EXOSNAP_VISUAL_NOTIFICATION_SCENARIO=many` | Six advisories in the notification hub, mixed severities. The empty state is the only one a healthy machine produces |
| `EXOSNAP_VISUAL_SOURCE_SCENARIO=many-windows` | Two displays and fifteen windows in the source picker, in place of whatever is open |

The Edit fixture deliberately opens nothing: it never starts a decode or an
export, so the player area reads `Preview unavailable` and the timeline tiles are
placeholders. Everything around them is the real surface.
