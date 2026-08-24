# ExoSnap Roadmap

This document captures the strategic direction from `0.1.0` toward `1.0.0`. It records the
**architecture guardrails** that govern new work and the **per-version scope** that gets ExoSnap
from a focused NVIDIA/NVENC MVP to a multi-vendor, reliability-first `1.0`.

Pre-`1.0` versions are independent waves, not a strict decimal sequence — SemVer treats `0.10.0`
as the tenth minor version, not a number smaller than `0.9.0`. This lets unrelated themes ship in
their own releases instead of being forced into oversized ones.

---

## Architecture guardrails

These decisions govern new encoder/container work. They are tracked as ADRs 0006–0013 under
`docs/decisions/` (plus ADR 0014 for the MP4 remux-on-stop strategy). The summaries below exist
for quick reference; the ADRs are the authoritative source for rationale and detail.

### Encoders

```
IVideoEncoder
├── NvencVideoEncoder      (current baseline)
├── AmfVideoEncoder        (later)
├── QsvVideoEncoder        (later)
├── X264VideoEncoder       (software fallback)
└── SvtAv1VideoEncoder     (optional software fallback)

VideoEncoderFactory · CapabilityProbe · EncoderSelectionPolicy · EncoderDiagnosticsAdapter
```

- **Hardware encoders use native SDKs** (NVENC, AMF, QSV/oneVPL) — not a single "everything through
  FFmpeg" path. This enables direct D3D11-surface use, no needless copies, precise capability
  detection, native forced-keyframe / rate-control / HDR features, and clearer vendor-specific errors.
