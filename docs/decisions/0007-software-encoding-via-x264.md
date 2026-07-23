# ADR 0007: Software Encoding via x264 (and Optional SVT-AV1)

## Status

**Superseded (2026-07-23).** ExoSnap's own build stays hardware-encoder-only for the
foreseeable future. x264/HEVC software encoding, if ever offered, is a user-supplied FFmpeg
install detected at runtime — not bundled, not built, not distributed by ExoSnap. See "2026-07-23
revision" below for the reasoning.

## Context (original, 2026-06)

ExoSnap needs a software H.264 encoder for three reasons:

1. **Universal fallback** — users without supported hardware (or whose GPU encoder is busy/failed)
   can still record.
2. **GPU-less testing** — CI and development environments without a GPU must be able to run the
   encode path without hardware stubs.
3. **ARM64 readiness** — a future ARM64 port has no NVENC/AMF/QSV; software encoding provides an
   initial working baseline.
4. **Hardware-init failure detection** — the software path catches regressions that GPU-only builds
   would silently mask.

The candidate software H.264 encoders are x264 and the FFmpeg internal encoder (`libx264` via
`libavcodec`). SVT-AV1 is the candidate for software AV1.

## Original decision (2026-06, superseded — kept for history)

### x264 for software H.264

x264 (`GPL-2.0-or-later`) is the software H.264 encoder. It is integrated behind the
`IVideoEncoder` interface as `X264VideoEncoder` (see ADR 0006).

x264 is not wired directly into the video thread or called from any UI layer. It is constructed
exclusively through `VideoEncoderFactory` and hidden behind `IVideoEncoder`. This keeps the
distribution/license gate centralized.

**License and patent gate:** A license + patent-distribution audit must be completed and signed off
before any release binary ships x264. x264's GPL-2.0-or-later license is compatible with
ExoSnap's GPL model, but patent licensing for H.264 distribution requires explicit review. The
audit gate is enforced in the release pipeline, not deferred to runtime.

### SVT-AV1 for software AV1 (optional)

SVT-AV1 is an optional software AV1 encoder, integrated as `SvtAv1VideoEncoder`. It is opt-in at
build time and disabled by default until the performance and binary-size impact is characterized.
AV1 patent licensing is confirmed royalty-free (AOM patent pool), so no additional patent audit is
required.

### Ordering rationale: software before AMD/Intel

The roadmap placed software encoding ahead of AMD AMF and Intel QSV:

- Software encoding closes the universal-fallback gap for all users immediately.
- AMD/Intel hardware encoders widen the audience but do not close a reliability gap for existing
  NVIDIA users.
- The software path validates the `IVideoEncoder`/`VideoEncoderFactory` interface under conditions
  not covered by NVENC alone.
- CI and ARM64 testing become possible without GPU access.

## 2026-07-23 revision: why the license/patent gate is not being cleared

The "license + patent-distribution audit" gate above was never scoped as a piece of work ExoSnap
could realistically do without paid legal counsel — and there is currently no budget for that.
Open-source web research (not a legal opinion) surfaced enough to change the plan:

- **AVC (H.264) vs. HEVC (H.265) are structurally different.** H.264 has one patent pool (Via LA)
  with a documented 100,000-units/year royalty-free tier. HEVC is split across at least two active
  pools (MPEG LA, which has a comparable 100k/year tier; Access Advance/HEVC Advance, whose public
  FAQ shows no equivalent free tier) plus historically unpooled patents — meaningfully harder to
  fully clear than H.264 alone. "Units" in these license structures count distributed
  copies/products, not encode operations.
- **"We only call a vendor SDK" is not a clean legal shield, contrary to a common assumption.**
  NVENC/AMF/QSV all encode the same patented AVC/HEVC bitstreams. The working assumption that a
  GPU vendor's own hardware-encoder license already covers software that merely calls their SDK
  is *not* confirmed for all three vendors — Intel's own public support documentation for Quick
  Sync explicitly states Intel does **not** hold an H.264 patent license for its processors and
  that license compliance is the developer's/OEM's responsibility. This means the same theoretical
  exposure ExoSnap is trying to avoid for x264/x265 already exists, in principle, for the NVENC
  path ExoSnap has shipped since the `0.1.0` MVP — reverting x264/x265 does not create a new
  problem, and keeping NVENC does not avoid the old one.
