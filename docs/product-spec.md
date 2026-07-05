# ExoSnap Product Specification

**Status:** Living document, pre-1.0. This is the durable, tracked record of ExoSnap's
user-visible product behavior and the reasoning behind it. It describes *what the product does
and why* — not implementation internals. Where a behavior is genuinely undecided, it is marked
**Open** rather than invented.

Pre-1.0 note: settings, preset, and recording-history schemas are not frozen and may change
incompatibly before 1.0.0. When product decisions here disagree with older internal notes, this
document and `CLAUDE.md` are authoritative.

---

## 1. Product intent and principles

ExoSnap is a Windows-native screen, application, and region recorder with a native NVENC pipeline.
It records MKV first and delivers MP4 by remuxing on stop. It is built around a small set of
principles that shape every visible decision:

- **Diagnostics-first.** Readiness is a feature, not an afterthought. The product tells the user up
  front whether a recording will succeed, classifies problems while recording, and reports health
  afterward.
- **Reliability-first.** Recording, recovery, and honest status come before breadth. An existing
  NVIDIA user benefits from reliable capture and recovery before the audience is widened to other
  vendors.
- **Honesty over reassurance.** The UI never offers a combination it has not vetted, never labels an
  encoder mode as something it is not (for example, never calling NVENC "CRF"), and states file-size
  and capability limits plainly ("approximately N GB", not a byte-exact promise).
- **Calm, not alarmist.** Diagnostics defaults to a quiet, factual tone. Only real, measured problems
  are surfaced; each problem carries one primary fix; deeper detail sits behind an expert toggle;
  hard blockers are always visible.
- **Privacy by default.** No analytics, no telemetry, no account. The app makes no network
  connection unless the user opts in to a specific feature.
- **Engine stays UI-agnostic.** Track resolution, capability, and reconciliation live in the engine;
  the UI submits editable models and renders resolved results.

---

## 2. Navigation and information architecture

Top-level navigation is **six items**, in order:

**Record · Device · Settings · Diagnostics · Logs · About**

- **Record** — the operational view: capture target, readiness, live preview before recording, and
  the live runtime (technical) view while recording.
- **Device** — encoder adapter selection plus the per-GPU capability matrix. One card per DXGI
  adapter (iGPU/dGPU); a per-adapter matrix shows codec support and provenance for the selected
  adapter, including per-codec 8-bit 4:4:4 (YUV444) encode support probed on that specific GPU
  (H.264 / HEVC — AV1 is 4:2:0 only). Not-yet-wired backends (AMD/AMF, Intel/QSV, software
  x264/SVT-AV1) appear as honest greyed-out "planned" rows — never fabricated probes.
- **Settings** — unified recording configuration, hosting six embedded sections:
  **Video · Audio · Output · Webcam · Hotkeys · Advanced**. Advanced is expert-only and collapsible.
  Hotkeys is an embedded full-width card, not a separate nav item.
- **Diagnostics** — the live, changeable environment as readiness cards (disk, display, audio,
  elevation, blockers), plus a capability-matrix reference section.
- **Logs** — runtime events and per-session recording diagnostics.
- **About** — application identity, build metadata, and links.

**Edit / Output / Save** is a post-stop **overlay over the Record page**, not a nav item. After
recording stops, the surface opens over Record (dimmed backdrop) on the Review step and is stepped
forward in three linear phases — **Review → Edit → Output** — one at a time via the primary button,
with a top stepper that always highlights the current phase. **Back** steps back one phase at a time
(Output → Edit → Review); from Review, Back (or Escape / backdrop click, except while exporting)
closes the overlay and returns to Record. The Review step consumes the post-flight diagnostic report
produced during recording.

The default theme is **dark mode**.

---

## 3. Recording defaults and profiles

The built-in default profile is **MKV + AV1 + Opus + CFR 60 fps**.

| Setting | Default |
|---------|---------|
| Theme | Dark mode |
| Container | MKV |
| Video codec | AV1 (NVENC) |
| Audio codec | Opus |
| Frame rate | CFR 60 fps |
| Rate control | Constant quality (CQ), quality "High" |
| NVENC encoder preset | P4 (all codecs) |
| Frame pacing | Smooth (phase-correct) |
| Color range | Limited |
| Cursor capture | On |
| Countdown | 0 seconds (selectable 0/3/5/10) |
| Audio sources | `APP`, `SYS`, `MIC` — all enabled, each a separate resulting track |
| Webcam | Off |

