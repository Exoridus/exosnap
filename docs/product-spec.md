# ExoSnap Product Specification

**Status: Current product contract, pre-1.0.** This document owns user-visible behavior, defaults, terminology and UI contracts. [Architecture](architecture/overview.md) explains implementation constraints. [Known limitations](../KNOWN_LIMITATIONS.md) qualifies support; the [roadmap](roadmap.md) describes future work, not implemented behavior. Settings and file schemas are not frozen before 1.0.

## 1. Product intent and principles

ExoSnap records a Windows display, application window or region with NVIDIA hardware video encoding, independently routed audio, optional webcam composition and recording diagnostics. It records locally without an account. MP4 delivery is a lossless remux, not a second video encode.

The product prioritizes reliable recording and actionable evidence over an exhaustive set of theoretical format choices. Readiness distinguishes hard blockers from advice. Runtime problems are measured rather than invented from configuration alone. Unknown values remain unavailable, not zero. A source or format is never silently replaced in a way that changes what the user agreed to record.

Configuration and compatibility have one C++ owner. Presentation consumes resolved options, track plans and state. No product behavior depends on a second implementation of those rules in QML.

## 2. Navigation and information architecture

The title band provides five direct destinations in this order: **Record, Settings, Diagnostics, Logs, About**. Hotkeys is a Settings card. Hardware inspection is a collapsed Diagnostics reference, not a Device destination or an encoder selector.

Settings owns user choices. Diagnostics owns observations. Record owns capture setup, preview and transport. Logs owns event inspection, startup measurements and support-bundle creation. About owns application identity and project links.

### Shell and window

The window has one custom 40 px title band containing brand, navigation, drag area, state, notification bell and native-behavior window controls. Tabs retain readable labels at the 860 × 700 minimum; their spacing yields before window controls disappear. Record and Settings use their own context/preset bands instead of an additional redundant page heading. About is a centered identity surface.

The preferred first-run size is 1280 × 720, centered and clamped to the primary work area. Valid saved geometry wins. Position, dimensions and maximized state are correct on the first visible frame. Native resize, double-click maximize/restore, system menu, Snap and supported Windows 11 Snap Layouts remain available. A maximized window has no active resize edges and restores its saved normal rectangle. A maximized window minimized to the taskbar returns maximized.

The operating system's supported corner/border treatment is used where available, with ordinary fallback rather than an error on systems lacking it. Native window identity stays `ExoSnap`; it is not localized because activation and updater handoff use it.

### Edit and modal surfaces

Edit / Output / Save is a workspace over Record, below the real title band, not another top-level destination and not a saved project. Back, Escape, another clip or navigation closes it and discards the unexported recipe without a dirty badge, confirmation or draft. Returning to Record shows the normal Completed state. Opening Edit again starts clean.

An export already started continues from its immutable snapshot after the workspace closes and reports through notifications. Navigation does not cancel it. Recovery, crash-report and recording-error surfaces are different: they are blocking questions, shown one at a time with queued requests retaining their order. They prevent starting a recording and navigating behind them, but do not disable stop/pause/resume for an existing session. A close confirmation participates in the same navigation guard.

Modal scrims cover the shell, including its title band; the content card remains below that band. The card has no imitation title bar. Headings and decision controls stay fixed while long explanatory content scrolls. Consent/remember controls remain visible with the actions, including at minimum size.

### Encode device

There is no encoder-device/backend selector. The recording device is determined by capture, and NVENC opens on that D3D11 device. Adapter cards in Diagnostics are inspection only. Settings can display the selected/observed encode adapter as read-only information; it must not imply an independently selectable cross-GPU path. Unsupported future backends do not appear as available hardware.

### Appearance

Appearance and accent are independent: Dark or Light, plus Aqua, Sky, Violet or Magenta. Defaults are Dark + Aqua. Changes apply without restart. Unknown stored preferences resolve to a valid default; supported older preference forms are mapped on load.

The accent identifies interaction/selection, not severity. Coral means recording, error or destructive action; amber means warning/attention or the distinct labeled Paused state; green means ready/success. Countdown, Preparing, Stopping, Saving and Locked are neutral. Resume is an action and remains accent-colored. Status never relies on color alone.

Text and filled-control ink target 4.5:1 contrast; state/focus indicators target 3:1. The product also uses a 3:1 visibility floor for unavailable controls, even though they are not interactive. Structural hairlines are separation, not status indicators. These are role-based token checks, not a blanket certification of every rendered state.

Unavailable controls retain labels/current values, drop interactive emphasis and expose the real reason. A locked-on switch preserves its on shape without looking pressable. A row dims only when all its controls are unavailable. Accent swatches show the value for the current appearance and use a separate selection ring and accessible name.

## 3. Recording defaults and profiles

| Setting | Default |
|---|---|
| Container / video / audio | MKV / AV1 / Opus |
| First-run video reconciliation | Best supported encoder: AV1, then HEVC, then H.264 |
| Frame timing | CFR 60 fps |
| Quality | High, canonical CQ 19 |
| NVENC preset | P4 |
| Frame pacing | Phase-correct where the selected capture path supports it |
| Color range | Limited |
| HDR handling | Tone-map to SDR on an HDR-active source |
| Cursor | Included |
| Countdown | Off; choices Off, 3, 5 or 10 seconds |
| Application audio | Configured on; contributes only for a specific window target |
| System audio / microphone | System on, microphone off |
| Tracks | One per enabled eligible source unless mixed into the previous track |
| Webcam / microphone DSP | Off |
| Mixed-bus limiter / clock slaving | On |
| Automatic update checks | Off |

The output folder defaults to Windows' Videos Known Folder plus `ExoSnap`, including redirected Videos locations. It is not necessarily a literal path beneath the user profile.

Five read-only built-in presets lead the list:

