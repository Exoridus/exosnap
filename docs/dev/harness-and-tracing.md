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
