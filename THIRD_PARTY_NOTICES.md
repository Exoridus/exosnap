# ExoSnap — Third-Party Notices

ExoSnap is built with and distributes components from the following third-party
projects. This document identifies each component, its version (where
deterministically known), its license, and its linkage or deployment role.

## Bundled dependencies

The license text for each shipped dependency is installed into the `licenses/`
directory alongside the ExoSnap binary.

### spdlog

- **Version:** 1.14.1
- **Project:** https://github.com/gabime/spdlog
- **License:** MIT
- **Linkage:** static
- **Bundled license:** `licenses/spdlog.txt`
- **Note:** spdlog bundles `fmt` (also MIT-licensed). The bundled fmt library is
  statically compiled into ExoSnap.

### nlohmann/json

- **Version:** 3.11.3
- **Project:** https://github.com/nlohmann/json
- **License:** MIT
- **Linkage:** header-only
- **Bundled license:** `licenses/nlohmann_json.txt`

### toml++

- **Version:** 3.4.0
- **Project:** https://github.com/marzer/tomlplusplus
- **License:** MIT
- **Linkage:** header-only
- **Bundled license:** `licenses/tomlplusplus.txt`

### Opus

- **Version:** 1.5.2
- **Project:** https://opus-codec.org / https://github.com/xiph/opus
- **License:** BSD 3-Clause (IETF variant)
- **Linkage:** static
- **Bundled license:** `licenses/opus.txt`

### FDK-AAC (fdk-aac-free, LC-only fork)

- **Version:** 2.0.2 base, `stripped4` branch @
  `529b87452cd33d45e1d0a5066d20b64f10b38845`
- **Project:** https://gitlab.freedesktop.org/wtaymans/fdk-aac-stripped
  (Third-Party Modified Version of the Fraunhofer FDK AAC Codec Library for
  Android; the source lineage Fedora distributes as `fdk-aac-free`)
