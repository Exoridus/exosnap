# ExoSnap 0.10.0: Known limitations

ExoSnap 0.10.0 is pre-1.0 Windows preview software. This page qualifies the [product contract](docs/product-spec.md), not a list of every theoretical capability of its dependencies. Settings, presets and recording-history schemas are not frozen; keep backups before switching preview versions.

## Platform and encoding

Windows 10/11 x64 only, with Windows 11 primary and Windows 10 best effort. No ARM64, Linux or macOS distribution. Real recording requires an NVIDIA GPU/driver exposing the selected NVENC codec. RTX 20-series or newer is recommended, but the exact codec/format capability must be probed. There is no AMD AMF, Intel QSV/oneVPL or CPU encoder fallback.

The capture adapter determines the D3D11 device used for NVENC. Cross-GPU capture-to-encode sharing is not a supported selectable workflow. Hardware inspection does not select the encoder device. Unsupported configurations are reconciled or blocked rather than silently sent to another backend.

The Microsoft Visual C++ x64 runtime is required. Portable/MSI/Scoop do not bundle it. WinGet declares the redistributable dependency; the Chocolatey package declares `vcredist140`. A package manager satisfying that dependency is distinct from embedding runtime DLLs in ExoSnap.

## Formats

| Container | Selectable video | Selectable audio |
|---|---|---|
| MKV | H.264, HEVC, AV1 | AAC, Opus, PCM, FLAC |
| MP4 | H.264, HEVC (`hvc1`) | AAC |
| WebM | AV1 | Opus |

AV1-in-MP4 and PCM-in-MP4 are not offered. PCM MP4 sample-entry compatibility needs its own validated player matrix. FLAC is MKV-only. A theoretically muxable stream is not supported merely because FFmpeg can write it.

HEVC, 10-bit and 4:4:4 paths are implemented but not validated across every NVIDIA generation/driver/player/editor. Validate the intended combination on the actual hardware. High frame-rate controls are requested rates, not guaranteed throughput.

Video supports 4:2:0 8-bit and HEVC/AV1 4:2:0 10-bit. Expert 4:4:4 is limited to 8-bit H.264/HEVC and a capable GPU. No AV1 4:4:4, 10-bit 4:4:4, 4:2:2 or native-HDR10 4:4:4 path ships.

## Capture and preview

Display capture uses DXGI Output Duplication; window capture uses Windows Graphics Capture. A normal region recording is a monitor target plus crop. Idle window/region preview uses WGC and need not have the same OS-indicator behavior as a plain display's duplication preview.

An idle duplication preview can affect desktop compositor/overlay/fullscreen-optimization behavior on some machines. Leaving the visible Record preview releases its preview-only ownership. The current product has no separate preview-off/frame-cap setting.

The recording preview uses the engine's composited pre-encode image where sharing is available. It is not a decoder verifying the finished file. Cross-adapter sharing and the rare already-PQ desktop are exceptions: independent preview can remain, so do not assume exact producer parity on an unsupported shared path. The source picker is a named list without live thumbnails, which cannot visually disambiguate identically titled windows.

Legacy exclusive-fullscreen **window** capture is not supported. Use display capture or a borderless window. ExoSnap does not inject or hook capture into the game. Detection is evidence-based, and a generic fullscreen shape alone cannot prove exclusive fullscreen. The full hardware/game matrix needs live verification.

A fullscreen-shaped window producing no new frames for ten seconds can raise a stall caution even if it is genuinely paused. Ordinary/minimized/hidden quiet windows are not necessarily reported. The notice does not reconstruct missing frames or automatically stop the recording. A display stall needs corroborating display-power/attachment evidence for a standing notice.

Source dimensions are fixed for a session. Resizing a captured window, a DPI move that changes captured dimensions, or a resolution change ends the recording with an explicit error while preserving the footage that can be finalized. Same-size temporary display loss can hold/reopen. GPU removal is fatal to that session. The engine does not arbitrarily retarget to a different device.

## Saved display identity

Saved targets use connector/panel identity data and normalized region geometry. Restoration is **not** an unconditional guarantee of selecting the same physical panel. The current matcher accepts an exact device path before comparing panel serial/model, so replacing or cable-swapping panels on an existing connector can select the panel now on that path. Serial-less identical panels are also intrinsically ambiguous. Confirm the preview after topology changes. Unresolved targets stay unselected; normalized regions restore proportionally rather than pixel-exact.

## HDR and color

HDR desktops default to tone-mapped SDR. Native HDR10 is explicit PQ/BT.2020, Limited, 10-bit HEVC/AV1. H.264 cannot carry that product path. HLG and wider-gamut handling beyond the implemented BT.2020 model are not supported.

A recorded window keeps the session's initial display/color configuration when moved to another monitor. Incompatible HDR mode changes stop the recording rather than changing a file's color interpretation halfway through. An SDR FP16 desktop is not automatically HDR content.

Changing Windows SDR-content brightness can leave a short exposure discrepancy before the next reading. The color notification path on supported Windows builds reduces latency, but the notification-to-frame delay is not an instantaneous guarantee. Polling fallback can take about two seconds.

The in-app HDR preview is an SDR approximation, not reference HDR monitoring. Edit preview tone-mapping uses a reference 1000-nit display peak rather than measuring the screen the editor is on. Rare already-PQ desktop input cannot use the normal overlay compositor, so webcam/cursor can be omitted with a notice and adjusted preview.

