# ExoSnap 0.9.1: Known Limitations

This document describes the current support boundary of ExoSnap **0.9.1**. It is factual and specific to this build. If a capability is not listed here as supported, do not assume it is available.

## Release status

- ExoSnap 0.9.1 is a **pre-v1 Windows preview**, not a final 1.0 release.
- Configuration, preset, and recording-history file schemas are **not frozen** and may change in incompatible ways before 1.0.0.
- Keep your own backup copies of presets you care about during preview releases. ExoSnap does not wipe existing data on upgrade, but forward/backward compatibility across preview versions is not guaranteed.

## Platform

- **Windows 10/11 x64 only.** Windows 11 is the primary target; Windows 10 is best-effort.
- No Windows ARM64, Linux, or macOS build.
- An **NVIDIA GPU with supported NVENC capability is currently required** for video encoding (RTX 20-series or newer recommended, with a current NVIDIA display driver).
- The Microsoft Visual C++ 2022 x64 runtime is required. It is normally already present on up-to-date Windows systems. Otherwise, install it from
  <https://aka.ms/vs/17/release/vc_redist.x64.exe>. The WinGet package
  installs this automatically as a declared dependency. MSI, portable ZIP, Chocolatey, and Scoop installs do not bundle it.

## Hardware encoding

Only **NVIDIA NVENC** video encoding is supported in this release. The following are **not** available and are **not** implied by this build:

- AMD AMF hardware encoding
- Intel Quick Sync / oneVPL hardware encoding
- Software (CPU) H.264 or AV1 encoding fallback

If a supported NVIDIA NVENC encoder is not detected, recording is blocked with a diagnostic message rather than silently falling back.

## Containers and codecs

Supported containers:

| Container | Status     |
| --------- | ---------- |
| MKV       | Supported  |
| WebM      | Supported  |
| MP4       | Supported (normal recording) |

Supported encoders actually selectable in this build:

- **Video:** H.264 (NVENC), AV1 (NVENC, where the installed GPU and driver expose it), and HEVC (NVENC). HEVC is available in MKV and MP4 (`hvc1` sample entry). **HEVC, hvc1, and 10-bit encoder paths are functional end-to-end but have not yet been validated across the full range of NVIDIA GPU generations under live recording conditions.** Use H.264 or AV1 if you encounter issues.
- **Audio:** AAC-LC (`AAC` in the UI), Opus, PCM (MKV only), and FLAC (MKV only). PCM and FLAC are **MKV-only**. See Container/codec rules above for why MP4 PCM is deferred.

Container/codec rules:

- MP4 uses H.264 or HEVC (`hvc1`) + AAC. Opus, PCM, and FLAC are not offered for MP4.
  - **AV1 in MP4 is deferred**: the pairing is muxable but not validated against the
    player/editor matrix, so it is classified experimental and never offered. Record
    AV1 to MKV (the default) or WebM. Switching an AV1 selection to MP4 reconciles it
    to H.264 + AAC.
  - **PCM in MP4 is deferred**: the project's libavformat (avformat-62) emits the
    `ipcm` (ISO/IEC 23003-5) sample entry instead of the broadly-compatible QuickTime
    entries (`sowt`/`in24`/`lpcm`), as confirmed via `ffprobe codec_tag_string=ipcm`.
    Windows "Films & TV", QuickTime, and many NLEs do not play `ipcm`. Use MKV for
    PCM recordings.
- WebM uses AV1 + Opus.
- MKV is the flexible default container and the home for PCM and FLAC audio.

Exact codec availability depends on your **NVIDIA GPU generation, driver version, the selected container, and the selected video/audio combination**. Invalid combinations are not offered.

## Video color pipeline

