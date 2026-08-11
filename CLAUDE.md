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
  - **Settings** hosts recording configuration across embedded sections (Container & codecs, Quality & timing, Audio, Output, Webcam, Overlays, Notifications & presence, Hotkeys, Updates, Appearance, Developer); no section is Expert-only — a shared Expert toggle reveals additional rows in place per section instead
  - **Overlays** configures the capture-excluded on-screen surfaces by behaviour and content (preset + per-element), never by visual token; the former single "Notifications & overlays" card was split once the overlays gained content configuration
  - **Device** hosts adapter selection + the per-GPU capability matrix (moved out of Diagnostics)
  - **Edit/Output/Save** is an overlay over the Record page (ADR 0022), not a nav item

## Coordinate before driving the running application

The developer works on the same machine and may be doing anything else on it (gaming with a
controller, moving the mouse) at the same moment. The actual failure mode this guards against
is **uncoordinated** input: taking window focus while a controller is in use breaks controller-
input recognition, and moving the OS cursor while the developer is moving it themselves causes
mis-clicks on their side. (Origin: PR #154 — an agent used `EnumWindows`/`GetWindowRect` hunting
for the app window and took over the mouse mid-session without warning.) This is **not** a
categorical ban on ever interacting with a running instance — it is a coordination requirement.

- **Never synthesize mouse/keyboard input or steal window focus without asking first and
  getting confirmation that the developer isn't actively using the mouse/keyboard/controller
  right now.** Ask in the same turn, before triggering anything; don't assume a earlier "go
  ahead" still holds later in the session. This applies to any application, not just ExoSnap.
- **UAC / Secure Desktop prompts cannot be scripted at all** — Windows blocks synthetic input
  across the Secure Desktop boundary by design. Tell the developer in advance exactly what the
  prompt will ask and what accept/decline does, then wait for them to have read that before
  triggering it — they cannot have another window open while responding to a Secure Desktop
  prompt.
- **Physical/hardware and system-level settings stay the developer's own action** even inside
  an otherwise-automated flow (e.g. turning HDR on, changing display refresh rate, unplugging an
  audio endpoint).
- Prefer structural automation (UI Automation invoke patterns / accessible names) over raw
  coordinate-based mouse synthesis where available — it does not move the real OS cursor and so
  cannot collide with the developer's own pointer.
- Starting the app **once** to confirm it does not crash at startup is always allowed (e.g.
  after a QSS theme edit); confirm the process survives, then close it.
- Judge pixels with the `--visual-test` render harness and behavior with the widget tests first
  — reach for live driving only when nothing else can verify the change, and say so.
- `--auto-record` is the same class of exception as `--visual-test`: CLI/env-configured, never
  mouse/keyboard synthesis or window automation. Bare mode never creates a window; preview mode
  creates one off-screen only to reuse the existing preview/hub and screenshot machinery, never to
  click through it. Recording output always goes to a scratch directory (`EXOSNAP_OUTPUT_DIR` when
  set, otherwise the system temp directory) and is never committed.

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