Presets are stored in a human-readable TOML store and can be exported and imported for sharing. A
preset manage dialog supports rename, duplicate, delete, and set-default. Presets are validated and
sanitized before storage; invalid values are clamped rather than rejected silently.

---

## 4. Container / codec / audio matrix

The UI offers only **vetted** combinations — never a theoretically-muxable pairing without a tested
player/editor matrix. Invalid combinations are blocked before recording starts by a compatibility
registry that answers: allowed? recommended? experimental? fallback? warning?

| Container | Video | Audio (offered) |
|-----------|-------|-----------------|
| MKV | AV1, HEVC, H.264 | Opus, AAC, PCM, FLAC |
| MP4 | AV1, HEVC (`hvc1`), H.264 | AAC |
| WebM | AV1 | Opus |

Rules and notes:

- **MKV** is the flexible default container and the only home for lossless **PCM** and **FLAC**
  audio.
- **MP4** offers only **AAC** audio. Opus, PCM, and FLAC are not offered for MP4; an Opus + MP4
  selection is rejected before recording begins.
  - **PCM in MP4 is deferred (experimental, not user-selectable):** the bundled libavformat emits the
    `ipcm` (ISO/IEC 23003-5) sample entry, which many players and editors (Windows "Films & TV",
    QuickTime, several NLEs) do not play. MKV is PCM's home.
  - **FLAC in MP4** is not a 1.0 target.
  - MP4 is delivered by **remuxing the transient MKV to a progressive, faststart MP4** via
    stream-copy on stop — no re-encode. During the remux the UI distinguishes **"Saving…"** (remux in
    progress) from **"Saved"** (complete); the remux is cancellable and cancelling keeps the valid
    MKV. If the remux fails, the playable MKV is retained and the error surfaced. The MP4 carries
    **BT.709 color metadata**, and HEVC in MP4 uses the **`hvc1`** sample entry (parameter sets in
    `hvcC`) for Apple/QuickTime compatibility.
  - MP4 shows a calm informational note that it has lower crash resilience than MKV (the file is only
    finalized at stop).
- **WebM** offers only AV1 + Opus. It must never be paired with H.264 or HEVC. (VP9 in WebM is a
  possible later addition.)
- Exact codec availability further depends on the installed **NVIDIA GPU generation, driver version,
  the selected container, and the selected video/audio combination**.

**Reconciliation rule.** When the user switches container, the selected video and audio codecs are
reconciled to a valid combination for the new container (the engine computes the nearest valid
combination). Reconciliation is engine logic, surfaced to the UI — the UI does not duplicate it.

**HEVC / 10-bit maturity.** HEVC, `hvc1`, and 10-bit encoder paths are functional end-to-end but not
yet validated across all NVIDIA GPU generations under live recording. The product advises falling
back to H.264 or AV1 if issues appear.

---

## 5. Audio model

**Source order and defaults.** The default audio source order is **`APP`, `SYS`, `MIC`** — all three
enabled, each producing its own separate resulting track.

**Editable source rows → resolved tracks.** The UI presents editable source rows. The engine resolves
the rows into final tracks; the UI never duplicates track-resolution logic. Per-source **gain** and
**mute** controls are live and interactive on the **Record page**; the Settings → Audio panel shows
the rows as locked previews.

- Per-source **gain** ranges roughly **−60 to +24 dB** (default 0 dB), shown as an "X.X dB" value.
- Each row has a **mute** button (labelled **"M"**); a muted source contributes silence. Default is
  not muted.
- The **Mic row hides its gain slider** (mic level lives on the dedicated mic gain control); its mute
  button is always shown.

**`Merge with above`.** The exact per-row control label is **`Merge with above`** (do not rename).
Checking it folds a source into the track above it instead of producing a separate track, so users
can combine sources (for example, merging system and app audio into one track) without engine-side
UI logic. The set of relevant sources is context-aware — it adapts to the capture target.

**Mix bus and limiter.** Per-source gain and mute are applied in the mixer. A **brickwall limiter**
sits on the mixed bus and is **on by default** at a 0 dBFS ceiling, so summed sources can exceed full
scale without hard clipping.