- **License:** Fraunhofer FDK AAC Codec Library license (see bundled text)
- **Linkage:** static
- **Bundled license:** `licenses/fdk-aac.txt`
- **Note:** This fork removes the patent-encumbered HE-AAC/HEv2 (SBR/PS) code
  paths and retains only AAC-LC encode/decode — the only profile ExoSnap uses.
  Fedora Legal approved distributing this stripped fork alongside GPL code
  (Red Hat Bugzilla #1501522); see ADR 0043 for the project's licensing
  position. Patent licenses for AAC encoding/decoding may still be required
  independently from the copyright license. This notice does not provide patent
  rights. Patent licensing is the user's responsibility. Most device
  manufacturers already license the relevant AAC patents.
- **Scheduled for removal (ADR 0052, supersedes ADR 0043):** ExoSnap is
  migrating its AAC-LC path to FFmpeg's native (LGPL) AAC encoder. FDK-AAC is
  still statically linked and shipped as of this notice, so it remains listed
  here. Once the cutover completes — an encoder-enabled `exosnap-ffmpeg-build`
  release is pinned and `audio_thread.cpp` constructs `FfmpegAacEncoder` — this
  dependency and its bundled license (`licenses/fdk-aac.txt`) will be removed.

### FLAC

- **Version:** 1.4.3
- **Project:** https://xiph.org/flac / https://github.com/xiph/flac
- **License:** BSD 3-Clause (libFLAC)
- **Linkage:** static
- **Bundled license:** `licenses/flac.txt`

### RNNoise

- **Version:** master @ `70f1d256acd4b34a572f999a05c87bf00b67730d`
- **Project:** https://github.com/xiph/rnnoise
- **License:** BSD 3-Clause
- **Linkage:** static
- **Bundled license:** `licenses/rnnoise.txt`
- **Note:** Neural microphone noise suppression. The trained model weights are
  not committed to the upstream git tree; the build downloads the pinned model
  tarball (verified by SHA256 from the repo's `model_version` file) at configure
  time. The tarball is mirrored on a project-owned GitHub release and fetched
  from there by default, falling back to the original upstream host
  `media.xiph.org` (same bytes, same license). ExoSnap builds its own static
  target over the upstream C sources (no upstream CMake).

### libebml

- **Version:** 1.4.5
- **Project:** https://github.com/Matroska-Org/libebml
- **License:** LGPL 2.1
- **Linkage:** static
- **Bundled license:** `licenses/libebml.txt`

### libmatroska

- **Version:** 1.7.1
- **Project:** https://github.com/Matroska-Org/libmatroska
- **License:** LGPL 2.1
- **Linkage:** static
- **Bundled license:** `licenses/libmatroska.txt`

### PresentMon (ETW present-diagnostics consumer)

- **Version:** v1.10.0 (pinned by commit SHA
  `2ce1158783e570539119f577d894252b395cadca`; see ADR 0033)
- **Project:** https://github.com/GameTechDev/PresentMon
- **License:** MIT
- **Linkage:** static
- **Bundled license:** `licenses/presentmon.txt`
- **Note:** Only a vendored subset is compiled — the `PresentData/` in-process ETW
  consumer translation units, not the upstream tool's `main()` executables. Feeds
  the opt-in present/tearing diagnostics correlation. Built by default
  (`EXOSNAP_WITH_PRESENTMON=ON`, the default CMake option); pass
  `-DEXOSNAP_WITH_PRESENTMON=OFF` to omit it from the build entirely.

### Qt

- **Version:** 6.9.0 (open source edition)
- **Project:** https://www.qt.io
- **License:** Available under LGPLv3, GPLv2, GPLv3, or Qt Commercial License.
  This distribution uses the open source edition.
- **Linkage:** dynamic (Core, Gui, Widgets, Svg modules)
- **Bundled license:** `licenses/qt.txt` (canonical LGPLv3 text from the Free
  Software Foundation)
- **Note:** Qt DLLs are deployed alongside the ExoSnap binary by `windeployqt`.
  Under the LGPLv3, users are entitled to relink against modified Qt libraries.
  The specific commercial/Qt licensing terms for this distribution depend on the
  license under which the Qt binary SDK was obtained.

### IBM Plex Mono

- **Project:** https://github.com/IBM/plex
- **License:** SIL Open Font License 1.1 (Copyright © 2017 IBM Corp. with
  Reserved Font Name "Plex")
- **Files:** `IBMPlexMono-Regular.ttf`, `IBMPlexMono-Medium.ttf` (latin),
  compiled into the Qt resource system and loaded via `QFontDatabase`
- **Bundled license:** `licenses/ibm-plex-mono.txt`
- **Role:** the application's monospace UI face (timecode, chips, kicker
  labels, log viewer, metadata values).

### Hanken Grotesk

- **Project:** https://github.com/marcologous/hanken-grotesk
- **License:** SIL Open Font License 1.1 (Copyright © 2021 The Hanken Grotesk
  Project Authors)
- **Files:** `HankenGrotesk-Regular.ttf`, `HankenGrotesk-Medium.ttf`,
  `HankenGrotesk-SemiBold.ttf`, `HankenGrotesk-Bold.ttf`, compiled into the Qt
  resource system and loaded via `QFontDatabase`
- **Bundled license:** `licenses/hanken-grotesk.txt`
- **Role:** the application's primary sans-serif UI face (body text, labels,
  headings). It is not the system-provided Segoe UI.

### FFmpeg

- **Version:** exosnap-ffmpeg-build release `r3` (upstream FFmpeg `n8.1.1`)
- **Project:** https://github.com/Exoridus/exosnap-ffmpeg-build (build/packaging
  repository) / https://ffmpeg.org (upstream FFmpeg source)
- **License:** LGPL-2.1-or-later (mux-only DLL set; no GPL codecs included)
- **Linkage:** dynamic (shared DLLs deployed alongside the ExoSnap binary)
- **Bundled license:** `licenses/ffmpeg.txt`
- **DLLs deployed:** `avformat-62.dll`, `avcodec-62.dll`, `avutil-60.dll`,
  `swresample-6.dll` (~2.3 MB compressed archive; avfilter/swscale/avdevice are not
  built by this component set and are excluded from the portable ZIP)
