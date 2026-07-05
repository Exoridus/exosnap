# ExoSnap 0.8.1 — Known Limitations

This document describes the current support boundary of ExoSnap **0.8.1**. It is
factual and specific to this build. If a capability is not listed here as
supported, do not assume it is available.

## Release status

- ExoSnap 0.8.1 is a **pre-v1 Windows preview**, not a final 1.0 release.
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

Supported encoders actually selectable in this build:

- **Video:** H.264 (NVENC), AV1 (NVENC, where the installed GPU and driver
  expose it), and HEVC (NVENC). HEVC is available in MKV and MP4 (`hvc1` sample
  entry). **HEVC, hvc1, and 10-bit encoder paths are functional end-to-end but
  have not yet been validated across the full range of NVIDIA GPU generations
  under live recording conditions (ValidUnvalidated).** Use H.264 or AV1 if you
  encounter issues.
- **Audio:** AAC-LC (`AAC` in the UI), Opus, PCM (MKV only), and FLAC (MKV only).
  PCM and FLAC are **MKV-only** — see Container/codec rules above for why MP4 PCM
  is deferred.

Container/codec rules:

- MP4 uses H.264 or HEVC (`hvc1`) + AAC. Opus, PCM, and FLAC are not offered for MP4.
  - **PCM in MP4 is deferred**: the project's libavformat (avformat-62) emits the
    `ipcm` (ISO/IEC 23003-5) sample entry instead of the broadly-compatible QuickTime
    entries (`sowt`/`in24`/`lpcm`); confirmed via `ffprobe codec_tag_string=ipcm`.
    Windows "Films & TV", QuickTime, and many NLEs do not play `ipcm`. Use MKV for
    PCM recordings.
- WebM uses AV1 + Opus.
- MKV is the flexible default container and the home for PCM and FLAC audio.

Exact codec availability depends on your **NVIDIA GPU generation, driver
version, the selected container, and the selected video/audio combination**.
Invalid combinations are not offered.

## Video color pipeline (0.7.0)

- **BT.709 color metadata** is written to all MKV and MP4 outputs.
- **Y'CbCr color range** is selectable per preset: Full or Limited, behind Expert mode
  (Settings → Video). Some common players (notably VLC) ignore the range flag and expand
  as Limited, so Full-range recordings can look too dark in those players; Diagnostics surfaces
  a compatibility notice with a one-click fix when Full is selected.
- **10-bit video output (P010)** is available for HEVC Main10 and AV1 in 10-bit
  mode. It serves two roles: higher color precision in SDR workflows, and the
  mandatory pixel format for native HDR10 recording (below).
- **Chroma subsampling: 4:2:0 (default) or 4:4:4.** 4:2:0 is universal (all codecs, 8- and
  10-bit). **4:4:4** is an Expert-mode option (Settings → Video), limited to **8-bit H.264 and
  HEVC** (NVENC High 4:4:4 Predictive / HEVC Range Extensions) on GPUs that report YUV444 encode
  support. Boundaries, all enforced by capability gating and the resolver:
  - **No AV1 4:4:4** — NVENC AV1 is 4:2:0 (Main) only.
  - **No 10-bit 4:4:4** — the 4:4:4 path is 8-bit only in this build.
  - **No 4:4:4 with native HDR10** — HDR10 requires 10-bit, which excludes 4:4:4.
  - **No 4:2:2** — the NVENC generation has no 4:2:2 encode path.
  - On the 4:4:4 path the **live in-app preview still works** (it shares the composited RGB frame
    with the preview before the AYUV conversion), but the **single-frame snapshot is disabled**
    (it would have to decode the packed AYUV 4:4:4 encode surface); recording is unaffected.
  - 4:4:4 uses the same BT.709 matrix and Full/Limited range selection as 4:2:0.
- **HDR displays are detected automatically.** By default an HDR desktop is
  recorded as tone-mapped SDR (BT.709) for universal playability. An expert
  setting ("HDR handling") switches to **native HDR10 recording**: PQ/BT.2020,
  P010 10-bit, limited range, with mastering-display metadata written to MKV and
  carried into remuxed MP4. Native HDR10 requires HEVC or AV1; H.264 is blocked
  by a pre-flight check with a one-click codec fix. HDR handling applies to both
  **monitor (duplication) capture and window/game capture** (Windows Graphics
  Capture): a window on an HDR display negotiates a scRGB FP16 frame pool and
  gets the same tone-map / native-HDR10 handling and the same H.264 blocker. The
  window's hosting display is resolved once at recording start — moving the
  window to a different monitor mid-recording keeps the session's initial HDR
  decision. HDR10 static metadata is written **both** at the container level
  **and in-band in the bitstream** — HEVC Mastering Display Colour Volume (SEI
  type 137) and Content Light Level Info (SEI type 144) messages, and AV1 HDR
  MDCV / HDR CLL metadata OBUs — emitted on every keyframe, so players that
  ignore container-level HDR metadata (notably some Apple players) still receive
  it. Content-light (MaxCLL/MaxFALL) metadata is only emitted when present; the
  current native path fills mastering-display data but leaves MaxCLL/MaxFALL
  absent (no per-frame content-light analysis). Current boundaries: no HLG, and
  the in-app recording preview shows an approximate SDR tone-map. That last point
  is also the one exception to the WYSIWYG-during-recording preview (below): for
  **native HDR10** the engine has no SDR intermediate to share, so during a
  native-HDR10 recording the Record-page preview keeps its own capture and shows
  the same approximate SDR tone-map rather than the exact encoded frame.

