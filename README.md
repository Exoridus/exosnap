#ExoSnap

Windows - native screen / app / region recorder with a professional NVENC pipeline, flexible container / codec profiles,
    multi - track audio routing, webcam overlay,
    and diagnostics.

        ##Product at a glance

            A Windows 11 recorder that captures an application,
    monitor, or region;
records video as CFR 60 fps through a native GPU pipeline;
captures `APP`, `SYS`, and `MIC` audio sources with reorderable / mergeable track layout;
stores recordings primarily as `MKV + H.264 + AAC`;
blocks invalid recordings before start; and exposes rich diagnostics and live recording metrics through a simple dark-mode-first UI.

## Product defaults

- **Theme:** Dark mode by default
- **Default container:** MKV
- **Default video codec:** H.264 NVENC
- **Default audio codec:** AAC
- **Default output frame rate:** CFR 60 fps
- **Default audio source order:** `APP`, `SYS`, `MIC`
- **Default audio source state:** all enabled, all separate tracks
- **Default audio resulting tracks:**  
  1. `APP`  
  2. `SYS`  
  3. `MIC`
- **Navigation:**  
  `Record`, `Video`, `Audio`, `Output`, `Webcam`, `Hotkeys`, `Diagnostics`, `Logs`, `Advanced`
- **1.0 excludes:** streaming, replay buffer, HDR, hook-based game capture, export/editor, Linux parity

## Recommended reading order

1. `.workspace/product/exosnap-1.0-end-spec.md`
2. `.workspace/architecture/system-overview.md`
3. `.workspace/architecture/recording-pipeline.md`
4. `.workspace/architecture/audio-track-model.md`
5. `.workspace/ui/navigation.md`
6. `.workspace/dev/milestone-plan.md`
7. `.workspace/dev/model-workflow.md`

## Pack structure

```text
.workspace/
  product/          mvp-scope, user-flows
  architecture/     system-overview, pipeline, audio-model, settings, telemetry, diagnostics
  ui/               per-page view specs
  decisions/        ADR-0001 through ADR-0010
  design/           visual direction and design reference boards
  dev/              repository-layout, milestone-plan, model-workflow
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

## Continuous integration

GitHub Actions runs configure, build, and test on Windows for both `windows-x64-debug` and `windows-x64-release`. Test logs are uploaded as artifacts only when a CI test step fails. A status badge can be added after the repository exists remotely.
