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

## Fast local development workflow

Use minimal validation during development, then run the complete gate once before merge.

Implementation loop:

```powershell
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug-exosnap
cmake --build --preset windows-x64-debug-presentation-state-tests
cmake --build --preset windows-x64-debug --target <focused_test_target>
ctest --preset windows-x64-debug -R "<focused_test_regex>" --output-on-failure
scripts\check-format.ps1
git diff --check
```

Final gate:

```powershell
scripts\check-format.ps1
git diff --check
cmake --build --preset windows-x64-debug
ctest --preset windows-x64-debug --output-on-failure
scripts\check-quality.ps1 -StaticOnly
cmake --build --preset windows-x64-release-exosnap
```

`scripts\check-quality.ps1` keeps its default configure/build/test behavior for humans and hooks. Use `-StaticOnly` after a full build and CTest have already run, so final validation does not rebuild solely to run standalone static checks.

### Ninja (optional, faster builds)

Install Ninja once:

```powershell
winget install --id Ninja-build.Ninja
```

Verify:

```powershell
ninja --version
```

The repository includes Ninja presets alongside the default Visual Studio presets. Ninja builds **must** be launched from a VS Developer PowerShell or Command Prompt so that `cl.exe` is on the PATH.

```powershell
# From a VS Developer PowerShell/Command Prompt:
cmake --preset windows-x64-ninja-debug
cmake --build --preset windows-x64-ninja-debug-exosnap
cmake --build --preset windows-x64-ninja-debug-presentation-state-tests
```

Available Ninja presets:
- `windows-x64-ninja-debug` / `windows-x64-ninja-release` (configure)
- `windows-x64-ninja-debug-exosnap` / `windows-x64-ninja-release-exosnap` (app-only build)
- `windows-x64-ninja-debug-presentation-state-tests` (focused test build)
- `windows-x64-ninja-debug` (test preset)

### sccache (optional, compiler cache)

Install sccache once:

```powershell
winget install --id Mozilla.sccache
```

Verify:

```powershell
sccache --version
sccache --show-stats
```

Enable sccache at configure time by adding `-DEXOSNAP_USE_SCCACHE=ON`. This works with both the Visual Studio and Ninja generators.

```powershell
# From a VS Developer PowerShell/Command Prompt:
cmake --preset windows-x64-ninja-release -DEXOSNAP_USE_SCCACHE=ON
cmake --build build/windows-x64-ninja-release --target exosnap
```

**Known limitation:** sccache can encounter PDB contention errors with MSVC Debug builds when third-party libraries (e.g. spdlog) enforce `/Zi`. Prefer sccache with Release builds or use `/Z7` to embed debug info in object files. Debug builds without sccache work reliably with both generators.

To disable sccache for a previously configured build, reconfigure without the option or clear the build directory.

Fall back to the default Visual Studio presets at any time. Existing builds continue to work without Ninja or sccache.

## Working rule

This pack is the initial source of truth. If implementation work discovers a real conflict, update the relevant spec and add or revise an ADR before letting the codebase drift.

## Continuous integration

GitHub Actions runs configure, build, and test on Windows for both `windows-x64-debug` and `windows-x64-release`. Test logs are uploaded as artifacts only when a CI test step fails. A status badge can be added after the repository exists remotely.