## Audio processing (Audio v2, 0.6.0)

- **Audio format model** (ADR 0030): the output **sample rate** (44.1 / 48 / 96 kHz),
  **channel count** (mono / stereo), and **bit depth** for the lossless codecs
  (PCM 16/24/32-bit; FLAC 16/24-bit) are configurable. Capture itself stays at
  48 kHz; the engine resamples/rematrixes once after the mix bus
  (libswresample). **Opus is locked to 48 kHz** (libopus accepts only
  8/12/16/24/48 kHz). Bit depth does not apply to the lossy codecs (Opus/AAC).
  Stereo→mono uses an averaging downmix (no clipping). **Deferred:** more than 2
  channels (5.1/7.1), float PCM (`A_PCM/FLOAT_IEEE`), and non-vetted sample rates.
  - A small (~10 ms) tail of audio may be dropped at stop when recording at a
    **non-default sample rate** (the resampler's internal buffer is not drained
    at end-of-stream); negligible for normal recordings.
- **Per-track gain & mute** and a **brickwall limiter** (on by default, 0 dBFS
  ceiling) on the mixed bus.
- **Microphone DSP chain**, each stage **off by default** (capture is byte-identical
  when all are off): high-pass filter → noise gate → AGC → RNNoise neural noise
  suppression. Stages are toggled individually; there is no single master switch.
- **FLAC compression level** (0–8, default 5) is configurable; lossless at every
  level (level only trades encode CPU vs. file size).
- The RNNoise model weights are fetched at **configure (build) time** from a
  project-owned mirror with upstream fallback — this affects building from source,
  not running the released binary.

## Recording split

- Recording **split is supported for MKV, WebM, and MP4** (0.2.0).
- For MP4 sessions, each completed segment is remuxed to MP4 in the background
  while recording continues into the next segment. "Saved" is reported only when
  all segment remuxes have completed.
- Already-finalized split segments remain independently usable.

## Crash safety and recovery

- **Crash recovery is available** (0.2.0). ExoSnap writes a recovery manifest
  before each recording starts. If a session is interrupted, the next launch
  shows a recovery overlay with three actions per candidate (ADR-0015):
  - **Finish** — saves the recording as originally configured (MKV rename/repair
    or MP4 remux, honouring the manifest snapshot; no user format choice).
  - **Continue** — shown only for non-finalized (true-crash) artefacts. Arms the
    coordinator in a paused state; Resume starts the next recording slice aligned
    with the per-segment machinery. The 1–2 s data loss at the crash boundary is
    accepted and visible as the slice boundary.
  - **Delete** — inline two-step confirm, permanently removes the artefact.
  - **Decide later** — explicit text button (replacing the bare `×`). Entries
    remain in the manifest; the overlay re-shows at the next launch.
- At most one **Continue** session can be armed at a time. Choosing Continue on a
  second candidate finalizes the first (its background remux completes; the new
  candidate takes its place).
- Continued sessions produce independent recording slices — no single-file concat.
  Use Quick Trim (planned for 0.11.0) for post-hoc joining.
- Notification toasts (recovery available, saved, unexpected stop, low storage) are shown via the tray notification system (0.3.0).
- For MKV/WebM split recordings, segments that were already finalized before an
  interruption remain usable; an interrupted **active** segment may not be
  recoverable.

## Disk space and filesystem

ExoSnap monitors free space on the output drive:

- **Warning (2 GB free):** a Notice appears in Diagnostics. Recording is still
  allowed.
- **Hard stop (500 MB free):** recording is blocked at start; a running recording
  stops gracefully. For MP4 sessions, the effective hard-stop threshold is higher
  because the transient MKV and the output MP4 coexist during the remux-on-stop
  phase (roughly 2× the file size must be available). For split MP4 sessions, the
  threshold is raised conservatively by the sum of all pending background remux
  job sizes plus the current live segment estimate.

ExoSnap detects the filesystem of the output volume and warns about known limitations:

- **FAT32 output volume (rec.008):** a Notice appears in Diagnostics. FAT32 volumes
  impose a 4 GiB maximum file size. Recordings under 4 GiB succeed normally; longer
  sessions will fail when the limit is reached. Move the output folder to an NTFS or
  exFAT volume for unlimited file sizes. Recording is **not blocked** — short clips
  on FAT32 work correctly.
- NTFS, exFAT, and other filesystem types pass silently.
- No automatic split-at-4-GiB-limit; that is a separate future slice.

## Other current limitations

