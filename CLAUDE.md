#CLAUDE.md

You are working on a Windows-native recording application MVP.

## Before doing anything

Read:

1. `README.md`
2. `AGENTS.md`
3. `.workspace/product/exosnap-1.0-end-spec.md`
4. `.workspace/architecture/system-overview.md`
5. Any document directly related to the task

## Product decisions (authoritative source: `.workspace/product/exosnap-1.0-end-spec.md`)

- Dark mode by default
- Default audio source order: `APP`, `SYS`, `MIC`
- Default resulting tracks: `APP`, `SYS`, `MIC`
- Exact label: `Merge with above`
- Default profile: `MKV + AV1 + Opus + CFR 60 fps`
- Recording start is blocked by diagnostic blockers
- Main UI pages:
  - Record
  - Video
  - Audio
  - Output
  - Webcam
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
