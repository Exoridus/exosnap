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

Every destination is directly reachable in the title band, in this order:

**Record · Settings · Diagnostics · Logs · About**

There is no overflow menu and no secondary navigation tier. Five words fit the band at the
**860 × 700** minimum window, so hiding three of them behind a glyph bought nothing and cost a click
plus a menu on the way to Diagnostics — the page a user opens precisely when something is already
wrong. Below the regular width class the tabs give up horizontal padding (12 px → 8 px) and the gap
after the wordmark narrows; the labels are never shortened, elided, or set smaller, and the window
buttons never give up room (§ title bar below).

The dividing rule for content is:

- **Settings** owns what the user tells ExoSnap to do.
- **Diagnostics** owns what ExoSnap observes about the machine and the runtime.

A read-only hardware inspector therefore does not live in Settings, and an editable product
setting does not live in Diagnostics.

- **Record** — the operational view: capture target, readiness, live preview before recording, and
  the live runtime (technical) view while recording.
- **Settings** — unified recording configuration, hosting embedded sections: **Container & codecs ·
  Quality & timing · Audio · Output · Webcam · Overlays · Notifications & presence · Hotkeys · Updates ·
  Appearance · Developer**. There is no separate Advanced section — a global **Expert** toggle
  (shared with Diagnostics) reveals additional rows in place within a section rather than hiding or
  revealing a whole card; every section, including Developer, is visible in both modes (§12).
  Hotkeys is an embedded full-width card, not a separate nav item.
- **Diagnostics** — the live, changeable environment as readiness cards (disk, display, audio,
  elevation, blockers), plus a **Hardware capabilities** section: one card per detected DXGI
  adapter (iGPU/dGPU), with the per-adapter capability matrix for whichever card is being
  inspected — codec support and its provenance, per-codec 8-bit 4:4:4 (YUV444) encode support
  probed on that specific GPU (H.264 / HEVC — AV1 is 4:2:0 only), and per-codec NVENC
  advanced-encode facts: B-frames (max count), Lookahead and Temporal AQ. All of it is
  informational; no Expert control reads these facts. Selecting a card is **inspection only** —
  the encode device is not a user choice (see §2.1). The section is collapsed by default and
  expanded in Expert mode; expanding it is what starts the adapter scan, so a cold DXGI
  enumeration and NVENC probe are never paid for unasked.
- **Logs** — runtime events and per-session recording diagnostics, a **Startup** latency table,
  and a **Create support bundle** action.
- **About** — application identity, build metadata, and links. The metadata table permanently
  shows **Version** (the full release version, e.g. `0.9.0-rc4`; developer builds honestly report
  `0.9.0-dev`), **Commit** (shortened, linking to the full SHA on GitHub), **Built** (the
  deterministic source-date timestamp, `YYYY-MM-DD HH:MM UTC`), **Install** (Portable / MSI /
  Scoop) and **Channel** (Stable / Preview). Deviations from an official release build appear only
  when true, as conditional notices: *Unofficial build*, *Debug build*, *Dirty source tree*.
  **Copy details** copies a full support block (version, tag, full commit SHA, build time, CI build
  ID, architecture, configuration, official flag, install mode, channel, executable path and its
  SHA-256 — the hash is computed lazily off the UI thread on first click).

**Edit / Output / Save** is a post-stop **overlay over the Record page**, not a nav item. After
recording stops, the surface opens over Record as **one view**: player, trim timeline, a right
rail with the details and export cards, and the post-flight report as an icon at the right end of
its header. The overlay spans the client area below the real title bar, so the window keeps its
title band and can be moved, minimized and closed for the whole edit session. Its backdrop is
opaque, so the Record page does not show through. **Back** (or Escape / backdrop click, except
while exporting) closes the overlay and returns to Record; when trim points or markers are set it
asks before discarding them.

**An open edit session is state of the Record destination, not a modality of the application.**

- The five top-level destinations stay **available** for the whole edit session — the tabs and
  `Ctrl+1`…`Ctrl+5` alike.
- Navigating to Settings, Diagnostics, Logs or About **does not close the session** and asks
  nothing. There is no unsaved-edits prompt on the navigation path; navigation is unconditional
  everywhere in the product.
- The workspace is **left, not ended**: it is visible on Record and only on Record. Nothing is
  unloaded, so no page is ever swapped underneath a surface that still covers it.
- Returning to Record shows the **same session** — the same clip, trim, markers, playhead,
  timeline strip and export state, down to where the details rail was scrolled to.
- Leaving Record **pauses playback** and keeps the position. Video and audio out of a surface the
  user is not looking at is both surprising on another page and decoder work nobody asked for.
  Coming back leaves it paused where it was; starting it again is the user's own action.
- A **running export keeps running** across a page change. It belongs to the session, not to the
  page, and returning to Record shows its current state. Close / Discard / Exit keep their own
  protections; navigation is not one of them.
- The session still ends only through **Back / Escape**, which still asks before discarding trim
  points or markers that were set.
- **Blocking surfaces** (recovery, crash report, recording error) do block navigation — they are
  modal about a question the user has not answered yet. An edit session is not that.
- The window controls are unaffected: the band remains draggable and Minimize/Maximize/Close keep
  working.

Every navigation intent answers to this one contract — a tab, a keyboard shortcut, a notification
action, a jump from the Diagnostics page. There is no second path with different rules.

> **Corrected 2026-08-16.** This paragraph previously stated the opposite — that the five tabs are
> *disabled* while an edit session is open — on the premise that the shipped code had always
> disabled them. That premise was false: the Widgets shell that shipped until the Qt Quick cutover
> let the tabs navigate for the whole edit session, and the lock arrived as an unnoticed porting
> regression. See the QCR-001 entry in the cutover backlog.

### 2.1 The encode device is not a user choice

ExoSnap does not offer an encoder-device or encoder-backend selector, and the UI never implies one.

The reason is the pipeline, not the design. The capture path creates one D3D11 device — matched to
the adapter that owns the target monitor for DXGI Output Duplication, or the default adapter for
window capture — and NVENC opens **on that same device**. There is no second, independently chosen
encode device to point somewhere else, and no adapter preference in the recorder configuration.
Presenting a picker would therefore either do nothing or require a cross-adapter transfer path the
pipeline does not have.

Consequences the UI must respect:

- Diagnostics states which adapter is *actually* carrying the encode, and every other detected
  adapter reads as **not encoding** — a fact about now, never a promise about a backend.
- Unshipped encoder backends (AMD/AMF, Intel Quick Sync, software x264/SVT-AV1) do **not** appear
  in production UI at all. Roadmap belongs in the repository roadmap, not in a product surface
  where it reads as an available capability.
- If explicit encode-device selection is ever shipped, it needs a persistent hardware identity
  first: the DXGI adapter LUID identifies an adapter only within the current desktop session and
  must not be persisted across reboots.

### 2.2 Appearance and accent

The product has exactly **two base appearances** — **Dark** (the default) and **Light** — and a
small curated **accent** palette chosen independently of them. The default is **Dark + Aqua**, the
Studio-Mint identity colour.

The two axes are separate because coupling them served nobody: the four complete themes this
replaces (`dark-default`, `dark-indigo`, `light-paper`, `light-slate`) each pinned one hue to one
set of neutrals, so choosing indigo also meant accepting a different background, and two light
themes existed mainly because neither could carry the other's accent.

| Accent | Intent |
|--------|--------|
| **Aqua** (default) | Studio mint — the ExoSnap identity |
| Sky | Petrol blue — cooler and quieter |
| Violet | Periwinkle — more contrast against the neutrals |
| Magenta | Warm pink — the most assertive of the four |

The list is deliberately small, and deliberately all cool. **Coral, amber and green are the
product's semantic colours** — recording/error, caution, ready/success — and an accent sharing one
of those hues would make an ordinary selection read as a state. **Semantic meaning wins over palette
breadth**: no state colour is ever derived from the accent, and switching accent never changes what
recording, caution or ready look like. There is no free colour picker, no user-defined surface
colour, and no separate primary/background/surface customisation.

The accent drives selection: the active nav underline, the primary action, active toggles, focus
rings and selected-card tint. Content drawn *on* a filled accent surface uses a curated contrasting
ink per accent and appearance, never a derived one.

**Light** is one base, rebuilt rather than inherited. It distinguishes four surface rungs —
application background, primary surface, raised control surface, and hover/selected — none of which
is pure white, plus restrained borders. Both light themes it replaces set the raised-control surface
*and* the hover surface to pure white, which collapsed two rungs into one; that is why the light UI
read as flat, and why a hovered control looked identical to a resting one. The page is a light cool
neutral so a near-white control has something to sit on, and hover steps back *down* towards the
page — there is no headroom above white to step up into.

An installation that stored one of the four old theme ids is migrated to the closest pair on first
launch and keeps the colour it had:

| Stored theme | Appearance | Accent |
|--------------|-----------|--------|
| `dark-default` | Dark | Aqua |
| `dark-indigo` | Dark | Violet |
| `light-paper` | Light | **Sky** |
| `light-slate` | Light | Violet |

Mapped by the accent hue the user actually saw, not by position in the old list — which is why
`light-paper` lands on Sky rather than on the default Aqua: its accent was petrol blue, exactly
Sky's light value. An unreadable or absent preference resolves to **Dark + Aqua**; it never
produces a blank or unstyled window.

Every shipped appearance/accent pair is held to contrast thresholds by semantic role, not to one
blanket ratio: **4.5:1** for text and for the ink on a filled accent or error control (WCAG 1.4.3 —
the product's largest button label is 16 px DemiBold, which is not "large text", so nothing gets a
discount), and **3:1** for the indicators that identify a control's state — the selected nav
underline, an active control's ring, the keyboard focus ring, and the recording / caution / ready
colours (WCAG 1.4.11). An **unavailable** control is exempt from both criteria, but the product
promises it stays visible rather than disappearing (§8), so it is held to the same 3:1 anyway. A
resting hairline is not: it separates surfaces, it is not what identifies a control.

Settings → **Appearance** exposes the two axes as two rows: Appearance as a segmented Dark/Light
control (two mutually exclusive values do not deserve a list that must be opened to see the other
one), and Accent as a row of round swatches. Each swatch is drawn in the value the accent will
actually take in the **current** appearance, so a light-mode swatch never shows its dark-mode
colour. Selection is marked by a ring in the appearance's own text colour rather than in the accent
— a swatch outlined in its own colour is invisible as a selection — and the ring stands **clear of
the dot**, so the mark is a shape rather than a slightly heavier outline. Resting swatches carry no
ring at all: the row has to answer "which one is active" at a glance, without the user clicking one
to find out. Every swatch is keyboard reachable and carries the accent's name as its accessible
name.

---

The frameless main window carries a **subtle native 1px border** in the active appearance's line
color (it follows Windows 11's rounded window corners and updates on an appearance switch). On
Windows versions without per-window border colors the default system frame is kept — never an error.

The window has exactly **one** title bar: ExoSnap's own 40 px band. Windows reserves no non-client
area for it, so no native caption is drawn above the product's. The band carries, left to right: the
brand mark and the **exosnap** wordmark (the second half in the accent colour), the five nav
destinations, the drag handle, the permanent engine-state pill, the
notification bell, and the three window buttons. The **selected destination is marked by an accent
underline**, not by an enclosing box — words in a 40 px band shared with the window buttons cannot each carry a frame without the
band reading as a toolbar. The keyboard focus ring is drawn only while a keyboard user has focus.
The window buttons are the last thing that may be given up when the band runs out of room: at the
860 px minimum window the nav tabs compress before Close does.

The window's **minimum size is 860 × 700**. A first launch — one with no persisted geometry — opens
at a **preferred 1280 × 720**, centred on the primary screen's work area and clamped to it, so a
small or heavily scaled display never opens the window under the taskbar or off-screen. This is a
preferred size, not a forced one: any valid persisted rect wins over it outright, and the window is
freely resizable, maximizable and snappable afterwards.

The window is **already at its final position, size and maximized state on the first frame it
shows**. It does not appear at one rect and move to another, and a window that was maximized when it
was last closed comes back maximized without showing its normal size first.

Every nav destination that has a page title states it the same way: the destination's name on the
page-title rung, on the same axis as its own content, with that page's controls (Expert toggle,
Rescan, filters) on the right of the same row. **Record** is the one destination with no page title
— its Preview Toolbar names the capture target instead, because the preview is that page's subject —
and **About** is the one with no header at all, being a single centred identity card.

**In-window modal surfaces** — the crash-report consent surface, the recovery prompt and the
recording-error surface — share one shape. Their **scrim covers the whole shell including the title
band**, because a window that still looks operable behind a modal is lying about what a click will
do; the **card itself stays inside the usable content region**, below that band, so a modal never
sits on the brand, the navigation or the window buttons. The card is **not drawn as a window**: it
has no title bar, no second wordmark and no imitation chrome of its own — the surface names itself
with a single eyebrow above its heading, because the real title bar is 40 px above it. Inside the
card, chrome, heading and actions are fixed and only the material being read scrolls; anything the
user is being asked to **decide** (a consent tick, a "remember this choice") stays visible with the
actions rather than scrolling away with the material.

**At most one of the three is on screen at any moment, and the one that is up keeps precedence.** A
surface asked for while another is showing is **queued**, not dropped and not allowed to supersede:
a crash prompt, a recovery offer or a failure report that vanishes mid-read takes its question with
it, and none of them can be got back without restarting. Queued surfaces come up one at a time, in
the order they were asked for, and each is shown once. Consequently a **recording cannot be started
while any of them is up** — a session whose transport is behind a scrim cannot be controlled, and
starting one would silently invalidate the offer an open recovery surface is still making. Stopping,
pausing and resuming are never blocked: a running recording the user cannot stop is the worse state.

---

## 3. Recording defaults and profiles

The built-in default profile is **MKV + AV1 + Opus + CFR 60 fps**.

