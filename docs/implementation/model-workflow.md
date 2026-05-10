# Model Workflow

## Recommended model mix

### Claude Opus
Use for:
- architecture
- product decisions
- ADRs
- deep reviews
- cross-cutting changes
- milestone closure

### Claude Sonnet
Use for:
- substantial bounded feature implementation
- probes
- engine modules
- UI pages against approved specs

### Codex
Use for:
- repo bootstrap
- mechanical implementation
- tests
- refactors
- build fixes
- repetitive subtasks
- explicit patch lists

## Standard milestone cycle

1. **Opus**
   - interpret spec
   - create implementation brief
   - identify risks
   - split tasks
2. **Sonnet**
   - implement major feature branch
3. **Codex**
   - execute explicit subtasks
   - tests
   - cleanup
4. **Opus**
   - review architecture, correctness, scope
5. **Codex**
   - apply concrete review fixes
6. **Opus**
   - approve milestone and update docs/ADRs

## Task assignment by milestone

| Milestone | Lead | Support |
|---|---|---|
| M0 Specification | Opus | — |
| M1 Foundation | Codex | Opus review |
| M2 Probes | Sonnet | Codex |
| M3 Core Models | Opus | Sonnet |
| M4 Audio Engine | Sonnet | Opus review, Codex tests |
| M5 Video Engine | Sonnet | Opus review, Codex tests |
| M6 Session/Muxing | Sonnet | Opus review |
| M7 Diagnostics | Sonnet | Opus spec, Codex checks |
| M8 UI MVP | Sonnet | Codex |
| M9 Hardening | Opus | Sonnet + Codex |
| M10 Release | Opus | Codex |

## Guardrails

- No silent MVP expansion.
- No agent may rename `Merge with above`.
- No UI duplication of track resolver logic.
- No code-first change to product behavior without spec update.
- No assumption that supported-by-UI means valid-at-runtime; engine validates independently.
