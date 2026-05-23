# CLAUDE.md

You are working on a Windows-native recording application MVP.

## Before doing anything

Read:

1. `README.md`
2. `AGENTS.md`
3. `docs/product/mvp-scope.md`
4. `docs/architecture/system-overview.md`
5. Any document directly related to the task

## Immutable MVP decisions unless the user explicitly changes them

- Dark mode by default
- No overlay/HUD in MVP
- Default audio source order: `APP`, `MIC`, `SYS`
- Default resulting tracks: `APP`, `MIC`, `SYS`
- Exact label: `Merge with above`
- Primary profile: `WebM + AV1 + Opus + CFR 60 fps`
- Recording start is blocked by diagnostic blockers
- Main UI pages:
  - Record
  - Video
  - Audio
  - Output
  - Hotkeys
  - Diagnostics
  - Logs
  - Advanced

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