Mastering metadata is written in containers and on keyframes in HEVC/AV1. Measured MaxCLL/MaxFALL is container-level only; in-band messages do not promise final maxima before recording ends. Split outputs can report cumulative session maxima rather than values exclusive to that segment. Players that ignore one metadata channel can render differently. Full-range SDR also depends on a player honoring its range flag; Limited is the compatibility default.

## Audio

Output is mono/stereo only. No 5.1/7.1 output. Opus is fixed at 48 kHz. The exposed supported output rates for other codecs are 44.1/48/96 kHz, subject to encoder initialization. PCM supports integer 16/24/32-bit and float32; FLAC integer 16/24-bit. Bit-depth controls do not make AAC/Opus lossless.

Microphone processing is optional and each stage defaults off: high-pass, noise gate, AGC and RNNoise. RNNoise has a fixed block-latency tradeoff; its model is obtained during building, not downloaded at runtime. Per-source gain/mix/channel conversion and enabled clock slaving can alter samples even with all microphone DSP disabled. Do not call a general recording byte-exact solely because those four switches are off.

Clock slaving is enabled by default for a track with one attributable device clock. It can engage on drift or projected rate, and changes samples through gentle resampling. Multi-source merged tracks have no single attributable device clock and are not slaved. Their FIFO relief is not a universal long-run synchronization guarantee. Within the correction envelope the feed-forward/proportional controller can approach zero residual; at the cap a residual can remain, and beyond the cap residual can grow. Broad device-clock validation remains limited.

Endpoint loss becomes silence while other tracks/video continue and reactivation is retried. Fixed devices remain fixed; semantic default sources can reopen the current default after loss. Merely changing the default does not switch a still-running microphone stream. Lost samples cannot be recovered. Complete and partial multi-source outages, real unplug behavior and different drivers require targeted validation, not just a fake-source PASS.

## Storage and recovery

MKV/WebM segments already finalized remain independently usable. An interrupted active segment is not guaranteed recoverable. Continue creates separate slices, not one seamless concatenated file. A failed recovery-manifest write does not stop a valid recording but means it may not be automatically offered on next launch. Recovery cannot guarantee preservation of an unflushed tail through every power/storage failure.

MP4 is delivered by stream-copy remux from a transient recording. Source and output coexist during remux, and outstanding split jobs increase the reserve. A successful single-file MP4 can retain an `.edit.mkv` master, using additional space. Atomic remux publication prevents a half-written new output from replacing a good final path, but does not make arbitrary cross-volume operations atomic.

Low-disk protection depends on a usable free-space reading. A reachable/writable destination whose free space cannot be queried can record with a logged inactive-protection warning. FAT32's per-file limit is advisory: no automatic safe split at that limit. Time/size splits are keyframe-bound and approximate, so use headroom rather than a size threshold exactly at the filesystem maximum.

## Editor

Editing is temporary, keyframe-accurate lossless trim and stream-copy export, not arbitrary frame-accurate cutting, a multitrack editing project or a re-encoder. Closing the workspace discards an unexported recipe without a draft. A running export continues from its snapshot.

Hardware decode uses D3D11VA where it negotiates, with software fallback at open. Hardware frames are read back to CPU planes before GPU presentation conversion; this is not zero-copy decode-to-display. A failed preview can leave stream-copy export usable. Audio tracks are mixed for playback, not presented as independent edit channels. Timeline audio rows have no decoded waveform.

Markers are not container chapters. Export writes a `<stem>.markers.json` only for surviving markers and removes a stale destination sidecar when none remain. Same-stem MKV/MP4 exports share that sidecar name. Split sessions and missing/failed files do not necessarily offer the same one-clip Edit action as a normal completed recording.

## Overlays, diagnostics and support

The recording, diagnostics, countdown, quick-control and toast windows are capture-excluded. A failed exclusion call hides the overlay rather than contaminating the recording. This is not a guarantee against every third-party capture technique. Scene-graph screenshots cannot prove their actual desktop composition. The quick control and toast are interactive; the other three are click-through.

Present/DPC diagnostics are optional, session-scoped and elevation-gated. Unavailable/dead traces withdraw measurements. DPC driver attribution is best effort. Capture/encode rate, CPU submission time, GPU query time and present cadence are different measurements and must not be conflated. A session ledger records occurrences, not a frame-by-frame timestamp history of every drop.

Support bundles are local/user-shared. Known sensitive shapes are scrubbed, but arbitrary personal prose cannot be guaranteed removed. Recent session reports are retained locally with a bounded count. No telemetry uploads them automatically.

## Updates, crashes and signing

Automatic update checks are off by default. App-driven update and manually launched updater have different confirmation flows. The signed pinned target is not re-resolved during app handoff. Same-version verification reinstall is explicit, temporary and retains signature/hash/downgrade guards. Scoop installations stay package-manager managed.

Portable restore is attempted when appropriate, but can fail and must be reported. MSI rollback is Windows Installer's responsibility; an unknown installation state is not a verified restoration. Do not interpret a blanket "rollback on any failure" statement as supported behavior.

Crash reports are next-launch, consent-controlled and official-build upload only. Local minidumps can exist without upload. Structured events are scrubbed; native minidump module paths can include a portable install's username segment. See [Privacy](PRIVACY.md). Symbols are separate release artifacts; symbol delivery must be checked for the release rather than assumed from a captured dump.

Portable/MSI builds are not yet Authenticode-signed in this source's documented distribution state. SmartScreen may warn. Signed update metadata is a different security mechanism. No replay buffer, immediate in-session crash-report UI or unsupported encoder backend is implied by this preview.