- **Software H.264 is x264** (`GPL-2.0-or-later`, compatible with the project's GPL model). x264 sits
  behind the encoder factory with its own distribution/license gate — never wired directly into the
  video thread. A license + patent-distribution audit must precede the first release that ships x264.
- **Media Foundation is transitional only.** It is not used for new preferred encoder paths; it may
  remain as a narrowly-scoped Windows fallback or legacy MP4 path until replaced.

### Containers and tooling

```
- libmatroska / libebml   → recording container: MKV / WebM
- libavformat (lgpl)      → remux / trim engine: progressive MP4 on stop, Quick Trim
- Media Foundation path   → removed in 0.2.0 (was transitional only)
```

Recording always writes MKV via libmatroska. MP4 output is delivered by remuxing the transient
MKV to progressive MP4 (faststart) via libavformat stream-copy after stop (see ADR 0014).
No fMP4 recording writer is built. Encoders and containers are deliberately decoupled:
libavformat remuxes/trims packets produced by any encoder without re-encoding.

### Rate control

`CRF` is x264/x265 semantics and is **not** universal. The UI exposes a canonical model and maps it
per encoder underneath:

```
Rate control
├── Constant quality   (NVENC CQ/CQP · AMF CQP · QSV ICQ · SVT-AV1 CRF-like)
├── Variable bitrate
├── Constant bitrate
└── Lossless
```

Encoders must never be forced to present as "CRF" when they don't use it.

---

## Version roadmap

| Version  | Theme                              | Highlights |
|----------|------------------------------------|-----------|
| `0.1.0`  | Initial MVP                        | NVIDIA/NVENC baseline, portable ZIP artifact. No further large features. |
| `0.2.0`  | Reliability foundation             | MKV as sole recording container; libavformat remux engine (progressive MP4 on stop, faststart); MF/SinkWriter removal; recovery manifest + startup recovery UI; low-disk guard (including remux reserve); filesystem/FAT32 checks; MP4 split via per-segment remux; container compatibility registry. |
| `0.3.0`  | Presence and notifications         | Recording + diagnostics overlays (excluded from capture), tray icon + recording/paused + unread badge, notification toasts (low storage / saved / unexpected stop / recovery available), class-1 countdown overlay, on-screen capture-frame control + opt-in interactive quick-control pill, refined region selection, close-to-tray. *(Shipped. The fullscreen/borderless/exclusive capture matrix was deferred to `0.10.0` — capture-engine reliability work, not presence.)* |
| `0.4.0`  | Crash reporting and updates        | Local-first Crashpad capture + privacy-scrubbed, consent-gated Sentry upload (EU data residency), symbol pipeline; update check with Stable/Preview channels, auto-updater, ed25519 (Monocypher verify) + SHA-256 verification, rollback. *(The original assisted GitHub-issue action was removed from the current crash dialog in 0.9 hardening.)* |
| `0.5.0`  | Settings & media-capability        | TOML config, profile export/import, encoder factory, capability model, compatibility registry, Basic/Advanced/Expert settings, video rate-control/bitrate, audio bitrate, buffers, encoder presets, split time + size, themes/accent, color-pipeline ADR, audio-format ADR. |
| `0.6.0`  | Audio v2                           | Per-track gain, mute, brickwall limiter, mic AGC, optional noise gate / high-pass / RNNoise, PCM, FLAC, channel/sample-format model. |
| `0.7.0`  | HDR and final codec matrix         | Finalize HEVC/AVC/AV1, 8-/10-bit, HDR10, color metadata, P010 compositor path, `hvc1`, MKV/MP4/WebM final matrix, Apple + NLE tests. |
| `0.8.0`  | Diagnostics as a feature           | First-class diagnostics engine (ADR 0033): `FixAction` model, pre-flight readiness gate, low-cost live monitoring (drops/drift/disk-ETA, encoder-vs-capture-vs-disk-bound classification), root-cause correlation (e.g. VRR-vs-CFR judder), incident→check catalog. Post-flight kept minimal (report card; full integrity review handed to 0.9.0). `PresentProvider` interface with PresentMon (tearing/game-present) as an opt-in, elevation-gated provider. The same in-process ETW consumer also powers a **DPC/ISR-latency check** (LatencyMon-style: names the offending kernel driver behind "smooth game, stuttery/crackling recording") — high-value, near-free once the ETW session exists. **Phase-correct CFR frame pacing** (ADR 0035): GPU-only, present-time-aware frame *selection* (not blending) so uncapped VRR / high-refresh sources record to smooth, judder-free 60 fps — pulled forward from 0.10.0; the engine-side fix for the judder that 0.8.0 *diagnoses* (a select control + a `FixAction`; default Smooth; no elevation). |
| `0.9.0`  | Edit / Output / Save               | Quick Trim (stream copy, keyframe-exact), marker display, marker JSON sidecar export (ADR 0042 — container chapters deliberately not written, not merely deferred); Edit/Output/Save surface (ADR 0022) as interactive shell; the "Review" step consumes the post-flight diagnostic report from 0.8.0. |
| `0.10.0` | Reliability hardening (vendor-independent) | Long-recording soak, A/V-sync drift validation, recovery drills, updater/installer/signing/SmartScreen reputation, privacy review, fullscreen/borderless/exclusive capture matrix (deferred from 0.3.0). The vendor-independent half of the former `0.12.x`. The developer harness for the first three (headless `exosnap-soak` tool, the `av-sync-check.py` drift analyzer, and the recovery drill matrix) landed early and is documented in [`docs/dev/soak-and-recovery-drills.md`](dev/soak-and-recovery-drills.md); its thresholds are advisory, not a release gate. |
| `0.11.0` | Software encoding (SVT-AV1 only)   | Optional SVT-AV1, GPU→CPU readback, performance warnings, fallback policy, software capability matrix. x264/HEVC software encoding is **not** bundled by ExoSnap (see ADR 0007, revised 2026-07-23: patent-licensing risk without legal budget to clear it) — if offered at all, it is a post-`1.0`, opt-in detection of a user-supplied FFmpeg install, never built/hosted by ExoSnap. |
| `0.12.0` | AMD hardware                       | Native AMF, hardware test matrix, diagnostics provider, fallback behavior. |
| `0.13.0` | Intel hardware                     | Native oneVPL/QSV, allocator/surface integration, hardware test matrix, diagnostics provider, fallback behavior. |
| `1.0.0`  | First stable release (cross-vendor RC gate) | Cross-vendor matrix + quality-validation matrix (SSIM/VMAF, A/V-sync, long recordings across all vendors) — the vendor-dependent half of RC stabilization. Ships only once these promises are genuinely validated. |

NVIDIA quality-measurement down payment: see [`docs/development/encoder-quality-matrix.md`](development/encoder-quality-matrix.md).

**Prioritization rationale:** an NVIDIA user benefits immediately from reliable recording, recovery,
and visible status. Additional vendor support mainly widens the audience; it does not close a
reliability gap for existing users. So reliability-and-feature work for the existing user — diagnostics
(0.8.0), Edit/Output/Save (0.9.0), and vendor-independent hardening (0.10.0) — comes **before** the
audience-widening vendor/encoder waves. Diagnostics leads because it is the framework every later wave
registers its checks/fix-actions into (each new encoder/vendor multiplies failure modes), and because
its post-flight analysis is exactly what the Edit/Output/Save "Review" step consumes. Software encoding
stays ahead of AMD/Intel because it provides a universal fallback, enables GPU-less testing, eases
later ARM64 work, and catches hardware-init failures.

Two constraints temper this resequence: (1) **Software encoding is the GPU-less CI enabler** — deferring
it to 0.11.0 means the encode path stays NVIDIA-hardware-gated in CI until then (mitigation: the
diagnostics `SelfTestRunner` + a synthetic mini-encode smoke can cover part of it). (2) **The cross-vendor
RC matrix cannot be pulled forward** — only the vendor-independent hardening can (0.10.0); the
cross-vendor quality/compat matrix stays at the 1.0 gate by definition.

---

## Final container / codec / audio matrix (1.0 target)

**This is the target matrix for `1.0`, not the currently-shipped state** — see `KNOWN_LIMITATIONS.md`
for what a given build actually offers (e.g. MP4 audio today is AAC only, MP4 video is H.264 or HEVC
only, and both PCM-in-MP4 and AV1-in-MP4 are deferred; ADR 0030). The UI must only offer **vetted**
combinations — never a theoretically-muxable pairing without a tested player/editor matrix.

| Container       | Video           | Audio                                                |
|-----------------|-----------------|------------------------------------------------------|
| MKV             | AV1, HEVC, AVC  | Opus, AAC, PCM, FLAC                                  |
| MP4             | AV1, HEVC, AVC  | AAC, PCM; Opus only deliberately experimental or not |
| WebM            | AV1             | Opus                                                  |
| WebM (optional) | VP9 (later)     | Opus                                                  |

- **WebM** must not be paired with H.264/HEVC.
- **PCM in MP4** exists in ISO-BMFF but compatibility varies by variant/tool — must be specified as a
  concrete sample-entry/player matrix, not a bare "PCM".
- **FLAC in MP4** is not a `1.0` target (FLAC fits MKV; MP4 compatibility is needlessly fragile).
- **`hvc1` vs `hev1`** matters for HEVC-in-MP4 Apple/QuickTime compatibility (`hvc1` = parameter sets
  in `hvcC`; `hev1` = in-band allowed). libavformat defaults to `hev1`; the remux path must set
  `codec_tag = MKTAG('h','v','c','1')` before `avformat_write_header()`. This must be verified on
  real files (`ffprobe` / Bento4 / MP4Box) in the 0.7.0 HEVC/HDR slice.

### Opus defaults (recording)

`OPUS_APPLICATION_AUDIO`, 20 ms frames, complexity 10 (when CPU budget allows), VBR/constrained VBR,
per-track/channel bitrate. `RESTRICTED_LOWDELAY` and 2.5/5 ms frames are expert-only.

### Chroma / bit depth (capability-gated)

Guaranteed for `1.0`: 4:2:0 8-bit for all final codecs; 4:2:0 10-bit for HEVC/AV1 where supported.
**4:4:4 (8-bit H.264/HEVC, GPU-gated) has already shipped** as an Expert-mode option — see
`KNOWN_LIMITATIONS.md`. 4:2:2 remains a later expert feature pending real hardware tests; no NVENC
generation currently exposes a 4:2:2 encode path.

### Automatic split (time + size)

```
Automatic split
├── Enabled
├── Maximum duration
├── Maximum file size  → "approximately N GB" (honest, not byte-exact)
└── First limit reached wins
```

Both limits optional; manual split stays independent; counters reset per split; size is measured from
committed container bytes (no `file_size()` polling); boundaries stay keyframe-safe; MP4 split
produces one remuxed progressive MP4 per segment.

---

## Cross-cutting foundations

These underpin multiple versions and must not be scattered into UI `if`-chains:

- **Encoder capability & settings schema** — each encoder declares codecs, profiles, levels, bit
  depths, chroma formats, rate-control modes, presets, resolution/FPS limits, HDR/lossless/B-frame/
  lookahead/forced-keyframe support. The UI is generated or validated from this.
- **Color-management architecture** — input/working/output color space, full/limited range, matrix,
  transfer, primaries, tonemapping policy. Precedes HDR and extended chroma.
- **Media compatibility registry** — single source answering: allowed? recommended? experimental?
  fallback? warning? Apple/browser/NLE compatibility?
- **Update security** — signed manifest, package hash, downgrade/rollback protection, no update during
  recording/finalization, no silent auto-restart, portable vs installed distinction, updates off by
  default for self-built binaries, no GitHub token in the client.
- **Disk & filesystem safety** — free-space monitoring, estimated remaining time, configurable warning
  + hard-stop threshold, finalization reserve, split on volume/filesystem limits; FAT32 4 GB limit,
  network/removable drives, path lengths, permissions, mid-recording disappearance.
- **Schema migration** — `RecordingPreset`/TOML versioning, migration, forward-unknown fields,
  export/import, downgrade behavior, secret/privacy fields explicitly excluded.
- **Installer & reputation** — installer/uninstaller, code signing, SmartScreen reputation,
  upgrade/downgrade, settings preservation, portable vs installed.
- **Quality validation matrix** — beyond "it builds": SSIM/VMAF preset comparison, A/V-sync drift,
  long recordings, vendor matrix, HDR metadata, player/NLE + Apple compatibility.
- **Video-encoder backend/codec decoupling** — `IVideoEncoder`/`VideoEncoderFactory` (ADR 0006)
  currently model one backend class per vendor+codec pair. Generalizing the hierarchy so a backend
  declares the codec(s) it supports, instead of one class per pair, is a prerequisite the AMD AMF
  (0.12.0) and Intel QSV (0.13.0) waves need regardless of the x264/HEVC software-encoding
  question (which ADR 0007, revised 2026-07-23, took off ExoSnap's own build entirely — see
  `0.11.0` above). Can start ahead of both AMD/Intel waves.

---

## Remaining work from the 2026-07-11 spec wave

A wave of 18 implementation-ready specs was written 2026-07-11; 9 shipped within a day (device
hotswap resilience, display identity, A/V clock slaving, exclusive-fullscreen detection, the
diagnostics support bundle, the reliability-soak harness, the real Edit-page video preview, the
privacy-review process, and the Preparing-state UI fix) and their specs now live under
`docs/superpowers/specs/2026-07-11-*-spec.md` as shipped-feature records. What's left, roughly by
usefulness:

- **Worth doing soon:** an SSIM/VMAF quality-comparison harness (the gate for any future encoder
  change — dev tooling only), German localization (`tr()` sweep + Qt Linguist, decided for `1.0`),
  and HLG output + tidying the HDR color-metadata path (HDR10 already ships; the transfer-function
  enum value already exists, just unused).
- **Conditional:** an async NVENC pipeline (only if the already-shipped perf-measurement stage
  shows it's needed) and NVENC B-frames/lookahead/temporal-AQ (needs the SSIM/VMAF harness first
  to prove the gain — AV1, the shipped default codec, doesn't support B-frames on most hardware).
  PCM/FLAC-in-MP4 remains a known gap (ADR 0030); 5.1/7.1 audio was deliberately declined, not
  merely deferred.
- **Reach, no urgency:** AMD AMF (`0.12.0`) and Intel QSV (`0.13.0`) widen hardware support without
  closing a gap for existing users. Authenticode code-signing would reduce SmartScreen friction but
  is explicitly deferred (cost, no current budget).
- **Superseded:** x264/HEVC software encoding, per ADR 0007's 2026-07-23 revision (patent-licensing
  risk, no legal budget) — SVT-AV1 stays a live but unprioritized option.

The still-open specs live in `.workspace/plans/` (untracked working notes, not part of this repo's
history) until they're picked up.

---

## Next step

**v0.7.0 — HDR and final codec matrix** *(shipped)*

`0.4.0` (crash reporting + updates), `0.5.0` (settings & media-capability) and `0.6.0` (**Audio v2**)
have shipped. `0.7.0` finalizes the video/codec matrix: HEVC/AVC/AV1, 8-/10-bit, HDR10, color
metadata, the P010 compositor path, `hvc1`, the MKV/MP4/WebM final matrix, and Apple/NLE tests.
Color foundation (ADR 0032), HEVC-in-MKV and **`hvc1`-in-MP4** (both ValidUnvalidated, ADR 0010/0014)
have landed. **10-bit / P010** has also landed (ValidUnvalidated, GPU-verified on NVENC): the
compositor converts BGRA → P010 for HEVC Main10 / AV1 10-bit, the hvcC parses the real SPS for its
profile/bit-depth fields, and `ffprobe` confirms `Main 10` / `yuv420p10le` for MKV and MP4 (with 8-bit
unaffected). 10-bit is **SDR-only** (studio BT.709); HDR transfer/primaries is the next slice. HDR10
and the Settings/preset UI remain, plus HDR verification.

### v0.8.0 — Diagnostics as a feature *(shipped 2026-06)*

Promoted ahead of Software encoding and AMD/Intel (see Prioritization rationale). Detail in **ADR 0033**.
Scope, kept tight to avoid ballooning:

1. **`FixAction` model** — `optional_fix` (today a string) becomes an executable action with a safety
   class (`Auto` / `Assisted` / `External`), `reversible` flag, and a `changes_summary` for
   preview/confirm. `ReconcileCodecs()` already computes "nearest valid combination" — expose it as a button.
2. **Pre-flight readiness gate** — run all blocker+notice checks before record; green/amber/red with fixes.
3. **Live monitoring** — O(1)/frame instrumentation taps, aggregated off-thread at ~1–4 Hz: dropped/
   duplicated frames, A/V drift, disk-fill ETA, encoder-vs-capture-vs-disk-bound classification. No
   per-frame image analysis on the hot path.
4. **Root-cause correlation** — first showcase: VRR/refresh-rate vs CFR-capture judder (extends the
   existing `checkRefreshRateMismatch` from a static config check to a live correlation using DXGI-OD
   `LastPresentTime`/`AccumulatedFrames` + NVAPI VRR state).
5. **Incident→check catalog** — environment/config conditions that recur per user (old driver, low disk,
   FAT32, unsupported codec on this GPU, audio-format mismatch). Explicitly **not** runtime checks for
   already-fixed internal bugs — those are covered by regression tests and would be dead weight.

- **Capture-card live wiring (health-first v1):** the six CAPTURE PIPELINE cards show live status (Healthy/Busy/Bottleneck) + CPU/GPU tag + one secondary number during recording, via a pure `ResolvePipelineHealth` resolver and cheap CPU timing brackets (no GPU timestamp queries).

**Post-flight** is intentionally minimal here (a report card surfacing the already-accumulated live
stats); the full post-flight integrity analysis is the natural content of the 0.9.0 Edit/Output/Save
"Review" step and lands there.

**PresentMon** (Intel, MIT; the engine behind FrameView) is the only robust source of per-game
present-mode / tearing data and slots in as an **opt-in, elevation-gated `PresentProvider`**. The engine
ships valuable diagnosis on the DXGI-OD/NVAPI baseline without it; PresentMon enriches the
window/game-capture path. It is never a hard dependency, and the portable build degrades gracefully when
not elevated. See ADR 0033 for the elevation/anti-cheat posture.

### v0.6.0 — Audio v2 *(shipped)*

The Audio v2 wave landed as CI-green PRs, then the consolidation round completed the
channel/sample-format model and verification:

- **Per-track gain & mute** (#78) — per-source `gain_db` + `muted`, applied in the mixer.
- **Brickwall limiter** (#94, ADR 0023) — a real peak limiter replacing the naive hard-clip;
  default on at 0 dBFS.
- **Mic-DSP chain** — a reusable `MicDspAudioSrc` decorator applying, in order, a **high-pass
  filter** (#95, ADR 0024), **noise gate** (#96, ADR 0025), **AGC** (#97, ADR 0026), and
  **RNNoise** neural suppression (#100, ADR 0029). Every stage defaults OFF, so unaltered capture
  is byte-identical when disabled.
- **Lossless codecs** — **PCM** (`A_PCM/INT/LIT`, #98, ADR 0027) and **FLAC** (libFLAC, `A_FLAC`,
  #99, ADR 0028) in MKV.
- **Channel / sample-format model** (ADR 0030) — first-class `{sample_rate, channels, bit_depth}`:
  vetted 44.1/48/96 kHz, mono/stereo, 16/24/32-bit lossless (FLAC 16/24), plus a configurable
  **FLAC compression level**. Resampling/rematrixing happens once after the mix bus via a new
  `OutputFormatAudioSrc` decorator (libswresample); the default 48 kHz/stereo path is a
  byte-identical no-op. Opus is locked to 48 kHz. Stereo→mono uses an averaging (no-clip) downmix.

Two third-party dependencies (libFLAC, RNNoise) integrate cleanly on MSVC. The preset schema
advanced 7 → 16 across the wave (pre-1.0: reset, no migration).

**Consolidation-round outcomes / notes:**
- **MP4 PCM deferred (Experimental).** Live verification showed the project's libavformat
  (avformat-62) writes the `ipcm` (ISO/IEC 23003-5) sample entry for PCM-in-MP4, which has limited
  player support; per ADR 0030 it was narrowed back to Experimental (not user-selectable) rather
  than shipped silently. MKV remains PCM/FLAC's home. The remuxer stays codec-agnostic so a future
  wave can re-enable it with a broadly-compatible sample-entry mapping + player matrix.
- **RNNoise model mirror.** The model tarball is now fetched from a project-owned GitHub-release
  mirror with upstream (`media.xiph.org`) fallback; the SHA256 guards both. One maintainer-gated
  step remains: publishing the mirror asset (`gh release upload`, documented in
  `third_party/CMakeLists.txt`); builds stay green via the upstream fallback until then.
- **CI discovery-timeout flake** fixed (`DISCOVERY_TIMEOUT 60` in `cmake/exosnap_testing.cmake`).
- **Deferred to a later wave:** more-than-stereo audio (5.1/7.1), float PCM, and PCM/FLAC in MP4.

Earlier waves for reference: the `0.3.0` presence layer shipped capture-excluded overlays
(ADR 0016), tray icon + unread badge, notification toasts, countdown overlay, capture-frame control,
refined region selection, and close-to-tray; the fullscreen/borderless/exclusive capture matrix was
deferred to `0.10.0` (vendor-independent capture-engine hardening).

---

## Production Suite design changes (shipped in feat/production-suite-redesign)

These structural UI decisions took effect before the 0.5.0 release and are now the canonical state:

**Top-level navigation: 6 → 5 items.** Hotkeys was removed as a top-level nav item and embedded as
a full-width card inside Settings (below the two-column grid). The IA is:
`Record · Settings · Diagnostics · Logs · About`. Settings sections: Video · Audio · Output · Webcam
· Hotkeys · Advanced (expert-only, collapsible via SettingsCardExpander).

**Top-level navigation: 5 → 6 items (Device tab, UI-redesign port).** A new `Device` nav item was
added between Record and Settings: `Record · Device · Settings · Diagnostics · Logs · About`. It
hosts the encoder-capability facts that used to sit at the top of Diagnostics — an adapter selector
(one card per DXGI adapter, iGPU/dGPU) and a per-adapter capability matrix (codec support, provenance)
for whichever adapter is selected. Not-yet-wired encoder backends (AMD/AMF, Intel/QSV, software
x264/SVT-AV1) are shown as honest greyed "planned" rows — no fabricated probes. Backend: additive
`capability::EnumerateAdapters()` / `capability::ProbeAdapterEncoderCapability()` (libs/capability);
the existing single-resolved `CapabilitySet` that still drives Settings/Diagnostics/Record is
unchanged. Diagnostics keeps only the live, changeable environment (disk/display/audio/elevation)
as readiness cards; the static "Capability Matrix" section there is unaffected by this slice.

**Top-level navigation: 6 → 5 direct tabs (Device tab removed).** The `Device` nav item was
deleted, leaving `Record · Settings · Diagnostics · Logs · About` as five direct destinations in
the title band. An intermediate step put the last three behind an overflow button; that was
reverted — all five fit at the 860 px minimum window, so the menu bought nothing and cost a click
on the way to Diagnostics, the page a user opens when something is already wrong. Device owned no
user-selectable configuration — selecting an adapter card was, and remains, inspection only — so
its read-only content moved into Diagnostics as a collapsed "Hardware capabilities" section, where
it sits under the rule that Diagnostics owns what ExoSnap *observes* and Settings owns what the
user *chooses*. The "ENCODER BACKENDS — ROADMAP" band (AMD/AMF, Intel QSV, software x264/SVT-AV1)
and the "Backend planned" badge were removed outright: production UI must not present backlog as
capability. No encoder-device selector was added — NVENC opens on the D3D11 device the capture path
creates for the target being recorded (`video_thread.cpp`), so the choice would not be honoured;
see product-spec §2.1.

**Four complete themes → two appearances + a curated accent.** `dark-default`, `dark-indigo`,
`light-paper` and `light-slate` were replaced by **Dark / Light** plus an independently chosen
accent (**Aqua** default, Sky, Violet, Magenta). Each old theme pinned one hue to one set of
neutrals, so picking indigo also meant accepting a different background, and the second light theme
existed mainly because the first could not carry its accent. The persisted `theme_id` is migrated
to the closest `(appearance_id, accent_id)` pair on load by accent hue — `light-paper` → Light +
Sky, because its accent token was petrol blue — and `theme_id` is dropped on the next save; an
unreadable value resolves to Dark + Aqua rather than to nothing. Settings version 20 → 21. The
accent list is deliberately all cool: coral, amber and green are the semantic state colours and are
never derived from, or displaced by, the accent (product-spec §2.2).

**Contrast gate over the appearance × accent matrix.** `quick_theme_contrast.` validates the
resolved tokens for all eight combinations, role by role rather than against one blanket ratio:
WCAG 1.4.3 (4.5:1) for text and for ink on filled accent/error controls, WCAG 1.4.11 (3:1) for the
indicators that identify state (nav underline, active ring, focus ring, state colours), and the
same 3:1 as a *product* floor for unavailable controls — which WCAG exempts outright, but which
product-spec §8 promises stay visible. A resting hairline is deliberately not held to 3:1: it is
separation, not the information that identifies a control.

Six pairs failed and were fixed by moving the responsible token, not the palette. Light `dim`
`#868D9C` → `#798192` (2.74 → 3.22 on the page), light `success` `#1E9E63` → `#1C915B`, light
`caution` `#B5801C` → `#A7761A`, light `error` `#CE4B36` → `#C94631` (its white ink on the filled
Stop pill was 4.48 against the 4.5 bar), dark `dim` `#65656A` → `#67676C` (2.93 → 3.02 on the raised
surface). Hue and saturation are unchanged throughout; only lightness moved. The Preview Toolbar's
format summary moved from `textDim` to `textMuted` — it is live secondary metadata, so it belongs on
a text rung. The locked-on dock state dropped its muted-accent alphas (45 % ring / 60 % icon, which
fell to 1.9:1 on a light dock) and now carries the full accent, saying "not interactive" through the
flat fill instead.

**Record page: context card → Preview Surface.** The separate full-width context card above the
preview was folded into a 38 px Preview Toolbar inside the preview's own border and radius, giving
the page's subject back roughly 70 px of stage. The transport dock's surface relationship was
inverted at the same time — the dock is now the recessed base and its round controls sit on it,
where before a raised dock with darker controls read as holes punched into the bar. Unavailable
controls drop to the dock's fill and carry a reason tooltip built from the adapter's own state
(product-spec §8).

**Preview presentation debt.** `PreviewUpdateScheduler` now tracks whether a published frame has
been followed by a render pass, and `ExoPreviewItem` re-issues exactly one update on the window
lifecycle transitions that drop render requests (expose, screen change, scene-graph
re-initialisation). Without it a window crossing a monitor boundary left the preview frozen until an
unrelated redraw — in practice, until the mouse moved. Producer-driven rendering is unchanged: with
no frame outstanding the re-issue does nothing at all. `EXOSNAP_PREVIEW_TRACE=1` reports the
transitions (AGENTS.md).

**Update-check UI → About overlay.** The UpdateSettingsPanel was moved from the Settings page into
the About overlay (PS-PHASE-E). It is no longer reachable from Settings. ADR 0012 (update security
model) is unchanged; only the UI placement changed.

**Notification Hub.** A persistent in-app notification layer sits alongside toast notifications
(ADR 0016). The tray badge counts unread items; the hub panel (accessible via the bell icon in the
title bar) lists them persistently. Architecture: `NotificationHub` singleton; toasts fire-and-forget
via `INotificationService`; hub entries survive until dismissed. See ADR 0016 for the on-screen
overlay architecture context.

**Edit / Output / Save surface (ADR 0022).** A post-stop surface (`EditExportPage`) is an in-window
mode (not a tab or a separate dialog): after recording stops the stack switches to the Edit/Export
view, and Back returns to Record. Three phases (Review → Edit → Output) are stepped. The engine
implementation shipped in 0.9.0: keyframe-accurate lossless Quick Trim, stream-copy remux, real
decoded preview playback (`EditPlayerSession`), and the marker JSON sidecar (ADR 0042 — container
chapters are a deliberate non-goal, not a deferral; see that ADR).

**Settings Expert split.** The Quality card gained an Expert section (CQ · VBR · CBR rate control,
bitrate, frame-timing — ADR 0009 mapping) hidden behind the Expert toggle. Audio likewise has an
context). As of the 0.6.0 Audio v2 wave the Expert audio section gained real controls for the
brickwall limiter (ADR 0023), high-pass filter (0024), noise gate (0025), AGC (0026), and RNNoise
(0029); PCM (0027) and FLAC (0028) are real audio-codec options in the Output card. The remaining
v1.0-placeholder rows (chroma subsampling 0.7, HEVC codec 0.7, bit depth 0.7, HDR10 0.7) are shown
to communicate the roadmap without enabling unimplemented controls.
