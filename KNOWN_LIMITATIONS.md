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
  - On the 4:4:4 path the **live in-app preview works** (it shares the composited RGB frame
    with the preview before the AYUV conversion) and the **single-frame snapshot works** as well
    (the packed AYUV 4:4:4 encode surface is decoded on the CPU with the exact inverse of the
    encoder's RGB→AYUV conversion).
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
  the in-app recording preview shows an approximate SDR tone-map of the HDR
  content. The preview is still WYSIWYG during a native-HDR10 recording: the
  engine shares its pre-encode HDR frame and the preview tone-maps it for
  display — what is shown is the recorded frame, viewed through the same
  roll-off an SDR player would approximate. The exception is the rare
  already-PQ 10-bit desktop (below), which has no shareable frame.

## Audio processing (Audio v2, 0.6.0)

- **Audio format model** (ADR 0030): the output **sample rate** (44.1 / 48 / 96 kHz),
  **channel count** (mono / stereo), and **bit depth** for the lossless codecs
  (PCM 16/24/32-bit int or 32-bit float; FLAC 16/24-bit) are configurable. Capture
  itself stays at 48 kHz; the engine resamples/rematrixes once after the mix bus
  (libswresample). **Opus is locked to 48 kHz** (libopus accepts only
  8/12/16/24/48 kHz). Bit depth does not apply to the lossy codecs (Opus/AAC).
  Stereo→mono uses an averaging downmix (no clipping). 32-bit float PCM
  (`A_PCM/FLOAT_IEEE`) is a raw passthrough of the mix bus's native format and is
  PCM-only (FLAC has no float mode). **Deferred:** more than 2 channels (5.1/7.1)
  and non-vetted sample rates.
  - A small tail of audio may be dropped at stop **whenever a resample context is
    active** — a non-default sample rate (~10 ms tail) or engaged audio clock
    slaving on the default 48 kHz path (sub-ms to a few ms, in filter-length order)
    — because the resampler's internal buffer is not drained at end-of-stream;
    negligible for normal recordings.
- **Per-track gain & mute** and a **brickwall limiter** (on by default, 0 dBFS
  ceiling) on the mixed bus.
- **Microphone DSP chain**, each stage **off by default** (capture is byte-identical
  when all are off, **unless audio clock slaving has engaged** — see below):
  high-pass filter → noise gate → AGC → RNNoise neural noise suppression. Stages are
  toggled individually; there is no single master switch.
- **A/V clock slaving** (on by default, codec-independent): once measured
  device-clock drift crosses ~15 ms the audio output timeline is resampled by a
  sub-audible ppm amount to track the video (QPC) clock. Consequences:
  - It leaves a **bounded residual that grows with the drift rate**: a proportional
    controller with a fixed 500 ppm cap holds ~3–6 ms at typical 50–100 ppm
    crystals, but from ~250 ppm upward the residual no longer drops below the 15 ms
    engage threshold (it converts unbounded drift into a bounded, still-inaudible
    residual, not zero drift).
  - Once it engages, the default 48 kHz/stereo path is **no longer byte-identical**
    (it is resampled), including for PCM/FLAC. Disable *Audio clock slaving*
    (expert) for bit-exact archival capture.
  - **Multi-source merged tracks are not slaved** (they mix several independent
    device clocks); the per-source FIFO drift relief bounds their inter-source
    skew instead. A single gain-adjusted source is slaved normally.
  - **Not yet live-validated** over a multi-hour soak (net drift ≤ threshold,
    audibly artifact-free); pending the 0.10 soak-gate check.
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
    or MP4 remux, honouring the manifest snapshot; no user format choice). The
    remux is written to a temporary file and atomically renamed onto the target
    path only on success, so an interrupted earlier remux never leaves a corrupt
    half-file where the finished recording belongs — recovery overwrites that
    stale partial in place rather than saving the good file under a different name.
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

- **Live preview during recording is WYSIWYG** for SDR, HDR-tone-map, 4:4:4, and
  native-HDR10 sessions: the preview shares the engine's composited pre-encode
  frame over a GPU texture and stops its own capture, so there is no second
  capture and the preview reflects the actual encoded content. A native-HDR10
  frame is tone-mapped to SDR by the preview for display (see the HDR section
  above). **The already-PQ 10-bit desktop is the exception** — it has no
  shareable frame, so the preview keeps its own capture there. Cross-GPU handle
  sharing is not supported: if the preview and engine devices resolve to
  different adapters the shared frame cannot be opened, so the preview never
  switches sources and simply keeps running its own live WGC capture (recording
  is unaffected).