| Setting | Default |
|---------|---------|
| Appearance | Dark |
| Accent | Aqua |
| Container | MKV |
| Video codec | AV1 (NVENC) |
| Audio codec | Opus |
| Frame rate | CFR 60 fps (Default list: 15/30/60/120 fps, 120 enabled only when a display can feed it; Expert mode swaps the list for a free-entry field capped at the fastest monitor's refresh rate; an off-list rate gets its own "`<n>` fps (Custom)" entry — see §6) |
| Rate control | Constant quality (CQ), quality "High" |
| NVENC encoder preset | P4 (all codecs) |
| Frame pacing | Phase-correct |
| Color range | Limited |
| Cursor capture | On |
| Countdown | 0 seconds (selectable 0/3/5/10) |
| Audio source rows | Order is `APP`, `SYS`, `MIC`. The Settings **Audio** card always lists all three rows: the `APP` row is a persisted setting that recedes (inactive meter, explanatory line "Takes effect while a specific application window is the capture target.") whenever a window is not the capture target, and it carries no **Merge with above** control because it is the first row. Which sources enter the recording plan still depends on the capture target — an `APP` source exists only while a specific application window is being captured. For screen capture the shipped default is `SYS` enabled and `MIC` present but off. |
| Resulting tracks | Each enabled source is a separate resulting track unless merged with the row above |
| Webcam | Off |

Four read-only built-in presets ship with the app and always appear first in the preset list. They
cannot be renamed, overwritten, or deleted; **Save as new…** derives an editable user preset from any
of them.

| Preset | Container | Codecs | CQ | NVENC preset | Intent |
|--------|-----------|--------|----|--------------|--------|
| Default | MKV | AV1 + Opus | 19 | P4 | balanced |
| Quality | MKV | AV1 + Opus | 16 | P6 | maximum sharpness; costs disk and GPU |
| Efficiency | MKV | AV1 + Opus | 30 | P6 | small files at usable quality |
| Compatibility | MP4 | H.264 + AAC | 19 | P4 | editing, upload, GPUs without AV1 encode |

The **live configuration is the source of truth**. It is persisted silently and continuously, so the
app restarts into exactly the state it was closed in. A preset is a named snapshot the live
configuration is compared against: when the two differ, the selector shows `Name (changed)` as a calm
hint. There is no Save button, no unsaved-changes warning, and no discard dialog. A write failure
(disk full, file locked, …) is not silent either — the change may be lost, so a notification says so.

Capture identity, video bit depth, and HDR mode are **environment facts**, not preset content.
Presets neither store nor override them, and a difference in them never counts as a change. Switching
presets therefore leaves the capture target alone. The existing H.264 8-bit clamp is the one
sanctioned exception.

Settings hosts the preset dropdown directly — there is no separate preset manager surface. Built-in
presets carry a small **Built-in** badge inside their dropdown option row, so the marker never sits
beside the dropdown and shifts the toolbar. The toolbar carries a single visible action button,
**Save as new…**, which appears while the preset is `(changed)`. Every other action lives in a `…`
overflow menu next to the dropdown: **Rename…** (disabled for a built-in), **Reset** (enabled only
while `(changed)`), **Delete** (enabled whenever a user preset is selected, never for a built-in),
**Export…**, and **Import…**. **Delete** is the one destructive action in that menu and it asks
first: a confirmation dialog naming the preset, defaulting to Cancel, so a single click never
destroys a saved configuration. It is deliberately a confirmation rather than an Undo — a deleted
preset also moves the selection and re-applies the configuration that takes its place, and restoring
all of that after the fact would mean a persisted history the product does not otherwise keep.
The Output page carries the same row. Switching presets applies immediately and
records a notification-hub entry offering **Undo**, which restores both the previous live
configuration and the previous selection. No toast appears — the combo box that performed the switch
already offers the way back.

Preset names are unique case-insensitively (leading and trailing whitespace trimmed); built-in names
are reserved. The naming dialogs reject a collision and let the user correct it; on import a
collision is resolved with a numeric suffix ("Name (2)", "Name (3)", …).

Presets are stored in a human-readable TOML store and can be exported and imported for sharing.
Values are validated and sanitized before storage; invalid values are clamped rather than rejected
silently, and a damaged store is repaired entry by entry — surviving entries are kept — instead of
being reset wholesale. A repair that actually discards something raises a notification; a mere schema
upgrade does not.

**An application-settings file that cannot be read is never overwritten by accident.** The
application settings (appearance, hotkeys, overlays, update channel, window placement, …) live in a
separate `settings.ini`. Three load outcomes are distinguished: no file yet (a first run — the
defaults are legitimate and are saved normally), a file read successfully, and **a file that exists
but could not be read** (corrupt, locked, unreadable). In the third case the session runs on
built-in defaults and says so once in the notification hub, and ExoSnap refuses every write it
performs on its own behalf — window placement, startup reconciliation, one-time flags — because none
of those is the user asking to replace their configuration. The first setting the user *does* change
supersedes the unreadable file: it is moved aside to `settings.ini.corrupt` and a fresh file is
written, so the old contents are still recoverable by hand. A settings write that fails is reported
like any other failed save; it is never silent.

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

**Source order and defaults.** The audio source row order is **`APP`, `SYS`, `MIC`**. The Settings
**Audio** card always lists all three rows: the `APP` row is a persisted setting, always present and
configurable, not conditional on the capture target. While no specific application window is the
capture target it recedes (inactive meter, explanatory line "Takes effect while a specific
application window is the capture target.") instead of disappearing, and — being the first listed
row — it carries no **Merge with above** control, since there is no row above it to fold into.
Whether an `APP` source actually contributes audio to the recording still depends on the capture
target: it contributes only while a specific application window is being captured. Defaults are
**context-aware** (see §3 and "Resolved-decision notes" below): the `APP` row defaults enabled; for
screen capture the shipped default is `SYS` enabled and `MIC` present but off. Each enabled source
produces its own separate resulting track.

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
all stages are off, **unless audio clock slaving has engaged** (> 15 ms of measured device-clock
drift); disable *Audio clock slaving* (expert) for bit-exact capture.

**Channel / sample-format model.** Output **sample rate** (44.1 / 48 / 96 kHz), **channel count**
(mono or stereo), and **bit depth** for lossless codecs (PCM 16/24/32-bit int or 32-bit float; FLAC
16/24-bit) are configurable. Capture itself stays at 48 kHz; the engine resamples/rematrixes **once**
after the mix bus. The default 48 kHz / stereo path is a byte-identical no-op **until audio clock
slaving engages** (see A/V drift below), after which even the default path is resampled by a
sub-audible ppm amount; the *Audio clock slaving* expert toggle restores bit-exact capture. **Opus
is locked to 48 kHz.** Bit depth does not apply to lossy codecs (Opus/AAC). Stereo→mono uses an averaging
(no-clip) downmix. **32-bit float PCM** is a raw passthrough of the mix bus's native format (no
conversion, no clipping headroom needed) and is PCM-only — FLAC has no float mode.

**Opus recording defaults.** Audio application profile, 20 ms frames, complexity 10 when CPU budget
allows, VBR/constrained VBR, per-track/channel bitrate. Restricted-lowdelay and 2.5/5 ms frames are
expert-only.

**FLAC compression level** (0–8, default 5) is configurable; every level is lossless (level only
trades encode CPU for file size).

Deferred: more than two channels (5.1/7.1), non-vetted sample rates. When resampling is active, the
resampler's filter-delay tail is **fully drained** into the encoder before end of stream on a clean
stop — no audio tail is expected to be lost. The session report records drained/undrained resampler
frames per track; `undrained_frames > 0` is a diagnostic error condition (captured audio that never
reached the file), not expected behavior. A failed or timed-out session does not reach the drain and
reports the counters as unavailable rather than claiming a clean drain.

---

## 6. Video model

**Rate control (canonical model).** "CRF" is x264/x265-specific and is never shown anywhere in the
product (UI, tooltip, or API). ExoSnap presents a canonical model mapped per encoder underneath:

- **Constant quality (CQ)** — the default (NVENC CQ/CQP under the hood).
- **Variable bitrate (VBR)**
- **Constant bitrate (CBR)**
- **Lossless** — hidden (not grayed) unless the active encoder+codec combination confirms support.

A bitrate control accompanies the bitrate-based modes. Switching encoders preserves the selected
canonical mode; only the internal mapping changes. Expert rate-control, bitrate, and frame-timing
controls sit behind the Expert toggle.

**Quality ladder.** In Default mode, quality is chosen from a five-tier, CQ-first-labelled ladder:

| Tier | Label | CQ |
|------|-------|----|
| 1 | Draft | 35 |
| 2 | Efficient | 30 |
| 3 | Balanced | 24 |
| 4 | High (default) | 19 |
| 5 | Ultra | 16 |

The top tier deliberately avoids "Best" (vague) and "Quality" (collides with the built-in Quality
preset's name). The built-in **Quality** preset's CQ 16 now lands exactly on the Ultra tier instead
of only approximating it. Expert mode replaces the ladder with the explicit **Rate control**
selector (CQ/VBR/CBR) plus a **CQ** spinbox (1–51, no suffix) or a **bitrate** spinbox, depending on
the selected mode.

**Encoder preset.** An expert **NVENC encoder preset** control (in the Container & codecs expert
section) exposes presets **P1–P7** (P1 fastest, P7 best) uniformly for all codecs; it is never
capability-gated (only the recording lock disables it). The **default is P4 for all codecs**. It
takes effect from the next recording.

**Frame rate and pacing.** The default is **CFR 60 fps**. The Default frame-rate control is a fixed
list — **15 / 30 / 60 / 120 fps** (the older 24 fps cinema and 25 fps PAL entries are dropped).
**120 fps is selectable only when an attached display can actually feed it**: capture never produces
more frames than the screen refreshes, so on a slower panel the entry stays visible but disabled,
with a hint naming the fastest attached display's actual rate. Disabled rather than hidden, because
a missing entry reads as "the product cannot do this at all" instead of "this hardware cannot". The
same rule guards the model: a listed rate above the ceiling never reaches it, since the disabled
item state alone only stops interactive selection. Expert
mode replaces the list with a **free-entry fps field**, not capability-checked at entry. Its maximum
is **the highest refresh rate of any attached monitor, rounded to whole fps, but never below 60**; the
minimum is 1. Capture never delivers more frames than the compositor/monitor rate, so a CFR target
above it is pure frame duplication. Rounding is to the *nearest* whole fps on purpose: recording 144
CFR from a 143.96 Hz source duplicates roughly one frame every 25 s and never drifts, whereas rounding
down to 143 would drop about one frame per second and show as micro-judder. The **60 floor** applies
both when no refresh rate can be read at all (headless host, remote session, a driver reporting 0 Hz)
and when every attached display genuinely reports something slower — a 30 Hz-only setup still gets a
maximum of 60, because the shipped default profile is CFR 60 fps and must stay expressible everywhere.

The ceiling is re-evaluated whenever a display is attached, removed, or changes its refresh rate; a
configured rate above the new ceiling is then **clamped down** in both the model and the UI, and the
clamped value is published like any other change. A user edit is likewise always accepted into the
enforced range. Clamping happens **only** on those two occasions: a rate that arrives from outside —
persisted settings or a loaded preset carrying a rate from a faster display — stays in effect and is
**displayed truthfully** (the Expert field widens its maximum far enough to show it, rather than
silently displaying the ceiling). It is brought into range at the next display change or the next
manual edit, whichever comes first.

When
the configured rate is not one of **15 / 30 / 60 fps** — after leaving Expert mode, or after loading a
preset that carries such a rate — the Default combo grows an extra entry labelled
**"`<n>` fps (Custom)"** carrying the real value, placed in numeric order among the fixed entries and
selected. The combo therefore never claims a frame rate the recorder is not using. The custom entry
disappears as soon as the configured rate is back on a listed value, whether because the user picked a
listed entry or because a preset set one; selecting the custom entry itself changes nothing. The
current-format footer always shows the true configured
value. An expert **"Frame pacing"** control offers
**"Phase-correct"** (default) and **"Lowest latency"**. Phase-correct selects frames by
present time (it does not blend), so uncapped VRR / high-refresh sources record to smooth,
judder-free 60 fps; it does not make 60 fps look like 144 Hz. It is GPU-only and requires no
elevation, and applies to **monitor (DXGI duplication) capture only** — window/region (WGC) capture
always uses newest-at-tick. VFR output is unaffected. When VRR/CFR judder is measured while in
Lowest latency, Diagnostics recommends switching to Phase-correct via a fix action.

The CFR timeline stays true to the wall clock. If the encoder cannot keep up in real time for a
sustained stretch, the recorder **skips** the output frames it could never have encoded in time —
counting them as **real** frame drops (part of the same real-drop count a caution toast reports)
rather than letting the video media time fall behind the audio and silently compress the recording.
The result is an honest, correctly-timed file with a visible drop count, not one that plays back out
of sync.

A **VFR** recording's timeline likewise never starts before the recording did: on monitor capture,
the first frame can carry the display's last real present timestamp from before Record was pressed
(a desktop that sat static reports its last repaint), and that stale origin is clamped to the
session start. The file's duration therefore matches the real recording time — a static source
before recording never inflates the lead-in — and the video timeline shares its origin with the
published A/V epoch.

**Bit depth.** 8-bit for all final codecs; **10-bit (P010)** is available for HEVC Main10 and AV1
where the GPU supports it (H.264 stays 8-bit only). The freely-choosable Expert **Bit depth** row is
**SDR-only** — higher precision, no HDR transfer curve or wide gamut — and only appears once the
selected codec carries 10-bit at all (HEVC/AV1); for H.264, which has no 10-bit path, the row is not
shown rather than shown-and-disabled, so the expert view lists only choices that apply to the current
codec. This is independent of native HDR10, which pins 10-bit unconditionally regardless of this
setting (§HDR below) — "10-bit" as a concept is not SDR-only, only this particular expert toggle is.

**Chroma.** **4:2:0** is the default and is universal (all codecs, 8- and 10-bit). **4:4:4** is an
Expert-mode option available for **H.264 and HEVC at 8-bit** (NVENC High 4:4:4 Predictive / HEVC
Range Extensions) on GPUs that report YUV444 encode support; it keeps full color resolution (sharper
text/UI) at the cost of larger files. **4:4:4 is not available for AV1** (NVENC AV1 is 4:2:0 only),
**not available at 10-bit**, and not available with native HDR10. The Expert **Chroma subsampling**
row itself only appears when the selected codec **and** the active GPU can carry 4:4:4 at all; for
AV1, or for a GPU whose probe reports no YUV444 encode support for the selected codec, the row is not
shown. When the row is relevant but a 10-bit selection is the sole conflict — a choice the user can
resolve right there by switching Bit depth back to 8-bit — the row stays visible with the 4:4:4 item
disabled and an explanatory hint instead of hiding, and an invalid stored selection is reconciled back
to 4:2:0. **4:2:2 remains unavailable** (the NVENC generation has no 4:2:2 path). While a recording
runs in **4:4:4**, the **live preview stays
available** (it shares the composited RGB frame with the preview before the AYUV conversion — see
the live-preview note in Section 7), and **frame snapshots stay available** as in 4:2:0: the
CaptureFrame hotkey reads back the packed AYUV encode surface and decodes it on the CPU with the
exact inverse of the encoder's RGB→AYUV conversion (same BT.709 matrix and Full/Limited range as
the recording).

**Color range and metadata.** **BT.709 color metadata** is written to every MKV and MP4 output. The
**Y'CbCr color range** (Full or Limited) is selectable behind Expert mode and is valid for every
codec/container combination (never gated), with **Limited** as the default. Because some players
(notably VLC) ignore the range flag and always expand limited→full — making Full-range recordings
look crushed/dark there — Diagnostics surfaces a compatibility tip with a one-click Full→Limited fix
when Full is selected. Presets carrying an older Full default are auto-migrated to Limited; an
explicit Full is respected as a deliberate opt-in.

**HDR handling.** HDR-capable displays are **detected automatically**; detection is not a setting and
cannot be turned off. Once an HDR-active display is detected, an **expert-only HDR handling control**
(Settings → Video, Container & codecs section) chooses the outcome:

- **Tone-map to SDR** (default) — the safe, universally compatible choice.
- **Record native HDR10** — keeps the original PQ / BT.2020 HDR10 signal.

The **HDR handling** row itself only appears once at least one probed display is HDR-active. On an
all-SDR system there is nothing this control could meaningfully change, so the row is not shown at
all rather than shown-and-inert; it appears the moment an HDR-active display is detected and
disappears again if none remains. A stored `hdr_mode` is never rewritten while the row is hidden —
it takes effect again exactly as before the moment a qualifying display returns.

Detection is not a single reading taken at launch. Windows HDR is a system-wide toggle the user can
flip at any moment, so the per-display facts are re-read on every display-configuration change, and
the recording-admission check reads them fresh regardless — a desktop switched to HDR after ExoSnap
started reaches the HDR handling row and the Diagnostics HDR card rather than waiting for a restart.

Behavior:

- On a non-HDR display the choice has no visible effect either way (the row is hidden there, per
  above).
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
- **Advanced Color Management (SDR desktop, HDR off):** with Windows' automatic color management
  enabled, the desktop composites to scRGB FP16 even though the display stays in SDR mode. Such a
  desktop carries SDR content (reference white = 1.0) and is recorded by encoding it with the sRGB
  transfer function — it is **not** tone-mapped, and it records in every HDR-handling mode, including
  `Off`. Only a display that actively reports an HDR color space is treated as HDR. Recorded output
  therefore matches the live preview and the desktop.
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

**The source picker is a named list, not a thumbnail grid.** "Change source" opens a modal picker
with two sections: **Displays**, where each display is one row carrying its own
`Region on <display>` action beside it, and **Application windows**, a scrolling list of the currently
capturable windows. A source is identified by its name — the display's sequential "Display N" label
(see below) or the window's title — and picking one selects it and closes the picker. The picker
opens no capture of its own and therefore holds nothing to release.

The **live** picture of the chosen source is the Record page's own preview, which the picker returns
to. That is deliberate: a grid of live thumbnails means one capture per visible source, all running
at once, before the user has decided what to record — and every one of them competing with the
preview for the same sources. One live preview of the one selected source answers the same question
for the cost of one capture.

**Record preview box (content-fit).** The Record-page preview box follows the **current source's
aspect ratio** — width-driven and vertically centered in the available space (height-clamped and
horizontally centered for sources taller than the area) — so **the edge of the box is the edge of
the recording**: there are no letterbox/pillarbox fill bars inside the box, and genuinely black
recorded content is distinguishable from padding. The box carries a **1px line on its edge** in the
current state's tone — quiet in the neutral state, and the recording / paused / warning / blocked
tone otherwise (plus the existing recording scanline treatment); that line is the Preview Surface's
own border, so the toolbar above the frame is inside the same ring (§8). The box follows
aspect changes live: switching targets, changing a region, or the captured source changing its own
dimensions re-fits the box. With no source selected (or before the source's dimensions are known)
the box keeps its default full-area size. The preview's meta/stats text rows (top target line,
bottom stats/format line) are **on-screen indicators drawn over the video — and over the webcam
PiP — in both the live (DXGI) and static preview paths**; they are never part of the recording or
of frame screenshots.

**Idle live preview.** Before recording, the Record-page preview of a **display** is fed by the same
DXGI Output Duplication backend the recording uses, owned by a shared capture hub: the preview is
VRR- and HDR-true, shows no OS capture indicator, draws the live cursor, and **holds its last frame
through a monitor unplug/replug** instead of blanking — production resumes when the display returns.
The capture exists only while the preview is visible and is closed with it. Window and Region
previews run their own Windows Graphics Capture of the selected target (see KNOWN_LIMITATIONS for
the exact boundary).

**Preview presentation is producer-driven, and never owes a frame.** The preview redraws when the
capture producer publishes a frame, not on a timer and not at display refresh — an idle desktop
costs nothing. The guarantee that comes with that: **if a frame has been published and the window
could not render it — it was moving between monitors, it was unexposed, its scene graph was being
rebuilt — that frame is presented as soon as the window can render again.** No second frame and no
unrelated interaction is needed to release it. In particular, moving the window from one display to
another never leaves the preview frozen until the mouse moves. This holds for the idle preview and
for the live preview during a recording alike.

**Ready-state dock gating.** The single-frame capture (screenshot) button reads a frame back from the
live preview, so in the Ready state it stays disabled until that preview has actually rendered its
first frame. The Record-dock microphone toggle is likewise non-interactive while no audio input
device is present, showing the tooltip `No microphone connected`.

**Live preview (WYSIWYG during recording).** **Once recording starts, the preview shows exactly the
frame the engine is encoding** — the composited, pre-encode source (cursor and webcam PiP already
baked in) is shared to the preview through a GPU texture, and the preview's own capture stops. There
is no second capture running alongside the recording, and the preview reflects what is actually being
recorded (so a black-screen or swap-chain problem is visible in the preview, not hidden by an
independent capture). During the pre-record countdown the preview holds its last live image until the
first recorded frame arrives, so there is no black flash. This includes **native HDR10** recording:
the engine shares its HDR frame and the preview tone-maps it to SDR for display, so what is shown is
the recorded frame, rendered the way SDR players will approximate it. The one exception is the rare
already-PQ 10-bit desktop, where no shareable frame exists and the preview keeps its own capture (see
KNOWN_LIMITATIONS).

**Webcam PiP.** A webcam picture-in-picture overlay is **composited into the recording** (it is an
in-video element, not an on-screen-only overlay) and rendered WYSIWYG with a real mirror option and a
selectable overlay placement. In a constant-frame-rate recording the PiP keeps moving at the encode
cadence even while the desktop is perfectly still: a screen capture only produces a frame when the
screen changes, so the held screen is composited again with the current camera image rather than
repeating the previous composited frame. During a capture-loss recovery the picture is held frozen
instead, until the capture source is reopened.

On the rare HDR10 display whose desktop frames arrive already in PQ, the engine cannot composite
overlays and records **without the webcam and cursor**. When that happens the Record preview drops
its picture-in-picture to match the file, and a notification says so. The preview never shows an
overlay the recording will not contain. Webcam opacity (0–100%, default 100%) is applied identically in the
Record-page preview and the recorded output; it is a live control on the Settings → Webcam card. The
mirror is a live, UI-editable control and its preview matches the file.
The chroma key composites the same way: the live preview uses the identical key color, tolerance, softness
and spill reduction the encoder uses, so a green screen that looks clean while setting up is the one that
lands in the file. Those chroma parameters — an on/off toggle, a key color (Green, Blue, Magenta or a
custom picked color), and tolerance, softness and spill-reduction sliders — are editable on the Settings →
Webcam card in a group that stays collapsed until chroma keying is turned on. Opacity and every chroma
parameter are pushed live while recording (they never require a restart), the same class as the mirror; only
the camera device and resolution are fixed for the duration of a recording. Its on/off is a single control
surfaced in two always-in-sync places — Settings → Webcam and the Record-page transport dock (camera
button) — and is off by default. Turning it on both includes the webcam in the recording and starts the
live setup preview; the camera opens only while it is on (opening Settings → Webcam no longer turns the
camera on by itself). The webcam is never recorded without a selected device (the first available
camera is pre-selected when one exists). By default the picture-in-picture starts in the
bottom-right corner of the preview with a small margin from both edges (it is not flush to the
corner); it can then be moved and resized anywhere **within the displayed video area, right up to
every edge**, and the preview renders it **exactly as it will appear in the file — never squished,
trimmed or clipped**. The preview's meta/stats text rows draw above the picture-in-picture as
on-screen indicators (they are not part of the recording), so a bottom-edge placement is shown in
full underneath them.
With **no camera attached** the Record dock's webcam
control is unavailable and says so; attaching one makes it available again. Unplugging the last
camera never loses the stored choice — it returns when the device does. When the toggle is on but the
selected camera cannot be opened, the dock button shows a coral error state and its tooltip names the
reason (`Camera can't be opened — <reason>`). Recognised failures read as a plain-language sentence
with the raw diagnostic in parentheses (e.g. the camera offers no compatible video format, or is in
use by another application); unrecognised failures show the raw reason verbatim. Turning the webcam
off clears the error, so re-enabling it starts neutral until a new open failure actually occurs. The
device, resolution, mirror, opacity and chroma-key controls stay editable regardless of the on/off state
(opacity and the chroma-key parameters are live-adjustable on the Settings → Webcam card, even mid-recording).
The webcam is the only feature that depends
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
is an **in-process** ETW consumer (a real-time trace session on a worker thread, ADR 0033), opt-in and
elevation-gated, never a hard dependency; the app degrades gracefully when not elevated. Three states
are reported and are distinguishable from each other, because "no measurement" has three different
causes the user can act on differently: `requiresOptIn`, `requiresElevation`, and available. Turning
the opt-in on never prompts for elevation — a settings toggle is not consent to restart the
application.

**Known target-identity boundaries.** A saved Display or Region target is remembered by a
hardware-stable identity (the monitor's device path plus its EDID vendor/product, and its serial
number when the panel reports one), not the GDI device name. Saved monitor targets therefore survive
unplug/replug, driver restarts, and reboots in a different port order, and never silently select a
*different* physical monitor. When the saved display genuinely cannot be found — it is unplugged, or
two identical monitors with no serial number were cable-swapped — the app does not guess: it shows a
calm "Saved display not found" notice and leaves the source unselected until you choose one (it is
then remembered). Region rectangles are stored relative to their anchor display, so a resolution
change carries the rectangle proportionally rather than pixel-exact.

**Device loss mid-recording (ADR 0046).** Losing a capture device during a recording is handled per
device type — a coherent, honest policy rather than a blanket "stop and restart". The recording is
never retargeted onto a *different* device; the engine only holds or reacquires the **same** source.

- **Display (monitor).** A brief loss (mode change, sleep/wake, reconnect) holds the last frame and
  reopens the same display (by GDI name), indefinitely, so the recording continues seamlessly. A GPU
  removal ends the recording cleanly (the segment stays valid).
- **Window.** Closing the captured window ends the recording cleanly — a closed window is gone for
  good and is not retargeted. (Merely minimizing or covering it keeps recording the last frame.)
- **Audio source.** An audio endpoint lost mid-recording (a mic unplugged, a headset switched, the
  system output changed, the audio service restarted) **no longer ends the recording**. The affected
  source goes to honest silence and the recording keeps running — video and every other audio source
  are untouched — while the engine reactivates the source with the same identity every 500 ms. In a
  merged track only the dead source's contribution falls silent; the others keep mixing. If *every*
  source of a merged track is lost at once, the track keeps running on exact-length silence for as long
  as the outage lasts — reopening the endpoint included — so its duration still matches wall time and it
  never slides against the video, and the first source to come back re-enters at the track's current
  position (no repeated audio, no backwards timestamp). A fixed-device microphone reacquires that exact device; the default microphone
  and the system-output capture follow the *current* Windows default (they represent "the system
  default", not one pinned device); an app/window audio capture keyed on a process reacquires only
  while that same process is still running (it never grabs a different process that reused the PID)
  and otherwise stays silent. The silence gap is real and shown: a calm live notice in Diagnostics
  while a source is degraded, a standing notification for the duration of the outage (updates in place
  if the degraded set changes, clears the moment every source reactivates or the recording ends), and
  the post-flight report notes that the recording contains a silence gap.
- **Webcam.** Losing the camera freezes the last picture-in-picture frame and reopens the same device,
  indefinitely; the recording continues.

If every audio source is lost at once, the recording continues **video-only** (the picture is the
primary source). The engine never auto-switches an audio source to a freely-chosen *other* device.

**The captured source changing size mid-recording ends the recording.** The encoder, the compositor
and the colour pipeline are all fixed at the source size resolved when the recording started, so a
source that changes its own pixel dimensions mid-session cannot be followed. The recording ends with
the explicit error `capture source size changed during session from <old> to <new>; restart recording
to reconfigure encoder`, and the file written up to that point stays valid and playable. Resizing the
captured **window** is the everyday case — including dragging it onto a display with a different
scaling factor, which changes its captured size; a **display** whose mode change alters its
resolution ends the recording the same way (a mode change that keeps the resolution, like a refresh
rate switch or sleep/wake, is the hold-and-reopen case above). Recording again picks up the new size.
Recording a moving or resizing window is not otherwise restricted: the recording follows the window
around the desktop for as long as its size stays the same.

**Displays are numbered sequentially everywhere.** The internal GDI names skip numbers after
plug/unplug cycles (`\\.\DISPLAY6`, `\\.\DISPLAY7` on a two-display desktop); the user-facing
"Display N" label re-sequences the attached displays to 1, 2, 3… and the source picker, the Record
header and the output filename all use the same numbering. A display that just left the topology
falls back to its raw number rather than borrowing another display's.

**Fullscreen capture matrix.** How a target behaves depends on the app's presentation mode:

- **Windowed** and **Borderless / windowed-fullscreen (FSO, flip-model)** record correctly on
  **every** target — Monitor, Window, and Region. This is the common case on Windows 10/11; most
  in-game "fullscreen" settings run as borderless/flip today.
- **Legacy exclusive fullscreen (FSE)** bypasses the desktop compositor. **Window** capture records a
  **black or frozen** frame — ExoSnap cannot capture an FSE window in isolation (hook/injection
  capture is deliberately rejected; see §14). **Monitor** (and Region, which is monitor + crop)
  capture *can* record exclusive fullscreen.
- **The honest rule:** to record an exclusive-fullscreen game, capture the **monitor**; to capture a
  **window** directly, run the game in borderless / windowed fullscreen. ExoSnap detects an
  FSE window target **pre-flight** (`rec.capture.exclusive_window`, §11): a proven-black window is a
  Blocker and stops the start, and the check offers a one-confirm "Record the monitor instead" fix.
  A game that changes the desktop resolution while recording a monitor ends the recording cleanly with
  an explicit size-change error (the footage up to that point stays valid).

  **A window that stops producing frames *during* a recording** — the shape a mid-session switch into
  FSE takes — is reported as a **capture stall**, not as a diagnosed cause. What ExoSnap measures is the
  absence of frame progress: while a **window** capture is recording, it watches the count of frames the
  capture backend actually produced. If that count does not move for **10 seconds** and the window is
  fullscreen-shaped, alive, visible and not minimized, a standing **caution** notification appears —
  *"Window capture appears to have stalled. … The recording is still running, but the captured window may
  be frozen."* The recording is **never** stopped automatically: the rest of it may be worth keeping and
  the source may recover. The notice is raised **once** per stall, is cleared the moment frames resume
  (a later independent stall raises a new one), and a session that had one records
  `window_capture_stall` in its session report.

  Only the stall is claimed. Exclusive fullscreen is named only when a fullscreen signal (the Shell's
  QUNS state or a PresentMon `ExclusiveFullscreen` observation) actually corroborates it, and even then
  as a conditional suggestion — never as *"exclusive fullscreen detected"*.

  **What this does not cover:** an *ordinary* (captioned or non-monitor-filling) window that stops
  producing frames stays silent, because mid-recording nothing distinguishes it from a window that
  simply has nothing to redraw, and a false alarm on an idle text editor is worse than silence. A
  minimized, hidden or virtual-desktop-cloaked window is likewise silent — it is supposed to stop.
  Detection is also a **notice, not a repair**: the frames already lost to the stall are gone.

  The trade-off runs the other way too, and is deliberate: a **fullscreen-shaped window whose content
  is genuinely at rest** — a borderless video player left paused for ten seconds — produces no frames
  either and raises the same caution. That is why the wording is *"appears to have stalled"* and *"may
  be frozen"* rather than a verdict: for those ten seconds the recording really did hold one frame.
  The notice clears itself the moment the content moves again.

*(Cells requiring a real legacy-FSE title are verified live before being promised as behavior; the
matrix above reflects the shipped detection + monitor path.)*

---

## 8. Recording lifecycle

**Pre-flight readiness gate.** Before a recording starts, all blocker and notice checks run and are
surfaced green/amber/red with fixes. **Recording start is blocked while any diagnostic blocker
exists** (for example: no supported NVENC encoder detected, hard-stop disk threshold reached, or an
unresolved HDR10-vs-H.264 conflict). If no supported NVIDIA NVENC encoder is detected, recording is
blocked with a diagnostic message rather than silently falling back. Blocking is a property of the
**start path**, not of the Diagnostics page: every blocker is enforced where the recording is actually
admitted, so a blocker holds even if the Diagnostics page was never opened. The start is refused with
the blocker's own reason — never a generic failure.

**Page composition.** The Record page is two things on one rhythm: the **Preview Surface** and the
**transport dock**, with the shared 24 px page inset around them and 16 px between them.

The **Preview Surface** is one surface with two parts — a compact **Preview Toolbar** and, directly
below it, the live frame — sharing one border, one radius and one width, divided only by a hairline.
It replaces a separate full-width context card that sat above the preview: two rounded rectangles a
scale step apart said the same thing twice (the card named the source, the preview showed it), and
the card's height plus the gap to the preview cost the page's subject roughly 70 px of stage for a
row of text that never changes while recording.

The toolbar is **38 px** — two rungs under the shell's 40 px title band, so the two never read as a
pair of title bars — and it is chrome *on* the stage, not a second page header. It carries, left to
right: a glyph for the capture-target kind (display / window / region), the source name, a
**LOCKED** badge while a running recording has fixed that choice, and at the right end the resolved
format summary as one understated run plus **Change source**. There is no separate `SCREEN` /
`WINDOW` eyebrow — the glyph and the source name already say it, and the accessible name still
states it in words. At the 860 px minimum window the toolbar stays **one line** and never grows: the
format summary gives up room first, the source identity and the way back to the picker never do.

The frame below the toolbar is the page's subject and takes all remaining height. It keeps the
source's aspect ratio. The Preview Surface's border is **structural and neutral in every state** —
it says "this is one bounded surface", never what the engine is doing. It used to take the state's
colour, which turned the largest object on the page into a roughly 1 000 px status light repeating
what the state pill in its own corner, the shell's status pill and the transport's one recommended
action all already said, and left "success" and "failure" differing only by the hue of a ring
half a screen wide. Over the frame sit the state pill (top left) and, while recording or paused, the
live readout (bitrate, drops, drift, output size) on the same ground as that pill. Nothing that
changes with engine state is placed *around* the frame, so it never resizes because a message
appeared beside it.

**State is said locally, and once.** Each state names itself through the status pill over the frame
and in the title band, through the transport's one recommended action, and — only where something is
unresolved — through the page notice. The **LOCKED** badge is a neutral statement of fact, not a
caution: the capture setup is locked for the duration of a recording by design, so there is nothing
for the user to attend to.

**State colour vocabulary.** Coral means recording, destructive or error; amber means an actual
caution; green means ready or success; the accent means selection, focus or the primary interactive
emphasis; everything else is neutral. **Countdown, Paused and Locked are normal states and never use
amber.** Paused takes the accent — the same colour its Resume action carries — and the momentary
transitions (countdown, preparing, stopping, saving) take a quiet neutral. Cancelling a countdown is
not destructive and is not drawn as such: it is the accent-filled primary action of that state, told
apart from Record by its glyph rather than by its colour.

**Transport dock.** The Record page's controls sit in one bar along the bottom, in three
groups of deliberately different weight. The **dock is the recessed base and its controls sit on
it** — a raised bar with darker controls read as holes punched into the transport, which made the
thing meant to be pressed the darkest thing on the page.

- **Left — the sources.** One round icon button per audio/video source (`APP`, `SYS`, `MIC`, camera),
  each carrying its live level as an arc on its own edge. These are icons, not labels: they are the
  most-used controls on the page and the four abbreviations that preceded them said nothing about
  what they toggled. Every one has a tooltip and an accessible name spelling the source out in full,
  and a source that cannot be used (no microphone, camera that will not open) stays visible and
  disabled rather than disappearing.
- **Centre — the elapsed time**, the largest element on the page. It is neutral while idle, coral
  while recording and accent while paused, using tabular figures so the digits do not shift.
- **Right — the actions**, in two tiers: the per-recording secondary actions (capture frame, add
  marker, split, pause) as round icon buttons, then the one recommended action — **Record**,
  **Resume**, **Stop** or, once a recording has finished, **Edit** — as a wider filled pill. There is
  never more than one filled pill. While a recording is **paused**, Resume holds that pill and Stop
  keeps its size and its coral but gives up its fill, so the bar still answers "what now?" with one
  control.

  After a successful recording the recommended action is **Edit**, because opening what was just
  recorded is what the product is for and starting the next one is not. Record is not removed — it
  keeps its split button and its countdown chevron and steps down to a plain outlined pill beside
  Edit, so there is always a way on from the completed state. Edit is hidden, not disabled, when the
  recording cannot be edited at all (a split recording, a missing file, a failed run): a permanently
  dead button next to a successful result reads as a defect. It is never a separate control floating
  between the Preview Surface and the dock — the page is one Preview Surface, 16 px, one dock.

**Page notice.** A single inline banner sits above the Preview Surface for conditions the page itself
cannot fix. Its colour states what the message **means** — a refused export is an error, an
unavailable display is a caution — and never the same tone for all of them. It names a produced file
by **file name**, never by full path: a path is unbounded, so a banner carrying one grows as wide as
the deepest folder the user records into and stops reading as a sentence.

The banner is for **unresolved conditions**, not for confirmations. **A successful recording raises
no page notice.** It used to: a full-width "Recording saved · name.mkv" banner appeared above the
Preview Surface, and because the preview is the page's fill-height element that banner came straight
out of its height — and, through the aspect-ratio fit, out of its width as well. Measured at
1 600 × 1 000, stopping a recording moved the Preview Surface down 60 px and narrowed it by 106 px:
the user had watched that frame for the whole recording, and the reward for finishing was the entire
composition jumping to announce something four other things already said — the shell's status pill
reads *Completed*, the transport's recommended action becomes *Edit*, a "Recording saved" toast
carries the path and the show-in-folder action, and the file is on disk. **Stopping a recording
successfully must not change the Preview Surface's bounds.** A failure is different: it is
unresolved, it is stated nowhere else on the page, and it keeps its banner.

**Round-control states.** Every round dock control reads the same way in every state, and the four
states are told apart by more than one cue:

| State | Reads as |
|-------|----------|
| Available, off | Raised surface, hairline border, secondary ink |
| Hover | One step brighter surface, stronger border, full-strength ink; no movement |
| Active / on | Accent-tinted surface **and** accent border **and** accent icon |
| Unavailable | Drops to the dock's own fill (no longer raised) and to the dimmest ink, keeping its hairline so the control is still visibly there; no hover or press state |

Unavailable and merely-off must never look alike. The earlier treatment moved only the icon colour,
which made "no microphone attached" and "microphone switched off" identical.

**Reason tooltips.** Every icon-only dock control has a tooltip naming it, and when it is
unavailable the tooltip carries a second line saying **why**, in the product's own words — the
camera's actual open failure when there is one, "no camera was detected", "no microphone was
detected", "the capture setup is locked while a recording runs", "the preview has not produced a
frame yet", "a manual split needs an MKV or WebM container". A reason is never invented when a real
one exists. The reason is also the control's accessible description, and it is reachable by hover
even though the control cannot be activated.

**Preparing.** Between the trigger (or the end of the countdown) and the running recording, the app
briefly shows a **Preparing** state while device setup — display facts, webcam open, and the capture
lease hand-off — runs off the UI thread, so the window stays responsive rather than freezing at the
moment of the click. The default countdown is 0 s, so for most starts Preparing is the first feedback
the user sees. Pressing the record hotkey during Preparing cancels the pending start and returns to
Ready (quietly, with a log entry — no dialog); a device step already in flight finishes before the
cancel takes effect.

**Live monitoring.** While recording, low-cost instrumentation (aggregated off-thread, ~1–4 Hz, no
per-frame image analysis) tracks dropped/duplicated frames, A/V drift, and disk-fill ETA, and
classifies the pipeline as encoder-, capture-, or disk-bound. Root-cause correlation is surfaced (the
first showcase being VRR/refresh-rate vs CFR-capture judder). The six capture-pipeline cards show
live status (Healthy / Busy / Bottleneck) with a CPU/GPU tag and one secondary number. A live A/V
drift and output-size overlay is available. An on-screen diagnostics overlay exists but is **off by
default** (enabled in Advanced).

A/V drift is measured, not inferred: the audio device clock (WASAPI per-packet device-position/QPC
timestamps) is compared against the QPC timeline video frames are paced on, normalized at capture
start and smoothed over roughly one second of packets. Positive values mean audio leads video.
Pipeline queue depths and encoder output timing play no part in the number. A track that merges
several audio sources mixes multiple device clocks, so drift reads as unavailable for it rather
than showing a guess (a single gain-adjusted source still reports).

**Audio clock slaving.** Because the audio device crystal and the video (QPC) clock differ by tens of
ppm, a long recording would drift out of sync (50 ppm ≈ 360 ms over 2 h). On by default and
codec-independent, clock slaving gently corrects this: once the measured drift crosses ~15 ms the
audio output timeline is resampled by a sub-audible amount (≤ 0.05 %, well under the pitch
perception threshold), pulling audio back onto the video clock. The A/V drift number then shows the
**residual** — what actually remains in the file — with the raw drift and the applied correction
visible in diagnostics. The correction is a gentle proportional pull with a fixed rate cap, so a
severe clock error leaves a small bounded residual rather than an audible artifact. It only engages
on real, measured drift, so most recordings never trigger it; the *Audio clock slaving* expert
toggle turns it off for byte-exact archival capture. A multi-source merged track is not slaved (it
mixes several device clocks).

**Post-flight report card.** After each recording, a report card surfaces frame-drop %, peak A/V
drift, and overall pipeline health. When a recording had **real** frame drops, a caution toast
("Frames dropped") appears alongside "Recording saved", with a "View diagnostics" action. (The
same three values ride along into the Edit/Output/Save surface, as an icon at the right end of
its header whose tooltip carries them. The icon is a quiet info glyph while the pipeline was
healthy, and an amber or coral warning triangle with a short label beside it when it was not —
so a real finding is not hidden behind a hover.)

A drop counts as **real** when the recording lost picture it should have had: the encoder could not
keep up (backpressure), or a captured frame failed to be processed into an encodable frame. Two
other categories are **benign** and never count towards the reported drop figure, the toast, or the
pipeline-health readout: *coalescing*, where the source produces frames faster than the recording's
frame rate and the surplus is deliberately discarded (a 144 Hz display recorded at 60 fps), and
*CFR pacing*, where a scheduled frame slot passes before the first frame has been captured at all.
Every surface that reports drops — the report card, the "Review" panel, the live counter, the toast,
and the Diagnostics capture card — uses this same definition, so they can never disagree about
whether a recording dropped frames. Diagnostics additionally breaks the total down by category.

**Reported duration.** Everywhere a *finished* recording's length is shown — the completed-state
timer and result card on the Record page, the recording-history entries, and the length of the
editor's trim timeline — the number is the **media duration of the file**, i.e. what a player
reports. Paused time and the stop/finalize tail are not part of the media and are therefore not
counted, so a 60 s recording paused for 30 s reads as 1:00, not 1:35. For a split recording the
reported total is the sum of the segments' media durations. The **live timer during recording** is a
separate readout with its own semantics: it counts the running session and keeps advancing while
paused, so the user can see how long ago they started.

**Automatic split (time + size).** Two independent auto-split axes — **maximum duration** and
**maximum file size** — with "whichever comes first" behavior. Size is reported honestly as
"approximately N GB" and measured from committed container bytes (no file-size polling); split
boundaries stay keyframe-safe; counters reset per segment. Split is supported for MKV, WebM, and MP4.
For MP4, each completed segment is remuxed to progressive MP4 in the background while recording
continues; "Saved" is reported only once all segment remuxes finish. Manual split is independent of
automatic split. The split controls are Default tier — always visible, laid out inline within the
Output card (time and size sub-sections), not tucked behind a popover or an Expert-mode gate. Each
sub-section leads with an on/off **toggle**; the interval selector (**Split by time**) and the
segment-size field (**Split by size**) appear only while their toggle is on. Toggling off is exactly
the "off" state — it changes no persisted value beyond the split mode itself, so presets and exported
TOML round-trip identically.

**Low-disk guard.** A configurable soft **warning threshold** (default around 2 GB free) shows a
Diagnostics notice but still allows recording; a lower **hard-stop threshold** (default around
500 MB free) blocks recording at start and stops a running recording gracefully. For MP4 sessions the
effective hard-stop threshold is raised to account for the transient MKV and output MP4 coexisting
during remux (roughly 2× file size), and for split MP4 sessions it is raised further by the sum of
pending background remux jobs plus the live segment estimate.

When the output volume **cannot be queried at all** (an unreachable network share, a denied volume),
the guard cannot measure and therefore does not fire: recording proceeds and a warning is written to
the log stating that low-disk protection is inactive for that session. A volume that reports **zero
bytes free** is a full disk, not a failed query, and is blocked like any other hard-stop.

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
- **Delete** — an inline two-step confirm that permanently removes the artefact. It is visually set
  apart from the safe Finish/Continue actions (right-aligned, destructive tint) to avoid mis-clicks.
- **Decide later** — an explicit text button; entries stay in the manifest and the overlay re-shows
  at the next launch.

Continued sessions produce independent recording slices (no single-file concat). For MKV/WebM split
recordings, segments finalized before an interruption remain usable; an interrupted active segment
may not be recoverable.

**When the manifest cannot be written.** The manifest is a safety net, not part of the recording, so a
failed write never blocks a start or aborts a running recording. It is also never hidden: ExoSnap says
so with a **"Recovery protection unavailable"** notification (amber — the recording is fine, only its
crash-recovery entry is missing) and treats the session as unprotected from then on, rather than
carrying on as if an entry existed. A recording made while the manifest is unwritable simply cannot be
offered for recovery afterwards.

Only entries with something to recover are offered. An artefact that is **missing** and one that is
**empty** are both dropped from the manifest at the next scan — an interruption before the muxer
wrote its first byte leaves a zero-byte file, and offering it would promise a recovery that cannot
happen, at every launch, until the user deletes it by hand.

**Edit / Output / Save (post-stop surface).** A single view lets the user trim and export without
leaving the app: the player and its trim timeline on the left, and a right rail with two cards —
**Details** (duration / size / resolution / frame rate / video / audio / container as right-aligned
mono values) and **Export** (the output choices plus the progress and result of a run, described
below). One action — **Export** — sits **bottom-right**, matching the Record page's transport
actions; it starts the export straight away and is unavailable while one is running. When an export
is possible, Export is the **accent-filled primary action**: it is the one thing this surface exists
to do, and an outlined control there was indistinguishable from a dismiss button.

**It is a workspace, not a dialog.** Ownership is unchanged — it is still a layer over the Record
page rather than a navigation destination (ADR 0022), and Record remains its parent context — but it
**occupies the normal page content region**, below the shell's own title band. It carries no scrim,
no floating gap, no outer rounded frame and no outer border; its shape comes from the header divider,
the panel boundaries inside it and the footer divider. The previous presentation dimmed the whole
application behind a rounded rectangle nearly the size of the window, which read as a modal question
the application was waiting on an answer to — and covered the shell's own minimize, maximize and
close buttons, so a window with the editor open could not be closed or dragged by its own chrome.

While the workspace is open Record stays marked as the current destination, because the workspace
belongs to it. The navigation tabs stay **available**: leaving Record hides the workspace without
ending the session, and coming back shows the same one (§2). Back is the way out of the session
itself, and it is a navigation action drawn as one, with the shared chevron.

**Header.** One line: `‹ Back`, the title, the clip's file name, and the post-flight **report
status** at the right end. The file name is the only element allowed to give up room and elides in
the middle, so neither Back nor the report status can be pushed off the band at the minimum window.
The report is a **status, never an action**: it names the pipeline's verdict (`Good`, `Warning`,
`Critical`, `Unavailable`) in a severity-toned badge behind a fixed `Report` label. It used to be a
single badge that read `Report` when nothing was wrong and `Warning` when something was — one element
switching between an action and a verdict, with a tooltip as its only behaviour either way.

**Structure.** Three bounded areas inside the workspace — player, timeline, inspector rail — each
with exactly one boundary. The timeline sits in its own panel: its track already had a hairline, but
the label zone, the loading hint and the clock row sat outside it on the bare page background, so
next to two bordered panels the timeline read as loose furniture. Nested elements do not repeat their
container's border.

By default, a successful recording opens straight into this overlay, preloaded with the clip —
the product's post-record path is editing, not a bare "recording saved" notice. The **Open editor
when finished** toggle (Settings → Output) turns this off: with it off, a "Recording saved" toast
with Edit / Show-in-folder actions appears instead and the user returns to Record.

The surface scales with the window. The right rail narrows as the window gets tighter and scrolls
when its two cards need more height than the window offers, but it is never hidden — it carries
the export controls, and the surface stays fully usable down to the minimum window size. At the
narrowest rail the **Details** card tightens — smaller card padding, shorter fact rows, and rule
lines only between fact groups rather than under every row — which hands roughly 50 px to the
**Export** card below it. No fact is dropped and no value gets smaller; only the space between
them. A wider window keeps the roomier card. The export never covers the clip it was started from:
the only thing that ever interrupts the view is the overwrite confirmation.

**Nothing in the rail moves when an export changes state.** The rail's scroll position is the
user's; starting, finishing or failing a run never scrolls it, and the tops of the Details and
Export cards stay exactly where they were.

Trimming is **direct manipulation on the timeline** under the player — there is no button row
or duration readout above the strip:

- **Rows.** The timeline is a stack: one video row, then one row per audio track the recording
  carries. Audio rows show the track's name as muxed (`System`, `Microphone`, `Application`, or a
  combined name for a merged track); a recording written before track names were muxed falls back
  to `Audio 1`, `Audio 2` rather than guessing a source from the track order. A recording with no
  audio has no audio rows. Audio rows carry a **label and a fill only — never a waveform**, since
  a peak envelope means decoding the whole soundtrack and an approximated one would state
  something about the audio that was never measured. Handles, markers and the playhead span every
  row: a trim applies to the whole clip.
