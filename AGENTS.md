# AGENTS.md

## Project intent

Build a Windows-native, diagnostics-first recording application MVP with a high-performance C++ engine and a Qt 6 + Qt Widgets user interface.

## Canonical product decisions

- Dark mode is the default app theme.
- MVP navigation is exactly:
  - Record
  - Video
  - Audio
  - Output
  - Hotkeys
  - Diagnostics
  - Logs
  - Advanced
- MVP excludes overlay/HUD work.
- Primary recording profile:
  - WebM (libwebm writes DocType=webm for all V_AV1+A_OPUS output)
  - AV1
  - Opus
  - CFR 60 fps
- Default audio source order:
  1. APP
  2. MIC
  3. SYS
- Default audio state:
  - all sources enabled
  - all sources separate tracks
- The exact UI label is **`Merge with above`**. Do not rename it.
- Recording start must be blocked when diagnostic blockers exist.
- UI must not duplicate track resolution logic. It submits editable source rows; the engine returns resolved tracks.
- MP4 must expose only compatible audio codecs. Opus must not be offered for MP4.
- When switching containers, the selected audio codec must be reconciled to a valid codec for the new container.
- MP4 shows a calm informational note about lower crash resilience than MKV.
- The Record view contains a live preview before recording and a technical recording view while recording.
- If a hotkey starts recording while the app window is visible, activate the Record view. If minimized, do not restore the window.

## Architectural rules

- Keep the recording engine independent from UI concerns.
- Keep capture, encode, mux, diagnostics, telemetry, and UI responsibilities separate.
- Use structured data models rather than ad hoc UI-bound state.
- Prefer explicit state machines for recording session lifecycle.
- Every live metric must have:
  - source
  - meaning
  - update cadence
  - UI consumer
  - log consumer
- Prefer isolated capability probes before production integration.
- Do not optimize speculatively; add profiling hooks and measure.

## Documentation rules

- Update specs when product behavior changes.
- Add an ADR for any cross-cutting architectural decision.
- Each milestone must end with:
  - implementation notes
  - tests
  - doc updates
  - explicit unresolved issues, if any

## Agent workflow

- Opus owns architecture, product decisions, cross-cutting reviews, and final approval.
- Sonnet implements substantial bounded features from approved specs.
- Codex handles repo bootstrap, mechanical work, tests, refactors, build fixes, and explicit task lists.
- No agent should silently expand MVP scope.

## Fast Iteration Policy

This policy applies to all future ExoSnap agent sessions.

### Scope

- Normal feature slices target 30-60 minutes.
- Keep each slice to one subsystem.
- Do not broaden scope without a blocking technical need.
- Record minor P2/polish findings for consolidated review instead of fixing everything immediately.

### Agent use

- Do not launch multiple Explore agents when direct repository inspection is sufficient.
- Use at most one architecture/exploration pass for normal slices.
- Use workers only for meaningful, clearly separated packages.
- Run independent workers in parallel worktrees only when their file ownership is disjoint.
- Never let workers concurrently edit high-churn files such as:
  - `MainWindow.cpp`
  - `RecordPage.cpp`
  - `ConfigPage.cpp`
  - shared stores/view models
  - shared CMake files

### Development validation

During implementation, workers should use the smallest sufficient validation:

1. Build only the affected target.
2. Run focused tests for the changed subsystem.
3. Do not run full Debug builds, full CTest, quality suite, and Release build after every worker or edit cycle.

Canonical principle: `Minimal validation during development; complete validation once at the final gate.`

### Final validation

Run once after the integrated branch is complete:

- format check
- `git diff --check`
- full Debug build
- focused tests if still useful
- full CTest
- quality/static checks once
- Release build only at the final gate or when Release-specific behavior is in scope

Do not invoke `check-quality.ps1` after already running the same complete configure/build/test sequence unless the script provides additional required checks that cannot be invoked separately. Prefer invoking only the missing static-analysis step when possible.

### Test and visual budgets

For a normal feature slice:

- approximately 5-15 targeted new tests
- approximately 2-4 Visual Harness scenarios
- no exhaustive matrix unless the feature is inherently high-risk
- no fragile physical-device or multi-step external automation

### Acceptance

- Green automated tests and clean deterministic visual scenarios are sufficient for implementation waves.
- Physical hardware checks and broad visual/product review are deferred to consolidated final review rounds.
- One or two documented minor limitations do not block merge when core behavior is correct.

## Project agent roles

Agent profiles exist for two tools:

- **OpenCode**: `.opencode/agent/` — model assignment, permission set, system prompt; invoke with `opencode --agent <name>`
- **Claude Code**: `.claude/agents/` — tool restrictions, system prompt; invoke with `/agents` picker or `@<name>`

Both sets define the same five roles with equivalent behavior.

| Agent | Model intent | Role |
|---|---|---|
| `exo-architect` | Claude Opus | Architecture, hard API rescue (NVENC/WASAPI/MF/WAS), rescue planning, implementation briefs. Read-first; writes only when explicitly asked. |
| `exo-builder` | DeepSeek V4 Pro | Bounded implementation from approved briefs. Stops on first gate failure. No scope expansion. |
| `exo-hygiene` | DeepSeek V4 Pro | Formatting, branch cleanup, small CMake registration, docs-only mechanical updates. Touches only explicitly allowed files. |
| `exo-research` | Claude Opus | Primary-source research and API verification. Read/web only; cites official sources; stops if sources unavailable. |
| `exo-reviewer` | Claude Opus | Final PR review before commit/merge. Scope compliance, diff review, acceptance checklist. Read/diff only by default; does not silently fix. |

**Shared rules for all agents:**
- One branch = one PR = one task. Do not mix unrelated fixes.
- Do not expand MVP scope silently.
- Do not commit unless explicitly instructed.
- At end of work: summary / files changed / commands + exit codes / unresolved issues / `git diff --stat` / `git status --short`.
- For hard APIs (NVENC, WASAPI Process Loopback, Media Foundation, Qt internals): cite official primary sources; stop if unavailable.
- Stop on first failing validation gate unless explicitly told to continue.
- Never restore old WIP stashes that contain unrelated files or generated binaries.
- Generated binaries must not be committed.
