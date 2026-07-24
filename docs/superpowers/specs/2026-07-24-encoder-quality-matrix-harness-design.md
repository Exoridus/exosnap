# Encoder Quality Matrix Harness — Design

Status: approved (brainstorming), 2026-07-24. Not yet implemented.

## Problem

NVENC exposes quality-relevant encode options ExoSnap doesn't yet use or tune with any
evidence: B-frames, lookahead, and temporal AQ (capability detection for these already
shipped — the app can tell whether a GPU/codec supports them — but nothing in the product
turns them on or picks values for them). Enabling any of them, or changing an existing
default (preset, CQ value), without an objective before/after measurement would be a guess.
There is currently no way to answer "does this actually improve quality per bit, and does it
cost more than it's worth" for any encoder-quality change.

This harness is the measurement infrastructure needed to make that call — for the specific
features already capability-probed, and for any future NVENC-quality-affecting change. It
produces the evidence a later feature decision (turning on B-frames/lookahead, or changing a
default) would need to cite. It ships nothing user-visible on its own.

## Non-goals

- No in-app quality measurement and no VMAF dependency in the shipped product — the metric
  side runs entirely through an external, developer-machine `ffmpeg` (full build with
  `libvmaf`), never vendored or shipped.
- No CI execution — the harness needs real NVIDIA hardware and a local `ffmpeg` with
  `libvmaf`; CI stays limited to the pure-function unit tests (Y4M parsing, I420→NV12,
  IVF framing, BD-rate math).
- No 10-bit/HDR clip matrix in this pass — 8-bit only; 10-bit/P010 is a follow-up once the
  8-bit workflow is proven out.
- No multipass/`lookaheadLevel`/UHQ tuning — those are newer SDK features with no measurement
  basis yet; speculative without this harness existing first.
- Does not itself decide whether to enable any NVENC feature — it produces the numbers; the
  gate rule below (already defined) decides.

## Gate rule (context, already decided — not part of this build)

A future encoder-quality change may become a shipped default when, over the reference clip
set: median BD-rate improves by at least 5% in the target rate-control mode, no single clip
regresses by more than 2% BD-rate, and p99 encode latency still stays inside the frame budget
(e.g. 60 fps → under ~16 ms). This harness is what produces the numbers that rule is checked
against; it doesn't implement the rule itself.

## Architecture

Three independent pieces, all dev-only, none touching the shipped product build:

1. **`tools/probes/probe_encode_file`** (new C++ probe, same pattern as the existing
   `tools/probes/*` — never packaged). Reads a Y4M reference file and drives it through the
   real `NvencEncoder` — the same construction and setter sequence `video_thread.cpp` uses —
   so what gets measured is exactly the product's own encode path, not a synthetic
   reimplementation of it. Writes a raw elementary stream (Annex-B for H.264/HEVC, IVF for
   AV1); no muxing, so `ffmpeg` can decode it directly without the probe needing any
   container/codec-private logic.
2. **`scripts/dev/encoder_quality_matrix.py`** (new). Takes a clip set (Y4M files) and a
   matrix definition (which preset/rate-control points to sweep), runs `probe_encode_file`
   per cell, then `ffmpeg -lavfi libvmaf=...;ssim=...` (plus PSNR) against the Y4M reference,
   collects bitrate from the output file size, computes BD-rate, and writes a CSV plus a
   Markdown summary. Every cell is measured under both CQ (the product default) and VBR,
   since lookahead/AQ gains mostly show up under bitrate-targeted rate control — a CQ-only
   result would be misleading. Logs `ffmpeg -version`, driver version, GPU name, and the probe
   binary's commit hash into the result file for reproducibility.
3. **`docs/development/encoder-quality-matrix.md`** (new, tracked). The workflow reference:
   where to get an `ffmpeg` build with `libvmaf` (a `gyan.dev` full build already has it — the
   two `ffmpeg` builds already installed on this dev machine both qualify, confirmed during
   design), the clip-set convention and how to (re)generate one, how to invoke the matrix
   script, where results are filed (`docs/development/quality-results/<date>-<gpu>.md`), and
   the gate rule above. This doc is the NVIDIA-only piece of a larger, cross-vendor 1.0
   quality gate the roadmap already reserves — it's a down payment on that, not the gate
   itself, and the roadmap's 1.0 line will link here.

## Clip-set

Sourced from existing test recordings, not recorded fresh: a documented, repeatable `ffmpeg`
decode command (in the workflow doc, not a one-off manual step) converts a chosen source
recording into a reference Y4M clip. Categories per the existing plan: fast and slow
gameplay, and a desktop/text-scroll clip, each roughly 10–30 seconds. Because the conversion
command is documented and repeatable, the clip set can be regenerated or extended later
without needing this exact session's help.

## Data flow

```
existing test recording (.mkv)
  → ffmpeg decode → reference Y4M (documented, repeatable command)
  → probe_encode_file --vcodec X --preset Y --rc Z ... → elementary stream
  → ffmpeg -lavfi libvmaf/ssim/psnr (elementary stream vs. reference Y4M) → metrics
  → encoder_quality_matrix.py aggregates across all cells → BD-rate computation
  → CSV + Markdown report (with ffmpeg version / driver / GPU / probe commit hash)
```

## Error handling

- Y4M parser: rejects any chroma format/bit depth outside `C420`/`C420jpeg`/`C420mpeg2` 8-bit
  with a clear message — no silent fallback or best-effort guess.
- `probe_encode_file`: if a requested encoder setter (e.g. lookahead) isn't actually applied
  by the driver, it's reported honestly in the probe's output, not silently dropped.
- Matrix script: aborts immediately with a clear message if the `ffmpeg` on `PATH` lacks
  `libvmaf`, instead of surfacing a cryptic `ffmpeg` filter error partway through a run.
- BD-rate computation needs at least 4 rate-control points for its log-interpolation; fewer
  points is a hard error, not a silently-degraded estimate.

## Testing

- Pure functions — Y4M parsing, I420→NV12 conversion, IVF framing, BD-rate computation — get
  unit tests that run in CI without a GPU, matching the project's existing pattern (e.g. the
  event-drain and slot-scan policies from the NVENC async work).
- `encoder_quality_matrix.py` gets a `--self-test` mode for the BD-rate math against known
  reference values, matching the existing `analyze-encode-perf.py` convention (stdlib only,
  no numpy/pandas).
- The actual GPU/NVENC encode-and-measure path is dev-only and manual, like every other
  live-hardware verification in this project — documented, not run in CI.
