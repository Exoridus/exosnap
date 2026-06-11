# ExoSnap 0.1.0 — Known Limitations

This document describes the current support boundary of ExoSnap **0.1.0**. It is
factual and specific to this build. If a capability is not listed here as
supported, do not assume it is available.

## Release status

- ExoSnap 0.1.0 is a **pre-v1 Windows preview**, not a final 1.0 release.
- Configuration, preset, and recording-history file schemas are **not frozen**
  and may change in incompatible ways before 1.0.0.
- Keep your own backup copies of presets you care about during preview releases.
  ExoSnap does not wipe existing data on upgrade, but forward/backward
  compatibility across preview versions is not guaranteed.

## Platform

- **Windows 10/11 x64 only.** Windows 11 is the primary target; Windows 10 is
  best-effort.
- No Windows ARM64, Linux, or macOS build.
- An **NVIDIA GPU with supported NVENC capability is currently required** for
  video encoding (RTX 20-series or newer recommended, with a current NVIDIA
  display driver).
- The Microsoft Visual C++ 2022 x64 runtime is required. It is normally already
  present on up-to-date Windows systems; otherwise install it from
  <https://aka.ms/vs/17/release/vc_redist.x64.exe>. The WinGet package
  installs this automatically as a declared dependency; MSI, portable ZIP,
  Chocolatey, and Scoop installs do not bundle it.

## Hardware encoding

Only **NVIDIA NVENC** video encoding is supported in this release. The following
are **not** available and are **not** implied by this build:

- AMD AMF hardware encoding
- Intel Quick Sync / oneVPL hardware encoding
- Software (CPU) H.264 or AV1 encoding fallback

If a supported NVIDIA NVENC encoder is not detected, recording is blocked with a
diagnostic message rather than silently falling back.

## Containers and codecs

Supported containers:

| Container | Status     |
| --------- | ---------- |
| MKV       | Supported  |
| WebM      | Supported  |
| MP4       | Supported (normal recording) |

Supported encoders actually selectable and validated in this build:

- **Video:** H.264 (NVENC) and AV1 (NVENC, where the installed GPU and driver
  expose it). HEVC is **not** implemented in 0.1.0.
- **Audio:** AAC-LC (`AAC` in the UI) and Opus. PCM and FLAC are **not**
  implemented in 0.1.0.

Container/codec rules:

- MP4 uses H.264 + AAC. Opus is not offered for MP4.
- WebM uses AV1 + Opus.
- MKV is the flexible default container.

Exact codec availability depends on your **NVIDIA GPU generation, driver
version, the selected container, and the selected video/audio combination**.
Invalid combinations are not offered.

## Recording split

- Recording **split is supported for MKV and WebM only**.
- **MP4 recording split is not available** in 0.1.0.
- Already-finalized split segments remain independently usable.

## Crash safety and recovery

- There is **no complete crash-recovery workflow** yet.
- Progressive MP4 is **not crash-safe**: if the application or system is
  interrupted while an MP4 is being written, the active file may be unusable.
- For MKV/WebM split recordings, segments that were already finalized before an
  interruption remain usable; an interrupted **active** segment may not be
  recoverable.
- Fragmented MP4 (fMP4) and recovery manifests are planned work after 0.1.0 and
  are not present in this build.
- MP4 in 0.1.0 does not guarantee `hvc1`/Apple-style HEVC compatibility; HEVC is
  not implemented.

## Other current limitations

- No automatic update checking or in-app updater.
- No installer and no code signing; this is a portable ZIP. Windows SmartScreen
  may warn on first launch.
- No Replay Buffer.
- No built-in editor, trimming, or Quick Trim.
- No HDR10 / 10-bit pipeline (8-bit 4:2:0 only).
- No multi-vendor hardware-encoder matrix (NVIDIA only — see above).
- Stable display identity uses the GDI device name (for example `\\.\DISPLAY1`),
  which can be reassigned on a monitor topology change. A saved Region or Display
  target may point to a different physical monitor after a reboot or
  reconnect/mode-set; re-select the source manually in that case.
- Hot-swap during recording is not supported. Disconnecting the configured
  capture device mid-session does not retarget the pipeline; stop and restart the
  recording after reconnecting or selecting a new device.

## Planned beyond 0.1.0 (not in this build)

The following are intentionally deferred and are documented here only so the
current boundary is unambiguous. They are **not** part of 0.1.0:
crash-safe recording and fragmented MP4, recording overlay/tray/notifications,
crash reporting, update checking, AMD and Intel hardware encoding, software
encoding fallback, and an expanded codec/color pipeline.
