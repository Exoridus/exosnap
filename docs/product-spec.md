# ExoSnap Product Specification

**Status: Current product contract, pre-1.0.** This document owns user-visible behavior, defaults, terminology and UI contracts. [Architecture](architecture/overview.md) explains implementation constraints. [Known limitations](../KNOWN_LIMITATIONS.md) qualifies support; the [roadmap](roadmap.md) describes future work, not implemented behavior. Settings and file schemas are not frozen before 1.0.

## 1. Product intent and principles

ExoSnap records a Windows display, application window or region with NVIDIA hardware video encoding, independently routed audio, optional webcam composition and recording diagnostics. It records locally without an account. MP4 delivery is a lossless remux, not a second video encode.

The product prioritizes reliable recording and actionable evidence over an exhaustive set of theoretical format choices. Readiness distinguishes hard blockers from advice. Runtime problems are measured rather than invented from configuration alone. Unknown values remain unavailable, not zero. A source or format is never silently replaced in a way that changes what the user agreed to record.

Configuration and compatibility have one C++ owner. Presentation consumes resolved options, track plans and state. No product behavior depends on a second implementation of those rules in QML.

## 2. Navigation and information architecture

The title band provides five direct destinations in this order: **Record, Edit, Settings, Diagnostics, About**. Logs is not a top-level destination: it is the full log view inside Diagnostics, reached from the Reference section, a failed recording's View log action or a Show in log action, and it keeps Diagnostics selected. Hotkeys is a Settings card. Hardware inspection is a collapsed Diagnostics reference, not a Device destination or an encoder selector.

The Recording profile inspector owns recording choices. Settings owns global application preferences. Diagnostics owns observations and hosts both its overview and the internal Logs view. Record owns capture setup, preview and transport. Edit owns the in-memory recording-edit workspace. About owns application identity and project links.

### Shell and window

The window has one custom 40 px title band containing brand, navigation, drag area, state, notification bell and native-behavior window controls. Tabs retain readable labels at the 860 × 700 minimum; their spacing yields before window controls disappear. Record and Settings use their own context/preset bands instead of an additional redundant page heading. About is a centered identity surface.

The preferred first-run size is 1280 × 720, centered and clamped to the primary work area. Valid saved geometry wins. Position, dimensions and maximized state are correct on the first visible frame. Native resize, double-click maximize/restore, system menu, Snap and supported Windows 11 Snap Layouts remain available. A maximized window has no active resize edges and restores its saved normal rectangle. A maximized window minimized to the taskbar returns maximized.

The operating system's supported corner/border treatment is used where available, with ordinary fallback rather than an error on systems lacking it. Native window identity stays `ExoSnap`; it is not localized because activation and updater handoff use it.

### Edit and modal surfaces

Edit is a top-level destination. Like other pages, it loads lazily and remains resident after first use. Leaving pauses playback and retains assets, clips, selection, playhead and undo history for the application lifetime. There are no persistent project files, drafts or recovery entries for edit workspaces.

An export already started continues from its immutable snapshot after leaving Edit and reports through notifications. Navigation does not cancel it. Recovery, crash-report and recording-error surfaces are different: they are blocking questions, shown one at a time with queued requests retaining their order. They prevent starting a recording and navigating behind them, but do not disable stop/pause/resume for an existing session. A close confirmation participates in the same navigation guard.

Modal scrims cover the shell, including its title band; the content card remains below that band. The card has no imitation title bar. Headings and decision controls stay fixed while long explanatory content scrolls. Consent/remember controls remain visible with the actions, including at minimum size.

### Encode device

Expert Settings offers an encoding-device row: Auto (default) or one enumerated physical adapter, shown as device name plus its backend. Auto encodes on the capture adapter when that adapter has a compatible, implemented backend; it never picks a different GPU. An explicit device is executable only when it is the capture adapter, because the current pipeline encodes the capture device's surfaces; a device on another adapter is shown unavailable with its reason. Explicit AMD/Intel devices have no implemented backend in this build and are never silently redirected to NVIDIA. The persisted preference stores a PCI fingerprint, not the boot-scoped adapter LUID; a missing or ambiguous device resolves to an honest failure rather than a guessed adapter. Adapter cards in Diagnostics remain inspection surfaces.

### Appearance and language

UI language offers System, English and Deutsch, with System as the default. German system locales use German; all other system locales fall back to English. A language change is saved immediately and shows a restart indication. The running process keeps its language until restart. The app-launched updater uses the running application's effective language; a manually launched updater uses System. English source text remains the fallback if a catalogue cannot be loaded.

Appearance and accent are independent: Dark or Light, plus Aqua, Sky, Violet or Magenta. Defaults are Dark + Aqua. Changes apply without restart. Unknown stored preferences resolve to a valid default; supported older preference forms are mapped on load.

The accent identifies interaction/selection, not severity. Coral means recording, error or destructive action; amber means warning/attention or the distinct labeled Paused state; green means ready/success. Countdown, Preparing, Stopping, Saving and Locked are neutral. Resume is an action and remains accent-colored. Status never relies on color alone.

Text and filled-control ink target 4.5:1 contrast; state/focus indicators target 3:1. The product also uses a 3:1 visibility floor for unavailable controls, even though they are not interactive. Structural hairlines are separation, not status indicators. These are role-based token checks, not a blanket certification of every rendered state.

Unavailable controls retain labels/current values, drop interactive emphasis and expose the real reason. A locked-on switch preserves its on shape without looking pressable. A row dims only when all its controls are unavailable. Accent swatches show the value for the current appearance and use a separate selection ring and accessible name.

The source picker groups displays, application windows and region actions in one scrollable selection. Search and section jumps change the visible choices without changing the active source. All eligible windows remain reachable; virtualization limits thumbnail work to visible rows. Confirmation resolves the pending lifetime identity against a fresh target list. A disappeared or recycled window is unavailable and never falls back to its old list index.

Hiding a window until restart applies only to that window lifetime. Persistent title rules explicitly choose equals or contains, case sensitivity and application scope. The default is contains, case insensitive and scoped to the selected application. Application-wide hiding requires separate confirmation. The picker menu reviews, disables and removes global rules independently of recording presets. The hidden count deduplicates eligible windows before text search; search matches have a separate count. Hidden windows have no thumbnail demand unless the user reveals them and they are in the viewport. Hiding the selected window does not stop or retarget its recording, and does not conceal its contents in a monitor recording.
## 3. Recording defaults and profiles

| Setting | Default |
|---|---|
| Container / video / audio | MKV / AV1 / Opus |
| First-run video reconciliation | Best supported encoder: AV1, then HEVC, then H.264 |
| Frame timing | CFR 60 fps |
| Quality | High, canonical CQ 19 |
| Encoding device | Auto (capture adapter) |
| NVENC preset | P4 |
| Frame pacing | Phase-correct where the selected capture path supports it |
| Color range | Limited |
| HDR handling | Tone-map to SDR on an HDR-active source |
| Cursor | Included |
| Countdown | Off; choices Off, 3, 5 or 10 seconds |
| Application audio | Added explicitly for a selected application window |
| System audio / microphone | System on, microphone off |
| Tracks | One named Audio 1 track with system audio assigned; microphone initially unassigned and off |
| Webcam / microphone DSP | Off |
| Mixed-bus limiter / clock slaving | On |
| Check for updates at startup | Off |

The output folder defaults to Windows' Videos Known Folder plus `ExoSnap`, including redirected Videos locations. It is not necessarily a literal path beneath the user profile.

Five read-only built-in presets lead the list:

| Preset | Container / codecs | Quality | NVENC preset |
|---|---|---|---|
| Default | MKV / AV1 / Opus | High, CQ 19 | P4 |
| Quality | MKV / AV1 / Opus | Ultra, CQ 16 | P4 |
| Compact | MKV / AV1 / Opus | Low, CQ 30 | P6 |
| Performance | MKV / AV1 / Opus | High, CQ 19 | P2 |
| Compatibility | MP4 / H.264 / AAC | High, CQ 19 | P4 |

The live configuration is persisted continuously. A preset is a named snapshot compared with that state. A difference marks the preset name with a neutral dot whose tooltip says the settings are kept either way, not an unsaved-project warning. Applying a preset preserves the capture target and applicable environment-derived depth/HDR choices; compatibility can still force a necessary clamp such as H.264 to 8-bit. Those preserved fields do not count as preset edits.

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

MP4 is delivered from a Matroska recording by progressive faststart stream copy. The UI distinguishes Finalizing/Saving from Saved and shows a percentage only once measured progress exists. Cancellation/failure retains valid source media where available. MP4 recordings retain the finalized MKV source of every segment as an edit master; these masters consume additional storage. A failed master rename leaves the valid source at its original path. Split MP4 recordings remux each completed segment while capture continues and are Saved only after required remux work completes.

SDR and native-HDR outputs carry their actual color descriptions. Native HDR is not relabeled BT.709 during MP4 delivery. The `hvc1` tag is part of the intended compatibility path, not a guarantee that every player or editor accepts every file.

## 5. Audio model

### Sources and tracks

New configurations use stable source, track and route identities. Settings presents track cards with names, roles, recording participation, assigned source chips and route gains. Empty tracks remain visible and produce no output stream. Move removes a source's previous routes and assigns it to the chosen track; Also add preserves existing routes. One source can feed several tracks, and several sources can feed one track. Output supports up to eight recorded tracks.