- **Thumbnails.** The video row shows frames decoded from the recording, each drawn **left-aligned
  at its own timestamp** so a tile answers "from here on it looks like this". Tile width follows
  the recording's aspect ratio and the number of tiles follows the available width, so resizing
  the window re-lays the strip. While tiles are still arriving, a muted **"Generating previews…"**
  hint sits in the quiet zone above the row stack and disappears once every tile the current width
  can hold has landed — no spinner, and no placeholder/skeleton tiles in the meantime. Tiles appear
  as they finish decoding; a position whose frame is
  not (yet) available leaves the row empty there rather than showing a placeholder.
  When the clip carries **nothing decodable at all** — it cannot be opened, it has no video stream, or
  a decode pass finished having produced no frame — the hint says **"Preview unavailable"** instead,
  in the same quiet rung and the same place, and stays there. That is a terminal statement, not a
  spinner: no tile is coming. The strip is the only thing affected — trim, playback, markers and
  export all keep working on a clip whose thumbnails cannot be produced. Cancelling a run (resizing
  the window, switching clips, closing Edit) is never treated as a failure.
- **Trim handles.** Draggable in/out handles sit at the start and end of the timeline; the
  trimmed-away ranges are dimmed. The handles constrain each other (they can never cross), and on
  release the cut point snaps to the nearest keyframe at or before the requested time and, within
  50 ms, to the nearest marker — trim stays **keyframe-accurate and lossless** (stream copy, no
  re-encode). The trim is applied only when the user clicks **Save & export**; until then nothing
  on disk changes.