- **In-app updates are implemented** (official build only): a manual "Check now" and a toggleable
  automatic check both look at GitHub Releases; a found update can be downloaded, signature- and
  hash-verified, and installed in place via a dedicated updater process, with rollback on failure.
  See the Crash reporting and updates section below for the full flow and its current boundaries.
  There is still no silent restart — every install step is visible and the last step (relaunch) is
  user-facing.
- No code signing (portable ZIP and MSI are both unsigned); Windows SmartScreen may warn on first
  launch. An MSI installer is provided in addition to the portable ZIP.
- No Replay Buffer.
- The built-in editor (Review → Edit → Output overlay, opened from a completed recording) supports
  keyframe-accurate lossless trim and markers, and exports via stream-copy (MKV/MP4). There is no
  video preview playback in the overlay yet — the bundled FFmpeg build ships only the mux-only DLL
  set (avformat/avcodec/avutil/swresample); `avfilter` and `swscale` are not deployed, so decoding
  frames to a displayable format is not wired up (planned for a later release) — and there is no
  chapter/container-metadata export (a JSON marker sidecar is written instead; see ADR 0042).
- **HDR handling covers both monitor and window/game capture** (expert opt-in
  for native HDR10; tone-mapped SDR is the default for HDR desktops). A window on
  an HDR display captures via a scRGB FP16 frame pool and follows the same HDR
  path as a monitor, keyed to the window's hosting display resolved at recording
  start (a mid-recording move to another monitor keeps the initial decision).
  Bitstream HDR10 static metadata (HEVC SEI / AV1 metadata OBUs) **is** written
  on every keyframe, in addition to the container-level metadata. HLG is not
  available.
- No 4:2:2 chroma subsampling (4:2:0 everywhere; 4:4:4 only on the 8-bit
  H.264/HEVC path described above).
- No multi-vendor hardware-encoder matrix (NVIDIA only — see above).
- Saved Display/Region targets are remembered by a hardware-stable identity
  (monitor device path + EDID vendor/product, plus serial when the panel reports
  one), so they survive unplug/replug, driver restarts, and reboots in a
  different port order. The one case that cannot be resolved is two *identical*
  monitors with no EDID serial number after their cables are swapped between
  ports: the app refuses to guess and shows a calm "Saved display not found"
  notice instead of silently recording the wrong monitor — re-select the source
  once and it is remembered. Region rectangles restore proportionally to their
  anchor display, not pixel-exact, so they follow a resolution change.
- Device loss mid-recording is handled per device type (ADR 0046), not by a
  blanket stop-and-restart. A brief display loss holds the last frame and reopens
  the same monitor; a GPU removal ends the recording cleanly. Closing the captured
  window ends the recording cleanly. An audio endpoint lost mid-recording (mic
  unplugged, headset switched, system output changed, audio service restarted) no
  longer ends the recording: the affected source goes to honest silence and the
  recording keeps running while the engine reactivates the same source every
  500 ms; in a merged track only the dead source's contribution falls silent. The
  webcam freezes its last frame and reopens. The pipeline is never *retargeted*
  onto a different device — the same source is held or reacquired, or (for a
  process-keyed app/window audio capture whose target exited) stays silent rather
  than grab a stranger.
  - Remaining boundary: when *every* inner source of a single merged audio track
    is lost at once, that track's timeline holds (no packets) until an inner
    returns, rather than being filled with exact-length silence; a bare
    single-source track and the video track always get exact-length silence /
    continuity. Video-only recording continues if all audio is lost.
  - The degraded state is surfaced in Diagnostics and the post-flight report; a
    standing user-facing notification for it is a follow-up.
  - Verified by unit/integration tests with fake sources; real endpoint-unplug
    behavior is a manual live check.

## Overlay and notification limitations (0.3.0)