**Microphone DSP chain.** The mic path has a four-stage chain applied in order:
**high-pass filter → noise gate → AGC → RNNoise** neural noise suppression. **Every stage is off by
default** and toggled individually — there is no master switch — and capture is byte-identical when
all stages are off.

**Channel / sample-format model.** Output **sample rate** (44.1 / 48 / 96 kHz), **channel count**
(mono or stereo), and **bit depth** for lossless codecs (PCM 16/24/32-bit; FLAC 16/24-bit) are
configurable. Capture itself stays at 48 kHz; the engine resamples/rematrixes **once** after the mix
bus. The default 48 kHz / stereo path is a byte-identical no-op. **Opus is locked to 48 kHz.** Bit
depth does not apply to lossy codecs (Opus/AAC). Stereo→mono uses an averaging (no-clip) downmix.

**Opus recording defaults.** Audio application profile, 20 ms frames, complexity 10 when CPU budget
allows, VBR/constrained VBR, per-track/channel bitrate. Restricted-lowdelay and 2.5/5 ms frames are
expert-only.

**FLAC compression level** (0–8, default 5) is configurable; every level is lossless (level only
trades encode CPU for file size).

Deferred: more than two channels (5.1/7.1), float PCM, non-vetted sample rates. At a non-default
sample rate, a small (~10 ms) audio tail may be dropped at stop.

---

## 6. Video model

**Rate control (canonical model).** "CRF" is x264/x265-specific and is never shown anywhere in the
product (UI, tooltip, or API). ExoSnap presents a canonical model mapped per encoder underneath:

- **Constant quality (CQ)** — the default (NVENC CQ/CQP under the hood).
- **Variable bitrate (VBR)**
- **Constant bitrate (CBR)**
- **Lossless** — hidden (not greyed) unless the active encoder+codec combination confirms support.

A bitrate control accompanies the bitrate-based modes. Switching encoders preserves the selected
canonical mode; only the internal mapping changes. Expert rate-control, bitrate, and frame-timing
controls sit behind the Expert toggle.

**Encoder preset.** An expert **NVENC encoder preset** control (in the Container & codecs expert
section) exposes presets **P1–P7** (P1 fastest, P7 best) uniformly for all codecs; it is never
capability-gated (only the recording lock disables it). The **default is P4 for all codecs**. It
takes effect from the next recording.

**Frame rate and pacing.** The default is **CFR 60 fps**. An expert **"Frame pacing"** control offers
**"Smooth (phase-correct)"** (default) and **"Newest (lowest latency)"**. Smooth selects frames by
present time (it does not blend), so uncapped VRR / high-refresh sources record to smooth,
judder-free 60 fps; it does not make 60 fps look like 144 Hz. Smooth is GPU-only and requires no
elevation, and applies to **monitor (DXGI duplication) capture only** — window/region (WGC) capture
always uses newest-at-tick. VFR output is unaffected. When VRR/CFR judder is measured while in
Newest, Diagnostics recommends switching to Smooth via a fix action.

**Bit depth.** 8-bit for all final codecs; **10-bit (P010)** is available for HEVC Main10 and AV1
where the GPU supports it (H.264 stays 8-bit only). 10-bit is **SDR-only** — higher precision, no HDR
transfer curve or wide gamut.

**Chroma.** **4:2:0** is the default and is universal (all codecs, 8- and 10-bit). **4:4:4** is an
Expert-mode option available for **H.264 and HEVC at 8-bit** (NVENC High 4:4:4 Predictive / HEVC
Range Extensions) on GPUs that report YUV444 encode support; it keeps full colour resolution (sharper
text/UI) at the cost of larger files. **4:4:4 is not available for AV1** (NVENC AV1 is 4:2:0 only),
**not available at 10-bit**, and not available with native HDR10. The Expert selector disables 4:4:4
with an explanatory hint whenever the current codec/bit-depth **or the active GPU** cannot carry it,
and an invalid stored selection is reconciled back to 4:2:0. **4:2:2 remains unavailable** (the NVENC generation has no
4:2:2 path). While a recording runs in **4:4:4**, the **live preview stays available** (it shares
the composited RGB frame with the preview before the AYUV conversion — see the live-preview note in
Section 7), and **frame snapshots stay available** as in 4:2:0: the CaptureFrame hotkey reads back
the packed AYUV encode surface and decodes it on the CPU with the exact inverse of the encoder's
RGB→AYUV conversion (same BT.709 matrix and Full/Limited range as the recording).

