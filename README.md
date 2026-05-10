# Recorder MVP Initial Specification Pack

This package contains the canonical initial product, architecture, UI, implementation, and agent-workflow specification for a Windows-native recording application MVP.

## MVP in one sentence

A Windows 11 recorder that captures an application, monitor, or desktop; records video as CFR 60 fps through a native GPU pipeline; captures `APP`, `MIC`, and `SYS` audio sources with reorderable/mergeable track layout; stores recordings primarily as `MKV + AV1 + Opus`; blocks invalid recordings before start; and exposes rich diagnostics and live recording metrics through a simple dark-mode-first UI.

## Product defaults

- **Theme:** Dark mode by default
- **Primary container:** MKV
- **Primary video codec:** AV1 via NVENC
- **Primary audio codec:** Opus
- **Default output frame rate:** CFR 60 fps
- **Default audio source order:** `APP`, `MIC`, `SYS`
- **Default audio source state:** all enabled, all separate tracks
- **Default audio resulting tracks:**  
  1. `APP`  
  2. `MIC`  
  3. `SYS`
- **MVP navigation:**  
  `Record`, `Video`, `Audio`, `Output`, `Hotkeys`, `Diagnostics`, `Logs`, `Advanced`
- **MVP excludes:** overlay/HUD, replay buffer, streaming, HDR, hook-based game capture, export/editor features

## Recommended reading order

1. `product/mvp-scope.md`
2. `architecture/system-overview.md`
3. `architecture/recording-pipeline.md`
4. `architecture/audio-track-model.md`
5. `ui/navigation.md`
6. `implementation/milestone-plan.md`
7. `implementation/model-workflow.md`
8. `prompts/claude-opus-master-prompt.md`

## Pack structure

```text
docs/
  product/
  architecture/
  ui/
  implementation/
  decisions/
  prompts/
```

## Coding standards

- C++ code is formatted with `clang-format`.
- Static analysis baseline uses `clang-tidy`.
- Repository-owned targets compile with strict warnings.
- Third-party dependencies do not inherit project warning flags.
- Developers can run `scripts/pre-commit.ps1` before committing.
- The script runs formatting checks, optional clang-tidy when a compilation database exists, configure/build/test.

## Working rule

This pack is the initial source of truth. If implementation work discovers a real conflict, update the relevant spec and add or revise an ADR before letting the codebase drift.
