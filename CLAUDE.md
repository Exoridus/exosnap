#CLAUDE.md

You are working on a Windows-native recording application MVP.

## Before doing anything

Read:

1. `README.md`
2. `AGENTS.md`
3. `docs/product-spec.md` — the tracked product specification (durable source for user-visible behavior)
4. `.workspace/architecture/system-overview.md`
5. Any document directly related to the task

## Product decisions (authoritative source: `docs/product-spec.md`)

- Dark mode by default
- Audio source row order: `APP`, `SYS`, `MIC`. The `APP` row is always present and configurable; it takes effect while a specific application window is the capture target. For screen capture the shipped default is `SYS` on, `MIC` off
- Each enabled source becomes its own resulting track unless merged with the row above
- Exact label: `Merge with above`
- Default profile: `MKV + AV1 + Opus + CFR 60 fps`
- Recording start is blocked by diagnostic blockers
- Top-level navigation: **Record, Device, Settings, Diagnostics, Logs, About** (6 items; Hotkeys moved into Settings as an embedded card — PS-PHASE-C; Device tab added in the final-redesign port)
  - **Settings** hosts recording configuration across embedded sections (Container & codecs, Quality & timing, Audio, Output, Webcam, Notifications & overlays, Hotkeys, Updates, Appearance, Developer); no section is Expert-only — a shared Expert toggle reveals additional rows in place per section instead
  - **Device** hosts adapter selection + the per-GPU capability matrix (moved out of Diagnostics)
  - **Edit/Output/Save** is an overlay over the Record page (ADR 0022), not a nav item

## Never drive the running application

The developer works on the same machine. Taking over the pointer or the foreground window
interrupts them and is not acceptable.

- **Never interact with a running ExoSnap instance.** No mouse or keyboard synthesis, no
  `EnumWindows` / `GetWindowRect` / `SetForegroundWindow`, no window enumeration, no clicking,
  no screenshots of the live app.
- Starting the app **once** to confirm it does not crash at startup is allowed, and is required
  after editing a QSS theme (an invalid `${token}` crashes at launch). Confirm the process
  survives, then close it. Nothing else.
- Judge pixels with the `--visual-test` render harness. Judge behavior with the widget tests.
- `--auto-record` is the same class of exception as `--visual-test`: CLI/env-configured, never
  mouse/keyboard synthesis or window automation. Bare mode never creates a window; preview mode
  creates one off-screen only to reuse the existing preview/hub and screenshot machinery, never to
  click through it. Recording output always goes to a scratch directory (`EXOSNAP_OUTPUT_DIR` when
  set, otherwise the system temp directory) and is never committed.
- If a change can only be verified by clicking through the live app, **stop and ask the developer
  to do it.** Never do it yourself, and never do it without asking first.

## Work style

- Do not collapse product and implementation decisions together.
- Do not infer undocumented behavior from convenience.
- Keep the engine UI-agnostic.
- Prefer explicit models and pure resolver logic where possible.
- When implementing anything that changes visible behavior, update the relevant spec.
- When reviewing code, look for:
  - duplicated track-resolution logic
  - invalid container/codec combinations
  - missing blocker propagation
  - speculative overengineering
  - hidden MVP expansion
  - UI logic leaking into engine internals

## Deliverable format for feature work

For every substantial task, provide:

1. Summary
2. Files changed
3. Behavior implemented
4. Tests added/updated
5. Remaining limitations
6. Whether specs or ADRs were updated