**Color range and metadata.** **BT.709 color metadata** is written to every MKV and MP4 output. The
**Y'CbCr color range** (Full or Limited) is selectable behind Expert mode and is valid for every
codec/container combination (never gated), with **Limited** as the default. Because some players
(notably VLC) ignore the range flag and always expand limited→full — making Full-range recordings
look crushed/dark there — Diagnostics surfaces a compatibility tip with a one-click Full→Limited fix
when Full is selected. Presets carrying an older Full default are auto-migrated to Limited; an
explicit Full is respected as a deliberate opt-in.

**HDR handling.** HDR-capable displays are **detected automatically**; detection is not a setting and
cannot be turned off. Once an HDR-capable display is detected, an **expert-only HDR handling control**
(Settings → Video, Container & codecs section) chooses the outcome:

- **Tone-map to SDR** (default) — the safe, universally compatible choice.
- **Record native HDR10** — keeps the original PQ / BT.2020 HDR10 signal.

Behavior:

- On a non-HDR display the choice has no visible effect either way.
- **Record native HDR10** is only selectable with a codec that can carry it (**AV1 or HEVC**). With
  H.264 the option is disabled and a calm inline note explains why ("Not available with H.264 —
  switch to AV1 or HEVC"). This is never shown as a warning/error state, and switching the codec
  updates availability immediately.
- If HDR10 recording is selected and the codec is later changed to H.264, the setting is **not**
  silently reset. The conflict is caught by the diagnostics readiness gate, which **blocks recording
  start** until it is resolved (switch codec, or choose Tone-map to SDR). The blocker carries a codec
  fix action ("Switch to AV1" / "Switch to HEVC", availability-aware). It fires only when HDR10 mode
  is selected, the codec cannot carry HDR10, and the display's HDR is currently on; H.264 + SDR
  tone-map is explicitly not a conflict.
- For native HDR10, the pipeline pins **limited range** and **10-bit**, writes HDR10 metadata **both
  at the container level** (MKV Colour / MasterMetadata, MP4 colr/mdcv on remux) **and in-band in the
  bitstream** — HEVC Mastering Display Colour Volume (SEI 137) and Content Light Level Info (SEI 144)
  messages, AV1 HDR MDCV / HDR CLL metadata OBUs, emitted on every keyframe so players that ignore
  container-level HDR metadata (notably some Apple players) still receive it. The on-screen monitoring
  preview is an SDR approximation of the HDR signal.
- SDR overlay sprites (webcam PiP, cursor) are placed at the captured display's Windows SDR-content
  brightness level (`DISPLAYCONFIG_SDR_WHITE_LEVEL`) so the PiP matches SDR windows on the same
  screen; 203 cd/m² is the fallback when the level cannot be read. The level is sampled once when
  the recording starts — moving the Windows SDR-brightness slider afterward does not retune an
  active recording.
- **HDR scope for 1.0:** HDR handling (both tone-map-to-SDR and native HDR10) applies to **monitor
  (duplication) capture** and to **window/game capture** (Windows Graphics Capture). When the window's
  hosting display is HDR-active and HDR handling is on, WGC negotiates a scRGB FP16 frame pool and
  feeds the same tone-map / native-HDR10 machinery as the monitor path (so the same H.264+HDR10
  blocker and expert control apply to a window on an HDR display). The hosting display is resolved
  once at recording start; moving the window to a different monitor mid-recording keeps the session's
  initial HDR decision. There is still no HLG or wide-gamut generalization beyond BT.2020.

---

## 7. Capture targets and webcam

Three capture targets:

- **Monitor / display** — captured via DXGI Output Duplication.
- **Application window** — window capture (Windows Graphics Capture path).
- **Screen region** — a rectangular region, with a refined region-selection overlay and a live
  cropped preview.

Cursor capture is a toggle (on by default). Single-frame capture (a "capture frame" action) is
available during recording via an on-screen dock control and a hotkey.

**Live preview (WYSIWYG during recording).** Before recording, the Record-page preview runs its own
lightweight capture of the selected target. **Once recording starts, the preview shows exactly the
frame the engine is encoding** — the composited, pre-encode source (cursor and webcam PiP already
baked in) is shared to the preview through a GPU texture, and the preview's own capture stops. There
is no second capture running alongside the recording, and the preview reflects what is actually being
recorded (so a black-screen or swap-chain problem is visible in the preview, not hidden by an
independent capture). During the pre-record countdown the preview holds its last live image until the
first recorded frame arrives, so there is no black flash. The one exception is **native HDR10**
recording: it has no SDR intermediate to share cheaply, so during a native-HDR10 recording the
preview keeps its own capture and shows the same SDR approximation used elsewhere for HDR monitoring
(see KNOWN_LIMITATIONS).

**Webcam PiP.** A webcam picture-in-picture overlay is **composited into the recording** (it is an
in-video element, not an on-screen-only overlay) and rendered WYSIWYG with a real mirror option and a
selectable overlay placement. Its opacity is adjustable (Settings → Webcam, 0–100%, default 100%) and
applied identically in the Record-page preview and the recorded output. It is off by default and
configured in Settings → Webcam and on the Record page. The webcam is the only feature that depends
on Windows Media Foundation: on Windows N/KN editions without the Media Feature Pack, the app still
launches and records normally, but the webcam UI is disabled with a notice referencing the Media
Feature Pack and a "Webcam (MF)" row appears in Diagnostics.

**Capture-exclusion.** ExoSnap's own on-screen overlays (recording status, diagnostics, countdown,
quick-control pill) are drawn with `WDA_EXCLUDEFROMCAPTURE` so they are visible on screen but not in
the recorded frame. They are also click-through. If capture exclusion cannot be guaranteed on a given
system, the overlay hides itself for the session rather than risk contaminating the recording.

