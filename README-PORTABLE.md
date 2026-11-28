# ExoSnap 0.11.0: Portable Windows guide

ExoSnap records a screen, window or region with NVIDIA NVENC, independently routed audio, optional webcam composition and recording diagnostics. This is a pre-1.0 preview; see [Known limitations](KNOWN_LIMITATIONS.md) before relying on a particular hardware/format combination.

## Install and launch

Use Windows 10/11 x64 (Windows 11 primary) and an NVIDIA GPU/driver supporting the selected NVENC codec. AMD, Intel and CPU video encoding are unavailable. The ZIP includes the Visual C++ runtime the recorder and updater need. Starting the portable application does not install a system prerequisite.

Extract the entire `ExoSnap-<version>-windows-x64-portable` folder and run `ExoSnap.exe`. The launcher starts the recorder in `runtime/`. Keep the whole folder together. Current unsigned builds can trigger a SmartScreen warning.

SDL3.dll is the application-local controller runtime. Its zlib license is included in
`licenses/sdl3.txt`. Controller input does not require a GameInput installation.

Choose a source, review APP/SYS/MIC routing and readiness, then Record. The default global start/stop shortcut is **Alt+Shift+R**, configurable in Settings > Hotkeys. Other defaults are Alt+Shift+P for pause/resume, S for a frame, M for a marker and C for a manual split. A default binding unavailable on your system may be cleared; inspect the current Settings value.

The built-in setup is MKV / AV1 / Opus / CFR 60. First-run codec reconciliation selects AV1, then HEVC, then H.264 according to available capability. The default output is the Windows Videos known folder plus `ExoSnap`, including folder redirection, not an assumed literal user-profile path.

## Files and settings

The bundled `portable.toml` uses `data_path = "./data"`. ExoSnap creates `data/` beside the launcher on first use and stores settings, presets, history, caches, recovery state, logs and reports there. You can set an absolute or UNC path. Relative paths start at the launcher folder, and an inaccessible configured location produces an error.

Remove `portable.toml` before launch to use normal per-user storage under `%LOCALAPPDATA%\ExoSnap`, including preferences from an earlier release. Both profiles retain their existing files and formats. Recordings use the separately configured output directory.

Keep valuable `.partial` recordings if recovery fails. The next launch can offer Finish, Continue or Delete; Continue produces independent slices. MP4 delivery needs space for the transient recording and output together, and a completed single-file MP4 can retain an edit master.

## Supported formats

MKV offers H.264/HEVC/AV1 and AAC/Opus/PCM/FLAC. MP4 offers H.264/HEVC with AAC. WebM offers AV1 with Opus. Exact availability depends on hardware and the selected combination; AV1-in-MP4 and lossless audio in MP4 are not offered.

HDR defaults to tone-mapped SDR. Native HDR10 is an explicit display-relevant setting, with HEVC/AV1, 10-bit and compatible playback. Expert 4:4:4 is 8-bit H.264/HEVC only on capable GPUs. A higher requested recording rate is not a throughput guarantee.

After a completed recording, the development editor supports a multi-track video/audio timeline, unlinking, waveforms and crossfades. Export writes MKV or supported MP4 through copy, render or hybrid paths; rendered exact cuts require supported SDR sources. Original-stream copy retains codec and keyframe constraints. v0.11 hardware/UX and media-profile qualification remains required. Markers travel in a sibling `<stem>.markers.json`, not chapters. Projects do not persist across application exit. Creating Recording Clips from a running recording is unavailable.

## Updates and crash reports

Update checks are off by default. Explicitly check or enable automatic checks in Settings > Updates. Stable excludes prereleases; Preview can offer them. The app's Update button authorizes the pinned version and opens a separate updater. A manually started `exosnap-updater.exe` instead asks separately to check, download and install.

The updater verifies the manifest's Ed25519 signature and the package SHA-256 before installing. With the launcher layout, portable updates stage and replace `runtime/`. The launcher, `portable.toml` and data outside that subtree stay in place during the swap and rollback. A canceled download leaves the installation intact. Critical swap steps refuse close. A user-writable portable tree normally needs no UAC.

Crash reporting is separate, next-launch and consent-controlled. Ask every time is the default; remembered automatic send and Never send are explicit choices. Official reports use the configured Sentry EU path, not a prefilled GitHub issue. Native minidumps are separate from structured-event scrubbing and can contain module paths. [Privacy](PRIVACY.md) states that boundary.

## Verify and report

Compare the downloaded ZIP's SHA-256 with its published sidecar:

```powershell
Get-FileHash '.\ExoSnap-<version>-windows-x64-portable.zip' -Algorithm SHA256
```

A checksum verifies bytes, not publisher identity. Keep it separate from executable code-signing and signed-update-manifest claims.

For a recording problem, create a local support bundle from Logs/Diagnostics, review it, and report the version, GPU/driver, source and format through <https://github.com/Exoridus/exosnap/issues>. Nothing uploads that bundle automatically. Security issues go through [Security](SECURITY.md), not a public issue.

ExoSnap is GPL-3.0-or-later. The extracted package contains `licenses/LICENSE` and `licenses/THIRD_PARTY_NOTICES.md`.