| Preset | Container / codecs | Quality | NVENC preset |
|---|---|---|---|
| Default | MKV / AV1 / Opus | High, CQ 19 | P4 |
| Quality | MKV / AV1 / Opus | Ultra, CQ 16 | P4 |
| Compact | MKV / AV1 / Opus | Low, CQ 30 | P6 |
| Performance | MKV / AV1 / Opus | High, CQ 19 | P2 |
| Compatibility | MP4 / H.264 / AAC | High, CQ 19 | P4 |

The live configuration is persisted continuously. A preset is a named snapshot compared with that state. A difference shows `Name (changed)`, not an unsaved-project warning. Applying a preset preserves the capture target and applicable environment-derived depth/HDR choices; compatibility can still force a necessary clamp such as H.264 to 8-bit. Those preserved fields do not count as preset edits.

Built-ins cannot be renamed, overwritten or deleted. Save as new creates an editable user preset and is the visible toolbar action while changed. The overflow provides Rename, Reset, Delete, Export and Import with truthful availability. Delete names the preset and defaults to cancellation. Preset switching applies immediately and records a hub entry with Undo, without a toast. Undo restores both previous live configuration and selection.

Names are trimmed and unique case-insensitively; built-in names are reserved. Imports resolve collisions with numeric suffixes. Import/export uses human-readable TOML, validated and repaired by the same store rules. Damaged entries are repaired individually where possible; loss-causing repair is reported instead of silently resetting everything.

Application preferences are separate in `settings.ini`. A missing file is a normal first run. An existing unreadable file is not overwritten by automatic housekeeping: the session uses defaults with a notice, and an explicit user edit can move the unreadable file to `settings.ini.corrupt` before replacing it. Failed settings/preset writes and transfers produce actionable notifications. No successful-save claim is made for a failed write.

## 4. Container / codec / audio matrix

| Container | Offered video | Offered audio |
|---|---|---|
| MKV | AV1, HEVC, H.264 | Opus, AAC, PCM, FLAC |
| MP4 | HEVC (`hvc1`), H.264 | AAC |
| WebM | AV1 | Opus |

Availability additionally depends on the actual GPU/driver and selected depth/chroma/mode. A selectable pairing is not a promise of validation across every NVIDIA generation or external editor. HEVC, `hvc1` and 10-bit paths have a narrower live-verification matrix than their implementation support.

Switching container reconciles video/audio to a permitted combination through the C++ resolver. An AV1 setup switched to MP4 resolves to H.264 + AAC. AV1-in-MP4 and PCM-in-MP4 remain unoffered; a muxer being able to write them is insufficient compatibility evidence. FLAC is MKV-only and not a current MP4 target. WebM never offers H.264/HEVC.

MP4 is delivered from a Matroska recording by progressive faststart stream copy. The UI distinguishes Finalizing/Saving from Saved and shows a percentage only once measured progress exists. Cancellation/failure retains valid source media where available. A successful single-file MP4 recording can retain an `.edit.mkv` master for editing; it consumes additional storage. Split MP4 recordings remux each completed segment while capture continues and are Saved only after required remux work completes.

SDR and native-HDR outputs carry their actual color descriptions. Native HDR is not relabeled BT.709 during MP4 delivery. The `hvc1` tag is part of the intended compatibility path, not a guarantee that every player or editor accepts every file.

## 5. Audio model

### Sources and tracks

Settings lists Application, System and Microphone in product order (`APP`, `SYS`, `MIC`). Application remains configured while a display/region target is selected but recedes with the explanation that it takes effect for a specific application window. It has no Mix into previous track control because it is first.

Eligible enabled sources form separate tracks by default. **Mix into previous track** appends a source to the track being built from the preceding eligible rows. Chained selections can produce one combined track. The engine resolves the actual track structure and names; the UI does not invent one from visible row positions.

During recording, a source admitted at start can be muted and unmuted. Muting preserves its track and duration with silence. A source absent from the initial plan cannot be added mid-session and its toggle remains locked. Changing capture device or track structure requires a new recording.

Source gain is −60 to +24 dB, default 0 dB, with microphone gain on its own card. Source mute contributes silence. Recording mixes use fixed configured-source weighting, so a quiet source does not automatically make another louder. The mixed-bus limiter defaults on at 0 dBFS; its Expert ceiling is −12 to 0 dBFS. Limiting reduces clipping risk but does not reconstruct a clipped capture input.

### Meters and microphone

Source blocks align the include control/name, measured level and mix-to-previous slot. Meter position, ruler and numeric readout use the same decibel value. Settings meters provide a full-width segmented scale with −60, −40, −20, −6 and 0 dBFS marks and peak hold. Silence is distinct from a finite −60 dB reading. An off source collapses to its header instead of displaying a meaningless full-width meter.

Microphone Device, Channels, Gain and Post-processing are a separate card below Audio sources. The summary identifies the chosen device/gain/processing and says No microphone connected when appropriate. A disconnected configured device is not silently replaced by an arbitrary one.

The optional microphone chain is **high-pass → noise gate → AGC → RNNoise**, each independently off by default. RNNoise adds one 10 ms block of latency at 48 kHz; turning processing off does not negate other conversions or clock compensation. No master DSP switch implies all stages share one setting.

### Audio formats

| Property | Choices |
|---|---|
| Channels | Mono or stereo |
| Output sample rate | 44.1, 48 or 96 kHz where applicable; Opus fixed at 48 kHz |
| PCM | 16/24/32-bit integer, or 32-bit float |
| FLAC | 16/24-bit integer; compression 0–8, default 5 |
| Lossy bitrate | Opus 32–510 kbps; AAC 64–320 kbps |
| Opus | Audio profile, 20 ms default frame duration, complexity 10 default; specialized frame-duration/complexity choices in Expert |