**Anti-cheat posture: no injection.** ExoSnap does not inject into or hook other processes, and does
no memory access, to capture — it uses OS-level capture APIs (DXGI duplication, Windows Graphics
Capture) only. Overlays are never auto-disabled; the user gets a global opt-out for on-screen
overlays plus a one-time, non-blocking banner: "Anti-cheat detected — ExoSnap overlays do not inject;
disable overlays if required by the game." The optional PresentMon-based present/tearing observation
is an out-of-process ETW consumer, opt-in and elevation-gated, never a hard dependency; the app
degrades gracefully when not elevated.

**Known target-identity boundaries.** Display identity uses the GDI device name (e.g.
`\\.\DISPLAY1`), which can be reassigned on a topology change, so a saved Region/Display target may
point to a different physical monitor after a reboot or reconnect — re-select in that case. Hot-swap
of the capture device mid-recording is not supported: stop and restart after reconnecting.

---

## 8. Recording lifecycle

**Pre-flight readiness gate.** Before a recording starts, all blocker and notice checks run and are
surfaced green/amber/red with fixes. **Recording start is blocked while any diagnostic blocker
exists** (for example: no supported NVENC encoder detected, hard-stop disk threshold reached, or an
unresolved HDR10-vs-H.264 conflict). If no supported NVIDIA NVENC encoder is detected, recording is
blocked with a diagnostic message rather than silently falling back.

**Live monitoring.** While recording, low-cost instrumentation (aggregated off-thread, ~1–4 Hz, no
per-frame image analysis) tracks dropped/duplicated frames, A/V drift, and disk-fill ETA, and
classifies the pipeline as encoder-, capture-, or disk-bound. Root-cause correlation is surfaced (the
first showcase being VRR/refresh-rate vs CFR-capture judder). The six capture-pipeline cards show
live status (Healthy / Busy / Bottleneck) with a CPU/GPU tag and one secondary number. A live A/V
drift and output-size overlay is available. An on-screen diagnostics overlay exists but is **off by
default** (enabled in Advanced).

**Post-flight report card.** After each recording, a report card surfaces frame-drop %, peak A/V
drift, and overall pipeline health. When a recording had **real** frame drops (encoder backpressure,
not benign coalescing/CFR drops), a caution toast ("Frames dropped") appears alongside "Recording
saved", with a "View diagnostics" action. (The fuller post-flight integrity review is the content of
the Edit/Output/Save "Review" step.)

**Automatic split (time + size).** Two independent auto-split axes — **maximum duration** and
**maximum file size** — with "whichever comes first" behavior. Size is reported honestly as
"approximately N GB" and measured from committed container bytes (no file-size polling); split
boundaries stay keyframe-safe; counters reset per segment. Split is supported for MKV, WebM, and MP4.
For MP4, each completed segment is remuxed to progressive MP4 in the background while recording
continues; "Saved" is reported only once all segment remuxes finish. Manual split is independent of
automatic split.

