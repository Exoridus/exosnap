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
| `0.1.0`  | Current MVP                        | NVIDIA/NVENC baseline, portable ZIP artifact. No further large features. |
| `0.2.0`  | Reliability foundation             | MKV as sole recording container; libavformat remux engine (progressive MP4 on stop, faststart); MF/SinkWriter removal; recovery manifest + startup recovery UI; low-disk guard (including remux reserve); filesystem/FAT32 checks; MP4 split via per-segment remux; container compatibility registry. |
| `0.3.0`  | Presence and notifications         | Recording overlay (excluded from capture), tray icon + recording/paused badge, notification center with unread badge, Windows toasts (low storage / saved / unexpected stop / recovery available), fullscreen/borderless/exclusive matrix. |
| `0.4.0`  | Crash reporting and updates        | Crashpad, separate report dialog, privacy scrubbing, opt-in upload, symbol pipeline + backend; update check with Stable/Preview channels, auto-updater, hash/signature verification, rollback. *(Only pull crash reporting forward once backend, privacy, and symbol hosting actually exist.)* |
| `0.5.0`  | Settings & media-capability        | TOML config, profile export/import, encoder factory, capability model, compatibility registry, Basic/Advanced/Expert settings, video rate-control/bitrate, audio bitrate, buffers, encoder presets, split time + size, themes/accent, color-pipeline ADR, audio-format ADR. |
| `0.6.0`  | Audio v2                           | Per-track gain, mute, brickwall limiter, mic AGC, optional noise gate / high-pass / RNNoise, PCM, FLAC, channel/sample-format model. |
| `0.7.0`  | HDR and final codec matrix         | Finalize HEVC/AVC/AV1, 8-/10-bit, HDR10, color metadata, P010 compositor path, `hvc1`, MKV/MP4/WebM final matrix, Apple + NLE tests. |
| `0.8.0`  | Software encoding                  | x264, optional SVT-AV1, GPU→CPU readback, performance warnings, fallback policy, software capability matrix. |
| `0.9.0`  | AMD hardware                       | Native AMF, hardware test matrix, diagnostics, fallback behavior. |
| `0.10.0` | Intel hardware                     | Native oneVPL/QSV, allocator/surface integration, hardware test matrix, diagnostics, fallback behavior. |
| `0.11.0` | Quick Trim & markers               | Quick Trim (stream copy), marker display, chapter export, optional post-recording transcription. |
| `0.12.x` | RC stabilization                   | Long recordings, recovery drills, all GPU vendors, fullscreen modes, A/V sync, updater, installer, signing, privacy review, compatibility matrix. |
| `1.0.0`  | First stable release               | Only once these promises are genuinely validated. |

**Prioritization rationale:** an NVIDIA user benefits immediately from reliable recording, recovery,
and visible status. Additional vendor support mainly widens the audience; it does not close a
reliability gap for existing users. Software encoding stays ahead of AMD/Intel because it provides a
universal fallback, enables GPU-less testing, eases later ARM64 work, and catches hardware-init
failures.

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

**v0.2.0 — wave 1: libavformat remux engine + reliability foundation**

The first post-0.1.0 work slice is the reliability foundation described in the 0.2.0 row above.
The backend strategy is decided: ADR 0014 (remux-first, no fMP4 recording writer) is accepted.
MKV is the sole recording container; libavformat is the remux/trim engine. Implementation of the
remux engine and the MF/SinkWriter removal is the next active work slice.
