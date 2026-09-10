<div align="center">

# ExoSnap

### Record your screen. Keep your audio separate. Know what happened.

[![Latest](https://img.shields.io/github/v/release/Exoridus/exosnap?style=for-the-badge&label=Latest&logo=github&color=44cc11)](https://github.com/Exoridus/exosnap/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/Exoridus/exosnap/total?style=for-the-badge&label=Downloads&logo=github)](https://github.com/Exoridus/exosnap/releases)
[![CI](https://img.shields.io/github/actions/workflow/status/Exoridus/exosnap/ci.yml?branch=main&style=for-the-badge&logo=githubactions&logoColor=fff&label=CI)](https://github.com/Exoridus/exosnap/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/Exoridus/exosnap?style=for-the-badge&color=44cc11)](LICENSE)

A Windows screen recorder for gameplay, software demos and everyday capture.
NVIDIA hardware encoding, multi-track audio, HDR10, lossless trimming and recording diagnostics.
Your recordings stay on your machine. No account required.

**[Download](https://github.com/Exoridus/exosnap/releases)** · **[Quick start](#your-first-recording)** · **[Formats](#formats-and-hardware)** · **[Known limitations](KNOWN_LIMITATIONS.md)** · **[Roadmap](docs/roadmap.md)**

</div>

![ExoSnap recording interface](docs/assets/readme-hero.png)

> **Windows preview, before 1.0.** Requires Windows 10/11 x64 and an NVIDIA GPU with NVENC.
> Windows 11 is the primary target; Windows 10 is best-effort. AMD, Intel and CPU encoding
> are not supported yet. Settings and preset formats may change between preview releases.

## What you can do

| Workflow | What ExoSnap provides |
| --- | --- |
| Record a demo or a game | Capture a display, application window or region; include the cursor and a webcam overlay. |
| Keep control of the audio | Record application, system and microphone audio to separate tracks, or merge selected sources. |
| Choose the output | H.264, HEVC or AV1 with compatible MKV, MP4 or WebM output; resolution and frame-rate controls. |
| See problems early | Readiness checks before recording, live drop/drift/storage monitoring, and a report after stopping. |
| Keep a long session manageable | Split recordings by time or size; recover interrupted sessions on the next launch. |
| Prepare a short clip | Play back, scrub, trim at keyframes and export without re-encoding; keep markers in a JSON sidecar. |

## Your first recording

1. Download the **MSI installer** or **portable ZIP** from [Releases](https://github.com/Exoridus/exosnap/releases).
   For the portable build, extract the whole folder and run `exosnap.exe`; keep its bundled files together.
2. Choose a screen, window or region. Select the application/system audio and microphone sources you want.
3. Review the readiness status. ExoSnap explains blockers before you start and offers a fix where possible.
4. Press **Record**, or use the default **Alt+F9** start/stop hotkey. Hotkeys are configurable.
5. Stop the recording and review the result. Open it in the built-in editor for a lossless trim if needed.

The built-in profile starts with **MKV, AV1, Opus and 60 fps CFR**. On first start, the video codec
is reconciled to the best available encoder on your GPU: AV1, then HEVC, then H.264.
Recordings default to `%USERPROFILE%\Videos\ExoSnap`; the destination is configurable.

The **Microsoft Visual C++ 2022 x64 Redistributable** is required. If startup reports a missing runtime
DLL, install it from [Microsoft](https://aka.ms/vs/17/release/vc_redist.x64.exe).
Current portable and MSI builds are unsigned, so Windows SmartScreen may warn on first launch.
See the [portable guide](README-PORTABLE.md) for setup and storage details.

## Formats and hardware

### Container compatibility

These are the offered combinations, not a list of everything the underlying codecs could theoretically mux.

| Container | Video | Audio | When to use it |
| --- | --- | --- | --- |
| **MKV** | H.264, HEVC, AV1 | AAC, Opus, PCM, FLAC | Flexible recording, separate audio tracks and lossless audio. |
| **MP4** | H.264, HEVC (`hvc1`) | AAC | Output for compatible players and editors; delivered by remuxing after capture. |
| **WebM** | AV1 | Opus | AV1/Opus output in a WebM container. |

**NVENC availability depends on your NVIDIA GPU and driver.** RTX 20-series or newer is recommended,
but not every GPU supports every codec. HEVC, `hvc1` and 10-bit paths work end-to-end but have not
been validated across every NVIDIA generation. AV1-in-MP4 is not offered; use MKV or WebM for AV1.
PCM and FLAC are MKV-only. Unsupported combinations are reconciled or blocked before recording.

### Picture and pacing

| Setting | Available options |
| --- | --- |
| Output size | Native source size, 720p, 1080p, 1440p or 2160p, with aspect-preserving scaling. |
| Frame-rate presets | 24, 30, 48, 50, 60, 90, 120, 144, 165 and 240 fps; Expert mode accepts custom values within its display-derived range. |
| SDR | BT.709 with Full/Limited color range; Limited is the default. |
| HDR10 | Native PQ/BT.2020 recording from compatible monitor or window/game sources, using HEVC or AV1 and a 10-bit path. |
| Bit depth | 8-bit; 10-bit P010 for HEVC Main10 and AV1. |
| Chroma | 4:2:0 by default; Expert 8-bit 4:4:4 for H.264/HEVC on compatible GPUs. No AV1 4:4:4, 10-bit 4:4:4 or 4:2:2. |

**60 fps is the default, not the maximum.** Presets above the fastest attached display's refresh rate
remain visible but disabled. A selected rate is not a throughput guarantee: capture, encoder, storage
and source refresh still determine what can be sustained. CFR may duplicate or drop frames to keep time.

HDR desktops record as tone-mapped SDR by default. Native HDR10 is an explicit option and needs
compatible hardware and playback software. See [Known limitations](KNOWN_LIMITATIONS.md) for the
precise support boundary rather than assuming every combination works on every system.

## Audio that stays editable

Application sound, system audio and microphone input have independent routing. Keep them on separate
tracks to adjust narration and game audio later, or merge a source into the track above it.
Each track has gain and mute controls; the mixed bus has a brickwall limiter enabled by default.

Optional microphone processing includes a high-pass filter, noise gate, automatic gain control and
RNNoise suppression. Each stage is individually switchable and off by default. Opus and AAC cover
compressed audio; MKV also supports lossless PCM and FLAC.

## Recording health and recovery

- **Before recording:** check encoder capability, disk space and configuration compatibility.
  Diagnostics distinguishes blockers from notices and provides a concrete remedy where available.
- **During recording:** monitor frame drops, audio/video drift, remaining storage and pipeline load.
  Recording overlays are visible to you and excluded from supported capture paths.
- **After recording:** inspect the session report, play the clip and trim it without another encoding pass.
- **After an interruption:** the next launch offers recovery for interrupted sessions. Already finalized
  MKV/WebM split segments remain usable; an interrupted active segment may not be recoverable.

MP4 delivery uses stream-copy remuxing from the recording, avoiding a second video encode. Low-disk
checks account for the temporary space needed while both files exist. Splitting can be triggered by
time or size, with per-segment background MP4 remux as each segment completes.

The editor supports **keyframe-accurate lossless trim**, not arbitrary frame-accurate cuts or a
multi-track editing timeline. Marker export is a JSON sidecar, not embedded chapters. There is no
replay buffer in the current preview.

## Local by default

Recordings are never uploaded or processed in the cloud. There is no account and no analytics telemetry.
Update checks use public GitHub Releases; crash reports require consent and are scrubbed before upload.
Self-built binaries disable the updater by default. Update installation is visible and user-controlled,
with verified downloads and rollback on failure.

Read [Privacy](PRIVACY.md) for the network and storage details, and [Security](SECURITY.md) for reporting
security issues. For recording bugs, include the app version, GPU/driver, capture source and selected
format in an [issue](https://github.com/Exoridus/exosnap/issues). Review logs before sharing them.

## Build and contribute

ExoSnap uses a **C++ recording engine and Qt 6 / Qt Quick UI**, with D3D11 capture/composition,
NVIDIA NVENC and FFmpeg. Recording policy and capability checks live in C++; QML handles presentation.

Requires Windows, Visual Studio 2022 with Desktop development with C++, CMake 3.27+ and Git.

```powershell
git clone https://github.com/Exoridus/exosnap.git
cd exosnap
git switch -c my-change origin/main
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug-exosnap
pwsh scripts/run-tests.ps1 -Filter recorder_core.
```

Use `scripts/verify.ps1 -Fast` for scoped iteration and `scripts/verify.ps1 -Full` for the complete
local gate before pushing. Hooks use the same entry point. Work on a branch and submit a pull request;
`main` is updated through merges. Follow [AGENTS.md](AGENTS.md) for repository rules.

| Reference | Contents |
| --- | --- |
| [Product specification](docs/product-spec.md) | Behavior, defaults and product decisions. |
| [Roadmap](docs/roadmap.md) | Planned work; not a promise of current support. |
| [Known limitations](KNOWN_LIMITATIONS.md) | Current platform, codec and workflow boundaries. |
| [Development tooling](docs/dev/harness-and-tracing.md) | Diagnostics harnesses and debugging. |

## License and acknowledgements

ExoSnap is **GPL-3.0-or-later**. See [LICENSE](LICENSE).
FFmpeg is distributed as LGPL-2.1-or-later shared libraries; component licenses are listed in
[Third-party notices](THIRD_PARTY_NOTICES.md). The binaries come from
[exosnap-ffmpeg-build](https://github.com/Exoridus/exosnap-ffmpeg-build).

Code-signing infrastructure is provided by [SignPath.io](https://signpath.io), with the open-source
program supported by the [SignPath Foundation](https://signpath.org). Current preview packages remain
unsigned until the certificate is issued.