**Low-disk guard.** A configurable soft **warning threshold** (default around 2 GB free) shows a
Diagnostics notice but still allows recording; a lower **hard-stop threshold** (default around
500 MB free) blocks recording at start and stops a running recording gracefully. For MP4 sessions the
effective hard-stop threshold is raised to account for the transient MKV and output MP4 coexisting
during remux (roughly 2× file size), and for split MP4 sessions it is raised further by the sum of
pending background remux jobs plus the live segment estimate.

**Filesystem checks.** ExoSnap detects the output volume's filesystem. A **FAT32** volume raises a
Diagnostics **notice** about the 4 GiB per-file limit; recording is **not** blocked and short clips
work correctly. NTFS, exFAT, and others pass silently. There is no automatic split at the 4 GiB
limit.

**Crash recovery (Finish / Continue / Delete).** A recovery manifest is written before each session.
If a session is interrupted, the next launch shows a recovery overlay offering, per interrupted
recording:

- **Finish** — saves the recording as originally configured (MKV repair/rename or MP4 remux, honoring
  the manifest snapshot; no user format choice).
- **Continue** — shown only for non-finalized (true-crash) artefacts. Arms the coordinator paused;
  Resume starts the next recording slice aligned with the per-segment machinery. A 1–2 s data loss at
  the crash boundary is accepted and visible as the slice boundary. At most one Continue session can
  be armed at a time; choosing Continue on a second candidate finalizes the first.
- **Delete** — an inline two-step confirm that permanently removes the artefact.
- **Decide later** — an explicit text button; entries stay in the manifest and the overlay re-shows
  at the next launch.

Continued sessions produce independent recording slices (no single-file concat). For MKV/WebM split
recordings, segments finalized before an interruption remain usable; an interrupted active segment
may not be recoverable.

**Edit / Output / Save (post-stop surface).** The three-phase Review → Edit → Output overlay lets the
user trim and export without leaving the app. The primary button steps forward one phase at a time
(Review → Edit → Output); Back steps back one phase at a time (Output → Edit → Review) before
finally closing the overlay from Review. The stepper always highlights the current phase.

Trim is **keyframe-accurate and lossless** (stream copy, no re-encode): a spin-box dialog snaps
entered cut points to the nearest keyframe and, within 50 ms, to the nearest marker. Markers placed
during or after a recording render as thin pins on the Edit timeline, positioned proportionally
between the recording's start and its total duration (a recording with unknown duration shows no
pins rather than guessing). Markers are edit-view only — they are never written as container
chapters, and chapter export (Split Chapter) remains out of scope for the MVP.

Output offers container **MKV / MP4** (both stream-copy, lossless) and a save mode of new file
(`<name>_edit.<ext>`, saved beside the source) or overwrite-original (atomic rename in place). The
save mode alone determines the destination — there is no separate destination-folder picker, since
the model leaves nothing else for the user to choose. A **keyframe interval** selector (Settings →
Advanced → Video: 2 s default / 1 s / 0.5 s) trades a little file size for finer trim accuracy. The
original recording is never mutated during export; not-yet-exported edits are discarded on dismiss.

**Current boundary:** trim, markers, and stream-copy export are implemented and reachable end to end.
Video preview playback inside the overlay and the Split Chapter action remain deferred to a later
release (0.11 per ADR 0022).

---

## 9. Presence and notifications

- **Tray icon** with idle / recording / paused states and an **unread notification badge**.
- **Toast notifications** (bottom-right, auto-dismiss): recording saved, low storage, unexpected
  stop, recovery available, frames-dropped caution. Toasts are not visually queued when several
  arrive at once — the most recent is shown. Windows Focus Assist / Do Not Disturb may suppress them.
- **Notification hub** — the canonical notification center is a **bell icon with a notification hub
  panel in the app header**; hub entries persist until dismissed. The **system-tray icon additionally
  shows an unread badge** for the same items. Toasts remain the transient fire-and-forget layer on
  top.
- **On-screen overlays** (all capture-excluded and click-through): a recording-status pill + elapsed
  timer (anchored top-right of the recorded monitor), a diagnostics readout overlay (bottom-right,
  **off by default**), a countdown overlay anchored to the recorded monitor's bottom-center, and an
  **opt-in** interactive quick-control pill (off by default; enabled in Advanced).