Bit depth is not a property of the lossy AAC/Opus setting. Stereo-to-mono averages channels. More than two output channels and arbitrary sample rates are not offered. Float PCM is a separate Expert choice relevant to PCM at 32-bit, not a FLAC mode.

A clean stop drains resampler and encoder tails. A failed/timed-out drain is not labeled clean; per-track counters distinguish measured drained data from unavailable evidence.

### Device loss, silence and clock slaving

An endpoint lost during recording silences the affected source while video and other sources continue. Reopening retries on the same declared identity. Fixed-device input stays fixed; semantic-default capture resolves the current Windows default when it reopens. Merely changing the default does not guarantee an existing stream moves to it. A default-input change during a running session is reported with the fact that the session retains its current device. A process target that exits is not replaced by another process with the same PID.

A fully unavailable track is held on an elapsed-time silence timeline. The completed-reactivation path includes reopening time. Exact accounting when only some sources recover from a total multi-source outage remains a validation boundary; no universal sample-exact partial-recovery guarantee is made. An ordinary quiet but connected loopback source is not labeled lost. Diagnostics and standing notifications identify real degradation and clear when it ends; the report records that a gap occurred. No system can recover sound that was not captured.

Clock slaving is on by default. It gently resamples a measurable single-device track to the video/QPC clock. It engages from drift above roughly 15 ms or a measured drift rate likely to exceed that within ten minutes. The displayed drift is the residual after actual correction; raw drift and applied rate are also diagnostic data. Correction is capped at ±500 ppm. Within that envelope the residual can approach zero; at/beyond the cap it can remain or grow. Multi-source merged tracks are not slaved because they have several clocks.

Expert can disable clock slaving for a bit-exact workflow. That only removes this correction: gain, mixing, channel conversion, quantization and lossy encoding still determine whether the overall chosen path is bit-exact.

## 6. Video model

### Quality and rate control

Default mode presents named quality tiers. Expert replaces them with CQ/VBR/CBR controls and numeric CQ or bitrate. A canonical quality value is not universally the native encoder quantizer and is never labeled CRF.

| Tier | Canonical CQ | H.264 QP | HEVC QP | AV1 qindex |
|---|---|---|---|---|
| Draft | 35 | 35 | 35 | 167 |
| Low | 30 | 30 | 30 | 135 |
| Balanced | 24 | 24 | 24 | 94 |
| High | 19 | 19 | 19 | 65 |
| Ultra | 16 | 16 | 16 | 42 |

Expert CQ spans 1–51; the native mapping is displayed beneath it. AV1 uses the calibrated curve, with interpolation between points. These tiers aim at comparable quality, not pixel equivalence between codecs. Adaptive quantization, B-frames and lookahead are not active product controls. Unsupported Lossless is not offered.

The Expert NVENC preset control offers P1–P7, default P4 for all codecs, taking effect on the next recording. Higher preset numbers are a computation/quality trade-off, not guaranteed improvement on every workload.

### Frame rate and timing

Default choices are 24, 30, 48, 50, 60, 90, 120, 144, 165 and 240 fps. Unavailable high entries remain visible with a reason. Expert accepts integer rates from 1 to the display-derived ceiling: the highest attached refresh rounded to whole fps, with a minimum ceiling of 60. That floor keeps the default expressible even on a slower/unknown display; it does not make the source produce 60 distinct pictures.

Manual edits and topology/refresh changes clamp to the active bounds. A loaded outside-range value remains displayed truthfully until the next relevant edit/display change rather than being mislabeled as the ceiling. Off-list values appear in Default as an inserted `<n> fps (Custom)` entry; selecting it alone does not change the recording rate.

CFR preserves a fixed output schedule through duplicates and explicitly accounted losses. Phase-correct pacing selects display frames by present time rather than blending/interpolating them. Lowest latency uses the newest available frame. The phase-correct implementation belongs to the duplication capture path; WGC uses its own newest-at-tick behavior. A normal region anchored to a monitor resolves through the monitor recording backend, whereas its idle preview uses WGC. VFR is not phase-correct CFR.

No pacing setting guarantees judder-free motion from every source or makes 60 fps motion equivalent to 144 Hz. Sustained encode lateness skips and reports missed output slots instead of compressing video time against audio. VFR epochs do not include stale pre-record present time as an artificial lead-in.

### Resolution, depth and chroma

Output resolution offers Native, 720p, 1080p, 1440p, 2160p and validated Custom dimensions. Scaling preserves aspect ratio with a contain-fit content rectangle and padding as needed. Selecting an incomplete Custom size is temporary edit state, not an invalid persisted recording configuration.

All codecs have an 8-bit path. HEVC/AV1 can offer 10-bit P010 on compatible hardware. The Expert depth choice controls SDR precision; native HDR independently requires 10-bit. The irrelevant H.264 depth row is hidden.

4:2:0 is the default. Expert 8-bit 4:4:4 is available for H.264/HEVC only where the GPU exposes the required encode support. The row is hidden when irrelevant, but a fixable 10-bit conflict leaves its unavailable 4:4:4 choice visible with a reason. AV1 4:4:4, 10-bit 4:4:4 and 4:2:2 are not product paths. Live preview and still capture remain supported for the implemented 4:4:4 path.

### Color and HDR

SDR output uses BT.709 and defaults to Limited range. Full range is an Expert choice with a player-compatibility advisory. Conversion, encoded bitstream and container describe the same range/matrix/transfer; a player's interpretation can still differ.

An HDR-active source exposes HDR handling in Default mode: tone-map to SDR by default, or native HDR10 when compatible. Native means PQ/BT.2020, Limited range and P010, requiring HEVC/AV1. A selected HDR10/H.264 conflict is surfaced and blocks start rather than silently resetting the user's intent. Tone-mapped H.264 is not that conflict.