The source list separates participation, availability and mute. New sources are unassigned until explicitly routed. Application sources use a specific selected capture application; independent application sources freeze that process instance instead of following later capture-target edits. Source details own device, channel, post-DSP gain and individual processing controls.

Existing configurations retain their ordered-row routing and shared microphone settings until a structural edit is explicitly converted. Conversion fixes the current track layout, preserves IDs and can be undone. Unsupported future configurations remain read-only. There is one persistent routing editor; during recording its Active view describes the frozen output and edits configure the Next plan.

During recording, a source admitted at start can be muted and unmuted. Muting preserves its track and duration with silence. A source absent from the initial plan cannot be added mid-session and its toggle remains locked. Changing capture device or track structure requires a new recording.

Source gain is −60 to +24 dB, default 0 dB, with microphone gain on its own card. Source mute contributes silence. Recording mixes use fixed configured-source weighting, so a quiet source does not automatically make another louder. The mixed-bus limiter defaults on at 0 dBFS; its Expert ceiling is −12 to 0 dBFS. Limiting reduces clipping risk but does not reconstruct a clipped capture input.

### Meters and microphone

Source rows show measured pre-mute RMS and peak independently of post-limiter track RMS. Values use dBFS and remain unavailable until measured; silence is distinct from a finite -60 dB reading. Meter updates publish separately from editable routing cards so they do not replace focused inputs or drafts. Active output meter snapshots reach the routing panel at 10 Hz.

Microphone Device, Channels, Gain and Post-processing belong to the explicitly chosen source's details. Legacy configurations retain a separate shared microphone card. A disconnected configured device is not silently replaced by an arbitrary one.

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

Merged sources share one QPC-aligned output timeline after normalization to 48 kHz stereo. A bounded 30 ms arrival horizon fills missing intervals with silence, including quiet sources and endpoint outages. Rejoining sources occupy their timestamped intervals rather than extending the track. Single-source loss retains elapsed-time silence accounting. Long-running independent device clocks and physical partial recovery require hardware validation. An ordinary quiet but connected loopback source is not labeled lost. Diagnostics and standing notifications identify real degradation and clear when it ends; the report records that a gap occurred. No system can recover sound that was not captured.

Clock slaving is on by default. It gently resamples each measurable physical capture to the video/QPC clock before logical-source DSP and track fanout. It engages from drift above roughly 15 ms or a measured drift rate likely to exceed that within ten minutes. The displayed drift is the residual after actual correction; raw drift and applied rate are also diagnostic data. Correction is capped at ±500 ppm. Within that envelope the residual can approach zero; at/beyond the cap it can remain or grow. Merged tracks consume already corrected sources and do not run a second clock controller.

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

Expert CQ spans 1–51; the native mapping is displayed beneath it. AV1 uses the calibrated curve, with interpolation between points. These tiers aim at comparable quality, not pixel equivalence between codecs. Unsupported Lossless is not offered.

The Expert encoding-device row offers Auto or a specific adapter and applies from the next recording; codec and format choices follow the selected device's capabilities. The Expert NVENC preset control offers P1–P7, default P4 for all codecs, taking effect on the next recording. It appears only for an NVENC device; the stored value is retained while another device or backend is selected and is never applied to a non-NVENC backend. Higher preset numbers are a computation/quality trade-off, not guaranteed improvement on every workload.
The NVENC Expert area also offers capability-gated B-frames (Off or explicit counts through the codec/GPU and implemented SDK limits), B-frame reference mode (Off, Each or Middle only as reported by the SDK), Lookahead with depth, Spatial AQ, Temporal AQ and multipass. B-frame reference mode requires an applicable nonzero B-frame count. Middle references require at least two B-frames. AV1 is limited to seven B-frames with the implemented SDK modes, even if the driver reports 31 for a newer hierarchical mode. Lookahead depth is 1 through `min(16, 31 - B-frames)`. The product cap of 16 limits surface memory, while the pinned SDK's joint limit remains authoritative. This is a conservative resource limit, not an output-format-aware VRAM budget or a guarantee of available memory on every GPU. Temporal AQ has its own capability gate and does not require Lookahead. Multipass offers Single pass, Quarter-resolution multipass and Full-resolution multipass only with VBR or CBR. C++ owns relevance, options and reconciliation.

Defaults remain B-frames Off, references Off, Spatial AQ Off, Temporal AQ Off and Single pass. Lookahead offers Auto, Off and On. New configurations use Auto. Existing configurations without an Auto policy field retain their saved explicit Off/On wish and depth. Selecting Auto preserves that manual wish and depth. Only explicit On exposes editable depth.

Auto enables Lookahead 16 for NVENC AV1/VBR when the selected adapter supports Lookahead. Other codecs/rate-control modes or unsupported hardware resolve Off. GPU model, driver version, preset, dimensions, frame rate, pacing, GOP, color format and other advanced preferences do not restrict this policy. This is a deliberate product generalization from quality evidence measured on RTX 5070 Ti, P4, 1080p60 SDR 8-bit 4:2:0. It does not imply universal hardware or output-format qualification. Explicit Off and On remain overrides. Requested mode, resolved state, depth and reason are recorded at admission. The live configuration and named presets retain advanced NVENC preferences when a codec or GPU temporarily cannot apply them. The UI shows effective values and an unavailable-preference hint. Recording admission resolves again against the selected adapter's actual capabilities, and NVENC validates the request against its own session. No NVENC tuning is applied to another backend. Built-in presets retain their existing behavior. Expert availability does not imply a recommendation or measured default-quality improvement.

### Frame rate and timing

Default choices are 24, 30, 48, 50, 60, 90, 120, 144, 165 and 240 fps. Unavailable high entries remain visible with a reason. Expert accepts integer rates from 1 to the display-derived ceiling: the highest attached refresh rounded to whole fps, with a minimum ceiling of 60. That floor keeps the default expressible even on a slower/unknown display; it does not make the source produce 60 distinct pictures.

Manual edits and topology/refresh changes clamp to the active bounds. A loaded outside-range value remains displayed truthfully until the next relevant edit/display change rather than being mislabeled as the ceiling. Off-list values appear in Default as an inserted `<n> fps (Custom)` entry; selecting it alone does not change the recording rate.

CFR preserves a fixed output schedule through duplicates and explicitly accounted losses. Phase-correct pacing selects display frames by present time rather than blending/interpolating them. Lowest latency uses the newest available frame. The phase-correct implementation belongs to the duplication capture path; WGC uses its own newest-at-tick behavior. A normal region anchored to a monitor resolves through the monitor recording backend, whereas its idle preview uses WGC. VFR is not phase-correct CFR. If a buffered encoder needs more input during a static VFR interval, the engine submits the last processed picture at most once per second. These repetitions keep B-frame/Lookahead output moving and are counted separately from CFR duplicates and lost frames. With no pending picture, static VFR audio continues without inserting pictures.

No pacing setting guarantees judder-free motion from every source or makes 60 fps motion equivalent to 144 Hz. Sustained encode lateness skips and reports missed output slots instead of compressing video time against audio. VFR epochs do not include stale pre-record present time as an artificial lead-in.

### Resolution, depth and chroma

Output resolution offers Native, 720p, 1080p, 1440p, 2160p and validated Custom dimensions. Scaling preserves aspect ratio with a contain-fit content rectangle and padding as needed. Selecting an incomplete Custom size is temporary edit state, not an invalid persisted recording configuration.

All codecs have an 8-bit path. HEVC/AV1 can offer 10-bit P010 on compatible hardware. The Expert depth choice controls SDR precision; native HDR independently requires 10-bit. The irrelevant H.264 depth row is hidden.

4:2:0 is the default. Expert 8-bit 4:4:4 is available for H.264/HEVC only where the GPU exposes the required encode support. The row is hidden when irrelevant, but a fixable 10-bit conflict leaves its unavailable 4:4:4 choice visible with a reason. AV1 4:4:4, 10-bit 4:4:4 and 4:2:2 are not product paths. Live preview and still capture remain supported for the implemented 4:4:4 path.

### Color and HDR

SDR output uses BT.709 and defaults to Limited range. Full range is an Expert choice with a player-compatibility advisory. Conversion, encoded bitstream and container describe the same range/matrix/transfer; a player's interpretation can still differ.

An HDR-active source exposes HDR handling in Default mode: tone-map to SDR by default, or native HDR10 when compatible. Native means PQ/BT.2020, Limited range and P010, requiring HEVC/AV1. A selected HDR10/H.264 conflict is surfaced and blocks start rather than silently resetting the user's intent. Tone-mapped H.264 is not that conflict.

Mastering metadata is carried in the container and on keyframes in-band. Measured MaxCLL/MaxFALL is container-level, not promised as final in-band maxima during a recording. Split values can accumulate across the session rather than describe independently reset per-file analysis.

Tone-map exposure follows measured luminance inside the selected source region with smoothing. Off-region pixels do not contribute to luminance histograms or MaxCLL/MaxFALL. Measurements retain source resolution before output scaling and letterboxing; they are not downsampled output-peak measurements. SDR overlays use Windows' SDR-content brightness, with a fallback when unavailable. Color notifications or polling refresh changes, so a short interval can retain old exposure. SDR Advanced Color/scRGB with HDR off is not automatically treated as HDR content merely because its texture is FP16.

