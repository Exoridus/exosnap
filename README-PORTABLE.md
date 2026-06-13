# ExoSnap 0.1.0 — Portable Release

Thanks for trying ExoSnap. This file is the quick-start guide for the portable
Windows build.

## What ExoSnap is

ExoSnap is a Windows-native screen / application / region recorder with a
high-performance NVIDIA NVENC pipeline, flexible container/codec profiles,
multi-track audio routing, a webcam overlay, and built-in diagnostics.

## Release status

This is **ExoSnap 0.1.0**, a **pre-v1 Windows preview**. It is not the final
1.0 release. Settings, presets, and recording-history file formats may change in
incompatible ways before 1.0.0. See `KNOWN_LIMITATIONS.md` for the full current
support boundary.

## System requirements

- Windows 10 or 11, 64-bit (Windows 11 recommended).
- An **NVIDIA GPU with supported NVENC** capability (RTX 20-series or newer
  recommended) and a current NVIDIA display driver. ExoSnap 0.1.0 requires
  NVIDIA NVENC for video encoding; AMD, Intel, and software encoding are not
  supported in this release.
- Microsoft Visual C++ 2022 x64 runtime (usually already installed). If the app
  fails to start with a missing-DLL error, install it from
  <https://aka.ms/vs/17/release/vc_redist.x64.exe>.

## How to launch

1. Extract the entire `ExoSnap-0.1.0-windows-x64` folder from the ZIP to a
   location of your choice.
2. Run `exosnap.exe` from the extracted folder.

There is no installer. Keep the folder intact — `exosnap.exe` depends on the Qt
runtime DLLs and the `plugins/` directory shipped alongside it. Windows
SmartScreen may warn on first launch because this build is not code-signed.

## Default Start/Stop hotkey

- **Alt+F9** starts and stops recording (global hotkey). Hotkeys are
  reconfigurable in the in-app Hotkeys view.

## Default output location

- Recordings are written to `%USERPROFILE%\Videos\ExoSnap` by default. You can
  change the output folder and filename pattern in the app.

## Where settings and history are stored

ExoSnap stores its configuration under your local application data directory:

- `%LOCALAPPDATA%\ExoSnap\settings.ini` — application settings
- `%LOCALAPPDATA%\ExoSnap\presets.ini` — recording presets
- `%LOCALAPPDATA%\ExoSnap\recording-history.json` — recording history

A best-effort startup diagnostics log is written to
`%TEMP%\exosnap-recorder-app-startup.log`. The in-app Logs view and the
Diagnostics view expose richer logging and a way to open the log folder.

## Supported output overview

- **Containers:** MKV, WebM, MP4.
- **Video:** H.264 (NVENC) and AV1 (NVENC, where your GPU/driver expose it).
- **Audio:** AAC-LC (`AAC`) and Opus.
- **Container rules:** MP4 uses H.264 + AAC (Opus is not offered for MP4);
  WebM uses AV1 + Opus; MKV is the flexible default.

Exact availability depends on your GPU generation, driver, and the selected
container/codec combination. HEVC, PCM, and FLAC are not implemented in 0.1.0.

## Recording split overview

- Split is supported for **MKV, WebM, and MP4** recordings (0.2.0).
- For MP4 sessions, each completed segment is remuxed to progressive MP4 in the
  background while recording continues. "Saved" is shown only when all remuxes
  complete. See `KNOWN_LIMITATIONS.md`.

## Verifying your download

A SHA-256 checksum is published next to the ZIP as
`ExoSnap-0.1.0-windows-x64.sha256`. To verify integrity in PowerShell:

```powershell
Get-FileHash .\ExoSnap-0.1.0-windows-x64.zip -Algorithm SHA256
```

The printed hash must match the value in the `.sha256` file. The checksum
verifies download integrity only; it is not a publisher signature. Code signing
is planned for a later release.

## Reporting a problem

Please open an issue at <https://github.com/Exoridus/exosnap/issues>. Including
your Windows version, GPU model, NVIDIA driver version, and the startup log helps
a lot.

## Licenses

- ExoSnap is licensed under **GPL-3.0-or-later**; see `LICENSE`.
- Third-party component licenses are listed in `THIRD_PARTY_NOTICES.md`, with the
  full license texts under the `licenses/` directory.

## More information

For the complete current support boundary and known issues, read
`KNOWN_LIMITATIONS.md` (shipped in this folder).