Mastering metadata is carried in the container and on keyframes in-band. Measured MaxCLL/MaxFALL is container-level, not promised as final in-band maxima during a recording. Split values can accumulate across the session rather than describe independently reset per-file analysis.

Tone-map exposure follows measured source luminance with smoothing. SDR overlays use Windows' SDR-content brightness, with a fallback when unavailable. Color notifications or polling refresh changes, so a short interval can retain old exposure. SDR Advanced Color/scRGB with HDR off is not automatically treated as HDR content merely because its texture is FP16.

Toggling the captured display's Windows HDR state ends the current recording cleanly once detected, keeping the footage and requesting a new recording. Moving a window between HDR/SDR displays retains the session's initial color decision. No seamless color rollover, HLG or general wide-gamut management beyond the implemented BT.2020 path is promised.

Native-HDR recording preview and Edit playback are SDR approximations. Edit uses the reference display peak, not a display-adaptive HDR preview. The already-PQ desktop exception has no ordinary composited tap and omits webcam/cursor with an explicit notification.

## 7. Capture targets and webcam

The target kinds are Display, Window and Region. Source selection is a named list, not a live-thumbnail gallery. Windows are filtered for useful capturable targets; unavailable targets explain their status rather than pretending they can be selected. Display labels use sequential user-facing numbering rather than exposing gaps in GDI names. A shared label resolver supplies picker, transport, recent targets, notifications and filename context.

Region selection offers Draw custom first, then 16:9, 9:16, 1:1 and 4:5 starting shapes. Presets create editable rectangles rather than immediately committing an immutable crop. Move/resize stays inside the anchor monitor in physical virtual-screen pixels, including negative origins; minimum geometry is 64 × 64. Selection handles and dimensions stay editable until recording locks/hides them.

The Record preview box content-fits the current source aspect ratio and follows target/region/source-size changes while idle. Its border stays a neutral structural line; state belongs to the local pill/timer/actions. On-screen metadata is drawn above the video and PiP but is not part of recordings or frame screenshots.

Idle display preview uses the selected capture hub and holds through transient loss. Window/region idle preview uses WGC. Preview stops when Record is no longer actually displayed. During recording it normally switches to the engine's composited image and stops independent capture, without a black flash during handoff. Cross-adapter sharing failure and the already-PQ path are explicit exceptions, so WYSIWYG is not unconditional on every configuration.

Preview redraw is producer-driven. A published frame missed during screen movement/exposure/scene reconstruction is presented when rendering is usable again without another frame or mouse event. Ready screenshots remain disabled until a usable preview frame exists.

### Target loss and identity

Saved display/region selection uses composite device/EDID identity with ranked matching, not just `DISPLAYn`. Ambiguous fallback or a missing anchor leaves the source unselected and reports the condition. Connector identity is not infallible physical-panel identity: the matcher accepts an exact device path before comparing serial/model, so a replacement panel on that connector can be selected. Confirm the preview after cable swaps or panel replacement, especially with identical serial-less panels or degraded GDI-only saves. A returning requested display can resolve automatically unless the user selected something else. Region restore is proportional to the anchor, not pixel-exact after resolution change.

Transient display capture loss holds/reopens the same runtime source. GPU removal or a closed target window ends the recording. Resizing the captured source ends it with an explicit old/new size error and asks for a new recording; encoder dimensions are not changed mid-file. A refresh-only change can use hold/reopen instead. Merely moving a window is allowed while captured dimensions remain unchanged.

Windowed/borderless content uses OS capture. Legacy exclusive-fullscreen window capture can be black or frozen; no injection/hook capture is provided. The recommended remedy is monitor capture or changing the target application to borderless. **Record the monitor instead** requires a summary/confirmation naming the wider scope and loss of the application-audio row. Real legacy-FSE behavior remains subject to title/driver verification, not a universal guarantee inferred from a heuristic.

A fullscreen-shaped, alive, visible window with no capture-frame progress for ten seconds can raise a standing caution that it appears stalled. It does not automatically stop. A genuinely still borderless window can meet the same condition; the language remains conditional and the notice clears when frames return. Ordinary static/minimized/hidden windows do not generate the same alarm. Display/region starvation needs corroborating sleep/disconnection evidence before a standing notice. Emitting CFR duplicates is not proof capture is healthy.

### Webcam

Webcam is an optional recorded PiP, not a fourth target. Device/resolution selection lives in Settings; its include control is synchronized with the Record camera control. Off is the default, and opening Settings alone does not open the camera. The first available camera can be preselected without recording it.

Placement/size are adjusted inside the preview's displayed video area, reaching its edges without distortion or clipping. Default placement is bottom-right with a margin. Metadata overlays stay above it. Mirror, opacity and chroma-key settings match the recording composition; opacity defaults to 100%. Chroma choices include green, blue, magenta or a custom color, with tolerance, softness and spill reduction. Extra key controls are disclosed when enabled.

Device and resolution are fixed during recording. Supported mirror/opacity/chroma and inclusion changes apply live. A healthy static desktop is recomposed with the current webcam each CFR tick; actual capture-loss recovery holds the picture. A lost camera holds its last image and retries the same device. No camera and a selected camera that fails to open are different states; the latter exposes the actual reason, not a fabricated generic device absence.

Media Foundation is a webcam dependency, not an AAC/recording dependency. Windows N/KN without the Media Feature Pack can still launch and record without webcam; webcam UI and Diagnostics explain the missing runtime.

## 8. Recording lifecycle

### Admission and transport

Every start route enforces blockers, even if Diagnostics was never opened. A missing source is No source/Unavailable, not Ready: Record stays visible but disabled and Choose source explains the next action. An invalid destination, unsupported encoder/format or genuine pre-flight blocker names the reason and appropriate remedy.