- The on-screen recording overlay, diagnostics overlay, countdown overlay, and quick-control pill
  all use `WDA_EXCLUDEFROMCAPTURE` to stay outside the captured frame. If the capture exclusion
  API fails on a given system, the overlay hides itself and logs the failure.
- The quick-control pill is **opt-in** (off by default). Enable it in Advanced settings.
- **The notification hub is the persistent record.** A bell icon in the app header opens a
  notification hub panel where every notification lands and persists until dismissed, keeping its
  action (recover, undo, show in folder, …); the tray icon additionally shows an unread badge for
  the same items. Toasts are a transient glance at the hub, anchored bottom-right of the screen
  hosting the ExoSnap window: at most one *timed* toast (something that already finished) is
  visible at a time — a newer one replaces it — while *standing* toasts (a condition that still
  holds, e.g. low storage, unexpected stop, recovery available) stack above it and never
  auto-dismiss.
- Countdown overlay is anchored to the recorded monitor's bottom-center. On multi-monitor setups,
  it follows the selected monitor. It is not configurable in 0.3.0.
- **Exclusive-fullscreen (legacy FSE) window capture is a named limitation, not a
  supported path.** A game in legacy exclusive fullscreen bypasses the desktop
  compositor, so **window** capture (WGC) records a black or frozen picture — ExoSnap
  cannot capture an FSE *window* in isolation (that would need hook/injection capture,
  which is deliberately rejected — see the privacy/anti-cheat posture). Record the
  **monitor** instead: monitor capture (DXGI Output Duplication) can capture exclusive
  fullscreen. ExoSnap now *detects* this pre-flight (the `rec.capture.exclusive_window`
  check) and offers a one-confirm "Record the monitor instead" fix; a window that goes
  FSE mid-recording is reported rather than silently frozen. Most modern "fullscreen"
  settings run as borderless/flip-model (FSO) and record fine on either path; the
  remaining hardening of this matrix is tracked for `0.10.0`.
- Tray notifications may be suppressed by Windows Focus Assist / Do Not Disturb mode.

## Capture previews

- **Source-picker tiles hold their last image** when a source stops producing
  frames (another app takes the surface — dragging the Snipping Tool across the
  desktop is the common case). The tile freezes rather than going empty or black,
  and resumes when frames return. A source that has **never** produced a frame
  shows "Preview unavailable" instead, because there is nothing to hold.
- **A plain display preview shares the recording's capture backend.** The idle
  Record-page preview of a display is fed by a DXGI Output Duplication capture
  hub — the same backend the recording uses — so it is VRR- and HDR-true, shows
  no OS capture indicator, and holds its last frame through a monitor hot-plug
  instead of blanking (ADR 0041). The hub is strictly refcounted: at most one
  duplication ever (the selected display), none while no preview is visible, and
  it is released to the engine for the duration of a recording. **Window and
  Region previews stay on Windows Graphics Capture**, as does a display driven
  by a different GPU than the preview (cross-adapter texture sharing is not
  supported; the preview falls back to WGC rather than opening a second
  duplication).
- **An idle duplication has potential desktop-wide side effects.** An Output
  Duplication held open while merely previewing can force DWM out of
  multiplane-overlay and fullscreen-optimisation paths on some systems,
  degrading a game running on the previewed monitor. Closing the preview (or
  leaving the Record page) closes the duplication.

## Crash reporting and updates (0.6.0)

- **Crash reporting is opt-in and consent-gated.** Capture is local-first (out-of-process Crashpad).
  Nothing leaves the machine without an explicit choice on the next-launch crash dialog.
- **Crash detection is next-launch only.** Crashes are surfaced and offered for reporting on the
  *following* launch (clean-exit marker + session sidecar). An immediate in-session crash reporter
  is deferred.
- **Stage 1 (automated Sentry upload) is present only in official builds.** The Sentry DSN is compiled
  in only under the official-build gate, so self-built binaries never upload. Stage 0 (assisted GitHub
  issue) is always available.
- **Server-side symbolication.** No client-side minidump parsing; stacks are symbolicated server-side
  from PDBs. Automated `sentry-cli` symbol upload is not yet wired (pending an auth token); symbols are
  archived per release in the meantime.