- **Playhead.** A white playhead line moves through the timeline while the preview is playing
  (play/pause toggle on the player area). **Scrubbing** — dragging the playhead or pressing
  anywhere on the track — jumps the preview position there; playback pauses for the duration of
  the drag and resumes on release **only if it was playing before** (paused stays paused).
- **Drag feedback.** While a handle or the playhead is being dragged, it scales slightly and a
  centered time label appears above it: `MM:SS.mmm`, with an hour field (`HH:MM:SS.mmm`) only when
  the recording is one hour or longer.
- **Markers.** Markers placed during recording render as thin secondary-accent (violet) verticals
  across the timeline, positioned proportionally (a recording with unknown duration shows an inert
  timeline rather than guessing). The caution color is deliberately not used here — it stays
  reserved for a real diagnostic warning elsewhere in the app, and Studio Mint stays reserved for
  the active trim handles/playhead. Marker editing is not part of the edit surface.

Markers are **never written as container chapters**. Instead, a trimmed/remuxed export writes a
JSON **marker sidecar** (`<export>.markers.json`, ADR 0042) next to the exported file — but only
when markers survive the trim. Surviving markers are re-based to the trimmed clip's start;
markers cut away by the trim are dropped, and an export with no surviving markers removes any
stale sidecar at the destination instead of writing an empty one.