- **Role:** Post-recording stream-copy remux of MKV → progressive MP4 (`+faststart`).
  No audio/video decoding or re-encoding is performed; the DLLs are used for
  container-level operations only.
- **Note:** ExoSnap is licensed GPL-3.0-or-later, which is compatible with LGPL-2.1.
  No additional obligations arise from adding these LGPL libraries. Users may replace
  the DLLs with compatible versions to exercise their LGPL rights. The
  exosnap-ffmpeg-build repository builds this exact mux-only component set from
  unmodified upstream FFmpeg `n8.1.1` sources — it does not fork or patch FFmpeg.

## Official-release-build-only dependencies (crash capture)

The following components are fetched, linked, and shipped only in official release
binaries built with `EXOSNAP_OFFICIAL_BUILD=ON` (see
`.github/workflows/release-candidate.yml`), which also turns on
`EXOSNAP_ENABLE_CRASH_CAPTURE=ON`. A self-build with the default CMake options does
not download, link, or ship any of the three components below, and does not produce
`crashpad_handler.exe`.

### sentry-native

- **Version:** 0.15.0
- **Project:** https://github.com/getsentry/sentry-native
- **License:** MIT
- **Linkage:** static
- **Bundled license:** `licenses/sentry-native.txt`
- **Role:** out-of-process crash-capture client library (ADR 0017); configured with
  the Crashpad backend and WinHTTP transport.

### Crashpad

- **Version:** vendored by sentry-native 0.15.0 as a pinned git submodule (no
  independent upstream release tag)
- **Project:** https://github.com/chromium/crashpad (fetched via sentry-native's
  `external/crashpad` submodule)
- **License:** Apache-2.0
- **Linkage:** dynamic — a separate binary, `crashpad_handler.exe`, deployed
  alongside `exosnap.exe`
- **Bundled license:** `licenses/crashpad.txt`
- **Role:** out-of-process crash-handler executable launched by sentry-native to
  observe the main process and write minidumps on crash.

### mini_chromium

- **Version:** vendored by Crashpad as a pinned git submodule of the sentry-native
  0.15.0 tree (no independent upstream release tag)
- **Project:** https://github.com/chromium/mini_chromium
- **License:** BSD 3-Clause
- **Linkage:** static (compiled into `crashpad_handler.exe`)
- **Bundled license:** `licenses/mini_chromium.txt`
- **Role:** minimal Chromium base-library subset required to build Crashpad as a
  standalone component outside the full Chromium tree.

## Build-only dependencies (not shipped)

These components are used during the build process and are not included in the
distributed binary.

### GoogleTest

- **Version:** 1.14.0
- **Project:** https://github.com/google/googletest
- **License:** BSD 3-Clause
- **Role:** build-only (test framework)

### NVIDIA Video Codec SDK

- **Project:** https://developer.nvidia.com/nvidia-video-codec-sdk
- **Role:** build-only (header file `nvEncodeAPI.h`). No SDK binaries are
  shipped. NVENC runtime support is provided by the NVIDIA display driver,
  which is a system component.

## System/runtime dependencies (not shipped)

ExoSnap uses the following Windows system APIs and redistributables. These are
provided by the Windows operating system and are not bundled.

- **Direct3D 11, DXGI** — Windows graphics infrastructure
- **Media Foundation** — Windows multimedia framework
- **WASAPI** — Windows audio session API
- **Microsoft Visual C++ Runtime** — `exosnap.exe` and the shipped Qt6 DLLs
  link the dynamic MSVC runtime (`/MD`). It is **not bundled** in any ExoSnap
  artifact (MSI, portable ZIP, Chocolatey, or Scoop package). The WinGet
  package declares `Microsoft.VCRedist.2015+.x64` as a dependency and installs
  it automatically. Users installing via the MSI, the portable ZIP,
  Chocolatey, or Scoop must install the Microsoft Visual C++ 2015-2022
  Redistributable (x64) themselves from
  <https://aka.ms/vs/17/release/vc_redist.x64.exe> if it is not already present.

---
*This document is maintained in the ExoSnap repository. For questions, see
the project README.*
