# 0065 — The main window's first show is owned by C++, not by QML

- Status: accepted
- Date: 2026-08-11
- Related: ADR 0064 (Qt Quick is the main frontend)

## Context

`Main.qml` declared `visible: true`, so the main window became visible part-way
through `QQmlApplicationEngine::load()`. Measured on the shipping build, that is
earlier than the window is ready to be seen.

The order Qt actually runs, captured with `--window-trace` (`window-msg:` lines
are Win32 messages, `window-trace:` lines are sampled state):

1. `QuickWindowChrome` attaches and calls `QQuickWindow::create()` to guarantee a
   non-null `winId()`. The HWND is created here — with the style Qt gives a plain
   window: `WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX |
   WS_MAXIMIZEBOX` (`0x86cf0000`), because `Qt::FramelessWindowHint` is part of
   the `flags` binding and has not been evaluated yet.
2. The `x`, `y` and `width` bindings are applied one at a time. Each one goes
   through `QWindowsWindow::setGeometry`, which adds the frame margins of the
   style the window currently has — 8 left, 31 top, 8 right. A requested
   `400,120 1280x820` reaches Windows as `392,89 1296x820`.
3. `flags` is applied. Qt rewrites the style to `WS_POPUP` (`0x86000000`) and
   does **not** re-apply the geometry. The offset is now baked in.
4. `visible: true` shows the window on that rect.

Two defects fell out of that single ordering:

- **The startup jump.** The first frame the user saw was `392,89 1296x820`; a
  deferred correction on `frameSwapped` moved it to `400,120 1280x820` a frame
  later. Correcting after the first frame cannot be anything but visible.
- **`WS_THICKFRAME` silently absent.** `ensureResizableStyle()` ran at step 1,
  found the bit already set by the temporary framed style, and returned. Step 3
  then removed it. The shipped window had `thickframe=0` — no native resize drag,
  no Aero Snap, no Win+Arrow — and nothing about it looks wrong in a screenshot.

Nothing declarative can fix this, because the ordering is Qt's: QML offers no
point that is after the flags and before the first show.

## Decision

`Main.qml` sets `visible: false`, and `QuickApplication::load()` is the single
owner of the main window's first show. Between the engine load and that show, and
in this order, it:

1. re-asserts the final native style (`QuickWindowChrome::applyNativeWindowStyle`)
   — after Qt's last style write, so it survives;
2. places the window on its resolved geometry
   (`ApplyStartupWindowGeometry`), whose result is verified against Windows and
   corrected if the two disagree — invisibly, because nothing is on screen;
3. makes the window visible (`showMaximized()` for a maximized restore, so the
   first visible state is the maximized one and the rect above stays its restore
   rect).

Activation is not requested anywhere in that sequence. A `--no-activate` start
withholds focus through `Qt::WindowDoesNotAcceptFocus` in the flags, exactly as
before.

The post-frame correction is removed rather than kept as a safety net. What
remains at the same point is a check: if the first frame is not on the rect the
window was placed on, that is logged as a warning and left alone, because a
correction there is a frame the user has already seen in the wrong place.

## Consequences

- There is exactly one place that makes the main window visible for the first
  time, and it is stated in both files. A future `visible: true` in QML would
  reintroduce the jump, which is why the QML comment says so explicitly.
- Every harness goes through the same lifecycle. None of them shows the window
  itself; `--visual-test-size` still takes the size over afterwards via
  `QuickWindowGeometry::detach()`, unchanged.
- `--hwnd-audit` now **asserts** `WS_THICKFRAME` rather than only reporting it.
  The former objection — that the bit makes Qt believe the window has a frame and
  misplace it — does not survive measurement: with `Qt::FramelessWindowHint`
  applied, Qt reports frame margins of `0,0,0,0` with the bit set.
- The premature `create()` in `QuickWindowChrome::setTarget` is deliberately left
  in place. It still causes one framed-then-restyled HWND and one Qt
  `setGeometry` warning during startup, but now entirely while the window is
  hidden. Removing it would defer the HWND past QML completion and take the
  native `WM_SETICON` path with it; that is a separate change with its own
  regression surface.