- **Close-to-tray** is opt-in.

---

## 10. Hotkeys

Global hotkeys are rebindable, with conflict detection and rollback on an invalid bind. They cover
recording start/stop, pause/resume, single-frame capture, and related actions. Pause/Resume default
to Unset. Hotkeys live as an embedded card inside Settings.

If a hotkey starts recording while the app window is visible, the Record view is activated; if the
window is minimized, it is not restored.

---

## 11. Diagnostics and fix actions

Diagnostics is a first-class engine, not a status readout. Its posture is **calm, not alarmist**: it
defaults quiet, reports only real/measured problems, gives one primary fix per problem, hides depth
behind an expert toggle, and always shows hard blockers.

**Underlying severity:** each check resolves to **Pass / Notice / Blocker**. A **Notice** is advisory
and never blocks recording; a **Blocker** prevents recording from starting.

**Calm presentation tiers:**

- **Tier 1 — Blocker.** Always shown; gates recording start.
- **Tier 2 — measured environment/config problem.** Earns its own card.
- **Tier 3 — optimisation tip** ("better, but it runs"). Bundled into a quiet tip chip; never turns
  the verdict amber.

The Diagnostics page has a **Simple (default) view** and an **Expert toggle** (a single global state
shared with Settings that reveals depth, not a second mode). Simple shows a verdict plus exactly four
wide readiness tiles — **Readiness · Encoder · Disk · Display** — then any Tier-1/Tier-2 card, then
one bundled Tier-3 tip chip. The static capability matrix lives on the **Device** page, not here.

**FixAction model.** Each detected issue can carry a typed, executable fix action with a safety
class, never applied silently:

- **Auto** — a safe, reversible, config-only change the app can apply after showing a change summary
  to preview/confirm (for example, reconcile to the nearest valid codec combination, or
  Full→Limited color range).
- **Assisted** — opens the right pre-focused Settings panel or folder, or copies a command (for
  example, "switch the game to borderless" for exclusive-fullscreen black capture).
- **External** — the app cannot perform it (for example, a driver install); it shows the exact
  required version and a deep link only.

Example fix labels include "Switch to a supported codec for this container" and, for the HDR
conflict, "Switch to AV1" / "Switch to HEVC".

**Check catalog.** Checks cover recurring environment/config conditions: old driver, low disk, FAT32,
unsupported codec on this GPU, audio-format mismatch, color-range (VLC) compatibility, refresh-rate
vs CFR judder, and the HDR10-vs-H.264 conflict. Diagnostics does not add runtime checks for
already-fixed internal bugs.

**Present / tearing / latency diagnostics.** An opt-in, elevation-gated provider (PresentMon, the
engine behind FrameView) enriches window/game-capture diagnosis and feeds judder correlation. The
same in-process ETW session powers a DPC/ISR-latency check that names the offending kernel driver
behind "smooth game, stuttery/crackling recording". The app does not run elevated by default; when
not elevated the toggle ("Present, tearing & latency diagnostics") is disabled with the hint "Restart
as Administrator to enable present/tearing diagnostics", and enabling it triggers a self-relaunch
offer (never during an active recording). The provider is never required, and the portable build
degrades gracefully.

---

## 12. Settings model (Default / Expert)

Settings uses a **Default / Expert split**: common controls are shown up front; expert controls (rate
control, bitrate, frame-timing, NVENC preset, frame pacing, audio DSP, color range, HDR handling,
keyframe interval, chroma subsampling) are hidden behind an **Expert** toggle. The Expert toggle is
a single global state shared with the Diagnostics page. Settings offers inline info hints (hover
popovers on info-i icons and the countdown chevron) and search. Roadmap-only controls may appear as
honest, disabled "planned" rows to communicate direction without enabling unimplemented behavior.

---

## 13. Updates and crash reporting

**Updates.**

- **Off by default for self-built binaries**; the official build's update check is opt-in and
  consent-gated. Both a manual "Check now" and a toggleable automatic update check exist.
- **Stable** and **Preview** channels.
- The client verifies a **signed manifest** (ed25519 via Monocypher + SHA-256) and **refuses
  downgrades**. No GitHub token is used by the client. No update is performed during recording or
  finalization, and the app never restarts silently.