Countdown is Off/3/5/10 seconds and blocks incompatible configuration edits. Cancel, Escape and the record hotkey cancel it without starting a session. Preparing exposes asynchronous validation/resource work and remains responsive. Its hotkey cancellation is cooperative. Stopping/finalizing/saving are distinct from a playable Saved result; no transient callback may report success before those operations finish.

The Record layout is a stable context strip, aspect-fitted preview and bottom transport dock. Status/error/success messages do not shrink the preview or move the dock. Locks preserve action slots and provide reason tooltips/accessibility descriptions. The source toggles, elapsed time and action group retain their relative positions.

| State | Recommended action and behavior |
|---|---|
| Ready | Outlined Record; source and countdown controls available as applicable |
| Countdown | Filled Cancel, neutral progress state |
| Preparing | Preparing feedback; pending start can be canceled cooperatively |
| Recording | Filled Stop, secondary pause/screenshot/marker/split where supported |
| Paused | Filled Resume; Stop remains available without competing fill |
| Saving/finalizing | Real progress/state, no false Saved claim |
| Completed | Edit when eligible, folder action and a back-to-idle control; no redundant Record beside Edit |
| Failed | Modal failure explanation and report action; no success result |

A split/missing/failed recording does not display a permanently dead Edit action. Completed state is not a dead end even when Edit is ineligible. Capture frame is not shown as an action on a finished session. Icon-only controls have descriptive tooltips, and unavailable reasons remain hover-readable.

### Runtime health and duration

Low-cost live readings include real dropped/duplicated frames, A/V drift, output bytes, storage risk and pipeline status. Real drops are backpressure, processing failure and unemitted ring eviction. Deliberate coalescing and empty pre-first-frame CFR slots are not counted as picture loss. All visible summaries share that distinction.

The live timer measures the running session and continues while paused. Finished duration uses the media file, excluding pause and finalization tail; split duration sums media segments. The post-flight report includes drop percentage, measured drift and pipeline health without inventing timestamps for individual drops.

### Destination, splitting and disk protection

The destination is a folder chip opening the native picker, with its identifying path tail visible. It is not a text/paste path entry. The configured path can be redirected, a junction/symlink, mapped drive or UNC destination. Asynchronous validation reacts to startup, edits, activation, relevant network changes, Output visibility and final preparation. Unavailable/unwritable destinations are blocked with focused recovery, not a permanent manual Check again requirement.

Live valuable artifacts stay on the configured output volume with a `.partial` suffix. Disposable remux/repair staging is separate, and successful output is published by a same-volume atomic replacement. A failed repair preserves its valuable input. The default Videos Known Folder respects redirection.

Automatic duration and file-size splitting are independent, whichever comes first. Controls are Default-visible and reveal values only while enabled. Disabling an axis preserves its configured interval/size. Size is approximate and keyframe-safe. MKV/WebM/MP4 automatic splitting is supported; the manual transport action follows its own format gate. Completed MP4 segments remux in the background, and session completion waits for required jobs.

Default free-space warning is about 2 GB; the hard-stop threshold is about 500 MB and grows for remux coexistence/pending jobs. Low space warns or stops gracefully as appropriate. An admitted writable destination whose space cannot be queried proceeds with a logged inactive-protection warning; measured zero space is blocked. FAT32 raises an advisory for its per-file limit. There is no implicit split at 4 GiB.

### Recovery

An interrupted recording can be offered next launch from its manifest:

| Action | Result |
|---|---|
| Finish | Save according to the interrupted session's original intent; no new arbitrary container selection |
| Continue | Unfinalized crash candidates only; arm a new paused recording slice, not a single-file concatenation |
| Delete | Explicit two-step destructive confirmation |
| Decide later | Keep the candidate for a later launch |

Only one continuation is armed at a time. Missing/empty artifacts are removed from offers. Already finalized segments remain useful; an interrupted active tail/segment may not recover. Periodic flushes are not a universal zero-loss or exact two-second loss guarantee on every storage device.

Manifest failure does not abort a good recording. It raises Recovery protection unavailable and marks that session unprotected instead of pretending an entry exists. Local recording recovery is independent of crash-upload consent.

### Edit / Output / Save

Completed recordings normally remain on Record with Edit as the next action. Open editor when finished is off by default. The workspace has player/timeline on the left, persistent Details and Export cards on the right, and one primary Export action bottom-right. It fills the regular content region without a modal scrim or covering the window controls.

The header contains Back, title, middle-elided filename and a labeled report status. A report badge is status, not a mislabeled action. The right rail remains available and scrollable at minimum size. Starting/finishing/failing export does not move its scroll position or card anchors. Output choices remain visible, disabled while running.

The video timeline shows real decoded thumbnails at their timestamps, sized by clip aspect ratio and available width. Audio tracks are labeled/fill rows, **not waveforms**. Unknown track names use Audio 1, Audio 2 rather than guessing sources. A silent video-only file has no audio row. Loading and terminal unavailable thumbnail states are distinct; thumbnail failure does not automatically disable export.

Trim handles cannot cross and snap to a keyframe at or before the requested point, with marker snapping within 50 ms. Handles/markers/playhead span all rows because the trim applies to the clip. Changes are applied only on Export. Scrubbing pauses playback and resumes only when it was playing before the drag. Drag labels expose precise time. Player controls remain keyboard operable while their visual transport overlay fades during playback/scrubbing.

Export offers MKV or MP4 lossless stream copy, either beside the source as `<name>_edit.<ext>` or explicit confirmed replacement. It does not re-encode for arbitrary frame-accurate cuts. Overwrite is not the destructive default button. Normal destination follows save mode; an output failure can offer Choose another folder for a one-time recovery destination rather than a meaningless repeat of the same failing write.