- **The uploaded minidump binary (not the structured event) can carry a path with a username
  segment.** The scrubber only touches the structured Sentry event; a hard-crash minidump's module
  list includes the full install path of `exosnap.exe`, which can include the username portion of
  the path for a portable install run from under `%USERPROFILE%`. See `docs/privacy-review.md` and
  `PRIVACY.md` for the precise boundary; no code mitigation ships yet.
- **In-app updates are implemented, with a dedicated updater process.** Stable and Preview channels
  are supported, with both a manual "Check now" and a toggleable automatic check. The client
  verifies the manifest against a detached ed25519 signature (Monocypher; shipped as a sibling
  `update-manifest.json.sig` release asset) plus each package's SHA-256 hash, and refuses
  downgrades. Finding an update hands off to a separate `exosnap-updater.exe` process that
  downloads, verifies, closes the running app, swaps the files in place (staged rename for
  portable installs; an elevated `msiexec /qn` for MSI installs, one UAC prompt), verifies the
  result, and relaunches — restoring the previous version automatically if verification fails at
  any step. The app never restarts silently: every step is shown, and the final relaunch is the
  one moment the user sees the new version start.
- **The automatic update check is off by default for every build** (opt-in from the Settings update
  card); self-built binaries additionally never run it at all, regardless of the setting, and
  require the embedded official public key to verify a release even if they did. No GitHub token
  is used by the client.
- **Two moments are not fully in-app:** the UAC prompt for MSI installs, and the brief window while
  the app is closed during the file swap. No update runs during an active recording or
  finalization.

## Diagnostics logs and support bundle

- **The support bundle is created and shared manually.** ExoSnap sends no telemetry; a
  **Create support bundle** action (Logs page, and the Diagnostics page) packages the rotated logs,
  the recent per-recording session reports, and GPU/adapter/display facts into a scrubbed `.zip`.
  Nothing is uploaded — you save it and share it yourself.
- **Scrubbing covers paths, username, machine name, and capture-target window titles**, and
  structured files include only an allowlist of known-safe fields. It cannot anticipate an
  arbitrary personal string a user typed into a field that ends up in a log; the scrubber targets
  the known shapes (drive/UNC paths, user/machine names, `target="…"` window titles).
- **Per-recording session reports** are written to `%LOCALAPPDATA%\ExoSnap\logs\reports\`; the ten
  most recent are kept (pruned on write). There is no in-app viewer or clear button for them.
- **The engine JSON-lines log now appends and rotates** across launches (5 MiB × 3 files) instead
  of resetting on each launch.
- **The Startup latency table** on the Logs page reflects the milestones recorded up to the moment
  it is shown; it is not a continuously updating profiler.
- **Video encode is synchronous, with measurement instrumentation in place but no user-facing
  perf surface yet.** NVENC submits and waits for each frame's bitstream before the next; capture,
  convert, and encode do not overlap across frames. To decide whether that is worth changing, every
  recording now writes structured `perf` records to the engine JSON-lines log: a rolling window of
  encode-latency and frame-time percentiles (about every 10 seconds) and a whole-session distribution
  summary at the end. These are **log-only** — there is no new diagnostics card or reading in the UI,
  and they carry no personal data. `scripts/dev/analyze-encode-perf.py` turns one or two logs into a
  per-session table or a before/after comparison. Whether encode latency ever earns a visible
  diagnostics value is deferred until this measurement shows it matters.

## Planned beyond 0.7.0 (not in this build)

The following are intentionally deferred and are documented here only so the
current boundary is unambiguous. They are **not** part of 0.7.0:
in-place auto-update with restart (has since shipped as a dual-swap in-app updater — see the
Crash reporting and updates section above), immediate in-session crash reporter, automated symbol
upload, AMD and Intel hardware encoding, software encoding fallback, HLG and wide-color-gamut
management beyond BT.2020 signaling (native HDR10/PQ has since shipped for both monitor and
window/game capture, with in-band HEVC SEI / AV1 metadata OBUs in addition to container-level
metadata), 4:2:2 chroma subsampling (4:4:4 has since shipped for 8-bit H.264/HEVC), more-than-stereo
audio (32-bit float PCM has since shipped), PCM/FLAC in MP4, and the remaining hardening of the
fullscreen/exclusive capture matrix (0.10.0; exclusive-fullscreen detection + the "record the
monitor instead" path have since shipped — see the capture-matrix limitation above).
