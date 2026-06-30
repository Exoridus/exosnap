# ExoSnap

Windows-native screen, application, and region recorder with a native NVENC pipeline.
MKV-first recording with on-stop MP4 remux (faststart), multi-track audio routing,
webcam PiP overlay, crash recovery, and built-in diagnostics. Dark-mode-first Qt 6 UI.

## Status / current release

- **Latest release:** `0.8.1` — "Diagnostics as a feature + robust MSI packaging".
- **Pre-v1 notice:** settings, preset, and recording-history schemas may change incompatibly before 1.0.0.
- **Platform:** Windows 10/11 x64 (Windows 11 is the primary target; Windows 10 is best-effort).
- **Hardware encoder:** NVIDIA NVENC only. An NVIDIA GPU with supported NVENC capability is required.
  AMD, Intel, and software encoding are not yet supported.
- **Distribution:** portable ZIP and MSI. Builds are not yet code-signed (see
  [Code signing](#code-signing) below); Windows SmartScreen may warn on first launch.
- **Privacy:** no analytics and no background telemetry. Recordings and settings stay local. The only
  network features are **opt-in and consent-gated**: an update check against public GitHub Releases
  (off by default for self-built binaries) and a privacy-scrubbed crash report you explicitly choose to
  send (Stage 0 assisted GitHub issue, or Stage 1 automated upload to Sentry with EU data residency).
  See [`PRIVACY.md`](PRIVACY.md).

## Features (0.8.1 — diagnostics wave + robust MSI packaging)

- **Diagnostics as a feature** — a first-class diagnostics engine with a typed **`FixAction`** model
  (Auto / Assisted / External) so each detected issue carries the safest concrete remedy.
- **Pre-flight readiness gate** — surfaces notices and hard blockers before a recording starts, so a
  misconfigured capture is caught up front rather than failing mid-record.
- **Live pipeline monitoring** — frame drops, A/V drift, and disk-fill ETA at low cost, with
  encoder-vs-capture-vs-disk-bound classification and root-cause correlation (e.g. VRR-vs-CFR judder).
- **Post-flight report card** — frame-drop %, peak A/V drift, and pipeline health after each recording.
- **`PresentProvider` with PresentMon** — an opt-in, elevation-gated provider for tearing/game-present
  observation feeding the judder correlation.
- **In-app updates** — the Settings Updates card is wired to the consent-gated GitHub-Releases flow.
- **Settings polish** — reliable hover popovers (info-i and the countdown chevron), corner-anchored
  compare hints, and a de-nested Hotkeys card in line with the v10 canon.

### From the 0.7.0 codec & color wave

- **HEVC video** (NVENC) in MKV and MP4 (`hvc1` sample entry), alongside H.264 and AV1.
- **10-bit output** (P010) for HEVC Main10 and AV1 — SDR at higher precision (no HDR transfer curve).
- **BT.709 color metadata** on every MKV/MP4 output, plus a selectable Y'CbCr **color range**
  (Full/Limited). Per-display HDR capability is surfaced in Diagnostics.
- **Recording-error dialog** — a modal failure report with an opt-in, privacy-scrubbed GitHub issue.
- **Theme picker** as four live preview cards (two dark, two light); **single-click** source
  selection commits and returns straight to the preview.

> HEVC, `hvc1`, and 10-bit encoder paths are functional end-to-end but not yet validated across all
> NVIDIA GPU generations under live recording. Use H.264 or AV1 if you encounter issues.

### From the 0.6.0 Audio v2 wave

- **Per-track gain & mute** and a **brickwall limiter** (on by default) on the mixed audio bus, so
  per-source levels can exceed full scale without hard clipping.
- **Microphone DSP chain** — high-pass filter, noise gate, AGC, and **RNNoise** neural noise
  suppression — each stage **off by default** (capture is byte-identical when disabled), toggled
  individually.
- **Lossless audio**: uncompressed **PCM** and compressed **FLAC** (both MKV-only), alongside Opus
  and AAC.
- **Channel / sample-format model** (ADR 0030): selectable output **sample rate** (44.1 / 48 / 96 kHz),
  **mono or stereo**, and **16/24/32-bit** depth for the lossless codecs, plus a **FLAC compression
  level** control. Opus stays at 48 kHz; capture is resampled/rematrixed once after the mix bus.

### From the 0.5.0 settings & media-capability wave

- TOML preset store with profile **export/import**: presets are human-readable and shareable; the
  nested `[[audio.sources]]` model serializes cleanly. Flat app settings stay on QSettings.
- Canonical **video rate control** — Constant quality (CQ), Variable bitrate (VBR), Constant bitrate
  (CBR) — mapped per encoder underneath (NVENC), plus a bitrate control. Never mislabeled as "CRF".
- **Audio encoding** controls: bitrate plus Opus frame duration and complexity.
- **Settings redesign** with a Default/Expert split: common controls up front, expert rate-control,
  bitrate, frame-timing, and audio DSP placeholders behind an Expert toggle, with inline info hints
  and search.
- **Curated themes** (two dark, two light) replacing the free accent picker, so theme colors never
  collide with status colors.
- **Automatic split by size** (in addition to by time): the first limit reached wins; MP4 split
  produces one remuxed progressive segment each.
- Preset **manage dialog** (rename, duplicate, delete, set default).

### Carried over from earlier waves

- Local-first crash capture (out-of-process Crashpad) with a next-launch, privacy-scrubbed,
  consent-gated crash dialog; two-stage delivery (Stage 0 assisted GitHub issue, Stage 1 opt-in
  Sentry upload with EU data residency under the official-build gate).
- Update checking with **Stable** and **Preview** channels and a signed update manifest (ed25519 via
  Monocypher + SHA-256, downgrade/rollback protection); updates off by default for self-built binaries.

- Capture-excluded on-screen overlays (recording status, diagnostics, countdown) — visible on screen
  but excluded from the captured frame via `WDA_EXCLUDEFROMCAPTURE`.
- Tray icon with idle/recording/paused states and an unread notification badge.
- Notification toasts: saved, low storage, unexpected stop, recovery available.
- Close-to-tray (opt-in); opt-in interactive quick-control pill overlay.
- Refined region-selection overlay and capture-frame dock button.
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

## Code signing

ExoSnap participates in the [SignPath Foundation](https://signpath.org) free code-signing
program for open-source projects, with code-signing infrastructure provided by
[SignPath.io](https://signpath.io). Once the certificate is issued, Windows release binaries
(portable ZIP and MSI) will be signed; pre-certificate builds are unsigned, so Windows
SmartScreen may warn on first launch.

## Contributing / CI

GitHub Actions runs configure, build, and test on Windows for both `windows-x64-debug` and
`windows-x64-release`, plus a lint pass. PRs are the review gate.