The **Export** card in the rail offers container **MKV / MP4** (both stream-copy, lossless) and a
save mode of new file (`<name>_edit.<ext>`, saved beside the source) or overwrite-original (atomic
rename in place); two short lines under them state what the current choice writes and where —
`Lossless stream copy` followed by either `Saved beside the source as "<name>_edit.mkv"` or
`Replaces the original recording`. The save mode alone determines the destination — there is no
separate destination-folder picker, since the model leaves nothing else for the user to choose.
Overwrite-original asks for confirmation before the export starts, naming the file it will
replace; the destructive choice is not the default button.

The same card reports the run, and it reports it **directly under the card's heading, above the
output choices** — so a result is visible without scrolling and without anything above it shifting.
There is a progress bar with **Cancel** while it is going, the result with **Show in folder** when it
succeeds, and the real error text with **Retry** when it does not.

A successful run names the file on **its own line and the folder on a second one**, each elided in
the middle, with the full path on hover. The two were one wrap-anywhere run, which in the narrow rail
broke lines inside the extension (`…2026-08-10 2` / `1-14-08_edit.mkv`) — the one part of a path a
reader scans for. There is **one** folder action, not two: `Show in folder` opens the containing
folder *and* selects the file, so it does everything a separate `Open folder` did, and two labels for
one outcome is a choice the user cannot win.

The follow-up actions stay stacked (the rail is too narrow for them side by side) but are kept
compact, so the finished state is not conspicuously taller than the other three. In the resting
state the card reserves no space for the status it is not showing. The output choices stay visible
throughout — greyed out while a run is in flight — so a finished export leaves the settings in
place for the next one; at the minimum window size they are what scrolls out of view. A **keyframe
interval** selector (Settings →
Advanced → Video: 2 s default / 1 s / 0.5 s) trades a little file size for finer trim accuracy. The
original recording is never mutated during export; not-yet-exported edits are discarded on dismiss.
Dismissing the surface ends the edit session rather than only hiding it: the clip is closed, the
preview decoder and the thumbnail strip's decoder are released, and the recording can be moved,
renamed or deleted again without quitting ExoSnap. Opening Edit afterwards starts from a clean
session.