- **Live preview during recording is WYSIWYG** for SDR, HDR-tone-map, and 4:4:4
  sessions: the preview shares the engine's composited pre-encode frame over a GPU
  texture and stops its own capture, so there is no second capture and the preview
  reflects the actual encoded content. **Native HDR10 is the exception** — the
  preview keeps its own capture and shows an approximate SDR tone-map there (see
  the HDR section above). Cross-GPU handle sharing is not supported: if the
  preview and engine devices resolve to different adapters the pushed frame cannot
  open and the preview holds its last live image (recording is unaffected).
- Update checking is **notify-only**: the official build checks GitHub Releases and points you to
  the releases page. There is no in-place download, no auto-install, and no silent restart (see the
  Crash reporting and updates section below).
- No code signing (portable ZIP and MSI are both unsigned); Windows SmartScreen may warn on first
  launch. An MSI installer is provided in addition to the portable ZIP.
- No Replay Buffer.
- The built-in editor (Review → Edit → Output overlay, opened from a completed recording) supports
  keyframe-accurate lossless trim and markers, and exports via stream-copy (MKV/MP4). There is no
  video preview playback in the overlay yet, and there is no chapter/container-metadata export.
- **HDR handling covers both monitor and window/game capture** (expert opt-in
  for native HDR10; tone-mapped SDR is the default for HDR desktops). A window on
  an HDR display captures via a scRGB FP16 frame pool and follows the same HDR
  path as a monitor, keyed to the window's hosting display resolved at recording
  start (a mid-recording move to another monitor keeps the initial decision).
  Bitstream HDR10 static metadata (HEVC SEI / AV1 metadata OBUs) **is** written
  on every keyframe, in addition to the container-level metadata. HLG is not
  available.
- No 4:2:2 or 4:4:4 chroma subsampling (4:2:0 only).
- No multi-vendor hardware-encoder matrix (NVIDIA only — see above).
- Stable display identity uses the GDI device name (for example `\\.\DISPLAY1`),
  which can be reassigned on a monitor topology change. A saved Region or Display
  target may point to a different physical monitor after a reboot or
  reconnect/mode-set; re-select the source manually in that case.
- Hot-swap during recording is not supported. Disconnecting the configured
  capture device mid-session does not retarget the pipeline; stop and restart the
  recording after reconnecting or selecting a new device.

## Overlay and notification limitations (0.3.0)

- The on-screen recording overlay, diagnostics overlay, countdown overlay, and quick-control pill
  all use `WDA_EXCLUDEFROMCAPTURE` to stay outside the captured frame. If the capture exclusion
  API fails on a given system, the overlay hides itself and logs the failure.
- The quick-control pill is **opt-in** (off by default). Enable it in Advanced settings.
- The notification "center" is implemented as a tray unread badge only — there is no persistent
  notification history panel in this release.
- Toast notifications appear in the bottom-right corner and auto-dismiss. They are not queued
  visually when multiple arrive simultaneously; only the most recent is displayed.
- Countdown overlay is anchored to the recorded monitor's bottom-center. On multi-monitor setups,
  it follows the selected monitor. It is not configurable in 0.3.0.
- The fullscreen/borderless/exclusive capture matrix (capturing games that use exclusive fullscreen)
  is deferred to 0.12.x (RC stabilization wave).
- Tray notifications may be suppressed by Windows Focus Assist / Do Not Disturb mode.

## Crash reporting and updates (0.6.0)

- **Crash reporting is opt-in and consent-gated.** Capture is local-first (out-of-process Crashpad).
  Nothing leaves the machine without an explicit choice on the next-launch crash dialog.
- **Crash detection is next-launch only.** Crashes are surfaced and offered for reporting on the
  *following* launch (clean-exit marker + session sidecar). An immediate in-session crash reporter
  (Crash A2) is deferred.
- **Stage 1 (automated Sentry upload) is present only in official builds.** The Sentry DSN is compiled
  in only under the official-build gate, so self-built binaries never upload. Stage 0 (assisted GitHub
  issue) is always available.
- **Server-side symbolication.** No client-side minidump parsing; stacks are symbolicated server-side
  from PDBs. Automated `sentry-cli` symbol upload is not yet wired (pending an auth token); symbols are
  archived per release in the meantime.
- **Update checking is notify + manual download.** Stable and Preview channels are supported. The
  client verifies a signed manifest (ed25519 via Monocypher + SHA-256) and refuses downgrades, but it
  does **not** download or install the update itself, and never restarts silently. In-place
  auto-update (Update C) is deferred.
- **Updates are off by default for self-built binaries** and require the embedded official public key;
  no GitHub token is used by the client.

## Planned beyond 0.7.0 (not in this build)

The following are intentionally deferred and are documented here only so the
current boundary is unambiguous. They are **not** part of 0.7.0:
in-place auto-update with restart, immediate in-session crash reporter, automated symbol upload,
AMD and Intel hardware encoding, software encoding fallback, HLG and wide-color-gamut management
beyond BT.2020 signaling (native HDR10/PQ has since shipped for both monitor and window/game
capture, with in-band HEVC SEI / AV1 metadata OBUs in addition to container-level metadata),
4:2:2/4:4:4 chroma subsampling, more-than-stereo
audio, float PCM, PCM/FLAC in MP4, and the fullscreen/exclusive capture matrix (0.12.x).