- **The distinction that matters in practice is who is a plausible enforcement target, not who is
  theoretically exposed.** Every mainstream recording/streaming tool (OBS, Bandicam, NVIDIA's own
  ShadowPlay, Xbox Game Bar, …) ships hardware-accelerated AVC/HEVC without an individual patent
  license, for 15+ years, without known enforcement against the software maker specifically.
  Known German enforcement cases (Nokia v. Lenovo over H.264, Broadcom v. Netflix over HEVC at LG
  München) target large commercial distributors with real per-unit revenue, not small free tools.
  Compiling and shipping one's own software encoder (x264/x265) pulls a project into a smaller,
  more specifically-scrutinized category (this is exactly why Debian/Fedora/Ubuntu keep compiled
  `libx264` out of their official repositories, even though they ship FFmpeg with hardware
  backends by default) — a materially different risk posture than staying in the "calls a vendor
  SDK" cohort ExoSnap is already in via NVENC.
- **A click-through license/EULA in front of a download ExoSnap itself hosts does not transfer
  patent-distribution liability.** Liability follows whoever performs the distribution act; an end
  user's "I agree" cannot retroactively make ExoSnap not the distributor of a file ExoSnap served.
  The pattern that *does* meaningfully change the picture is dynamic loading of a genuinely
  third-party-sourced FFmpeg build (BtbN, gyan.dev, ffmpeg.org, or similar) that the user acquires
  themselves — ExoSnap never compiles, hosts, or mirrors the patent-encumbered binary. This is the
  same distinction Debian/RPM Fusion and Fedora already draw between their own repositories and
  third-party ones for exactly this reason.

## Decision (current)

- **ExoSnap's own vendored FFmpeg build stays hardware-encoder-only.** No `libx264`/`libx265` is
  compiled into or shipped with any ExoSnap release. An `r6` build of `exosnap-ffmpeg-build`
  briefly added them (2026-07-23) as a build-capability proof; the CMake pin was reverted to `r5`
  (LGPL-2.1-or-later, no GPL code) the same day once the patent-risk implications were clear. The
  `r6` tag remains available in the build repo (immutable release tags are never deleted) but is
  not referenced by ExoSnap's build.
- **`X264VideoEncoder`/HEVC-software-encoder wiring into `IVideoEncoder`/`VideoEncoderFactory` is
  not planned.** No formal license/patent audit is scheduled; there is no budget for it at this
  project's current stage.
- **If software H.264/HEVC encoding is ever offered, it is via runtime detection of a
  user-supplied FFmpeg installation** — the user points ExoSnap at (or ExoSnap passively detects)
  an FFmpeg build they acquired themselves from an independent third party, dynamically loaded
  (not statically linked at ExoSnap's build time). ExoSnap never compiles, bundles, hosts, or
  auto-downloads the patent-encumbered binary itself. A capability probe confirms `libx264`/
  `libx265` availability and ABI/version compatibility before offering the option; if absent,
  ExoSnap shows an informational message pointing at where to get one — no in-app auto-downloader.
  This is deliberately post-`1.0` scope: no current release depends on it, and it is only worth
  building once there is real user demand.
- **SVT-AV1 remains unaffected by this revision** — AV1 patent licensing is confirmed royalty-free
  (AOM patent pool), so the concerns above do not apply. It stays a build-time opt-in, disabled by
  default until performance/binary-size impact is characterized, whenever that work happens.
- **NVENC/AMF/QSV are unaffected** — this ADR does not change ExoSnap's existing/planned use of
  vendor hardware encoders (ADR 0006). The theoretical exposure discussed above already exists for
  NVENC today; it is accepted as-is, consistent with how the entire hardware-accelerated
  recording/streaming software category operates. Re-litigating that is out of scope for this
  revision.

## Consequences

- ExoSnap ships no software H.264/HEVC encoder in any first-party binary, now or in the currently
  planned roadmap.
- The `0.11.0` "Software encoding" roadmap wave no longer describes a bundled feature; see
  `docs/roadmap.md` for the updated framing.
- Any future move to bundle x264/x265 directly (reversing this decision) requires a real license/
  patent audit first — this ADR's 2026-07-23 reasoning is a research starting point, not a legal
  opinion, and should not be treated as clearance on its own.
- GPU → CPU readback (NV12 or BGRA frame copy) for hardware encoders is unaffected by this
  revision; it remains an expected cost, not a pipeline defect (unchanged from the original
  decision).