The Export card reports running/progress/cancel, success/file/folder or failure/remedy. There is one actionable retry route, not competing Retry and Export operations. Successful output names file and folder on separate elided lines and offers Show in folder. Cancel stays running until the worker actually completes cancellation. Closing the workspace releases decoders and its temporary recipe while an already-running export continues independently.

Exported markers use a JSON sidecar only when markers survive the trim; timestamps are rebased and removed markers are dropped. With none surviving, an old destination sidecar is removed. The filename replaces the media extension: `clip_edit.mp4` produces `clip_edit.markers.json`, not `clip_edit.mp4.markers.json`. Media with the same stem shares this convention. No container chapters are written.

Playback supports the implemented 8/10-bit 4:2:0 and 8-bit 4:4:4 formats, including an SDR tone-map of properly tagged HDR10. Hardware decode can fall back to software at open. Multi-track audio is mixed for playback; it is not a multitrack editing timeline. Preview failure does not change the lossless export mechanism.

## 9. Presence and notifications

The in-app notification hub is the record. Every notification reaches it regardless of the toast preference and retains its action until dismissed. Severity has a glyph, word and accessible description. The bell and tray unread indicator reflect the same unread state; the bell's color reflects the worst unread severity, not a numeric counter.

Success entries arrive read. Deliberately dismissing or acting on a toast reads its hub entry; an automatic timeout does not. Informational actions such as an available update or preset Undo remain findable. Clear/dismiss does not undo the completed product operation.

Timed toasts describe events and last 5 seconds for a glance or 10 seconds when a problem/action needs attention. A newer timed toast replaces the visible one. Standing toasts describe an ongoing condition such as low storage, source degradation or capture stall, stack separately and clear when that condition ends. An unexpected stop or recovery offer is an event, not an unbounded standing condition. The hub retains its full text.

Toasts anchor to the screen hosting the app, wrap bounded text, preserve full detail in the hub and never steal keyboard focus. Their actions/dismiss affordances remain operable. Escape dismisses the relevant toast only while an ExoSnap window has focus. Transparent gaps do not intercept the desktop. General ledger measurements are recorded without interrupting capture; explicit ongoing-condition notices remain their own policy.

There are five capture-excluded windows: recording pill, diagnostics pill, countdown, quick controls and toasts. The first three are click-through; quick controls/toasts accept their explicit interactions. Diagnostics and quick controls default off. If capture exclusion cannot be applied, the affected overlay hides rather than risk contaminating recording. The ordinary app window has a separate opt-in capture-exclusion preference, off by default, applying beyond ExoSnap to screen sharing/screenshots too; failure leaves that window visible and logs it.

Overlay content is configurable, not arbitrary styling. Recording offers Minimal or Custom content. Diagnostics offers Health, Technical or Custom tokens. Every offered value has a runtime producer; unmeasured values render as an em dash. An empty content selection hides the overlay. Quick controls stay inside the monitor work area with a margin; recorded-picture overlays use the relevant monitor geometry. Countdown progress depletes continuously rather than moving only at whole-second ticks.

Tray/taskbar states are Idle, Recording, Processing, Paused, Saved and Failed. Recording animates brightness, not size, while active; pause stops it. Saved has a bounded dwell; Failed remains until the next recording. Processing animates while meaningful non-interruptible work runs. The icon uses the chosen accent separately from state color.

The native tray menu retains stable transport rows with unavailable entries greyed. It provides start, pause/resume, stop, show window, output folder, notifications and quit, with a blocker reason when applicable. Click/double-click restores the app; it never toggles recording accidentally. Native shell menus/taskbar surfaces follow Windows appearance, not necessarily the application's theme. Taskbar integration failure is nonfatal.

Minimize-to-tray is opt-in and off by default. Without a usable tray, minimize behaves normally. Win+D remains the shell's reversible show-desktop action. Close always means close, not hide: an in-flight operation uses the appropriate confirmation/refusal guard, and an approved close explicitly ends the process despite remaining overlay windows. Finalization can refuse close while writing the container. Tray Quit uses the same lifetime policy.

## 10. Hotkeys

| Global action | Default |
|---|---|
| Start / stop | `Alt+Shift+R` |
| Pause / resume | `Alt+Shift+P` |
| Capture frame | `Alt+Shift+S` |
| Add marker | `Alt+Shift+M` |
| Split recording | `Alt+Shift+C` |

Global bindings can be changed or cleared. Invalid/reserved/conflicting bindings are rejected with rollback rather than losing a working binding silently. An unregisterable startup binding stays cleared; a lost user-chosen binding raises a Rebind notification, while an unavailable shipped default can be removed with a log entry. Windows does not identify the process holding a conflicting global shortcut.

Starting by hotkey activates Record if the app is visible but does not restore a minimized window. Countdown/Preparing use their cancellation contracts rather than starting a second session.

### In-window keyboard operation

`Ctrl+1` through `Ctrl+5` select the five destinations and share the same modal/navigation guard as tabs. These are not global rebindable hotkeys. Surface-local keys apply only to the focused surface; text editing must not be consumed by unrelated shortcuts.

Controls have keyboard focus indication and accessible names; buttons/toggles use Space activation, with dialog-default Enter behavior. Segmented groups use one tab stop and arrow/Home/End navigation. Unavailable controls show the real reason and no pointing-hand affordance. Clickable, text-editable and draggable surfaces use their appropriate cursors.

The Edit timeline is one tab stop: Left/Right moves one second, Shift ten seconds, Ctrl a tenth; Home/End reaches bounds; `[`/`]` chooses playhead/in/out manipulation; I/O sets trim at the playhead; Space toggles playback. Focus and accessibility tests establish specific contracts, not blanket proof of every assistive-technology combination.

## 11. Diagnostics and fix actions

Diagnostics is state-ordered, not a separate Simple/Expert mode. The Expert toggle belongs only to Settings.