- **A complete color description is written to all MKV and MP4 outputs**, so no recording is color-ambiguous. For SDR output that description is **BT.709** (primaries, transfer and matrix); a native HDR10 recording is tagged with its own **PQ / BT.2020** values instead, plus HDR mastering metadata.
- **Y'CbCr color range** is selectable per preset: Full or Limited, behind Expert mode (Settings → Video). Some common players (notably VLC) ignore the range flag and expand as Limited, so Full-range recordings can look too dark in those players; Diagnostics surfaces a compatibility notice with a one-click fix when Full is selected.
- **10-bit video output (P010)** is available for HEVC Main10 and AV1 in 10-bit mode. It serves two roles: higher color precision in SDR workflows, and the mandatory pixel format for native HDR10 recording (below).
- **Chroma subsampling: 4:2:0 (default) or 4:4:4.** 4:2:0 is universal (all codecs, 8- and 10-bit). **4:4:4** is an Expert-mode option (Settings → Video), limited to **8-bit H.264 and HEVC** (NVENC High 4:4:4 Predictive / HEVC Range Extensions) on GPUs that report YUV444 encode support. Boundaries, all enforced by capability gating and the resolver:
  - **No AV1 4:4:4**: NVENC AV1 is 4:2:0 (Main) only.
  - **No 10-bit 4:4:4**: the 4:4:4 path is 8-bit only in this build.
  - **No 4:4:4 with native HDR10**: HDR10 requires 10-bit, which excludes 4:4:4.
  - **No 4:2:2**: ExoSnap has no 4:2:2 capture, conversion or encode path, on any
    GPU. This is a product limit, not a statement about what NVENC can do.
  - On the 4:4:4 path the **live in-app preview works** (it shares the composited RGB frame
    with the preview before the AYUV conversion) and the **single-frame snapshot works** as well
    (the packed AYUV 4:4:4 encode surface is decoded on the CPU with the exact inverse of the
    encoder's RGB→AYUV conversion).
  - 4:4:4 uses the same BT.709 matrix and Full/Limited range selection as 4:2:0.
- **HDR displays are detected automatically.** By default an HDR desktop is recorded as tone-mapped SDR (BT.709) for universal playability. An expert setting ("HDR handling") switches to **native HDR10 recording**: PQ/BT.2020, P010 10-bit, limited range, with mastering-display metadata written to MKV and carried into remuxed MP4.

  Native HDR10 requires HEVC or AV1. H.264 is blocked by a pre-flight check with a one-click codec fix.

  HDR handling applies to both **monitor (duplication) capture and window/game capture** (Windows Graphics Capture): a window on an HDR display negotiates a scRGB FP16 frame pool and gets the same tone-map or native-HDR10 handling and the same H.264 blocker. The window's hosting display is resolved once at recording start, so moving the window to a different monitor mid-recording keeps the session's initial HDR decision.

  The two kinds of HDR metadata are written differently:

  - **Mastering-display metadata** goes **both** at the container level **and in-band in the bitstream**, as an HEVC Mastering Display Colour Volume message (SEI type 137) or an AV1 HDR MDCV metadata OBU, emitted on every keyframe, so players that ignore container-level HDR metadata (notably some Apple players) still receive it. It is known before the first frame, from the captured display's reported primaries and luminance range.
  - **Content-light metadata (MaxCLL/MaxFALL)** is measured per frame and written at the container level only. MKV `MaxCLL`/`MaxFALL` is carried into the `clli` box of a remuxed MP4. It is **not** carried in-band, because both values are maxima over the finished stream and a bitstream message is emitted while the recording is still running, so it could only state the maximum seen so far.

  A player that reads container metadata therefore gets the measured levels; one that reads only in-band metadata sees none, and tone-maps as it would for any HDR10 file without them. A recording split into several files reports the levels measured up to each part's own boundary, so a later part can name a highlight that occurred in an earlier one.

  Current boundaries: no HLG, and the in-app recording preview shows an approximate SDR tone-map of the HDR content. The preview is still WYSIWYG during a native-HDR10 recording, because the engine shares its pre-encode HDR frame and the preview tone-maps it for display, so what is shown is the recorded frame viewed through the same roll-off an SDR player would approximate. The exception is the rare already-PQ 10-bit desktop (below), which has no shareable frame.
- **Moving the Windows SDR-content-brightness slider during a recording leaves wrongly exposed material behind: up to about two seconds of it on Windows before 11 build 22621, and a shorter unmeasured amount on 22621 and later.** That level decides the brightness the desktop is composed at, so tone-map, overlays and preview all follow it.

  Two mechanisms find the change:

  - **Windows 11 build 22621 and later**: the engine subscribes to the OS colour-state notification for the captured monitor. Windows raises one per step of a drag, the capture loop re-reads the facts on its next frame, and a 30-second re-read stays underneath only to cover a monitor change.
  - **Where no such notification exists**, on Windows 10 and in any process the interop is unavailable to: the level is re-read on a two-second cadence, and that interval is the whole exposure error.

  The delay between the user's slider movement and the notification arriving has not been measured, so the notified path is stated as shorter, not as immediate. In both cases the recording corrects itself from the next reading on and nothing needs restarting.
- **A recorded window that moves between an HDR and an SDR monitor keeps the colour state of the monitor it started on.** The colour pipeline (the frame pool format, the native-versus-tone-map decision, the bit depth and the colour description written into the file) is committed when the recording starts, and a file whose own metadata describes half its frames would be worse than one exposed for the wrong monitor. Following the move would mean rebuilding the session or starting a new file.

## Audio processing

- **Audio format model** (ADR 0030): the output **sample rate** (44.1 / 48 / 96 kHz), **channel count** (mono / stereo), and **bit depth** for the lossless codecs (PCM 16/24/32-bit int or 32-bit float; FLAC 16/24-bit) are configurable. Capture itself stays at 48 kHz; the engine resamples/rematrixes once after the mix bus (libswresample). **Opus is locked to 48 kHz** (libopus accepts only 8/12/16/24/48 kHz). Bit depth does not apply to the lossy codecs (Opus/AAC). Stereo→mono uses an averaging downmix (no clipping). 32-bit float PCM (`A_PCM/FLOAT/IEEE`) is a raw passthrough of the mix bus's native format and is PCM-only (FLAC has no float mode). **Deferred:** more than 2 channels (5.1/7.1) and non-vetted sample rates.
- **Per-track gain & mute** and a **brickwall limiter** (on by default, 0 dBFS ceiling) on the mixed bus.
- **Microphone DSP chain**, each stage **off by default** (capture is byte-identical when all are off, **unless audio clock slaving has engaged** (see below)): high-pass filter → noise gate → AGC → RNNoise neural noise suppression. Stages are toggled individually; there is no single master switch.
- **A/V clock slaving** (on by default, codec-independent): once measured device-clock drift crosses ~15 ms the audio output timeline is resampled by a sub-audible ppm amount to track the video (QPC) clock. Consequences:
  - It leaves a **bounded residual that grows with the drift rate**: a proportional
    controller with a fixed 500 ppm cap holds ~3-6 ms at typical 50-100 ppm
    crystals, but from ~250 ppm upward the residual no longer drops below the 15 ms
    engage threshold (it converts unbounded drift into a bounded, still-inaudible
    residual, not zero drift).
  - Once it engages, the default 48 kHz/stereo path is **no longer byte-identical**
    (it is resampled), including for PCM/FLAC. Disable *Audio clock slaving*
    (expert) for bit-exact archival capture.
  - **Multi-source merged tracks are not slaved** (they mix several independent
    device clocks). The per-source FIFO drift relief bounds their inter-source
    skew instead. A single gain-adjusted source is slaved normally.
  - **Live validation is narrow, not absent**: a 2-3 h live soak on the release
    system (net drift ≤ budget, audibly artifact-free) is a mandatory release
    gate (`docs/release-checklist.md` §7). Broad validation across many
    audio devices, drivers, and hardware configurations remains limited. One
    passing soak on one machine does not generalize to every device clock.
- **FLAC compression level** (0-8, default 5) is configurable; lossless at every level (level only trades encode CPU vs. file size).
- The RNNoise model weights are fetched at **configure (build) time** from a project-owned mirror with upstream fallback. This affects building from source, not running the released binary.

## Recording split

- Recording **split is supported for MKV, WebM, and MP4**.
- For MP4 sessions, each completed segment is remuxed to MP4 in the background while recording continues into the next segment. "Saved" is reported only when all segment remuxes have completed.
- Every MP4 remux (the single-file remux-on-stop and each per-segment remux) writes to a sibling `.part` temp on the target's own volume and is atomically renamed onto the output path only on success. A crash mid-remux never leaves a half-written file at the user-visible output path. It leaves only the temp, which the next launch cleans up. This is the same durability guarantee the crash-recovery remux carries.
- Already-finalized split segments remain independently usable.

## Crash safety and recovery

- **Crash recovery is available.** ExoSnap writes a recovery manifest before each recording starts. If a session is interrupted, the next launch shows a recovery overlay with three actions per candidate (ADR-0015):
  - **Finish**: saves the recording as originally configured (MKV rename/repair
    or MP4 remux, honouring the manifest snapshot, no user format choice). The
    remux is written to a temporary file and atomically renamed onto the target
    path only on success, so an interrupted earlier remux never leaves a corrupt
    half-file where the finished recording belongs. Recovery overwrites that
    stale partial in place rather than saving the good file under a different name.
  - **Continue**: shown only for non-finalized (true-crash) artefacts. Arms the
    coordinator in a paused state. Resume starts the next recording slice aligned
    with the per-segment machinery. The 1-2 s data loss at the crash boundary is
    accepted and visible as the slice boundary.
  - **Delete**: inline two-step confirm, permanently removes the artefact.
  - **Decide later**: explicit text button (replacing the bare `×`). Entries
    remain in the manifest. The overlay re-shows at the next launch.
- At most one **Continue** session can be armed at a time. Choosing Continue on a second candidate finalizes the first (its background remux completes; the new candidate takes its place).
- Continued sessions produce independent recording slices (no single-file concat). Post-hoc joining is planned but not in this build.
- Notification toasts (recovery available, saved, unexpected stop, low storage) are shown via the tray notification system.
- For MKV/WebM split recordings, segments that were already finalized before an interruption remain usable; an interrupted **active** segment may not be recoverable.

## Disk space and filesystem

ExoSnap monitors free space on the output drive:

- **Warning (2 GB free):** a Notice appears in Diagnostics. Recording is still allowed.
- **Hard stop (500 MB free):** recording is blocked at start; a running recording stops gracefully. For MP4 sessions, the effective hard-stop threshold is higher because the transient MKV and the output MP4 coexist during the remux-on-stop phase (roughly 2× the file size must be available). For split MP4 sessions, the threshold is raised conservatively by the sum of all pending background remux job sizes plus the current live segment estimate.

ExoSnap detects the filesystem of the output volume and warns about known limitations:

- **FAT32 output volume (rec.008):** a Notice appears in Diagnostics. FAT32 volumes impose a 4 GiB maximum file size. Recordings under 4 GiB succeed normally. Longer sessions will fail when the limit is reached. Move the output folder to an NTFS or exFAT volume for unlimited file sizes. Recording is **not blocked**. Short clips on FAT32 work correctly.
- NTFS, exFAT, and other filesystem types pass silently.
- No automatic split at the 4 GiB limit; that is a separate future change.

## Other current limitations

- **Live preview during recording is WYSIWYG** for SDR, HDR-tone-map, 4:4:4 and native-HDR10 sessions. The preview shares the engine's composited pre-encode frame over a GPU texture and stops its own capture, so there is no second capture and the preview reflects the actual encoded content. A native-HDR10 frame is tone-mapped to SDR by the preview for display (see the HDR section above).

  **The already-PQ 10-bit desktop is the exception**: it has no shareable frame, so the preview keeps its own capture there.

  Cross-GPU handle sharing is not supported. If the preview and engine devices resolve to different adapters the shared frame cannot be opened, so the preview never switches sources and simply keeps running its own live WGC capture. Recording is unaffected.
  - The webcam PiP "click to enlarge" magnifier is a no-op while a recording is
    running: the preview is showing the engine's composited frame, which already
    has the PiP baked in at its confirmed placement, and that cannot be enlarged
    after the fact without diverging from what is actually being recorded. The
    magnifier works normally on the idle preview before recording starts.
- **In-app updates are implemented** (official build only): a manual "Check now" and a toggleable automatic check both look at GitHub Releases. A found update can be downloaded, signature- and hash-verified, and installed in place via a dedicated updater process, with rollback on failure. See the Crash reporting and updates section below for the full flow and its current boundaries. There is still no silent restart. Every install step is visible and the last step (relaunch) is user-facing.
- No code signing (portable ZIP and MSI are both unsigned); Windows SmartScreen may warn on first launch. An MSI installer is provided in addition to the portable ZIP.
- No Replay Buffer.
- The built-in editor (Review → Edit → Output overlay, opened from a completed recording) supports keyframe-accurate lossless trim and markers, and exports via stream-copy (MKV/MP4).

  It also plays back real decoded video and audio (`EditPlayerSession` and `EditPlayerEngine`, avcodec-based) in sync through play, pause and scrub, for 4:2:0 recordings and for a clip recorded with the Expert 4:4:4 chroma option alike. Pixel format conversion is hand-rolled rather than `swscale`, because the bundled FFmpeg build still ships only the mux-only DLL set.

  A natively-HDR10 recording is tone-mapped to SDR for the preview through the same reference curve as the recording preview (ADR 0040), but against the **reference display peak of 1000 nits, not the peak of the screen the editor is actually on**. The player engine is UI-agnostic and has no display to query. That shifts where the highlight roll-off begins, not whether the image reads correctly.

  There is no chapter or container-metadata export. A JSON marker sidecar is written instead; see ADR 0042.
- **HDR handling covers both monitor and window/game capture** (expert opt-in for native HDR10; tone-mapped SDR is the default for HDR desktops). A window on an HDR display captures via a scRGB FP16 frame pool and follows the same HDR path as a monitor, keyed to the window's hosting display resolved at recording start (a mid-recording move to another monitor keeps the initial decision). Bitstream HDR10 static metadata (HEVC SEI / AV1 metadata OBUs) **is** written on every keyframe, in addition to the container-level metadata. HLG is not available.
- No 4:2:2 chroma subsampling (4:2:0 everywhere; 4:4:4 only on the 8-bit H.264/HEVC path described above).
- No multi-vendor hardware-encoder matrix (NVIDIA only). See above.
- Saved Display/Region targets are remembered by a hardware-stable identity (monitor device path + EDID vendor/product, plus serial when the panel reports one), so they survive unplug/replug, driver restarts, and reboots in a different port order. The one case that cannot be resolved is two *identical* monitors with no EDID serial number after their cables are swapped between ports: the app refuses to guess and shows a calm "Saved display not found" notice instead of silently recording the wrong monitor. Re-select the source once and it is remembered. Region rectangles restore proportionally to their anchor display, not pixel-exact, so they follow a resolution change.
- Device loss mid-recording is handled per device type (ADR 0046), not by a blanket stop-and-restart:

  | what is lost | what happens |
  |---|---|
  | a display, briefly | holds the last frame and reopens the same monitor |
  | the GPU, removed | ends the recording cleanly |
  | the captured window, closed | ends the recording cleanly |
  | an audio endpoint: mic unplugged, headset switched, system output changed, audio service restarted | does **not** end the recording. The affected source goes to honest silence and the recording keeps running while the engine reactivates the same source every 500 ms. In a merged track only the dead source's contribution falls silent |
  | the webcam | freezes its last frame and reopens |

  The pipeline is never *retargeted* onto a different device. The same source is held or reacquired, and a process-keyed app or window audio capture whose target exited stays silent rather than grabbing a stranger.
  - Every audio track keeps exact-length silence through an outage, including a
    merged one: when *every* inner source of a merged track is lost at once the
    track switches from source-driven to clock-driven and fills the outage,
    including the time the reacquire itself takes, with exact silence on its own
    timeline, so its duration still matches wall time. A returning source
    re-enters at the track's current position. Where the track is clock-slaved at
    all (a single gain-adjusted source, which is wrapped in the mixer, a true
    multi-source merge has no attributable device clock and is never slaved), the
    drift baseline and the slaving controller restart on the reacquired stream
    instead of reading its restarted device position as drift. Video-only
    recording continues if all audio is lost.
  - The degraded state is surfaced in Diagnostics, the post-flight report, and a
    standing notification that stays up for as long as a source is silenced,
    updates in place if the degraded set changes, and clears the moment every
    source reactivates (or the recording ends).
  - Verified by unit/integration tests with fake sources; real endpoint-unplug
    behavior is a manual live check.

## Overlay and notification limitations

- The on-screen recording overlay, diagnostics overlay, countdown overlay, and quick-control pill all use `WDA_EXCLUDEFROMCAPTURE` to stay outside the captured frame. If the capture exclusion API fails on a given system, the overlay hides itself and logs the failure.
- The quick-control pill is **opt-in** (off by default). Enable it in Advanced settings.
- **The notification hub is the persistent record.** A bell icon in the app header opens a notification hub panel where every notification lands and persists until dismissed, keeping its action (recover, undo, show in folder, and more). The tray icon additionally shows an unread badge for the same items. Toasts are a transient glance at the hub, anchored bottom-right of the screen hosting the ExoSnap window. At most one *timed* toast (something that already finished) is visible at a time, and a newer one replaces it. *Standing* toasts (a condition that still holds, e.g. low storage, unexpected stop, recovery available) stack above it and never auto-dismiss.
- Countdown overlay is anchored to the recorded monitor's bottom-center. On multi-monitor setups, it follows the selected monitor. It is not configurable.
- **Exclusive-fullscreen (legacy FSE) window capture is a named limitation, not a supported path.** A game in legacy exclusive fullscreen bypasses the desktop compositor, so **window** capture (WGC) records a black or frozen picture.

  ExoSnap cannot capture an FSE *window* in isolation: that would need hook or injection capture, which is deliberately rejected for privacy and anti-cheat reasons. Record the **monitor** instead, because monitor capture (DXGI Output Duplication) can capture exclusive fullscreen.

  ExoSnap *detects* this **pre-flight**, through the `rec.capture.exclusive_window` check: a proven-black window blocks the start, and the check offers a one-confirm "Record the monitor instead" fix.

  A window that stops producing frames *during* a recording, which is the shape a mid-session switch into FSE takes, is now reported as a **capture stall**. After 10 seconds without a single new capture frame from a fullscreen-shaped, live, non-minimized window, a standing caution says the capture appears to have stalled and that the recording is still running. The recording is never stopped automatically, the notice is cleared when frames resume, and the session report records it.

  **What is still not covered:**

  - The stall notice does not fire for an *ordinary* windowed target, because mid-recording nothing separates "stopped producing" from "nothing to redraw".
  - It never claims exclusive fullscreen as the cause unless a QUNS or PresentMon signal corroborates it.
  - It is a notice, not a repair. The frames lost to the stall are gone.
  - In the other direction, a fullscreen-shaped window whose content is genuinely at rest, such as a borderless video left paused, raises the same caution. That is why the wording says *appears to have stalled* rather than pronouncing a verdict, and it clears itself when the content moves again.

  Most modern "fullscreen" settings run as borderless or flip-model (FSO) and record fine on either path. The remaining hardening of this matrix is tracked for a later release.
- Tray notifications may be suppressed by Windows Focus Assist / Do Not Disturb mode.

## Capture previews

- **The source picker shows no thumbnails.** "Change source" opens a named list (displays, a "Region on <display>" entry per display, and application windows by title), not a grid of live tiles. Picking an entry selects it and closes the picker; the live picture of the chosen source is the Record page's own preview, described below. Frame-accurate identification from the picker alone is therefore not possible for two windows with the same title.
- **A plain display preview shares the recording's capture backend.** The idle Record-page preview of a display is fed by a DXGI Output Duplication capture hub, the same backend the recording uses, so it is VRR- and HDR-true, shows no OS capture indicator, and holds its last frame through a monitor hot-plug instead of blanking (ADR 0041).

  The hub is strictly refcounted: at most one duplication ever, for the selected display, none while no preview is visible, and it is released to the engine for the duration of a recording.

  **Window and Region previews stay on Windows Graphics Capture**, as does a display driven by a different GPU than the preview. Cross-adapter texture sharing is not supported, so the preview falls back to WGC rather than opening a second duplication.
- **An idle duplication has potential desktop-wide side effects.** An Output Duplication held open while merely previewing can force DWM out of multiplane-overlay and fullscreen-optimisation paths on some systems, degrading a game running on the previewed monitor. Closing the preview (or leaving the Record page) closes the duplication.

## Crash reporting and updates

- **Crash reporting is opt-in and consent-gated.** Capture is local-first (out-of-process Crashpad). `Ask every time` is the default; `Send automatically` and `Never send` are explicit, revisitable policies. `Never send` suppresses only the report prompt, not local recording recovery.
- **Crash detection is next-launch only.** Crashes are surfaced and offered for reporting on the *following* launch (clean-exit marker + session sidecar). An immediate in-session crash reporter is deferred.
- **Automated Sentry upload is present only in official builds.** The Sentry DSN is compiled in only under the official-build gate, so self-built binaries never upload. The next-launch dialog keeps local minidumps accessible through `Open crash folder`; it has no prefilled GitHub-issue action.
- **Server-side symbolication.** No client-side minidump parsing; stacks are symbolicated server-side from PDBs. Automated `sentry-cli` symbol upload is not yet wired (pending an auth token); symbols are archived per release in the meantime.
- **The uploaded minidump binary (not the structured event) can carry a path with a username segment.** The scrubber only touches the structured Sentry event; a hard-crash minidump's module list includes the full install path of `exosnap.exe`, which can include the username portion of the path for a portable install run from under `%USERPROFILE%`. See `docs/privacy-review.md` and `PRIVACY.md` for the precise boundary; no code mitigation ships yet.
- **In-app updates are implemented, with a dedicated updater process.** Stable and Preview channels are supported, with both a manual "Check now" and a toggleable automatic check.

  The client verifies the manifest against a detached ed25519 signature (Monocypher, shipped as a sibling `update-manifest.json.sig` release asset) plus each package's SHA-256 hash, and refuses downgrades.

  Finding an update hands off to a separate `exosnap-updater.exe` process, which downloads, verifies, closes the running app, swaps the files in place, verifies the result, and relaunches. The swap is a staged rename for portable installs and an elevated `msiexec /qn` for MSI installs, which costs one UAC prompt. It restores the previous version automatically if verification fails at any step.

  The app never restarts silently: every step is shown, and the final relaunch is the one moment the user sees the new version start.
- **The automatic update check is off by default for every build** (opt-in from the Settings update card); self-built binaries additionally never run it at all, regardless of the setting, and require the embedded official public key to verify a release even if they did. No GitHub token is used by the client.
- **Two moments are not fully in-app:** the UAC prompt for MSI installs, and the brief window while the app is closed during the file swap. No update runs during an active recording or finalization.

## Diagnostics logs and support bundle

- **The support bundle is created and shared manually.** ExoSnap sends no telemetry; a **Create support bundle** action (Logs page, and the Diagnostics page) packages the rotated logs, the recent per-recording session reports, and GPU/adapter/display facts into a scrubbed `.zip`. Nothing is uploaded. You save it and share it yourself.
- **Scrubbing covers paths, username, machine name, and capture-target window titles**, and structured files include only an allowlist of known-safe fields. It cannot anticipate an arbitrary personal string a user typed into a field that ends up in a log; the scrubber targets the known shapes (drive/UNC paths, user/machine names, `target="…"` window titles).
- **Per-recording session reports** are written to `%LOCALAPPDATA%\ExoSnap\logs\reports\`; the ten most recent are kept (pruned on write). There is no in-app viewer or clear button for them.
- **The engine JSON-lines log now appends and rotates** across launches (5 MiB × 3 files) instead of resetting on each launch.
- **The Startup latency table** on the Logs page reflects the milestones recorded up to the moment it is shown; it is not a continuously updating profiler.
- **Video encode runs async at a fixed pipeline depth of 1, with measurement instrumentation in place but no user-facing perf surface yet.** NVENC submits each frame and reaps its bitstream on a dedicated output buffer and completion event, instead of a single shared buffer with a blocking lock.

  This is a correctness fix, because the previous single-buffer aliasing was not SDK-covered for the sync path, and it gives precise per-frame encode-latency timing. At depth 1, though, capture, convert and encode still do not overlap across frames, so it carries no throughput benefit yet. GPUs without async encode capability fall back to the previous synchronous path automatically.

  A user-facing pipeline-depth setting, which would enable real overlap, is deferred behind a data-driven gate on the same measurement below.

  Every recording writes structured `perf` records to the engine JSON-lines log: a rolling window of encode-latency and frame-time percentiles, about every 10 seconds, and a whole-session distribution summary at the end. These are **log-only**: there is no new diagnostics card or reading in the UI, and they carry no personal data. `scripts/dev/analyze-encode-perf.py` turns one or two logs into a per-session table or a before-and-after comparison.

  Whether encode latency ever earns a visible diagnostics value, and whether a deeper pipeline is worth exposing, is deferred until this measurement shows it matters.

## Not in this build

Intentionally deferred, listed so the current boundary is unambiguous:

- AMD and Intel hardware encoding; software (CPU) encoding fallback
- HLG, and wide-color-gamut management beyond BT.2020 signalling
- 4:2:2 chroma subsampling
- More than two audio channels (5.1 / 7.1)
- PCM and FLAC audio in MP4
- Replay buffer
- Multi-track editing timeline and frame-accurate (non-keyframe) cuts
- Chapter / container-metadata marker export (a JSON sidecar is written instead)
- Immediate in-session crash reporter; automated crash-symbol upload
- Remaining hardening of the fullscreen / exclusive-capture matrix