Toggling the captured display's Windows HDR state ends the current recording cleanly once detected, keeping the footage and requesting a new recording. Moving a window between HDR/SDR displays retains the session's initial color decision. No seamless color rollover, HLG or general wide-gamut management beyond the implemented BT.2020 path is promised.

Native-HDR recording preview and Edit playback are SDR approximations. Edit uses the reference display peak, not a display-adaptive HDR preview. Native PQ10 desktop capture is decoded from PQ/BT.2020 into linear scRGB FP16 before webcam/cursor composition. The same composited FP16 image feeds the shared preview, luminance measurement and the single PQ/P010 encode conversion. Physical HDR10 recording qualification remains distinct from shader tests.

## 7. Capture targets and webcam

The target kinds are Display, Window and Region. The picker is a responsive 1-4 column grid of identifying cards: displays may use fewer columns than the width allows so two displays stay compact, cards cap at 420 px wide and the grid centres rather than stretching. The Window tab has a search field and a filtered count; Display and Region do not. Selection is pending until Use source, Enter on an available card or a double click; it is carried by the target's stable identity, so a rescan keeps the same source selected and a vanished source locks confirmation instead of adopting the card that inherited its index. The Region tab is unchanged: the existing presets, anchor display, draw, confirm and cancel semantics remain, and no region is drawn automatically at record start. Each card contains its still within a fixed-dark 16:9 thumbnail surface; window cards show the window title above the app name. A still is not a live 60/144 Hz feed: cards keep their last valid still while it refreshes, a stale still is dimmed and labelled, and an unavailable or placeholder state never pretends a source can be selected. Windows are filtered for useful capturable targets. Display labels use sequential user-facing numbering rather than exposing gaps in GDI names. A shared label resolver supplies picker, transport, recent targets, notifications and filename context.

Region selection offers Draw custom first, then 16:9, 9:16, 1:1 and 4:5 starting shapes in physical monitor pixels. Presets create editable rectangles rather than immediately committing an immutable crop. The selection window covers the anchored desktop monitor, independently of the Record window and preview canvas. The outside is dimmed while the selected area remains transparent. Move/resize stays inside the anchor monitor in physical virtual-screen pixels, including negative origins; minimum geometry is 64 × 64. Selection handles and dimensions stay editable until recording locks/hides them.

The Record preview canvas fills the available page area with the same inset and gap as the transport dock. The source image remains centered and contained at its own aspect ratio as the target or region changes. Its border stays a neutral structural line; state belongs to the local pill/timer/actions. On-screen metadata is drawn above the video and PiP but is not part of recordings or frame screenshots.

Idle display preview uses the selected capture hub and holds through transient loss. Window/region idle preview uses WGC. Preview stops when Record is no longer actually displayed. During recording it normally switches to the engine's composited image and stops independent capture, without a black flash during handoff. Cross-adapter sharing failure remains an explicit exception, so WYSIWYG is not unconditional on every configuration.

Preview frame rate is a global preference under Settings / App behaviour: Off, 15, 30, 60 or 120 fps, default 60. Unsupported display-derived choices remain visible but disabled. It caps idle acquisition/publication and preview presentation independently of recording cadence. Off releases the idle capture subscription and shows Preview off; framing and webcam placement cannot be checked there while disabled. Recording and its composited tap continue. The preference is not part of a recording preset.

Preview redraw is producer-driven. A published frame missed during screen movement/exposure/scene reconstruction is presented when rendering is usable again without another frame or mouse event. Ready screenshots remain disabled until a usable preview frame exists.

### Target loss and identity

Saved display/region selection uses composite device/EDID identity with ranked matching, not just `DISPLAYn`. Ambiguous fallback or a missing anchor leaves the source unselected and reports the condition. Connector identity is not infallible physical-panel identity: the matcher accepts an exact device path before comparing serial/model, so a replacement panel on that connector can be selected. Confirm the preview after cable swaps or panel replacement, especially with identical serial-less panels or degraded GDI-only saves. A returning requested display can resolve automatically unless the user selected something else. Region restore is proportional to the anchor, not pixel-exact after resolution change.

Transient display capture loss holds/reopens the same runtime source. GPU removal or a closed target window ends the recording. Resizing the captured source ends it with an explicit old/new size error and asks for a new recording; encoder dimensions are not changed mid-file. Minimizing a captured window is not a resize: the recording holds its last frame while the window is minimized and resumes capturing when it is restored at its recorded size. A refresh-only change can use hold/reopen instead. Merely moving a window is allowed while captured dimensions remain unchanged.

Windowed/borderless content uses OS capture. Legacy exclusive-fullscreen window capture can be black or frozen; no injection/hook capture is provided. The recommended remedy is monitor capture or changing the target application to borderless. **Record the monitor instead** requires a summary/confirmation naming the wider scope and loss of the application-audio row. Real legacy-FSE behavior remains subject to title/driver verification, not a universal guarantee inferred from a heuristic.

A fullscreen-shaped, alive, visible window with no capture-frame progress for ten seconds can raise a standing caution that it appears stalled. It does not automatically stop. A genuinely still borderless window can meet the same condition; the language remains conditional and the notice clears when frames return. Ordinary static/minimized/hidden windows do not generate the same alarm. Display/region starvation needs corroborating sleep/disconnection evidence before a standing notice. Emitting CFR duplicates is not proof capture is healthy.

### Webcam

Webcam is an optional recorded PiP, not a fourth target. Device/resolution selection lives in Settings; its include control is synchronized with the Record camera control. Off is the default, and opening Settings with the webcam off does not open the camera. The first available camera can be preselected without recording it.

Resolution and frame-rate choices list only the modes the selected camera offers. A stored mode the camera lacks shows, and is captured as, the smallest offered size at least as large, else the largest; then the closest offered rate. Enabling the camera on an idle Record page shows it in the preview immediately.

The Webcam card shows the included camera live, drawn exactly as recorded; the camera is opened for it only while the card is on screen. Its Camera image group lists the standard UVC controls the selected camera reports (brightness, contrast, hue, saturation, sharpness, gamma, white balance, backlight compensation, gain, anti-flicker, exposure, focus, zoom, pan, tilt), each with Auto where the camera offers it. A control left alone keeps the camera's own value; set values belong to the profile, are applied whenever the camera opens and change live, including during a recording. Reset returns every control to the camera's defaults, and choosing another camera drops the values.