| State | Main contents |
|---|---|
| Idle | Readiness verdict, Encoder/Disk/Display/Audio tiles, issues, tips and reference rows |
| Recording/Paused | Verdict, six-stage pipeline, live metrics and session ledger, blockers/tips/reference |
| After stop | Readiness verdict plus Last session card and frozen findings |

Self-test, Hardware capabilities, Environment & configuration and Support bundle are collapsed references with summaries and their relevant actions. Probes run on relevant visibility/configuration and periodic refresh rather than needing a misleading permanent check button. Live metrics use their measured cadence and do not masquerade as raw per-frame analysis.

Each diagnosis declares both severity (Pass/Notice/Blocker) and tier: Blocker, Measured problem, Optimization or Fact. Blockers prevent start. Measured problems remain represented and attention-colored. Optimization tips bundle quietly and cannot alone make readiness amber. Facts are neutral reference and do not count in the verdict.

Issue cards expose diagnostic ID, reason, measured value/budget and a typed fix. Evidence/log excerpts are secondary. Session ledger entries need distinct consecutive observations, retain active/quiet state and worst values, and remain until stop. Static configuration conditions do not become runtime incidents. Last session reports drops, achieved rate, drift and file status plus actual ledger occurrences. Individual frame drops have no fabricated timestamp marks.

In-depth diagnostics is a session-only header switch, off each launch, unavailable while recording. It adds present/health/DPC/GPU-related readings only where their producers can measure. Unmeasured tiles state why. Opt-in and elevation are separate: switching on in a standard process offers Restart as administrator but does not itself prompt. UAC decline is recoverable and does not persist consent. A successful elevated successor returns to Diagnostics with the requested session observation enabled.

Present/tearing/DPC traces are optional. Standard capture/cadence diagnosis remains useful without them. Present data describes the current attribution/recording and becomes unavailable when its process/trace ends. It must not carry a previous recording's totals into a new one. Driver attribution and legacy-FSE detection remain evidence-qualified.

Auto fixes are executable reversible changes after review/confirmation; scope/track changes always name consequences. Assisted fixes reveal the existing control/location. External fixes direct to an operation ExoSnap cannot do. Nothing applies silently. No new Windows administration interface is created through Diagnostics.

Reports/logs/support bundles are local. A per-recording report retains the ten newest files and uses unavailable rather than fabricated counters. Support bundle creation gathers scrubbed logs/reports and allowlisted facts into a user-chosen ZIP, never uploads it and never includes recordings/raw configuration/crash dumps. Known-shape scrubbing is not a guarantee about arbitrary personal prose; review before sharing.

Logs provides a virtualized structured list with Time, Level, Category and Message, filtering/search and current-view count. Copy means the filtered visible history; Export means the full current in-memory history. Disk rotation and report retention are separate. Startup measurements are recorded milestones, not a general continuously running profiler.

## 12. Settings model (Default / Expert)

Settings serves technically inclined users without assuming video/audio specialization. Default exposes practical format, source, quality, output and recovery choices. Expert reveals raw or compatibility-sensitive mechanics **within** existing cards; it never hides an entire section.

At desktop width the left column is Recording format, Audio sources, Microphone, Output, App behaviour, Hotkeys and Appearance. The right is Video quality & timing, Audio encoding, Webcam, Overlays, Updates and Support & diagnostics. Narrow layout retains the same sections and operability. Preset actions and Expert share the first toolbar.

Expert owns numeric rate control/CQ/bitrate, custom frame-rate entry, pacing, NVENC preset, keyframe interval, color range/depth/chroma, specialized Opus/sample-rate/clock controls and relevant raw gain/limiter/float choices. Relevance can narrow these further: no H.264 10-bit row, no unsupported 4:4:4 row, and no lossy-codec bit-depth field. HDR handling is Default-visible on a relevant HDR display. Automatic splitting, microphone processing and ordinary audio controls are not hidden just for sounding technical.

Selective information hints explain a real trade-off. Multi-option popovers explain and identify the current option but do not become a second reduced picker; the actual control changes the setting. A locked control retains its value and real unavailable reason. Planned work is not presented as active capability.

## 13. Updates and crash reporting

### Updates

Checks default off. A manual check is explicit action; changing channel alone does not contact the feed. Stable excludes prereleases, Preview includes them. Switching channel invalidates its old answer and discards an in-flight answer for the previous channel. Normal startup is Unchecked, not a false Up to date.

The Settings card states Unchecked, Checking, Up to date, Available, managed by Scoop, Updater running, Restart pending or an actionable Error. A same-version verification run can additionally show Verification reinstall available. Checking does not move the Settings scroll position. Applying is blocked during recording/finalization and never silently restarts the app.

A signed offer pins one full target version. The app hands the separate updater its versioned document plus manifest/signature; the child re-verifies and installs that target or nothing. An app-handoff updater does not search for a newer release itself. Signed-manifest verification precedes parsing, and package SHA-256 is checked before use. Downgrades are refused. Scoop-managed installations are not swapped.

Portable apply stages verified files, retains a backup through verification and restores it on supported failure paths. A failed restoration is reported. MSI invokes Windows Installer with its one elevation boundary; an MSI post-install verification failure does not prove the previous version was restored, so unknown installation state is possible. Neither path promises every conceivable interruption can be repaired automatically.

Manual updater start is idle and requires Check, Download and Install confirmations. App-handoff mode continues the already-approved specific update instead. Cancel is accepted only where the worker observes it, notably download. Critical install/verify/launch steps refuse close. A canceled download is neutral and says nothing was installed. Mode-aware retries do not offer to repair an immutable invalid handoff by repeating it.

