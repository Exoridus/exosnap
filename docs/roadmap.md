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
| `0.3.0`  | Presence and notifications         | Recording + diagnostics overlays (excluded from capture), tray icon + recording/paused + unread badge, notification toasts (low storage / saved / unexpected stop / recovery available), class-1 countdown overlay, on-screen capture-frame control + opt-in interactive quick-control pill, refined region selection, close-to-tray. *(Shipped. The fullscreen/borderless/exclusive capture matrix was deferred to `0.12.x` — capture-engine reliability work, not presence.)* |
| `0.4.0`  | Crash reporting and updates        | Local-first Crashpad capture + privacy-scrubbed, consent-gated delivery — Stage 0 (assisted GitHub issue) and Stage 1 (automated opt-in upload to Sentry, EU data residency), symbol pipeline; update check with Stable/Preview channels, auto-updater, ed25519 (Monocypher verify) + SHA-256 verification, rollback. |
| `0.5.0`  | Settings & media-capability        | TOML config, profile export/import, encoder factory, capability model, compatibility registry, Basic/Advanced/Expert settings, video rate-control/bitrate, audio bitrate, buffers, encoder presets, split time + size, themes/accent, color-pipeline ADR, audio-format ADR. |
| `0.6.0`  | Audio v2                           | Per-track gain, mute, brickwall limiter, mic AGC, optional noise gate / high-pass / RNNoise, PCM, FLAC, channel/sample-format model. |
| `0.7.0`  | HDR and final codec matrix         | Finalize HEVC/AVC/AV1, 8-/10-bit, HDR10, color metadata, P010 compositor path, `hvc1`, MKV/MP4/WebM final matrix, Apple + NLE tests. |
| `0.8.0`  | Diagnostics as a feature           | First-class diagnostics engine (ADR 0033): `FixAction` model, pre-flight readiness gate, low-cost live monitoring (drops/drift/disk-ETA, encoder-vs-capture-vs-disk-bound classification), root-cause correlation (e.g. VRR-vs-CFR judder), incident→check catalog. Post-flight kept minimal (report card; full integrity review handed to 0.9.0). `PresentProvider` interface with PresentMon (tearing/game-present) as an opt-in, elevation-gated provider. |
| `0.9.0`  | Edit / Output / Save               | Quick Trim (stream copy), marker display, chapter export; Edit/Output/Save surface (ADR 0022) as interactive shell; the "Review" step consumes the post-flight diagnostic report from 0.8.0. |
| `0.10.0` | Reliability hardening (vendor-independent) | Long-recording soak, A/V-sync drift validation, recovery drills, updater/installer/signing/SmartScreen reputation, privacy review, fullscreen/borderless/exclusive capture matrix (deferred from 0.3.0). The vendor-independent half of the former `0.12.x`. |
| `0.11.0` | Software encoding                  | x264, optional SVT-AV1, GPU→CPU readback, performance warnings, fallback policy, software capability matrix. Universal fallback + GPU-less CI enabler. |
| `0.12.0` | AMD hardware                       | Native AMF, hardware test matrix, diagnostics provider, fallback behavior. |
| `0.13.0` | Intel hardware                     | Native oneVPL/QSV, allocator/surface integration, hardware test matrix, diagnostics provider, fallback behavior. |
| `1.0.0`  | First stable release (cross-vendor RC gate) | Cross-vendor matrix + quality-validation matrix (SSIM/VMAF, A/V-sync, long recordings across all vendors) — the vendor-dependent half of RC stabilization. Ships only once these promises are genuinely validated. |

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

## Final container / codec / audio matrix

The UI must only offer **vetted** combinations — never a theoretically-muxable pairing without a
tested player/editor matrix.

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
4:2:2 / 4:4:4 are later expert features pending real hardware tests.

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

---

## Next step

**v0.7.0 — HDR and final codec matrix** *(in progress)*

`0.4.0` (crash reporting + updates), `0.5.0` (settings & media-capability) and `0.6.0` (**Audio v2**)
have shipped. `0.7.0` finalizes the video/codec matrix: HEVC/AVC/AV1, 8-/10-bit, HDR10, color
metadata, the P010 compositor path, `hvc1`, the MKV/MP4/WebM final matrix, and Apple/NLE tests.
Color foundation (ADR 0032) and HEVC-in-MKV (ValidUnvalidated, ADR 0010) have landed; 10-bit/P010,
HDR10, `hvc1`-in-MP4 and the Settings/preset UI remain, plus GPU/HDR verification.

### v0.8.0 — Diagnostics as a feature *(next after 0.7.0; resequenced 2026-06)*

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
- **Lossless codecs** — **PCM** (`A_PCM/INT_LIT`, #98, ADR 0027) and **FLAC** (libFLAC, `A_FLAC`,
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
deferred to `0.12.x` (RC stabilization).

---

## Production Suite design changes (shipped in feat/production-suite-redesign)

These structural UI decisions took effect before the 0.5.0 release and are now the canonical state:

**Top-level navigation: 6 → 5 items.** Hotkeys was removed as a top-level nav item and embedded as
a full-width card inside Settings (below the two-column grid). The IA is:
`Record · Settings · Diagnostics · Logs · About`. Settings sections: Video · Audio · Output · Webcam
· Hotkeys · Advanced (expert-only, collapsible via SettingsCardExpander).

**Update-check UI → About overlay.** The UpdateSettingsPanel was moved from the Settings page into
the About overlay (PS-PHASE-E). It is no longer reachable from Settings. ADR 0012 (update security
model) is unchanged; only the UI placement changed.

**Notification Hub.** A persistent in-app notification layer sits alongside toast notifications
(ADR 0016). The tray badge counts unread items; the hub panel (accessible via the bell icon in the
title bar) lists them persistently. Architecture: `NotificationHub` singleton; toasts fire-and-forget
via `INotificationService`; hub entries survive until dismissed. See ADR 0016 for the on-screen
overlay architecture context.

**Edit / Output / Save surface (0.11 wave, ADR 0022).** A new post-stop surface (`EditExportPage`)
is an in-window mode (not a tab or a separate dialog): after recording stops the stack switches to
the Edit/Export view, and Back returns to Record. Three phases (Review → Edit → Output) are stepped;
the engine implementation (Quick Trim, stream-copy remux, chapter export) is deferred to 0.11. The
surface ships as a UI shell + placeholder banner in the Production Suite wave.

**Settings Expert split.** The Quality card gained an Expert section (CQ · VBR · CBR rate control,
bitrate, frame-timing — ADR 0009 mapping) hidden behind the Expert toggle. Audio likewise has an
context). As of the 0.6.0 Audio v2 wave the Expert audio section gained real controls for the
brickwall limiter (ADR 0023), high-pass filter (0024), noise gate (0025), AGC (0026), and RNNoise
(0029); PCM (0027) and FLAC (0028) are real audio-codec options in the Output card. The remaining
v1.0-placeholder rows (chroma subsampling 0.7, HEVC codec 0.7, bit depth 0.7, HDR10 0.7) are shown
to communicate the roadmap without enabling unimplemented controls.
