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

### FDK-AAC

- **Version:** 2.0.3
- **Project:** https://github.com/mstorsjo/fdk-aac
- **License:** Fraunhofer FDK AAC Codec Library license (see bundled text)
- **Linkage:** static
- **Bundled license:** `licenses/fdk-aac.txt`
- **Note:** Patent licenses for AAC encoding/decoding may be required
  independently from the copyright license. This notice does not provide patent
  rights. Patent licensing is the user's responsibility. Most device
  manufacturers already license the relevant AAC patents.

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
  labels, log viewer, metadata values). UI sans text uses the system-provided
  Segoe UI; no sans-serif font is bundled.

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
