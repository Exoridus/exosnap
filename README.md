# ExoSnap

Windows-native screen, application, and region recorder with a native NVENC pipeline.
MKV-first recording with on-stop MP4 remux (faststart), multi-track audio routing,
webcam PiP overlay, crash recovery, and built-in diagnostics. Dark-mode-first Qt 6 UI.

## Status / current release

- **Development version:** `0.2.0` — "Reliability foundation" (unreleased; landing on main now).
- **Latest published portable build:** `0.1.0`.
- **Pre-v1 notice:** settings, preset, and recording-history schemas may change incompatibly before 1.0.0.
- **Platform:** Windows 10/11 x64 (Windows 11 is the primary target; Windows 10 is best-effort).
- **Hardware encoder:** NVIDIA NVENC only. An NVIDIA GPU with supported NVENC capability is required.
  AMD, Intel, and software encoding are not yet supported.
- **Distribution:** portable ZIP — no installer, no code signing yet. Windows SmartScreen may warn on
  first launch.

## Features (0.2.0 reliability wave)

- MKV-first recording; MP4 delivered via libavformat remux-on-stop (progressive, faststart); MKV and
  WebM are also supported as direct output formats.
- Crash recovery: a recovery manifest written before each session, plus a startup recovery UI that
  lets you finish, recover, or discard each interrupted recording on the next launch.
- Low-disk guard: warning at a configurable soft threshold, hard stop at a lower threshold; for MP4
  sessions the effective threshold accounts for the transient MKV and output MP4 coexisting during remux.
- FAT32 output-volume detection: a Diagnostics Notice about the 4 GiB per-file limit (recording is not
  blocked; short clips on FAT32 work correctly).
- Container/codec compatibility registry: invalid combinations are blocked before recording starts.
- Recording split with per-segment background MP4 remux for split MP4 sessions.
- Global hotkeys, single-frame capture, webcam PiP with mirror, live A/V drift and size overlay.

See [`KNOWN_LIMITATIONS.md`](KNOWN_LIMITATIONS.md) for the precise current support boundary.

## Product defaults

| Setting | Default |
|---------|---------|
| Theme | Dark mode |
| Container | MKV |
| Video codec | AV1 (NVENC) |
| Audio codec | Opus |
| Frame rate | CFR 60 fps |
| Audio source order | `APP`, `SYS`, `MIC` — all enabled, all separate tracks |
| Navigation pages | `Record`, `Video`, `Audio`, `Output`, `Webcam`, `Hotkeys`, `Diagnostics`, `Logs`, `Advanced` |

## Install / run

See [`README-PORTABLE.md`](README-PORTABLE.md) for the portable-release quick start.

**Runtime prerequisite:** the **Microsoft Visual C++ 2022 x64 Redistributable** is required.
It is normally already present on up-to-date Windows systems. If the app fails to start with a
missing-DLL error, install it from <https://aka.ms/vs/17/release/vc_redist.x64.exe>.

The portable build is not code-signed. Windows SmartScreen may warn on first launch — this is expected.

## Building from source

### Prerequisites

- Visual Studio 2022 (Desktop development with C++ workload) — provides `cl.exe`, `MSBuild`, and the
  Windows SDK.
- CMake 3.25 or newer.
- Git.

### Fast local development loop

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

`scripts\check-quality.ps1` keeps its default configure/build/test behavior for humans and hooks.
Use `-StaticOnly` after a full build and CTest have already run, so final validation does not rebuild
solely to run standalone static checks.

### Ninja (optional, faster builds)

Install Ninja once:

```powershell
winget install --id Ninja-build.Ninja
```

Ninja builds **must** be launched from a VS Developer PowerShell or Command Prompt so that `cl.exe`
is on the PATH. Available Ninja configure presets: `windows-x64-ninja-debug` / `windows-x64-ninja-release`.

```powershell
# From a VS Developer PowerShell/Command Prompt:
cmake --preset windows-x64-ninja-debug
cmake --build --preset windows-x64-ninja-debug-exosnap
```

### sccache (optional, compiler cache)

Install sccache once:

```powershell
winget install --id Mozilla.sccache
```

Enable at configure time: `-DEXOSNAP_USE_SCCACHE=ON`. Works with both Visual Studio and Ninja generators.

```powershell
cmake --preset windows-x64-ninja-release -DEXOSNAP_USE_SCCACHE=ON
cmake --build build/windows-x64-ninja-release --target exosnap
```

**Known limitation:** sccache can encounter PDB contention errors with MSVC Debug builds when
third-party libraries enforce `/Zi`. Prefer sccache with Release builds or use `/Z7` to embed debug
info in object files.

### Coding standards

- C++ code is formatted with `clang-format`.
- Static analysis baseline uses `clang-tidy`.
- Repository-owned targets compile with strict warnings.
- Run `scripts\pre-commit.ps1` before committing. It runs formatting checks, optional clang-tidy, and
  configure/build/test.

## Repo layout

```text
app/          application source (UI, models, recording pipeline)
libs/         internal libraries (recorder_core, capability, etc.)
docs/
  roadmap.md  version roadmap and architecture guardrails
  decisions/  architecture decision records (ADRs)
packaging/    WinGet, Chocolatey, and Scoop packaging manifests
scripts/      build, quality, and release scripts
tests/        test targets
cmake/        CMake helper modules (VendorFFmpeg, etc.)
third_party/  vendored third-party sources (libmatroska, libebml, etc.)
tools/        developer tooling
```

## Third-party / licensing

- ExoSnap is licensed under **GPL-3.0-or-later**; see [`LICENSE`](LICENSE).
- ExoSnap bundles FFmpeg as **LGPL-2.1-or-later** shared DLLs (dynamic linking). Third-party component
  licenses are listed in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
- The FFmpeg binaries are produced by the companion repository
  [Exoridus/exosnap-ffmpeg-build](https://github.com/Exoridus/exosnap-ffmpeg-build).

## Contributing / CI

GitHub Actions runs configure, build, and test on Windows for both `windows-x64-debug` and
`windows-x64-release`, plus a lint pass. PRs are the review gate.