**Current boundary:** trim, markers, stream-copy export, and real decoded-frame preview (video +
synchronized audio, FFmpeg-decode + Qt-paint) are implemented and reachable end to end, including
the playhead/scrub/trim-handle interactions against the real decoder. This covers 4:2:0 recordings
(8- and 10-bit) as well as a 4:4:4-chroma recording (Section 6's Expert 4:4:4 option, H.264/HEVC
8-bit only) — the player's decoder converts the fully-planar 4:4:4 pixel format the same
software decoders produce for that option, with no chroma-upsampling step since 4:4:4 carries full
color resolution per pixel. A recording made in native HDR10 (PQ / BT.2020, 10-bit) is tone-mapped
to SDR for the preview through the same reference curve the recording preview uses, so the same
frame does not change colour between the two views; the tone-map is applied only for unambiguously
PQ-tagged 10-bit material, and anything else takes the SDR path. The tone-map targets the reference
display peak rather than the peak of the screen the editor happens to be on (see
`KNOWN_LIMITATIONS.md`). Export is unaffected by any of this, since export is pure stream-copy
and never depends on the preview decoder. The Split Chapter action remains deferred to a later
release (0.11 per ADR 0022).

---

## 9. Presence and notifications

- **Tray icon** with idle / recording / paused states and an **unread notification badge**.
- **Notification hub** — the canonical notification center is a **bell icon with a notification hub
  panel in the app header**. **The hub is the record: every notification lands there**, persists until
  dismissed, and keeps its action (recover, undo, show in folder, …). The **system-tray icon
  additionally shows an unread badge** for the same items. The "Show notifications" setting gates only
  the toasts — the hub records regardless. **A hub entry carries its severity as a glyph and a word,
  not only as a colour**: the same four severity glyphs the readiness tiles, the issue cards and the
  inline notices use, and an accessible name that starts with the severity ("Warning. Storage running
  low."). Colour alone would say nothing to a user who cannot separate those hues and nothing at all
  to a screen reader.
- **The bell's unread dot** carries urgency, not a count. It appears whenever anything is unread and
  takes its colour from the **worst** unread entry — **mint** when nothing unread is more than a
  notice, **amber** when at least one is a warning (frames dropped, a source degraded, a dead hotkey,
  a repaired settings file, a settings file that could not be read, an omitted overlay, a recoverable
  session, a recording without crash-recovery
  protection), **coral** when at least one is
  a failure (unexpected stop, low storage stopping a recording, a failed settings write, a rejected
  capture action). The exact number is deliberately not shown in the title bar: it is never the thing
  you act on, and the hub states it in full one click away.
- **Toast notifications** — a transient glance at the hub, anchored bottom-right **of the screen
  hosting the ExoSnap window**. A notification is **timed** when it reports an **event that already
  happened** and **standing** when it reports a **condition that is true right now and will clear
  itself** when it stops being true. Only three conditions qualify: low storage, an audio source
  that lost its device, and a window capture that stopped producing frames — each is dismissed the
  moment its condition ends. Everything else is an event and leaves on its own, including a
  recording that stopped unexpectedly and an unfinalized recording offered for recovery: neither
  will ever be cleared by anything, so standing meant *forever*, and neither loses anything by
  going — the hub keeps every entry and the recovery surface offers itself again at startup.

  Timed toasts have exactly two dwells, chosen by whether there is anything to do about them.
  **10 s** when the card offers a way to act or reports a problem worth noticing — long enough to
  read it, decide and reach the button, including while the user is still coming back from whatever
  was being recorded. **5 s** when a glance is the whole interaction (a repaired setting, an omitted
  overlay). Nothing is longer than 10 s: past that a toast reads as standing, and the reflex to
  dismiss toasts unread is what would cost the three real standing notices their effect. At most
  one timed toast is visible — a newer one replaces it; standing toasts stack above it, never
  auto-dismiss, and always carry an explicit action out. A countdown bar appears exactly on the
  toasts that leave on their own. The card grows to fit its content: no reserved space for an absent
  body; the body word-wraps and the card grows with it up to six lines, ellipsizing beyond that (the
  hub always keeps the full untruncated text); with a single action the card itself is clickable
  (marked `›`); two actions get named buttons. A preset switch raises no toast — only the hub entry
  with **Undo** (the combo box that switched is the way back).
  - A toast is **operable**: its dismiss ✕ and every action it offers respond to the mouse. Only the
    transparent gaps between stacked cards fall through to whatever is behind them.
  - A body that has no place to wrap — a file path is one unbreakable token — breaks mid-token
    rather than growing past the card. No body may overrun the card at any length.
  - The "Recording saved" toast names the **file**, not its full path. The path is what the actions
    act on (**Show in folder** opens it, **Edit** receives it), not what the card spends its width
    on: the user chose the output folder and already knows it.
- **On-screen overlays**: a recording-status pill (anchored top-right of the recorded monitor), a
  diagnostics readout pill directly beneath it (**off by default**), a countdown overlay centred on
  the recorded monitor, and an **opt-in** interactive quick-control pill (off by default). All four
  are capture-excluded, as is the toast window above them — five capture-excluded windows in total.
  The first three are click-through; the quick-control pill (ADR 0016) and the toast stack are
  interactive by design and take mouse input.
  - The **recording pill** carries a state glyph plus the configured text. There is no REC or PAUSED
    word — the pill only appears while a capture is live, so the glyph carries the state: a coral dot
    while recording, an amber pause symbol while held, an amber warning symbol once **measured**
    frame drops occur. A failed recording does **not** raise the pill: the in-window error surface
    owns that, and a click-through pill could not be dismissed.
  - The **countdown overlay** shows whole remaining seconds as a digit, and its ring depletes
    **continuously**. The ring is a progress indicator, not a second way of printing the digit: a
    ring that only moves once a second is correct for an instant and stale for the rest of it.
  - **Overlay content is configurable** in Settings → Overlays, by preset with per-element
    overrides. Recording: **Minimal** (elapsed time) or **Custom** (elapsed time · file size ·
    source name). Diagnostics: **Health** (only the tokens that can report a problem — drop, drift,
    muted sources), **Technical** (all of fps · drop · drift · size · muted sources), or **Custom**.
    Toggling any single element turns the preset into Custom, starting from the set currently shown.
  - Every offered element has a measured runtime producer; an element with no producer is not
    offered. A configured-but-unmeasured value renders as an em dash, never as zero. An overlay whose
    elements have all been unticked does not appear rather than appearing empty.
  - Overlay **appearance is not configurable**. Opacity, radius, shadow, colour and placement are
    design-system values, not preferences; the Settings section configures behaviour and content
    only. Click-through is likewise not a setting — it is a correctness property of a window that
    sits over whatever the user is recording.
- **Close-to-tray** is opt-in and **off by default**.

**Closing the window.** Two inputs decide the outcome, and work in flight outranks the preference:

| Close-to-tray | Recording, export or remux in flight | Outcome |
|---|---|---|
| off | no | the application closes completely |
| off | yes | the window comes to the front and asks; confirming closes completely |
| **on** | **yes** | **the same** — the window comes to the front and asks; confirming closes completely |
| on | no | the window hides to the tray |

The third row is the rule worth stating: a running recording is asked about **whichever way the
preference is set**. Hiding tears nothing down, which is why it needs no warning of its own — but
what the user asked for was to *close*, and silently turning that into "hide" left them believing a
recording had ended when it had not. What they are answering is therefore always "close for real",
and confirming never resolves to a hide.

A finalize still in flight is the one case that blocks a full close without asking (the container is
being written), yet still permits a hide — hiding does not end the process, so the half-written file
the block exists to prevent cannot arise.

**An approved close ends the process.** It does so explicitly rather than relying on the toolkit's
"quit when the last window closes" behaviour, because the five capture-excluded overlays are
top-level windows in their own right: a standing notification kept the application alive after its
only window was gone, with the tray icon still showing and its Quit routing through a window that no
longer existed. **Quit** in the tray menu works whether the window is visible, hidden, or already
gone.

---

## 10. Hotkeys

Global hotkeys are rebindable, with conflict detection and rollback on an invalid bind. They cover
recording start/stop, pause/resume, single-frame capture, and related actions. Every action ships
unset; the user picks a combo that does not collide on their own machine. Hotkeys live as an
embedded card inside Settings.

If a hotkey starts recording while the app window is visible, the Record view is activated; if the
window is minimized, it is not restored.

If a persisted hotkey can no longer be registered at startup (another app or Windows already holds
it), ExoSnap **drops the unregisterable binding** (it stays cleared across launches rather than
silently swallowing the key or re-warning every start). Windows exposes no way to name the holding
process or to reclaim the combo.

Because no action ships with a default binding, a non-empty binding can only exist because the user
set it themselves — it worked when they chose it, so losing it is always worth telling them about.
Losing any registered binding at startup raises a notification naming the affected action, with a
**Rebind** action that deep-links to Settings → Hotkeys, where the user can bind a working shortcut;
attempting a combo already held elsewhere is reported inline there as a conflict.

### 10.1 In-window keyboard operation

Global hotkeys are one of four kinds of key handling, and only the first works while ExoSnap is not
the focused application. The other three are in-window and are **not** rebindable:

| Kind | Scope | Examples |
|---|---|---|
| Global hotkeys | the whole desktop | start/stop, pause/resume, capture frame, marker |
| Window shortcuts | the ExoSnap window | `Ctrl+1`…`Ctrl+5` select Record / Settings / Diagnostics / Logs / About |
| Surface-local keys | one page or overlay while it holds the keyboard focus | the Edit timeline's transport keys, the webcam overlay's arrows, `Escape` on a modal |
| Text editing | a focused text field | everything else |

Every shortcut ExoSnap adds inside its own window is modifier-qualified, so no shortcut can consume
a keystroke meant for a text field. The five navigation shortcuts are inactive while a blocking
surface (recovery, crash report, recording error, release notes) or an unanswered close prompt is
open, for the same reason the navigation tabs are: nothing may swap the page under a surface that
still covers it. The close prompt is named separately because it is the one case a scrim cannot
cover on its own — the desktop notification toast is its own always-on-top window and reaches the
same navigation intents from outside the shell. An open **edit session** is
not one of those — the shortcuts and the tabs share one navigation contract, and under it the
session is state of the Record destination (§2).

**Every interactive control is reachable and operable with the keyboard alone.** A control that is
semantically a button, a toggle or a selectable item appears in the tab order, shows a focus ring
while the keyboard put it there (never after a click), and is activated by **Space** — the Windows
convention, and Qt's own; Enter belongs to a dialog's default button and ExoSnap adds no second
activation key of its own. A group of mutually exclusive choices (a segmented control) is one tab
stop, with the arrow keys, Home and End moving inside it.

The **Edit surface** is fully operable without a pointer. The trim timeline is a single tab stop:

| Key | Effect |
|---|---|
| `←` / `→` | move by one second, `Shift` ten seconds, `Ctrl` a tenth of a second |
| `Home` / `End` | jump to the clip's start / end |
| `[` / `]` | choose whether the arrows move the playhead, the in point or the out point |
| `I` / `O` | set the in / out point at the playhead |
| `Space` | play / pause |

---

## 11. Diagnostics and fix actions

Diagnostics is a first-class engine, not a status readout. Its posture is **calm, not alarmist**: it
defaults quiet, reports only real/measured problems, gives one primary fix per problem, hides depth
behind an expert toggle, and always shows hard blockers.

**Underlying severity:** each check resolves to **Pass / Notice / Blocker**. A **Notice** is advisory
and never blocks recording; a **Blocker** prevents recording from starting.

**The four-tier honesty model.** Every diagnostic **declares its own tier** as part of the diagnosis
— the tier is not re-derived downstream from an id list. The tier sets both the color and the
default visibility, and the honesty rail is a hard rule: **hiding is only ever for noise (Tier 3 +
4); a real problem (Tier 1 + 2) is always visible in both Simple and Expert.** No check may show a
Tier-3 optimization in a warn color.

- **Tier 1 — Blocker** (coral). Always shown; gates recording start.
- **Tier 2 — measured problem** (amber). A live or environment problem that was actually *measured*
  (judder, disk write-stall, low disk, audio device loss, unresolved saved display). Always shown as
  its own card. Only ever fires when the condition is real — never predicted.
- **Tier 3 — optimization tip** (mint, "better, but it runs" — codec, container, color range, FAT32,
  Opus-in-MP4). Bundled into one quiet tip chip; never turns the verdict amber.
- **Tier 4 — fact** (neutral). Capability/environment facts (elevation baseline, live audio format).
  Run through the same model, shown only in the Expert Environment panel, never counted in the
  verdict.

**Entry cards.** Each Tier-1/Tier-2 card carries a mono **ID chip**, an optional **Elev** lock badge
(for checks measured from the elevated present-path baseline), its typed FixAction, and a collapsed
**Evidence** disclosure (measured value → "why" recommendation → a log excerpt).

The Diagnostics page has a **Simple (default) view** and an **Expert toggle** (a single global state
shared with Settings that reveals depth, not a second mode). It is a top-anchored readiness dashboard
that fills the page height rather than a centered, compact statement: a header band (verdict icon +
headline + subline, the Run-check control, and the last-check timestamp), then a responsive tile grid
— **Readiness · Encoder · Disk · Display · Audio · Capture target**, plus a **Last session** tile once
a completed recording exists — that reflows from four columns down to two as the window narrows. Any
Tier-1/Tier-2 card follows, then one bundled Tier-3 tip chip integrated into the same layout. The
per-adapter capability matrix follows below, inside the collapsed **Hardware capabilities** section
(§2) — health first, technical capability second.

**FixAction model.** Each detected issue can carry a typed, executable fix action with a safety
class, never applied silently:

- **Auto** — a safe, reversible change the app can apply after showing a change summary to
  preview/confirm (for example, reconcile to the nearest valid codec combination, or Full→Limited
  color range). Auto is normally config-only, but it also covers a **confirmed capture-target
  change** (e.g. "Record the monitor instead" for an exclusive-fullscreen window). **Rule:** when an
  Auto fix changes the recording *scope* or *track structure* (not just encoder settings), the
  confirm dialog with a `changes_summary` is **mandatory** (never one-click), and the summary must
  name the consequences. For "Record the monitor instead" the summary states that the whole monitor
  is recorded (other windows, notifications) and that the per-application (APP) audio row is removed
  (only System/Microphone remain).
- **Assisted** — opens the right pre-focused Settings panel or folder, or copies a command (for
  example, "switch the game to borderless" for exclusive-fullscreen black capture). Assisted cannot
  execute anything; when the app *can* execute the fix (the monitor retarget above), it is Auto, not
  Assisted.
- **External** — the app cannot perform it (for example, a driver install); it shows the exact
  required version and a deep link only.

Example fix labels include "Switch to a supported codec for this container" and, for the HDR
conflict, "Switch to AV1" / "Switch to HEVC".

**Check catalog.** Checks cover recurring environment/config conditions: old driver, low disk, FAT32,
unsupported codec on this GPU, audio-format mismatch, color-range (VLC) compatibility, refresh-rate
vs CFR judder, the HDR10-vs-H.264 conflict, and an **exclusive-fullscreen window target**
(`rec.capture.exclusive_window`): elevation-free detection that the selected window is in legacy
exclusive fullscreen, which window capture records black. It is a **Notice** when only a fullscreen
signal is present and a **Blocker** when capture has demonstrably produced no frames; its primary fix
is the "Record the monitor instead" Auto retarget above. Diagnostics does not add runtime checks for
already-fixed internal bugs.

**Support channel (no telemetry).** Because ExoSnap sends no telemetry, the diagnostic artifact a
user shares *is* the support channel. Three connected pieces make that shareable (see ADR 0044):

- **Structured logs with a session key.** The engine writes a canonical JSON-lines stream that
  appends and rotates across launches, and every record carries a launch session id that also
  appears in the human-readable log's startup banner — one key ties the two streams together.
- **Per-recording session report.** After each recording a `session-<id>.json` is written beside
  the logs (10 most recent kept), capturing the resolved format, config, encoder init parameters,
  drop/dup/discontinuity counters, duration skew, A/V drift and peak drift, the segment list, and
  the failure phase. Metrics with no measurement read `"unavailable"`, never a fabricated zero.
- **One-click support bundle.** A **Create support bundle** action (on Logs; also from the
  Diagnostics page) packages the rotated logs, the recent session reports, and GPU/adapter/display
  facts into a single scrubbed `.zip`. It is a neutral tool, not an error trigger — wording is
  "Create a diagnostic package to share with support", with no alarming styling. Nothing is
  uploaded: the file is saved via a dialog and revealed in the file manager. Paths, username and
  machine name are scrubbed, and capture-target window titles are redacted to `[capture-target]`.

A **Startup** table on the Logs page shows start-to-milestone latencies so startup regressions are
visible; it also rides along in the bundle.

The Logs page states how much of the stream is currently shown ("Showing 22 of 22 entries · All · no
search") as the page header's own subtitle, beside the title, rather than as a separate band between
the toolbar and the list. The log list itself is deliberately full-width — a log line is data, and
truncating it to protect a reading measure would be the wrong trade on a tool surface.

**Present / tearing / latency diagnostics.** An opt-in, elevation-gated provider (PresentMon, the
engine behind FrameView) enriches window/game-capture diagnosis and feeds judder correlation. A
second in-process kernel trace on the same opt-in and the same elevation gate powers a
DPC/ISR-latency check that names the offending kernel driver behind "smooth game, stuttery/crackling
recording". It reports a peak only while it is measuring one: a trace that is stopped, refused or
that ended by itself withdraws the reading and with it the recommendation, for the same reason the
present figures do below. The app does not run elevated by default; when
not elevated the toggle ("Present, tearing & latency diagnostics") is disabled with the hint "Restart
as Administrator to enable present/tearing diagnostics", and enabling it triggers a self-relaunch
offer (never during an active recording). The provider is never required, and the portable build
degrades gracefully.

The reported present figures always describe the **current** attribution window and nothing else.
Starting a recording opens a window; ending it closes one, and the per-recording present, discarded
and mode-flip totals reset rather than standing on the idle Diagnostics page describing a session
that is over. The same rule covers the two ways a window can end without anyone asking: if the
attributed process exits mid-recording, or the trace session itself dies, the reading goes back to
unavailable. A stale number presented as a live one is worse than no number, because it is the only
one a reader cannot tell apart from a measurement.

---

## 12. Settings model (Default / Expert)

Settings uses a **Default / Expert split** drawn around an explicit gating criterion rather than by
how a control looks. A row is Expert-only when at least one holds: misconfiguring it can produce an
incompatible or broken file (**bit depth, color range, chroma subsampling, rate control**); it is
meaningless without format expertise (**NVENC encoder preset, keyframe interval, Opus frame
duration/complexity**); or it protects pipeline integrity and disabling it can ruin a long recording
(**audio clock slaving**). Everything else is Default-visible even when it looks technical — mic
gain, mic channel mode, audio bitrate, channel count, audio bit depth and FLAC compression, the
brickwall limiter, microphone post-processing, and automatic split all live in Default. The Expert
toggle is a single global state shared with the Diagnostics page.

Expert mode adds, in place within their existing section: **Bit depth**, **Color range**, **NVENC
encoder preset**, **Keyframe interval**, and **Chroma subsampling** (Container & codecs); the
**Rate control** selector (CQ/VBR/CBR) with its **CQ** or **bitrate** spinbox, the free-entry **frame
rate** field (1 fps up to the fastest monitor's refresh rate), and **Frame pacing** (Quality &
timing); **Opus frame duration**, **Opus
complexity**, **Sample rate**, and **Audio clock slaving** (Audio).

**HDR handling** is Default, not Expert: HDR-capable displays are mainstream, the row is already
display-conditional (Section 6), and its default (Tone-map to SDR) is the safe, universally
compatible choice, so gating it further behind Expert added no safety.

Within Expert mode, a narrower relevance gate still applies to two rows whose visibility depends on
the active codec/GPU rather than on Expert mode alone: **Bit depth** (shown only for a codec that
carries 10-bit — HEVC/AV1) and **Chroma subsampling** (shown only when the selected codec and the
active GPU can carry 4:4:4 at all). A row that fails its relevance gate is not shown rather than
shown-and-disabled, so the expert view lists only what currently applies; the Chroma row is the one
exception that stays visible-but-disabled for a 10-bit conflict specifically, because that conflict
is fixable in place (switch Bit depth back to 8-bit).

**No section is Expert-gated.** Every embedded Settings section, including **Developer** (a logging
level, an honest disabled "planned" NVTX/profiling-markers row, and crash-report consent), is visible
in both Default and Expert; Expert only reveals additional rows in place inside a section, it never
hides or reveals a whole section.

Settings offers inline info hints (hover
popovers on info-i icons and the countdown chevron) and search. Info-i placement is **selective**:
only rows with a genuine A/B tradeoff carry the icon (plain boolean rows do not), and per-option
helper text lives inside the popover rather than as a separate line under the control. The
multi-option compare popovers are **explanatory only**: they list each option with its qualitative
tradeoff and mark the currently active value, but the setting itself is always changed with the
row's own control (the combo box next to the glyph), never by clicking inside the popover — the
popover carries no second, reduced picker. Single-line info hints render as calm tooltips; longer
ones use a short bold lead line with the explanation beneath. Roadmap-only
controls may appear as honest, disabled "planned" rows to communicate direction without enabling
unimplemented behavior.

---

## 13. Updates and crash reporting

**Updates.**

- **Off by default for every build, including official builds** (`check_updates_on_start` defaults
  to `false`, ADR 0045) — a first launch never contacts a server before the user turns the
  automatic check on from the Settings update card. Self-built binaries additionally never run the
  check at all, via a separate compile-time gate (`IsUpdateCheckEnabled()`), regardless of this
  setting. Both a manual "Check now" and a toggleable automatic update check exist; a manual check
  is itself the user's explicit action and needs no separate consent step.
- **Stable** and **Preview** channels. The selection **takes effect immediately** — the next check
  queries the channel now shown, not the one that was selected at launch — and it **invalidates the
  previous channel's answer**: "Update available — <ver>" describes a feed, not the application, so
  switching returns the card to **Unchecked** rather than carrying the other channel's verdict over.
  Switching does **not** start a network check on its own; a check remains the user's explicit
  action. A check already in flight when the channel changes is discarded rather than presented
  under the new channel.
- The client verifies the manifest against a **detached ed25519 signature** (Monocypher; shipped as
  a sibling `update-manifest.json.sig` asset, verified over the exact received manifest bytes
  before any field is parsed) plus each package's **SHA-256** hash, and **refuses downgrades**. No
  GitHub token is used by the client. No update is performed during recording or finalization, and
  the app never restarts silently.
- **UI home:** the update UI lives on the **Settings update card**, plus a **dedicated updater
  window** (per design canon `Updater.html`). The earlier About-overlay placement is superseded,
  and the hidden legacy `UpdateSettingsPanel`/`AboutOverlay` compatibility shim has been removed —
  the Settings card is the **only** update state model.
- **Version identity.** The app knows its **full release version** (e.g. `0.9.0-rc4`) everywhere:
  runtime `kVersion`, the handoff document's `currentVersion`, manifest `version`, About page, update card,
  support bundles and crash metadata. RC builds are therefore honestly older than the final of the
  same base version, and an older RC naturally discovers a newer RC on the Preview channel.
  Developer builds identify as `<base>-dev` and never impersonate a release.
- **The update that was offered is the update that gets installed.** Applying hands the updater a
  single versioned handoff document naming exactly one release, plus the signed manifest that proves
  it; the updater verifies that signature itself before reading any manifest field and installs that
  version or nothing (ADR 0068). It never resolves a release of its own. If the offered release's
  manifest could not be fetched, the update stays visible — a newer release that exists must not be
  reported as "up to date" — and applying refuses with that reason instead of starting an updater
  that cannot work.
- **Update card states** (normative): **Unchecked** (`No update check has run yet.`, button
  `Check for updates`; the state at launch and after a channel switch — deliberately distinct from
  "Up to date", which would be an assertion the app has not earned) · **Up to date**
  (`✓ Up to date · <last checked>`, button
  `Check for updates`) · **Checking** (`Checking for updates…`, action disabled; the click never
  moves the page's scroll position or steals focus out of the card) · **Available**
  (`Update available — <ver>`, button `Update to <ver>` launches the real external updater for
  Portable and MSI) · **Scoop** (`Managed by Scoop — update with 'scoop update exosnap'`, button
  `Open releases page`; the swap updater never touches a Scoop tree) · **Updater running**
  (`Updater running… follow the updater window`, action disabled, while the detached updater is
  open and the app still owns its normal lifetime) · **Pending** (`Restart pending… finishing the
  update`, button disabled, only after the updater's marked close/handoff request has been accepted)
  · **Error** (honest message, button `Retry`). An updater that exits or fails before handoff
  returns the card to an actionable Available/Verification-reinstall state. Pending is never
  reconstructed from a persisted UI flag on a later normal launch.
- **Verification reinstall mode.** Starting the app with `--verify-update-reinstall` enables a
  **non-persistent** test mode that offers exactly one extra action: reinstalling the **identical**
  full version (exact string match) through the complete production path — official feed, channel
  selection, embedded key, signed manifest, package SHA-256, recording/finalizing guards, real
  updater handoff, swap/install, verify, relaunch and cleanup. The card then shows
  `Verification reinstall available — <ver>` with `Reinstall <ver>` and the notice
  *Reinstalls the currently running signed version.* The mode can never offer a downgrade, never
  relaxes signature/hash checks, is logged in the app log and support bundle while active, and
  remains explicit in the updater content — the eyebrow above the version pills reads
  `REINSTALLING EXOSNAP`, and the working copy says so too (for example,
  `Downloading version <ver> again…` and `Reinstalling version <ver>…`) — rather than through a
  title-bar badge. It is gone after the next restart. Without the flag, the same version is never
  offered.
- **The offered version is the installed version.** The version the card offers is handed to the
  updater as its **pinned target**, and the updater installs **that** version or nothing at all: the
  signed manifest must name it byte-for-byte (the same exact-string equality the verification
  reinstall gate uses, because SemVer equality collapses foreign prerelease labels). If a newer
  release appears in the feed between the app's check and the updater's own resolution, the run
  stops before a single package byte is fetched, with `The offered version is no longer what the
  channel serves` and the installation untouched; a fresh check is the way forward. Without this,
  the offer, the "What's new" notes written for the next launch, the applied-version loop guard and
  the build actually installed could each name a different version. The **What's new** payload is
  bound to the same pinned target for the same reason.
- **Manual updater start.** `exosnap-updater.exe` ships next to `exosnap.exe` and is
  double-clickable, so starting it by hand is a real entry point — and the only way back when a
  failed update has left the app unable to start. Started **without arguments** it opens its normal
  window at rest and works out its own context (installed vs. portable, the install directory, and
  the version actually on disk); it never checks, downloads or installs anything on its own. The
  flow is *Idle → Checking → **Up to date** | **Update available** → Downloading → **Ready to
  install** → Installing → Verifying → Launching*, with a confirmation at each bold step:
  **Check for updates**, then **Download update**, then **Install now**. "Nothing newer" is a
  **result** (`ExoSnap is up to date`), not a download error. A build that may not contact the feed
  says so plainly and contacts nothing. The handoff started by the app is unchanged: it enters
  through its arguments and still runs start-to-finish without asking, because the user already
  confirmed in the app.
- **A cancellation is not a failure.** Stopping a download on purpose leaves the updater in a
  **Canceled** state — neutral card, no error tone, the stopped step back to *queued* rather than
  marked failed, and the truth that matters spelled out: *nothing was installed and nothing was
  changed*. It carries no failure case and offers no retry, because there is no fault to re-enter;
  a manual run offers a fresh **Check for updates**, a handoff run offers only **Close** (its
  confirmation was given in the app). Cancellation is only honoured where the engine actually
  observes it — while a package is downloading. The feed check and the wait for ExoSnap to close
  take no cancellation at all, and the install/verify/relaunch steps must not be interrupted; in
  all of those the request is refused rather than accepted and ignored.
- **The updater's exit code is its outcome.** `0` update applied and verified (including the case
  where only the automatic relaunch did not open), `1` the update failed, `2` the command line could
  not be understood, `3` a manual check found nothing newer, `4` installed and Windows must restart
  to finish, `5` canceled, or closed before any outcome. It used to be `0` for every run that
  reached the event loop, including a visibly failed one, so nothing that launched the updater could
  tell the two apart — and a cancellation must not report `1`, which would send a release script
  looking for a fault that never happened.
- **Development feed override.** `exosnap.exe --update-base-url <https url>` points the update
  check at a named feed instead of the production one, so a development build can exercise its own
  check and the full app→updater handoff. It is **refused in an official build**, refused on
  anything that is not an https URL with a host, never persisted, and the same URL is handed to the
  updater — app and updater must resolve one feed, not two. It relaxes nothing else: the recording
  guard, the signature verification and the package hash are unchanged, and a development build's
  pinned key is all zeros, so no manifest from any feed can pass verification there.
- **Shipped flow:** the update check (automatic or manual) finds a new version → an "update
  available" notification deep-links to the Settings update card → clicking **Update** opens the
  dedicated updater, a separate process that performs every step itself. Its step list (as
  rendered) is:
  1. **Downloading update** — fetches the signed manifest and the package (progress %); the ed25519
     manifest signature and the package SHA-256 are verified within this step, and a mismatch
     aborts and keeps nothing.
  2. **Closing previous version** — waits for the app to exit (the running image is locked).
  3. **Installing new files** — swaps the files in place: portable does a staged rename (old →
     backup, verified new → live); an installed build runs `msiexec` (one UAC prompt).
  4. **Verifying installation** — confirms the installed version; on failure the backup is restored.
  5. **Launching ExoSnap** — relaunches the app on the new version; on a healthy start the backup
     is discarded.

  The swap is **staged and reversible** (dual-swap) and nothing is swapped until verification passes.
  The updater uses one fixed, non-resizable **520 × 680 logical-pixel** window in every working and
  terminal state. Its 56 px title bar follows the main ExoSnap window's geometry and identity:
  the standard icon and two-tone **exosnap** wordmark at the same inset and scale, followed by a
  stable, neutral **Updater** role label, plus 46 × 56 **Minimize** and **Close** controls with the
  main window's hover/pressed treatment. A disabled maximize control is not shown. Phase, warning,
  error, completion and verification-reinstall state are not duplicated in the title bar; they
  remain in the progress/status and result content that explains them. Below the fixed step list,
  every state uses the same 110 px three-row state panel and the same separate 36 px action row, so
  status copy and buttons never move vertically. Result actions use the same compact button
  component: retryable results
  show **Retry/Re-download + Close**, a completed update that needs a manual launch shows
  **Open ExoSnap + Close**, and non-retryable results show **Close** only. The ambiguous
  **Open current version** label is not used.

  The window shows **one** progress language at a time. Until the run has measured something — the
  pre-flight frame it opens on — the ring carries **no percentage and no arc**, and the status line
  under it says what is happening; a percentage appears only once there is real progress behind it,
  and the ring never changes size between the two. The **eyebrow** above the version pills names
  what kind of run this is (`UPDATING EXOSNAP`, `REINSTALLING EXOSNAP`, or after a terminal failure
  `EXOSNAP WAS NOT UPDATED`), because the title bar's role label is the same word in every state.

  **Version emphasis follows what is actually installed.** While a run is in flight — and after one
  that applied — the target version carries the accent. After a terminal failure it does not: the
  emphasis moves to the version that is on the machine, so a failed update can never read as
  "the new version is installed".

  **No text is ever cut off at a widget edge.** Version identifiers come from the release manifest
  and installer detail text comes from Windows Installer, so neither has a length the window can
  assume. Anything too long is shortened with an ellipsis and keeps its full value on the tooltip
  and for screen readers: version pills shorten in the **middle** (the build suffix identifies which
  build it is), prose shortens at the **end**. The 520 × 680 window never grows to fit long content,
  and long content never pushes the action row out of view.

  During **Downloading** and **Closing previous version**, the normal title-bar close action safely
  opens the same confirmation as the visible **Cancel update** action. The confirmation says that
  download/preparation progress is discarded while the installed version remains unchanged; only
  the explicit destructive confirmation cancels the worker and closes the updater.
  While the swap-critical steps (Installing, Verifying, Launching) are in flight the updater window
  **refuses to close** — the close control is disabled and Alt+F4, the taskbar close, and a Windows
  logoff are all ignored, while the fixed action row shows a disabled **Close** action and a concise
  non-interruptible hint — so the in-place rename cannot be torn apart mid-swap. If a forced kill
  (or a power loss) does interrupt a portable swap between its two renames, the **next** update run
  detects the orphaned backup and restores the last-known-good install before proceeding. After a
  successful update the downloaded manifest, signature, and package are removed from the temp
  directory; a failed run keeps them so a retry can reuse them.
  Failure is shown as one of three variants. The headline, user-oriented detail, safety line, icon
  and actions live inside one status-tinted result card; raw WinHTTP/MSI/path diagnostics stay in
  logs rather than appearing as a second loose UI error. Retry/Re-download actions carry a refresh
  icon. Each variant **always names the version that is safe to run right now**:
  - **Amber (retryable):** a pre-swap step failed (download, app would not close, install could not
    start) or elevation was declined; the **current** version is intact and the step is retryable.
  - **Red (hard stop):** the card names the safe state explicitly — a failed download verification
    stops before anything is installed ("nothing was installed"); a failed installation
    verification restores the backup ("your previous version was restored"); a failed `msiexec`
    run reports the installer's own result ("your previous version is still usable").
  - **Green (installed and verified, only the auto-relaunch didn't start):** the **new** version is
    live and safe — the updater offers a manual start rather than presenting it as an error.

  Two moments are unavoidably not fully in-app: the UAC prompt (installed builds only) and the brief
  window while the app is closed for the swap.
- **What's new (shipped).** Release notes are surfaced from the GitHub release bodies already present
  in the `/releases` payload the update check fetches — no extra network call. One in-window overlay
  shows the notes as a single, always-expanded scrolling document (no collapse/expand), newest first;
  bodies are Markdown with their links in the accent colour, and a footer **"All releases"** link
  (bottom-left, with an external-link icon)
  opens the releases page; a primary **Close**/**Got it** button sits bottom-right. It has two entry
  points, which differ in *which* notes they show:
  - **Pre-update:** while the Settings update card shows "Update available — vX.Y", a **"See what's
    new in vX.Y"** link opens the overlay with the **full reference list for the active channel**
    (every non-draft release; Preview includes release candidates, Stable does not) — not just the
    pending gap, so the list itself doesn't go empty when there's nothing new to update to.
  - **Post-update (one-time):** clicking **Update** persists the gap notes — every version in
    `(installed, target]` — as a pending payload; on the first launch of the new build — when the
    payload's target equals the running version and the suppress setting is off — the overlay is
    shown once with those gap notes and the payload is cleared. This mode carries a **"Show release
    notes after updates"** checkbox, **checked by default**, persisting `whats_new_suppressed`
    (unchecking it suppresses future auto-shows); that setting only gates the post-update auto-show
    and never hides the card link. First install, downgrade, and manual-ZIP updates leave no matching
    payload, so no overlay appears.

**Crash reporting.**

- **Opt-in and consent-gated**, local-first (out-of-process Crashpad). Nothing leaves the machine
  without an explicit current or previously remembered choice.
- **A crash always leaves a local minidump.** Official builds capture it out-of-process via
  Crashpad; builds without it fall back to an in-process handler. The dump stays on the machine
  and is never uploaded without consent.
- **Next-launch only** — crashes are offered for reporting on the following launch.
- **Single explicit delivery path:** automated upload to **Sentry with EU data residency** is
  compiled in only for official builds and remains consent-gated — so self-built binaries never
  upload. The crash dialog does not construct or open a prefilled GitHub issue.
- **Crash-dialog actions:** `Send report` (when the official upload path is active), `Don't send`
  and a directly visible tertiary `Open crash folder`. There is no overflow menu.
- The dialog leads with the truthful next-launch fact ("the previous session did not shut down
  normally"), recovery availability, dump availability, cause availability and the non-empty
  version/encoder context from the previous-session sidecar. It does not render empty
  exception/module/thread/stack rows or substitute current-machine OS/GPU probes as crash facts.
- Privacy details are a collapsed, keyboard-accessible disclosure. They distinguish the
  privacy-scrubbed structured event from the native minidump binary, including the dump's possible
  loaded-module paths and IP transit to Sentry's EU ingest region.
- **Persisted policy:** `Ask every time` (default), `Send automatically`, or `Never send`.
  Migration maps legacy `auto_send_crash_reports=true` to `Send automatically`; false or a missing
  key maps to `Ask every time`, never to `Never send`. `Never send` suppresses only the report
  consent prompt; the independent local recording-recovery surface remains available.
- The dialog checkbox is **Remember this choice for future crashes**, unchecked by default. It is
  **always visible** — at the 860 × 700 minimum window, with the report-contents disclosure
  expanded, the contents scroll and the checkbox does not, because a consent decision whose control
  has scrolled off screen is one the user can commit without seeing. It is
  draft state only: Send + remember commits `Send automatically`; Don't send + remember commits
  `Never send`; either action without remember leaves `Ask every time`; close, Escape and backdrop
  dismissals never change policy. Settings → Developer → **Crash reports** exposes all three
  choices and applies the corresponding consent state immediately.
- A Send action without remember is one-shot consent. The app releases and flushes the pending
  Sentry envelope, then resets sentry-native consent to unknown, so later reports in the same
  session do not inherit that decision. Remembered automatic send remains persistent until changed.

**Signing status.** Builds are **not yet code-signed** (portable ZIP and MSI); Windows SmartScreen
may warn on first launch. ExoSnap participates in the SignPath Foundation free code-signing program;
release binaries will be signed once the certificate is issued.

---

## 14. Privacy

- **No analytics, no telemetry, no account.** By default ExoSnap makes **no network connections** —
  including on first launch, for every build.
- Only two features can contact external services, each strictly opt-in and only when the user acts:
  the **update check** (public GitHub Releases API, no auth token; sends only the request's IP and a
  fixed User-Agent identifying the checker — **no ExoSnap version number is sent**, the
  newest-release comparison is entirely client-side) and **crash reporting** (Sentry, EU data
  residency).
- **Crash-report allowlist (what is sent):**

  <!-- PRIVACY-ALLOWLIST-TABLE-BEGIN -->
  | Tag key | What it carries |
  |---|---|
  | `os.name` | Windows edition name (e.g. "Windows 11") |
  | `os.version` | Windows build/version string |
  | `gpu.model` | GPU adapter name |
  | `gpu.vendor` | GPU vendor (e.g. "NVIDIA") |
  | `gpu.driver` | GPU driver version |
  | `app.version` | ExoSnap version |
  | `encoder_backend` | Active encoder backend (e.g. "nvenc") |
  | `container` | Output container (e.g. "mkv") |
  | `video_codec` | Selected video codec |
  | `audio_codec` | Selected audio codec |
  <!-- PRIVACY-ALLOWLIST-TABLE-END -->

  plus the crash stack/minidump — nothing else. Only `encoder_backend`/`container`/`video_codec`/
  `audio_codec` are populated on the Sentry path today; `os.*`/`gpu.*`/`app.version` are allowlisted
  (so a future change could set them) but are not populated by current app code. Kept honest by
  `scripts/validate-privacy-allowlist.ps1` (see `docs/privacy-review.md`).
- **Never sent (structured event):** usernames, file paths (including output folder and recording
  filenames), machine name, breadcrumb logs. Recording content is never captured. No persistent
  device identifier is created. **Exception:** the Crashpad minidump binary (uploaded on a hard
  crash, separately from the structured event above) carries the full install path of
  `exosnap.exe` in its module list — for a portable install run from under `%USERPROFILE%` this can
  include the username segment of that path. See `PRIVACY.md` and `docs/privacy-review.md` for the
  full explanation and the recommended mitigation (install to a path with no username component).
- A capture-target **window title** is never written to the on-disk log in the first place (a
  `[window]` placeholder is logged instead) and is additionally redacted if it ever reaches the
  one-click support bundle.
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
  out of scope for the MVP); HDR beyond BT.2020 (HDR handling now covers both monitor and WGC
  window/game capture via an FP16 frame pool; no HLG/wide-gamut is the confirmed 1.0 scope); 4:2:2
  chroma and 10-bit 4:4:4
  (8-bit 4:4:4 for H.264/HEVC is implemented as an Expert option); multi-vendor hardware encoding;
  immediate in-session crash reporter; the fullscreen/borderless/exclusive game-capture matrix. The
  in-place dual-swap updater has since shipped (see Section 13) and is no longer on this list.

**Licensing.** ExoSnap is GPL-3.0-or-later and bundles FFmpeg as LGPL-2.1-or-later shared libraries
(dynamic linking).

---

## Resolved-decision notes

- **Default audio state.** The canonical default is **context-aware**, per Section 3: the `APP` row
  is always present in the Settings Audio card, is a persisted setting, and defaults enabled — but
  it only actually contributes audio while a specific application window is the capture target; for
  a screen or region capture it recedes and no `APP` source enters the recording. For screen capture
  the shipped default is `SYS` enabled and `MIC` present but off. Each enabled source is a separate
  resulting track (per `CLAUDE.md`). An older internal preset note described a System-only default
  with no `MIC` row at all; Section 3's context-aware default is authoritative.