- **UI home:** the update UI lives on the **Settings update card**, plus a **dedicated updater
  window** (per design canon `Updater.html`). The earlier About-overlay placement is superseded.
- **Target flow (designed):** the automatic update check finds a new version → an "update available"
  notification deep-links to the Settings update card → clicking **Update** opens the dedicated
  updater, a separate process that performs every step itself — download, verify download (signature
  and hash), wait for the app to close, install/swap the new files, verify the installation — and
  then restarts the app on the new version. The swap is **staged and reversible** (dual-swap): a
  failure before the swap leaves the current version intact and retryable; a verification failure
  after installing restores the previous version. Nothing is swapped until verification passes.
- **Current shipped behavior:** check + notify + manual download. The update card shows
  "Available · Update to vX.Y" and opens the releases page; the app does not yet download or install
  the update itself.
- **Planned:** after a completed update, a **"what's new" window/overlay** is shown on the first
  launch of the new version, with a checkbox to suppress it for future updates.

**Crash reporting.**

- **Opt-in and consent-gated**, local-first (out-of-process Crashpad). Nothing leaves the machine
  without an explicit choice on the next-launch crash dialog.
- **Next-launch only** — crashes are offered for reporting on the following launch.
- **Two-stage delivery:** Stage 0 is an assisted GitHub issue (always available); Stage 1 is an
  automated upload to **Sentry with EU data residency**, compiled in only for official builds — so
  self-built binaries never upload.
- Reports are privacy-scrubbed (see Privacy).

**Signing status.** Builds are **not yet code-signed** (portable ZIP and MSI); Windows SmartScreen
may warn on first launch. ExoSnap participates in the SignPath Foundation free code-signing program;
release binaries will be signed once the certificate is issued.

---

## 14. Privacy

- **No analytics, no telemetry, no account.** By default ExoSnap makes **no network connections**.
- Only two features can contact external services, each strictly opt-in and only when the user acts:
  the **update check** (public GitHub Releases API, no auth token, sends only the version string and
  the request's IP) and **crash reporting** (Sentry, EU data residency).
- **Crash-report allowlist (what is sent):** OS build, GPU model and driver version, ExoSnap version,
  active encoder backend, container and codec, and the crash stack/minidump — nothing else.
- **Never sent:** usernames, file paths (including output folder and recording filenames), machine
  name, breadcrumb logs. Recording content is never captured. No persistent device identifier is
  created.
- Settings, presets, recording history, recovery manifest, logs, and recordings are stored locally
  (under `%LOCALAPPDATA%\ExoSnap\`) and never transmitted; the user can delete any of them at any
  time.

---

## 15. Platform support and known boundaries

- **Windows 10/11 x64 only** (Windows 11 primary, Windows 10 best-effort). No ARM64, Linux, or macOS.
- **NVIDIA NVENC only** for video encoding (RTX 20-series or newer recommended, current driver). AMD
  AMF, Intel QSV/oneVPL, and software (CPU) encoding are **not** available and are not implied.
- Requires the **Microsoft Visual C++ 2022 x64 Redistributable** (bundled as a declared dependency by
  the WinGet package; not bundled by MSI, portable ZIP, Chocolatey, or Scoop).
- Distributed as portable ZIP and MSI (both unsigned for now).
- Not present in current builds: Replay Buffer; chapter export from the Edit/Output/Save surface
  (Quick Trim and markers are implemented and reachable; container chapter export is deliberately
  out of scope for the MVP); video preview playback inside the Edit/Output/Save overlay; HDR beyond
  BT.2020 (HDR handling now covers both monitor and WGC window/game capture via an FP16 frame pool;
  no HLG/wide-gamut is the confirmed 1.0 scope); 4:2:2 chroma and 10-bit 4:4:4
  (8-bit 4:4:4 for H.264/HEVC is implemented as an Expert option); multi-vendor hardware encoding; the
  in-place dual-swap updater (designed, not shipped); immediate
  in-session crash reporter; the fullscreen/borderless/exclusive game-capture matrix.

**Licensing.** ExoSnap is GPL-3.0-or-later and bundles FFmpeg as LGPL-2.1-or-later shared libraries
(dynamic linking).

---

## Resolved-decision notes

- **Default audio state.** The canonical default is all three sources (APP/SYS/MIC) enabled as
  separate tracks (per `CLAUDE.md` and README). An older internal preset note described a
  System-only default; the enabled-all default is authoritative.