The updater uses a stable 520 × 680 logical-pixel window, a 56 px title band, fixed progress/state/action regions and readable elision for long versions or system messages. The role label stays Updater; phase/reinstall/failure belongs to content. Installed-version emphasis reflects what is actually safe on disk. Retryable, hard-stop and installed-but-needs-manual-launch results remain distinct.

Updater exit codes are 0 applied/verified, 1 failed, 2 invalid command line, 3 manual check up to date, 4 installed with reboot required, 5 canceled/closed without an applied outcome. The UI and automation state use the same outcome owner.

`--verify-update-reinstall` is explicit, nonpersistent and exact-version only. It adds no downgrade/signature/hash exception, uses the real production path and does not survive a normal restart. An ordinary developer build cannot verify releases with its zero placeholder key. The HTTPS development feed override is refused in official builds and persists nowhere.

What's new uses release bodies already obtained by the checker. The pre-update view can show the active channel's release list. The approved update stores target-bound gap notes for a one-time post-update presentation only when the new running identity matches. The default-on Show release notes after updates choice gates that automatic display, not manual access. Manual ZIP replacement/first install does not fabricate a pending update payload.

### Crash reporting

Local crash capture and recording recovery remain available without upload consent. Reports are offered on the next launch, not by an immediate in-session reporter. The dialog describes previous-session facts actually known, with separate dump/cause/recovery availability, and does not fill unknown fields with current machine guesses.

Ask every time is the default. Send automatically and Never send are revisitable Settings → Support & diagnostics choices. Never send suppresses the upload prompt, not recording recovery. Send without Remember is one-shot consent; remembered policy is committed only by Send/Don't send with the unchecked-by-default checkbox enabled. Close/Escape changes no policy. Open crash folder is directly available.

Only official builds include the configured Sentry upload path. Structured events are scrubbed; native minidumps are a separate binary channel and can contain module paths. The privacy disclosure states this and stays keyboard accessible without scrolling the consent control away. There is no prefilled GitHub issue action in the crash dialog.

Current portable/MSI artifacts are not code-signed in this source configuration. SmartScreen can warn. A future certificate or hosted service state must not be inferred from this specification.

## 14. Privacy

No analytics, telemetry, account or recording upload. Default launches make no intentional runtime network request. Update checks/downloads and crash reporting are separate explicitly enabled/user-initiated network features. Update checking sends no authentication token or installed-version field; comparison is local. Remembered automatic choices remain explicit opt-ins, not a requirement to click on each launch.

The public details and retention/recipient statements are in [PRIVACY.md](../PRIVACY.md). This synchronized allowlist describes permitted structured crash tags, not a promise that every tag is populated:

<!-- PRIVACY-ALLOWLIST-TABLE-BEGIN -->
| Tag key | What it carries |
|---|---|
| `os.name` | Windows edition name |
| `os.version` | Windows build/version |
| `gpu.model` | GPU adapter name |
| `gpu.vendor` | GPU vendor |
| `gpu.driver` | GPU driver version |
| `app.version` | ExoSnap version |
| `encoder_backend` | Active encoder backend |
| `container` | Output container |
| `video_codec` | Selected video codec |
| `audio_codec` | Selected audio codec |
<!-- PRIVACY-ALLOWLIST-TABLE-END -->

Current application code sets encoder_backend/container/video_codec/audio_codec on this tag path; the other keys are allowed but not populated there. Structured event scrubbing excludes user paths/names, machine identity and breadcrumbs. A native Crashpad minidump is not processed by that scrubber and its module list can expose a portable install path containing a username. Recordings are not crash inputs.

Settings, presets, history, recovery metadata, reports and logs are local application data. Recordings are in the configured output folder, not automatically inside LocalAppData. Window titles are neutralized at logging sources and additionally scrubbed from recognized bundle fields. Support bundles are manually created/shared local files and do not include crash dumps or recordings.

## 15. Platform support and known boundaries

Windows 10/11 x64 is supported, with Windows 11 primary and Windows 10 best-effort. No Windows ARM64, Linux or macOS release is provided. Recording requires a supported NVIDIA NVENC device/driver; AMD, Intel and software video fallback are absent. A working GPU generation does not establish every codec/depth/chroma combination.

The dynamic Visual C++ x64 runtime is required. Portable ZIP/MSI do not bundle it; package-manager dependency declarations are channel-specific. Install the runtime when absent. Keep the portable runtime files together.

Replay buffer, arbitrary frame-accurate re-encode cuts, project/multitrack editing, embedded chapters, surround output, HLG, 4:2:2, 10-bit 4:4:4 and cross-vendor encoding are not current features. See [known limitations](../KNOWN_LIMITATIONS.md) for narrower behavioral and verification boundaries, and [roadmap](roadmap.md) for explicitly future work.

ExoSnap is GPL-3.0-or-later. Bundled components retain their own notices and license terms in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

## Implementation references

The observable contracts are implemented through [Quick composition](../app/quick/ExoSnap/Quick/QuickApplication.cpp), [Settings](../app/quick/ExoSnap/Quick/SettingsAdapter.cpp), [Record transport](../app/quick/ExoSnap/Quick/RecordTransportDock.qml), [preset model/store](../app/settings/RecordingPresetStore.cpp), [recording coordinator](../app/services/RecordingCoordinator.cpp), [compatibility registry](../libs/capability/src/container_compat_registry.cpp), [hotkeys](../app/services/GlobalHotkeyService.cpp), [diagnostics](../app/diagnostics/RecommendationEngine.cpp), [updater](../apps/updater) and [privacy scrubber](../libs/crash_capture/include/crash_capture/crash_scrubber.h). Focused tests under [app/tests](../app/tests), [Quick tests](../app/quick/tests), [engine tests](../libs/engine/tests), [capability tests](../libs/capability/tests) and [update tests](../libs/update/tests) pin those rules. Test presence is not a substitute for running the appropriate checks on the target artifact.
