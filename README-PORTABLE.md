# ExoSnap 0.10.0: Portable Windows guide

ExoSnap records a screen, window or region with NVIDIA NVENC, independently routed audio, optional webcam composition and recording diagnostics. This is a pre-1.0 preview; see [Known limitations](KNOWN_LIMITATIONS.md) before relying on a particular hardware/format combination.

## Install and launch

Use Windows 10/11 x64 (Windows 11 primary) and an NVIDIA GPU/driver supporting the selected NVENC codec. AMD, Intel and CPU video encoding are not available. The Microsoft Visual C++ 2022 x64 Redistributable is required and is not bundled with the portable ZIP. Install it from <https://aka.ms/vs/17/release/vc_redist.x64.exe> when missing.

Extract the entire `ExoSnap-<version>-windows-x64-portable` folder and run `exosnap.exe`. Keep its DLLs, plugins, QML runtime and other files together. Do not copy only the executable or mix files from releases. Current unsigned builds can trigger a SmartScreen warning.

Choose a source, review APP/SYS/MIC routing and readiness, then Record. The default global start/stop shortcut is **Alt+Shift+R**, configurable in Settings > Hotkeys. Other defaults are Alt+Shift+P for pause/resume, S for a frame, M for a marker and C for a manual split. A default binding unavailable on your system may be cleared; inspect the current Settings value.

The built-in setup is MKV / AV1 / Opus / CFR 60. First-run codec reconciliation selects AV1, then HEVC, then H.264 according to available capability. The default output is the Windows Videos known folder plus `ExoSnap`, including folder redirection, not an assumed literal user-profile path.

## Files and settings

Portable describes application deployment, not settings stored beside the executable. Normal local application data is under `%LOCALAPPDATA%\ExoSnap`: `settings.ini`, `presets.toml`, `recording-history.json`, recovery state, logs and reports. Recordings use the chosen output directory. Back up important settings/presets before moving between preview versions; schemas are not frozen.

Keep valuable `.partial` recordings if recovery fails. The next launch can offer Finish, Continue or Delete; Continue produces independent slices. MP4 delivery needs space for the transient recording and output together, and a completed single-file MP4 can retain an edit master.

## Supported formats

MKV offers H.264/HEVC/AV1 and AAC/Opus/PCM/FLAC. MP4 offers H.264/HEVC with AAC. WebM offers AV1 with Opus. Exact availability depends on hardware and the selected combination; AV1-in-MP4 and lossless audio in MP4 are not offered.

HDR defaults to tone-mapped SDR. Native HDR10 is an explicit display-relevant setting, with HEVC/AV1, 10-bit and compatible playback. Expert 4:4:4 is 8-bit H.264/HEVC only on capable GPUs. A higher requested recording rate is not a throughput guarantee.

After a normal completed recording, Edit can preview, scrub and keyframe-trim without re-encoding. Export writes MKV or MP4. Markers travel in a sibling `<stem>.markers.json`, not chapters. This is not a frame-accurate or multitrack project editor.

## Updates and crash reports

Update checks are off by default. Explicitly check or enable automatic checks in Settings > Updates. Stable excludes prereleases; Preview can offer them. The app's Update button authorizes the pinned version and opens a separate updater. A manually started `exosnap-updater.exe` instead asks separately to check, download and install.

The updater verifies the manifest's Ed25519 signature before trusting its fields and verifies package SHA-256 before installing. Portable update stages a replacement and backup, verifies the result and relaunches. A canceled download leaves the install untouched; critical swap steps refuse close. Restoration failures remain possible and must be reported rather than called successful rollback. A user-writable portable tree normally needs no UAC.

Crash reporting is separate, next-launch and consent-controlled. Ask every time is the default; remembered automatic send and Never send are explicit choices. Official reports use the configured Sentry EU path, not a prefilled GitHub issue. Native minidumps are separate from structured-event scrubbing and can contain module paths. [Privacy](PRIVACY.md) states that boundary.

## Verify and report

Compare the downloaded ZIP's SHA-256 with its published sidecar:

```powershell
Get-FileHash '.\ExoSnap-<version>-windows-x64-portable.zip' -Algorithm SHA256
```

A checksum verifies bytes, not publisher identity. Keep it separate from executable code-signing and signed-update-manifest claims.

For a recording problem, create a local support bundle from Logs/Diagnostics, review it, and report the version, GPU/driver, source and format through <https://github.com/Exoridus/exosnap/issues>. Nothing uploads that bundle automatically. Security issues go through [Security](SECURITY.md), not a public issue.

ExoSnap is GPL-3.0-or-later. See `LICENSE`, [Third-party notices](THIRD_PARTY_NOTICES.md) and the bundled `licenses` directory.