Placement/size are adjusted inside the preview's displayed video area, reaching its edges without distortion or clipping. Default placement is bottom-right with a margin. Metadata overlays stay above it. The idle preview draws the camera with the recording's own compositor, so mirror, opacity and chroma key look exactly as recorded; opacity defaults to 100%. The chroma key uses one key color, green by default, typed as hex (a leading # and the three-digit shorthand are accepted) or picked; it stays visible while the key is off so it can be set first. Tolerance, softness and spill reduction are disclosed when the key is enabled. Invalid input marks the field and explains itself on hover or focus without changing the layout.

Device and resolution are fixed during recording. Supported mirror/opacity/chroma and inclusion changes apply live. A healthy static desktop is recomposed with the current webcam each CFR tick; actual capture-loss recovery holds the picture. A lost camera holds its last image and retries the same device. No camera and a selected camera that fails to open are different states; the latter exposes the actual reason, not a fabricated generic device absence.

Media Foundation is a webcam dependency, not an AAC/recording dependency. Windows N/KN without the Media Feature Pack can still launch and record without webcam; webcam UI and Diagnostics explain the missing runtime.

## 8. Recording lifecycle

### Admission and transport

Every start route enforces blockers, even if Diagnostics was never opened. A missing source is No source/Unavailable, not Ready: Record stays visible but disabled and Choose source explains the next action. An invalid destination, unsupported encoder/format or genuine pre-flight blocker names the reason and appropriate remedy.

Countdown is Off/3/5/10 seconds and blocks incompatible configuration edits. Cancel, Escape and the record hotkey cancel it without starting a session. Preparing exposes asynchronous validation/resource work and remains responsive. Its hotkey cancellation is cooperative. Stopping/finalizing/saving are distinct from a playable Saved result; no transient callback may report success before those operations finish.

The Record layout has a full preview canvas with persistent context chrome and a bottom transport dock. The top-left pill shows resolution, codec, CFR rate or VFR, and HDR10 where selected. The top-right stateful Source Picker button names the display, app or region, including active region selection. It retains the existing pending-selection/commit picker. The bottom-left pill appears only with live recording metrics: canonical real Drops, residual A/V drift after correction and actual output size. All three use fixed-dark overlay surfaces and ink. Webcam composition remains in the recorded source rectangle at bottom-right. Status/error/success messages do not shrink the preview or move the dock. Locks preserve action slots and provide reason tooltips/accessibility descriptions. The source toggles, elapsed time and action group retain their relative positions.

| State | Recommended action and behavior |
|---|---|
| Ready | Filled primary Record; source and countdown controls available as applicable |
| Countdown | Filled Cancel, neutral progress state |
| Preparing | Preparing feedback; pending start can be canceled cooperatively |
| Recording | Filled Stop, secondary pause/screenshot/marker/split where supported |
| Paused | Filled Resume; Stop remains available without competing fill |
| Saving/finalizing | Real progress/state, no false Saved claim |
| Completed | Same as Ready, available immediately: Record, sources and an idle clock; no result controls, no automatic navigation |
| Failed | Modal failure explanation and report action; Back acknowledges the failure before Record returns; no success result |

A successful recording is confirmed by the Saved notification, whose Edit and folder actions open that recording, and by the shell's brief Saved indication. Record keeps no persistent success layout; the recording remains reachable from Edit History. A split recording can open in Edit as ordered segment clips. Missing/failed media is represented honestly rather than silently omitted. Capture frame is not shown beside a failed run. Icon-only controls have descriptive tooltips, and unavailable reasons remain hover-readable.

### Recording workspace and profile

Sources on the left and Recording profile on the right start closed. Opening an overlay leaves the preview geometry unchanged. Below 1200 logical pixels of workspace width, opening one overlay closes the other. Pinning is an explicit action and requires at least 480 by 280 logical pixels for the preview. A resize that violates this minimum closes the most recently opened panel with a visible explanation. Growing the window does not reopen it. Escape closes the active panel, and focus returns to its opening control without stopping a recording.

The source overview uses the configured source identities and shows availability, participation and measured levels. The profile inspector owns format, quality, output, audio routing, microphone, audio encoding and webcam configuration. It reuses the shared preset draft and the single track routing editor. Named preset apply retains environment-owned capture and HDR fields. During recording, Active recording is a frozen read-only view. Edit next recording explicitly exposes the separate next draft, whose changes take effect at the next start. Operational mute and webcam participation remain session actions.

The transport uses compact source meters and a geometrically centered timer with reserved width for hours. Stop remains directly reachable while secondary actions use a named overflow at narrow widths. Recording Clip creation is unavailable in the current development build. Opening the editor retains its existing availability rules.

### Runtime health and duration

Low-cost live readings include real dropped/duplicated frames, A/V drift, output bytes, storage risk and pipeline status. Real drops are backpressure, processing failure and undrained encoder output. Deliberate coalescing and empty pre-first-frame CFR slots are not counted as picture loss. All visible summaries share that distinction.

The live timer measures the running session and continues while paused. Paused Diagnostics shows session health up to pause and the frozen last measured pipeline state. Finished duration uses the media file, excluding pause and finalization tail; split duration sums media segments. The post-flight report includes drop percentage, measured drift and pipeline health without inventing timestamps for individual drops.

### Destination, splitting and disk protection

The destination is a folder chip opening the native picker, with its identifying path tail visible. It is not a text/paste path entry. The configured path can be redirected, a junction/symlink, mapped drive or UNC destination. Asynchronous validation reacts to startup, edits, activation, relevant network changes, Output visibility and final preparation. Unavailable/unwritable destinations are blocked with focused recovery, not a permanent manual Check again requirement.

Live valuable artifacts stay on the configured output volume with a `.partial` suffix. Disposable remux/repair staging is separate, and successful output is published by a same-volume atomic replacement. A failed repair preserves its valuable input. The default Videos Known Folder respects redirection.

Automatic duration and file-size splitting are independent, whichever comes first. Controls are Default-visible and reveal values only while enabled. Disabling an axis preserves its configured interval/size. Size is approximate and keyframe-safe. MKV/WebM/MP4 automatic splitting is supported; the manual transport action follows its own format gate. Completed MP4 segments queue for one background storage worker, and session completion waits for required jobs. Queued sources remain protected recovery artifacts and count toward storage reserve.

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

### Recording clips

Recording Clip creation is outside the currently offered product scope. The transport and overflow menu contain no Save clip action, and no clip panel is constructed. A compile-time C++ product capability rejects application requests before planning, leases, staging or background work. QML methods, stale adapter state and control-channel requests cannot enable it. There is no configuration, runtime switch or production development bypass.

The retained clip engine, UI components and regression tests remain engineering groundwork. Reactivation requires an intentional source change, proof of the inclusive 33,554,432-byte per-active-job memory contract and complete codec, long-duration and recording/isolation qualification. The memory contract is unresolved, not passed or waived. No H.264, HEVC, AV1 or audio clip-creation profile is advertised as available.

Previously saved standalone clips remain ordinary media files. Import, playback, History, editing, stream-copy trim and supported render/export use their existing media rules. This availability boundary changes creation only. Normal recording, splits, markers, recovery, MKV/WebM writing, MKV-to-MP4 delivery and shared background I/O admission remain active.
### Edit workspace and export

Successful recordings remain on Record, which is immediately ready for the next recording, and appear in Edit history. Saved-recording notifications follow the notification preference. Editing starts only through an explicit user action. History and notification Edit actions navigate to the Edit destination and add the relevant recording to its resident workspace.

The Editor has a stable dark viewer above one timeline card with one compact toolbar and a navigator row that holds zoom; there is no permanent status row. A rail at each viewer edge opens the panels: Media on the left; Details, Audio mix and Export on the right, which share one right panel host. A rail button for the context already shown closes its host, another context replaces it in place, and pin and width belong to the host. The empty viewer offers media import without a gradient or project-title/status header. Panels start closed and open from explicit rail buttons or shortcuts. Opening a panel uses an overlay until the user pins it. Pinning requires a window width of at least 1000 logical pixels and at least 480 logical pixels of remaining viewer width, including panel gaps. When those limits no longer fit, the panel becomes an overlay while retaining its pin preference. If both preferred panels cannot fit, the most recently pinned panel overlays first. Widening restores eligible pins. Each panel remembers its own width, bounded from 240 to the smaller of 560 logical pixels or 45% of the window width. Its splitter supports keyboard resizing and reset. The icon bars provide localized tooltips, shortcuts, focus and accessible names. Disabled actions expose their actual reason through accessibility and hover tooltips. Export state shows on its rail button: a progress line while a job runs, a dot for an unseen review or result and a warning glyph after a failure. A completed result is marked outdated when its timeline content or used source bindings change. Importing media without placing it does not mark the result outdated. Refusals and pending imports appear in a transient line under the timeline only while there is something to report. Snapping, Ripple editing, Auto Crossfade and Linked Selection are independent controls. Changing an editing option first validates any pending numeric draft under its existing policy; an invalid draft preserves both the draft and the old option. Snapping, Auto Crossfade and Linked Selection start on; Ripple editing starts off. Delete always lifts selected media without closing time; Shift+Delete explicitly removes media and time. Normal trim keeps unselected suffix clips at their fixed positions. Ripple moves and trims preview every affected clip before an atomic commit. The volume popup changes local monitoring volume. Closing an Edit panel with the pointer returns focus to the timeline. Keyboard panel closing returns focus to the control that opened it, so Space keeps driving playback after Ctrl+D or Ctrl+E from the timeline; when that control was inside the panel or is no longer visible, the context's rail button takes focus. Escape closes the right panel's current context rather than switching it. Details follows the active clip, junction, range, track or timeline context. Audio mix lists every audio track with mute, solo and gain: mute and gain are the same track values Details edits and apply to export, solo only changes monitoring. A fader drag commits one undo step on release and Escape during the drag restores the stored value. No level meter is shown without a measured signal. Export choices belong to the Export context; media-cache clearing belongs to Settings Storage.

Playback requests retain the latest Play/Pause intent while opening or seeking. Ordinary seeking continues active playback and preserves pause. Explicit frame stepping pauses for inspection. Scrubbing quiesces playback temporarily, and an explicit Pause during the gesture prevents resumption. Play remains available on a nonempty timeline while its preview opens; pending intent does not claim acknowledged playback. An unresolved source-audio choice does not block timeline video or admitted authored audio. A compact Choose audio tracks action opens the source-audio choice dialog. Named streams, their roles and channel counts form a draft until Apply validates the current source and track locks. Cancel changes no input mix.

A recorded starting audio mix becomes authoritative only after content verification validates its manifest against the held source. This completes import without adding an undo step or changing the authored revision. An explicit audio choice or authored clip enable override remains intact. Undo of a later audio choice returns to the verified recorded starting mix, and redo restores the explicit choice. Verification preserves a valid captured move, range or paste candidate.

Media presents Project, Folders and Recordings. Project holds referenced source files in the current workspace and offers native Add files and Add folder actions. Recordings uses the existing completed-recording history, including names, duration, resolution/FPS and unavailable-media state. Click and double-click only select a library row; double-click on a folder opens it. Enter or the row's insert button places the media at the playhead, and dragging places it at the pointer time. Delete and Backspace in the library never act on the timeline selection. Folders retains browsing roots between launches and enumerates only explicitly opened directories. Entries arrive in bounded batches, nested directories stay closed, and linked directories are not followed. Refresh and remove-root actions preserve unrelated browsing state. A directory dropped for placement asks once whether to add browsing roots or insert its ordered immediate supported files. Source files remain in place. Metadata probing admits useful media before background content verification completes; previews can appear while verification completes, and a failed verification blocks exports that use that source. Import continues after individual file failures. A changed timeline never receives the stale placement: successful media stays in Project with an explicit placement action. Selection changes preserve Project rows and browsing state. Crossfade remains a timeline/context action rather than a source-browser segment. Invalid boundaries refuse the edit visibly. The overlap overlay is selectable; Enter or double-click opens a compact duration action, and Delete turns it into a hard cut without moving clips.

Project source actions offer Relink source through a native single-file picker. A review shows both paths, expected and observed content identity, replacement duration and every affected clip. Preserving identity requires an exact established SHA-256 match. Adopting changed content requires an explicit choice and establishes a new source version only on Apply. Replacement audio choices use the actual named streams and require overlap confirmation where needed. The complete replacement must fit every source interval and stream layout and respect affected track locks. An incompatible or stale review leaves the source unchanged. Apply updates all references in one undoable transaction. Undo and redo restore their exact old and new bindings. Closing the review or removing its browser cancels pending work.

One vertical splitter resizes the viewer and timeline regions. The upper region retains a minimum height of 160 logical pixels and the timeline 220. The viewer takes the remaining width after explicitly pinned panels. Overlay panels do not resize the viewer. These layout preferences are independent of the temporary edit workspace.

The C++ timeline supports multiple typed video/audio tracks. A recording or imported file enters as one linked group with one clip per source stream: its video and each audio stream on its own audio track. A single audio stream, or a verified recorded default without overlapping alternatives, is enabled directly. When audio provenance is ambiguous, every audio stream arrives disabled and the first confirmed monitoring selection becomes that source's starting mix; alternatives such as a recorded master and its stems are never summed automatically. The first choice is one undoable authored change: undo restores the unresolved mix and its prior clip enablement, while redo restores the selected defaults. Earlier edits retain their own history, explicit enable edits keep their value, and a confirmed empty selection is deliberate silence rather than an open decision. Sources that cannot be bound to exact bytes or carry an audio layout beyond stereo keep a single linked audio row. Add, horizontal move, source-in/out trim, split at the playhead, delete and ripple delete operate on linked groups. Splitting keeps the original outer fades and introduces no fades at the new inner edges. A split inside an active fade or junction is unavailable rather than changing its envelope; exact fade boundaries remain splittable. A same-track overlap is valid only when the later clip starts inside the earlier one and ends after it; containment, equal starts and three simultaneous clips on one track are rejected, and trims outside the source are rejected. Every valid overlap owns exactly one junction, a full-overlap Crossfade by default, and the facing edge fades it claims are cleared in the same edit. Crossfade to next clip previews a transition around an adjacent cut, with an editable default duration of 500 ms on the operation grid. It extends the outgoing and incoming source windows into their available handles, placing the earlier half of the grid units before the cut and the remainder after it. Surrounding clips and timeline duration stay in place. Missing handles, incompatible links, locks or evaluator limits refuse the whole candidate with a reason. Enter commits the preview once and Escape cancels it. Auto Crossfade uses the existing validated overlap-creation policy. Convert to hard cut preserves clip overlap and source timing, choosing the previous transition midpoint on the track's grid; cleared fades do not return. Remove overlap is a separate structural preview. It trims paired source endpoints inward to the selected cut, or the crossfade window midpoint when no explicit cut has been chosen, while preserving their outer endpoints, surrounding positions and timeline duration. Linked targets share the operation grid and are checked together for locks and compatible remaining fades. Enter commits once and Escape cancels. Deleting an endpoint removes its junctions with the clip, then applies ordinary delete (retain gap) or ripple delete (close gap). Undo/redo covers all these mutations. Snapping aligns moves to neighboring boundaries and playhead; edits are not restricted to keyframes.

The timeline scrolls horizontally and vertically. Its navigation extent includes a tail of at least two seconds or 2% of timeline duration, whichever is greater; an empty timeline spans ten seconds. Fit is a one-shot action and later duration changes do not rescale the view. Previous View restores up to twenty completed zoom/fit operations; panning does not add history. Ctrl+wheel zooms around the pointer and Ctrl+Plus/Minus around the visible playhead. The exact rational output frame grid drives Left/Right stepping, including fractional rates, rather than a fixed 33 ms interval. New video edit boundaries use that grid, and independent audio edits use the 48 kHz output sample grid regardless of source sample rate. Linked A/V uses one common frame transformation. Quantization ties choose the earlier boundary. Moving an aligned clip preserves its frame/sample span across unequal frame intervals. A legacy off-grid move preserves its source window and duration while aligning the chosen anchor; trimming, splitting and fades normalize only the edited boundary. Undo restores exact previous values. This does not identify individual variable-frame-rate source pictures. Exact time fields share the C++ parser and preserve an unchanged legacy microsecond value. Escape in a field discards its open draft. Without a draft, the surrounding popup or panel handles Escape. If no surface handles it, focus returns to the timeline. The format menu offers non-drop-frame timecode, seconds with up to six decimal places, and 48 kHz samples for audio precision. Audio-only workspaces without a valid output frame rate display and edit seconds by default; timecode is unavailable until a valid video frame rate exists. Explicit seconds and sample formats remain available without inventing a frame rate. Explicit f, ms, s and smp units and relative values are accepted with decimal comma or point. Invalid drafts keep their text and focus until corrected or cancelled. Clip and track gain use 0.1 dB increments, with clip bounds -60 to +24 dB and track bounds -60 to +12 dB. Compatible multi-selection gain addresses only the explicitly selected audio clips in one atomic undoable command; mute remains a separate enabled state. One coordinate mapping drives ruler, clips and playhead. Track labels and ruler remain visible during scrolling, and clip delegates, ruler ticks and drag ghosts follow the visible interval. Clips fill their track rows, with names and media icons in a compact header above the previews. A drag starts beyond the platform threshold and leaves committed geometry in place beneath separate candidate ghosts. C++ validates the complete transaction against a frozen revision, including linked muted members, new tracks, junctions and ripple suffixes. Invalid candidates remain unavailable; release applies exactly the last valid preview at that revision or rejects it without history. Esc cancels. Explicit front/rear video and new audio drop zones create tracks only on successful release. With the selection tool (V), a drag from empty track space draws a marquee that selects every touched clip on the covered tracks, adding link partners while Linked Selection is on; Ctrl adds to the existing selection and Alt selects exactly the touched clips. A clip body then drags like its header, and a plain click positions the playhead on release. The range tool (R) keeps time-range drags in clip bodies and empty space; the ruler's range band works with both tools. Marquee selection creates no undo step. Escape restores the complete selection that preceded the marquee. Link selection and Unlink in the clip menu are explicit undoable link-group edits. A grip in each track header is the only reorder handle: a drag shows an insertion marker, release commits one undoable reorder, Escape cancels, locked tracks cannot be reordered and the focused grip moves its track with Up and Down. Marquee and reorder drags, and media dragged in from the library, autoscroll at the timeline edges; leaving the timeline or cancelling the drag stops the scroll, and a drop lands at the pointer time after scrolling. Track deletion removes metadata, never source files; nonempty tracks require a revision-bound confirmation.

Video clips show source-time thumbnails and audio clips show per-channel waveform peaks when available. Audio clips display one waveform lane for declared mono sources and two lanes for stereo sources. Legacy sources without validated channel metadata retain the normalized stereo overview. Audio trimming retains at least one source sample using the declared source rate, or 48 kHz when the rate is unknown. Rounded microsecond boundaries do not impose a fixed 21-microsecond minimum. At fine zoom, bounded normalized 48 kHz sample detail replaces the peaks as background decoding completes. Actual decoded silence remains visible; uncovered sample intervals stay blank. Pending or memory-limited detail can retain the peak overview. Source content identity, stream mapping and analysis/render schema identify persistent caches, so move, trim, split and gain/fade edits reuse source data. Missing data has a neutral placeholder. Zoom and pan update geometry immediately; model and preview requests publish together before the next frame. Only vertically visible preview bodies request media, and hidden timelines defer view work until shown. Cache presence never establishes source availability. Waveforms use a 512 MiB disk budget, thumbnails 1 GiB, with 32 MiB of decoded thumbnail RAM and a combined 32 MiB waveform/PCM budget, including consumer-pinned data. Settings > Storage owns Clear media cache. Clearing retires the current cache generation immediately; background cleanup removes retired files without blocking the editor. Clearing retires pending thumbnail requests, and later requests remain independent of old worker results. Visible clips automatically register their thumbnail and waveform interests again after clearing, including unchanged source windows. Recording and export suspend optional preview analysis. During playback, visible thumbnail demand and throttled waveform chunks take priority over speculative work. Visible clip boundaries, the current playhead and active trim edge are urgent; sparse thumbnail coverage grows from the visible center before filling remaining tiles. Changing this relevance keeps existing image delegates and source-time keys. Sibling clips retain independent interests, and separate visible sources share service fairly. Waveform generation streams decoded audio and completed peak blocks through bounded memory into private disk staging. Long sources are admitted by cache disk capacity rather than by retaining their entire peak pyramid in RAM. Completed caches are published atomically and reused without decoding. Split recordings retain ordered segment intervals. Missing or failed segments block only exports to which they contribute; an excluded or disabled missing source does not block a finished selection. Unknown-duration sources remain in Media and cannot be placed until their duration is established.

Gaps are ordinary time without a warning: an empty upper track reveals lower video, and an entirely empty output is black with no audio contribution. Clicking a gap selects its blank interval. Delete does not close it. Close Gap and Shift+Delete show the full ripple candidate, which requires Apply or Enter and can be cancelled with Esc; a conflicting contribution on another layer rejects closure. Copying selected blank duration and pasting creates a Spacer: stable, undoable track metadata with no file or decoder. Spacers can move and trim, and deleting one does not shift following content. Follow content end is the default timeline end mode. Details can set an undoable fixed end, which retains trailing black/silence after deletion and extends when content moves beyond it. Command selection, the editing range and playback bounds are independent. Loop latches the current editing interval, or the whole timeline when none is active, without starting playback. Selecting or trimming clips retains that latched loop. Range Once copies its interval and keeps it across later range or Loop changes, pauses and resumes within it, and stops at its end. Range track scope limits editing, while audition uses the normal project mix. Changing scope alone does not seek. An explicit seek outside the current half-open playback bounds exits Range Once and disables Loop. Clearing the editing range clears the latched loop interval; enabled Loop then covers the whole timeline. Viewport navigation remains independent. A blank timeline can export using its previously established output dimensions and exact frame rate; without an established output format it remains unavailable.

The Range Inspector lists its explicit track scope.
Changing that scope or choosing Include linked tracks stages one bounds-and-scope review.
The review shows the nearest boundaries on the resulting frame or sample grid, including a collapsed invalid interval.
Apply commits only a valid candidate, and Cancel retains the previous range.
Editing commands cannot use the old committed scope while this review is pending.
Including linked tracks follows the intersecting link groups explicitly. Subsequent commands still enforce every affected track lock.
Scope changes do not create authored undo entries or move the playhead.

Range gain changes are additive and affect only the scoped audio inside the selected interval.
The Inspector exposes the change in decibels. Invalid splits, links or locks refuse the whole command.
Remove time on all tracks is a separate named alternative to scoped ripple removal.
It previews the complete affected timeline before Enter commits or Escape cancels.

Clip-header double-click opens the contextual Inspector without closing an already open panel.
Double-clicking a trim edge focuses its exact Source in or Source out field.
A fade-corner double-click resets only that fade end.
Audio and video fade controls name their target and expose the evaluator's Linear, Smooth and Bias curves.
Changing fade duration retains its curve and bias. Junction-owned ends remain unavailable with a reason.
Overlaps narrower than eight logical pixels have a two-pixel marker while their evaluated window keeps its actual width.
Fit clips remains available in the toolbar menu and fits the occupied union with the same padding policy as selection fitting.

Preview and render export use exactly the same pure timeline evaluator and source-time mapping. Within one track a junction blends a complementary pair before that track is composited over lower tracks, so identical pictures keep their level through a Crossfade. Upper video tracks are in front. Clip opacity and edge fades composite over lower tracks, or over black where none contributes, and Contain letterbox bands show the lower track. Up to eight simultaneously visible video sources are admitted; a span that needs more is refused while the export is planned. Blank timeline time is black video and silent audio in preview and export. The same D3D11 linear-light compositor serves both paths. Audio junctions default to equal gain; a hard cut contributes exactly one source at the cut. Result audio mixes each source stream at absolute 48 kHz sample positions with clip and track gain, fades and junction gains, then the shared limiter; an enabled source that cannot be decoded fails the export instead of becoming silence. For per-stream clips, monitoring volume and the confirmed monitoring selection do not change the exported mix. A source with unresolved audio provenance exposes Choose audio tracks while timeline video and admitted audio remain usable. Apply confirms a single stream or deliberate silence directly. Unknown multi-stream content and verified overlapping alternatives require explicit overlap acknowledgement before admission. Seeking rejects stale generations. Hard-cut playback retains its wider 8/10-bit 4:2:0, 8-bit 4:4:4 and HDR10 SDR-approximation support. A preview failure does not itself disable eligible lossless export. General video effects are not implemented.

Export is the third context of the right panel, not a dialog. It shows the settings, the prepared summary, the running job, the result or the failure, all owned by the export job model, so closing the panel or switching context never changes the options or the job, and reopening shows the current state. Ctrl+E toggles the context like Ctrl+D; it is unavailable on an empty timeline without a job. Export… opens the native save dialog, and the prepared summary then waits for an explicit Export. Editing continues while a job runs; Cancel export is the only way to stop it. A result or failure stays until New export, Back to settings or a retry, and a finished result notes when the timeline changed after its snapshot. WebM and resolution-changing sizes are listed as unavailable with their reason. Export defaults to Match source. The Export context offers Entire Timeline, Occupied Content and Selection. Entire Timeline includes leading, interior and trailing blank time through the authoritative end. Occupied Content uses the bounding interval of enabled media, retaining interior gaps and rebasing output to zero. Selection explicitly chooses the edited result range, I/O time range or selected clip IDs; a custom result range is initially suggested when present. I/O takes all enabled contributions in that interval. Selected clips freeze only those selected/linked contributions and their relative offsets, with partial junction ownership reported as a typed blocking issue. Older linked imports require reimport for the new occupied/selection modes. The native save dialog starts in the configured ExoSnap output folder and allows filename, folder and MKV/MP4 container selection. Existing destinations require confirmation; originals are not overwritten implicitly, including through a hardlink or other alias of a source. There is no forced sibling `_edit` destination. Resolution-changing YouTube and Archive profiles remain unavailable; render output takes fixed geometry and exact rational frame rate from the first video source.

Export has two explicit intents. The finished result is the default and produces the authored composition with one stereo mix, or no audio stream when no audio contributes. Keep original streams, an Export result choice, copies the source video and every original audio stream with their identities for unedited source cuts; it is never chosen because it is faster, and mute, gain, fades, junctions or independent audio moves block it instead of being ignored. For the finished result, video is copied only when complete, compatible sources cover the whole exported timeline without video effects; otherwise it renders exactly to the output frame grid rather than copying from an earlier keyframe. Unchanged copied video can sit beside a newly rendered stereo mix, so an audio-only edit does not re-encode the picture. When exact rendering is unavailable, the finished result is refused with a reason that names the original-stream copy where that applies. Rendering accepts explicit BT.709 SDR 8-bit 4:2:0 sources and writes limited-range SDR video using the existing generic video/audio encoders and muxer. The mix is 48 kHz stereo PCM24 in MKV and AAC-LC 192 kbit/s in MP4; the MP4 result is written as Matroska first and delivered through the same faststart stream copy as recordings, with encoder priming signalled rather than audible. AV1 rendering into MP4 is not a qualified profile and is refused. Unknown/non-BT.709 color, HDR, 10-bit and 4:4:4 rendering fail explicitly; original-stream copy keeps existing HDR copy. Timelines from earlier linked imports keep the previous copy/render classification, including keyframe-accurate single-source trims. Before export starts, the Export context summary shows the video path, container, codecs, the audio result (Stereo, No audio or the number of original audio streams) and the mixed or excluded streams. The rendered mix receives a new track identity that records its input streams. Changes to the recipe, sources or selection require another review. Overwriting a file requires a separate confirmation. A retry keeps its failed destination and the replacement approval already given for that destination. Changing the retry folder does not transfer replacement approval. Render export uses the current encoder preset, backend tuning and lookahead policy with the same capability reconciliation as recording. Video and audio share one timeline duration rounded to the final output frame. Export snapshots are independent of navigation; cancellation remains pending until the worker stops, and completion/failure continues through notifications. Closing the application waits for cancelled export work and staging cleanup before completing teardown.

Marker sidecars use `<stem>.markers.json`, not a suffix appended to the media extension. Export filters and rebases surviving markers, removes stale sidecars when no markers remain and writes no container chapters.

Crossfade is the only implemented transition. New valid overlaps receive a Crossfade while Auto Crossfade is on, otherwise a hard cut; changing the toggle preserves existing junctions. Video and audio junctions can be edited independently through timeline handles and Details controls for the window, curve, cut position and Equal Gain/Equal Power law. Editing a window never moves clips. Junction before clip and Junction after clip in the clip context menu provide numeric access even when the overlap is too narrow for pointer editing. The scrolling popup names hard cuts and exposes their cut position; crossfades expose duration, window, curve and the applicable audio law. Conversion to a hard cut and the previewed structural removal of overlap are separate actions. Clip edge fades have handles and numeric controls. Persistent projects, other transitions, effects, transforms, titles and keyframes remain future work.

## 9. Presence and notifications

The in-app notification hub is the record. Every notification reaches it regardless of the toast preference and retains its action until dismissed. Severity has a glyph, word and accessible description. The bell and tray unread indicator reflect the same unread state; the bell's color reflects the worst unread severity, not a numeric counter.

Success entries arrive read. Deliberately dismissing or acting on a toast reads its hub entry; an automatic timeout does not. Informational actions such as an available update or preset Undo remain findable. Clear/dismiss does not undo the completed product operation.

Timed toasts describe events and last 5 seconds for a glance or 10 seconds when a problem/action needs attention. A newer timed toast replaces the visible one. Standing toasts describe an ongoing condition such as low storage, source degradation or capture stall, stack separately and clear when that condition ends. An unexpected stop or recovery offer is an event, not an unbounded standing condition. The hub retains its full text.

Toasts anchor to the screen hosting the app, wrap bounded text, preserve full detail in the hub and never steal keyboard focus. Their actions/dismiss affordances remain operable. Escape dismisses the relevant toast only while an ExoSnap window has focus. Transparent gaps do not intercept the desktop. General ledger measurements are recorded without interrupting capture; explicit ongoing-condition notices remain their own policy.

There are four capture-excluded windows: recording pill, countdown, quick controls and toasts. The recording pill carries the diagnostics tokens too, as a second section of the same surface rather than a separate window; the two sections are gated by their own independent settings. The recording pill and countdown are click-through; quick controls/toasts accept their explicit interactions. Diagnostics and quick controls default off. If capture exclusion cannot be applied, the affected overlay hides rather than risk contaminating recording. The ordinary app window has a separate opt-in capture-exclusion preference, off by default, applying beyond ExoSnap to screen sharing/screenshots too; failure leaves that window visible and logs it.

The recording pill uses a static coral indicator for recording, the selected accent for paused and amber for degraded capture health. It does not run a continuous decorative animation. Quick controls default to the target monitor's bottom centre. An Area selection prefers space below it, then above it. Dragging uses the full monitor boundary. A grip click collapses or expands the controls, while a drag preserves the chosen placement and re-clamps it when the monitor or dock size changes. The compact transport places quiet Pause/Resume and Capture Frame actions around the coral Stop action, with 44-pixel logical hit targets. The transport is 48 logical pixels tall, with 6-pixel vertical insets around its 36-pixel action surfaces. Hide sits above the drag/collapse grip in a secondary column on the right, both remaining available when collapsed. Their icons have equal vertical spacing above, between and below, with a reduced right inset. A manually placed dock keeps this column in position during collapse/expand whenever the monitor boundaries permit. Their separate hit targets are at least 24 logical pixels. Hide turns the persisted quick-controls preference off; Settings can enable it again.

Overlay content is configurable, not arbitrary styling. Recording offers Minimal or Custom content. Diagnostics offers Health, Technical or Custom tokens. Every offered value has a runtime producer; unmeasured values render as an em dash. An empty content selection hides its section; the recording pill's window itself hides only when neither section has anything to show. The recording pill anchors to the actually-recorded source rectangle (the region in Area mode, the live window rectangle in App mode, the monitor rectangle in Display mode) rather than to the monitor unconditionally, so it never drifts off the picture it is reporting on. Quick controls anchor near that same source rectangle by default (below an Area selection when there is room) but move freely across the full target monitor, including flush against the screen edge and over the ordinary taskbar there — there is no enforced taskbar safety margin. Countdown progress depletes continuously rather than moving only at whole-second ticks.

Recording Minimal shows the static state glyph and elapsed time. Recording Custom can also show file size and source name. Diagnostics Health shows real/problem drops, a compact pipeline status and muted/degraded audio indication. The status reuses the same engine PipelineHealth and bottleneck verdict as Diagnostics: OK while healthy, or a capture, encoder, storage, audio, GPU or unattributed warning. Muted audio is neutral; degraded audio is amber and means the source is recovering while recording continues with silence. Technical additionally offers capture rate, raw A/V drift and file size. Capture rate describes capture-frame activity, not the configured output FPS. Raw drift remains selectable in Custom. Existing Custom token lists keep their selections; unknown tokens are ignored.

Live overlay telemetry is change-driven and coalesced, with at least 200 ms between ordinary changed visible snapshots. Unchanged formatted content does not publish or schedule a periodic wake. State, visibility, warning, source/content configuration and exposed audio-state changes publish immediately. The interval is a UX/efficiency choice, not a proven hardware safety limit.

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

A binding needs Ctrl, Alt, Shift or Windows, except F13 to F24 and Pause, which foot pedals and macro keys send and no layout types with. The shortcut field shows the binding as key caps and captures while focused: held modifiers appear at once, the first other key commits, and focus moves to the field's clear button. Tab and Shift+Tab still leave the field, and Escape drops held modifiers. A refused combination is shown with the field's error mark and reason and is not saved; the clear button then discards it and the previous binding stays. While a field captures, ExoSnap's own global shortcuts are released, so pressing one reports the conflict instead of triggering the action.

Starting by hotkey activates Record if the app is visible but does not restore a minimized window. Countdown/Preparing use their cancellation contracts rather than starting a second session.

### In-window keyboard operation

`Ctrl+1` through `Ctrl+5` select Record, Edit, Settings, Diagnostics and About respectively and share the same modal/navigation guard as tabs. These are not global rebindable hotkeys. Surface-local keys apply only to the focused surface; text editing must not be consumed by unrelated shortcuts.

Controls have keyboard focus indication and accessible names; buttons/toggles use Space activation, with dialog-default Enter behavior. Segmented groups use one tab stop and arrow/Home/End navigation. Unavailable controls show the real reason and no pointing-hand affordance. Clickable, text-editable and draggable surfaces use their appropriate cursors.

The focused Edit canvas shares one C++ shortcut dispatcher. Text editors retain their ordinary editing shortcuts, and focused controls keep their local interaction. The command target follows the active clip, range, junction, gap, spacer or track selection; a remembered clip does not override that mode.

| Shortcut | Canvas action |
| --- | --- |
| Left/Right; Alt+Left/Right; Shift+Left/Right | Pause and step a frame; pause and step a sample in independent audio context; seek one second preserving intent |
| Up/Down; Alt+Up/Down | Pause at an edit boundary; select the previous/next clip |
| Home/End; I/O; Alt+X | Seek timeline bounds; set editing range bounds; clear range and loop bounds |
| Space; Shift+Space; Ctrl+L | Toggle playback on eligible release; play the copied range once on press; toggle loop |
| Comma/Period; Alt+Comma/Period | Nudge selected clips by one frame; use samples only for independent audio targets |
| Ctrl+Comma/Period; Ctrl+Shift+Comma/Period | Trim start later/end earlier; extend start earlier/end later |
| Ctrl+B or S; Delete; Shift+Delete | Split the command target; lift; ripple removal |
| Ctrl+C/X/V; Ctrl+Shift+V | Copy/cut/paste clips; insert pasted clips |
| Ctrl+Z; Ctrl+Y or Ctrl+Shift+Z | Undo; redo |
| Ctrl+Plus/Minus; Shift+Z; Z; Backspace | Zoom; fit timeline; fit selection/range; previous view |
| M; Ctrl+M; Ctrl+D; Ctrl+E | Active audio track mute; Media; Details; Export panel context |
| V; R | Selection tool; range tool |
| Menu or Shift+F10; Question mark | Context actions; shortcut reference |

The clip clipboard captures immutable source, timing, fade and link data from the selected clips.
Paste and insert-paste show the complete validated placement before changing the timeline.
Enter or Apply commits it in one undo entry, and Escape cancels it.
The view reveals the candidate when needed without changing the requested playhead or authored timeline end.
Insert-paste refuses a suffix shift that breaks the video grid or moves only part of a linked group.
Source access and current locks are checked again before commit.

Navigation and nudge keys may repeat; discrete commands ignore repeat. Fit is a one-shot view operation. Wheel zoom anchors the time under the pointer, and keyboard zoom anchors a visible playhead or the view center. Shift+wheel and the horizontal scrollbar move the visible time interval. Holding Space and dragging over the timeline pans horizontally. A pointer press consumes the reserved Space action even without movement, so releasing Space after panning never toggles playback. Focus changes, hiding or deactivating Edit, Escape and another key cancel the reservation. Releasing Space before the mouse stops panning and retains the grab until mouse release. Space pressed during an existing clip gesture does not convert that gesture. Pointer toolbar actions preserve the canvas focus. Buttons focused by keyboard retain ordinary Space activation, and text editors keep their shortcuts. Timeline focus is marked at the playhead without outlining the viewport. Zoom and scrolling keep clip surfaces bounded to the visible viewport, and hidden clip boundaries do not become trim handles. Focus and accessibility tests establish specific contracts, not blanket proof of every assistive-technology combination.

The top bar retains three noninteractive source-health indicators across every page: application/system audio together, microphone and webcam. Active included sources use success ink; intentionally muted/off or unconfigured sources remain dim/neutral. Recovering expected sources are amber; actual failures are coral. Recording lock is not a health condition. Accessible descriptions include individual APP/SYS states and any silence substituted during device recovery.

## 11. Diagnostics and fix actions

Diagnostics keeps four main live tiles: Frame pacing, Encoder, Storage and Audio sync. The recording verdict and ledger describe measured impact. Raw timing, presentation, GPU/memory and kernel evidence stays in grouped In-depth details; missing PresentMon is neutral. Compensated condition periods stay in In-depth/reports. Diagnostics is state-ordered, not a separate Simple/Expert mode. The shared Expert preference is edited in the recording profile inspector.

| State | Main contents |
|---|---|
| Idle | Readiness verdict, Encoder/Disk/Display/Audio tiles, issues, tips and reference rows |
| Recording/Paused | Verdict, six-stage pipeline, live metrics and session ledger, blockers/tips/reference |
| After stop | Readiness verdict plus Last session card and frozen findings |

Self-test, Hardware capabilities, Environment & configuration and Support bundle are collapsed references with summaries and their relevant actions. Probes run on relevant visibility/configuration and periodic refresh rather than needing a misleading permanent check button. Live metrics use their measured cadence and do not masquerade as raw per-frame analysis.

Audio resources retain distinct physical capture, logical source and track output rows. Opaque capture/clock IDs link shared resources without exposing endpoint paths, process IDs or window titles. Physical capture and source DSP counters are counted once per owner, independent of route count. Runtime rows show reader counts, clock quality, raw/residual drift and compensation, reinitialization, source gaps and reader overflow, source pre-mute RMS/peak, and track post-limiter RMS/peaks and current gain reduction.

Processing costs are cumulative elapsed milliseconds for capture/clock conversion, logical source processing, and track mixing/limiting. They are not CPU utilization or an estimate from route counts. Unmeasured values show n/a with a reason; measured silence remains distinct. The retained clip adapter can project exact job counters for engineering tests. The current product admits no ClipJobs and shows no active clip rows. A shared recording storage row shows the disk floor plus the current recording/remux allowance enforced by clip admission. It is shown once regardless of job count and is not a disk free-space measurement. Index commit lag and CPU utilization remain unavailable because the runtime does not publish those measurements.

Each diagnosis declares both severity (Pass/Notice/Blocker) and tier: Blocker, Measured problem, Optimization or Fact. Blockers prevent start. Measured problems remain represented and attention-colored. Optimization tips bundle quietly and cannot alone make readiness amber. Facts are neutral reference and do not count in the verdict.

Issue cards expose diagnostic ID, reason, measured value/budget and a typed fix. Evidence/log excerpts are secondary. Session ledger entries need distinct consecutive observations, retain active/quiet state and worst values, and remain until stop. Static configuration conditions do not become runtime incidents. Last session reports real frame loss, affected pacing slots, audio interruptions, peak residual drift and finalization status, plus actual ledger occurrences. Individual frame drops have no fabricated timestamp marks.

In-depth diagnostics is a session-only header switch, off each launch and viewable while recording. The opt-in is the request; each optional source starts only if this process may take it, and reports its own state: recording pipeline and GPU/memory measurements stay available in a standard process, a refused presentation trace says access denied or session conflict, and DPC/ISR states its separate elevated kernel-trace boundary. A missing optional source is neutral and never disables the others. Switching on in a standard process offers Restart as administrator but does not itself prompt. UAC decline is recoverable and does not persist consent. A successful elevated successor returns to Diagnostics with the requested session observation enabled.

Present/tearing/DPC traces are optional. Standard capture/cadence diagnosis remains useful without them. Present data describes the current attribution/recording and becomes unavailable when its process/trace ends. It must not carry a previous recording's totals into a new one. Driver attribution and legacy-FSE detection remain evidence-qualified.

Auto fixes are executable reversible changes after review/confirmation; scope/track changes always name consequences. Assisted fixes reveal the existing control/location. External fixes direct to an operation ExoSnap cannot do. Nothing applies silently. No new Windows administration interface is created through Diagnostics.

Reports/logs/support bundles are local. A per-recording report retains the ten newest files and uses unavailable rather than fabricated counters. Support bundle creation gathers scrubbed logs/reports and allowlisted facts into a user-chosen ZIP, never uploads it and never includes recordings/raw configuration/crash dumps. Known-shape scrubbing is not a guarantee about arbitrary personal prose; review before sharing.

Logs provides a virtualized structured list with Time, Level, Category and Message, filtering/search and current-view count. Copy means the filtered visible history; Export means the full current in-memory history. Disk rotation and report retention are separate. Startup measurements are recorded milestones, not a general continuously running profiler.

## 12. Settings model (Default / Expert)

Settings retains card-based global application preferences: App behaviour, Hotkeys, Appearance, Overlays, Storage, Updates and Support & diagnostics. Recording choices are edited in the Recording profile inspector. A Settings shortcut opens that inspector rather than providing another editable copy. Expert reveals raw or compatibility-sensitive mechanics within the recording cards; it never hides an entire section.

Global Settings adapts its card columns to the available width. The inspector uses one scrollable column and retains preset actions and the shared Expert preference.

Expert owns numeric rate control/CQ/bitrate, custom frame-rate entry, pacing, NVENC preset and advanced tuning, keyframe interval, color range/depth/chroma, specialized Opus/sample-rate/clock controls and relevant raw gain/limiter/float choices. Relevance can narrow these further: no H.264 10-bit row, no unsupported 4:4:4 row, and no lossy-codec bit-depth field. HDR handling is Default-visible on a relevant HDR display. Automatic splitting, microphone processing and ordinary audio controls are not hidden just for sounding technical.

Selective information hints explain a real trade-off. Multi-option popovers explain and identify the current option but do not become a second reduced picker; the actual control changes the setting. A locked control retains its value and real unavailable reason. Planned work is not presented as active capability.

## 13. Updates and crash reporting

### Updates

Checks default off. A manual check is explicit action; changing channel alone does not contact the feed. Stable excludes prereleases, Preview includes them. Switching channel invalidates its old answer and discards an in-flight answer for the previous channel. Normal startup is Unchecked, not a false Up to date. A startup check that falls into a recording waits until the session ends instead of reporting an error.

The Settings card states Unchecked (no status line), Checking, Up to date, Available, managed by an external distribution, Updater running, Restart pending or an actionable Error. A same-version verification run can additionally show Verification reinstall available. Checking does not move the Settings scroll position. Applying is blocked during recording/finalization and never silently restarts the app.

A signed offer pins one full target version. The app hands the separate updater its versioned document plus manifest/signature; the child re-verifies and installs that target or nothing. An app-handoff updater does not search for a newer release itself. Signed-manifest verification precedes parsing, and package SHA-256 is checked before use. Downgrades are refused. WinGet, Chocolatey, Scoop and unknown-managed installations retain update discovery and release notes but never self-apply. The card shows ownership even when up to date or a check fails. Known managers offer Copy command: `winget upgrade --id Codexo.ExoSnap --exact`, `choco upgrade exosnap` or `scoop update exosnap`. Unknown-managed installs instruct the user to use their original package manager without guessing a command. Direct installations retain Update. No package-manager command is executed by ExoSnap.

Portable apply stages verified files, retains a backup through verification and restores it on supported failure paths. A failed restoration is reported. MSI invokes Windows Installer with its one elevation boundary; an MSI post-install verification failure does not prove the previous version was restored, so unknown installation state is possible. Neither path promises every conceivable interruption can be repaired automatically.

Manual updater start is idle and requires Check, Download and Install confirmations. App-handoff mode continues the already-approved specific update instead. Cancel is accepted only where the worker observes it, notably download. Critical install/verify/launch steps refuse close. A canceled download is neutral and says nothing was installed. Mode-aware retries do not offer to repair an immutable invalid handoff by repeating it.

The updater uses a stable 520 × 680 logical-pixel window rendered with Qt Quick from the same controller state, a 56 px title band, fixed progress/state/action regions and readable elision for long versions or system messages. Its visible UI is Qt Quick; the update transaction, signature/hash verification, handoff, staged swap/MSI handling and exit contract are unchanged. The role label stays Updater; phase/reinstall/failure belongs to content. Installed-version emphasis reflects what is actually safe on disk. Retryable, hard-stop and installed-but-needs-manual-launch results remain distinct. The updater runs from a temporary staged copy of itself and the Quick runtime it needs, never from the installation it is about to replace.

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

The dynamic Visual C++ x64 runtime is required. No application artifact bundles it. ExoSnap Setup embeds the pinned Microsoft redistributable and installs it only when the machine's runtime is missing or older; the raw MSI refuses a direct install without it and names the prerequisite; WinGet and Chocolatey declare it as a dependency; the portable ZIP and Scoop entry expect it on the machine. Keep the portable runtime files together.

Windows installation is per-machine under `C:\Program Files\ExoSnap`, with the product marker at `HKLM\Software\ExoSnap` and an all-users Start Menu entry. The desktop shortcut is an unchecked Setup option and is absent for MSI and package-manager installs. Uninstalling preserves the current user's `%LOCALAPPDATA%\ExoSnap` unless the operator explicitly asks Setup to remove that local data; recordings and other output files are never removed by an uninstall.

Replay buffering, persistent edit projects, transitions beyond crossfade, embedded chapters, surround output, HLG, 4:2:2, 10-bit 4:4:4 and cross-vendor encoding are not current features. The development editor implements frame-accurate rendered cuts and video composition for supported SDR sources. Native hardware/UX and media-profile qualification for v0.11 remains required. See [known limitations](../KNOWN_LIMITATIONS.md) for narrower behavioral and verification boundaries, and [roadmap](roadmap.md) for explicitly future work.

ExoSnap is GPL-3.0-or-later. Bundled components retain their own notices and license terms in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

## Implementation references

The observable contracts are implemented through [Quick composition](../app/quick/ExoSnap/Quick/QuickApplication.cpp), [Settings](../app/quick/ExoSnap/Quick/SettingsAdapter.cpp), [Record transport](../app/quick/ExoSnap/Quick/RecordTransportDock.qml), [preset model/store](../app/settings/RecordingPresetStore.cpp), [recording coordinator](../app/services/RecordingCoordinator.cpp), [compatibility registry](../libs/capability/src/container_compat_registry.cpp), [hotkeys](../app/services/GlobalHotkeyService.cpp), [diagnostics](../app/diagnostics/RecommendationEngine.cpp), [updater](../apps/updater) and [privacy scrubber](../libs/crash_capture/include/crash_capture/crash_scrubber.h). Focused tests under [app/tests](../app/tests), [Quick tests](../app/quick/tests), [engine tests](../libs/engine/tests), [capability tests](../libs/capability/tests) and [update tests](../libs/update/tests) pin those rules. Test presence is not a substitute for running the appropriate checks on the target artifact.
