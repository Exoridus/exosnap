# Encoder quality matrix: workflow

Dev-only tooling to objectively measure NVENC quality-per-bitrate (SSIM/VMAF/BD-rate)
against ExoSnap's real encoder code path. Nothing here ships in the product — see
`tools/probes/probe_encode_file` and `scripts/dev/encoder_quality_matrix.py`.

This is the NVIDIA-only piece of a larger, cross-vendor 1.0 quality gate the roadmap
reserves (`docs/roadmap.md`) — a down payment on that gate, not the gate itself.

## Prerequisites

- An NVIDIA GPU with NVENC (the same requirement as the product).
- An `ffmpeg` build with `libvmaf` compiled in. A full build from
  <https://www.gyan.dev/ffmpeg/builds/> (or any build whose `ffmpeg -filters` output lists
  `libvmaf`) works — no ExoSnap-specific patches needed. Verify with:

  ```bash
  ffmpeg -filters | grep libvmaf
  ```

  If nothing prints, get a different build; the matrix script checks this itself and refuses
  to run otherwise.
- A local build of `probe_encode_file`: `cmake --preset windows-x64-debug -DEXOSNAP_BUILD_PROBES=ON`
  then `cmake --build build/windows-x64-debug --target probe_encode_file --config Debug`.

## Reference clip set

Y4M (8-bit 4:2:0) clips, 10-30 seconds each, covering:

- Fast gameplay (high motion, frequent scene changes)
- Slow gameplay (low motion, stable scenes)
- Desktop / text-scroll (sharp edges, low motion, the case most sensitive to blocking)

Source clips from real recordings, not synthetic test patterns, so results reflect actual
usage. Convert a section of any existing recording:

```bash
ffmpeg -i <recording>.mkv -ss <start> -t <duration> -pix_fmt yuv420p -vf scale=1920:1080 <name>.y4m
```

This command is the whole clip-set-generation "tool" — repeatable any time a new or better
reference clip is needed; there is no separate script.

## Running the matrix

Per codec, per clip:

```bash
python scripts/dev/encoder_quality_matrix.py \
    --clip desktop-scroll.y4m \
    --vcodec av1 \
    --output docs/development/quality-results/2026-07-24-rtx5070ti-av1-desktop-scroll
```

Repeat for `--vcodec h264`/`hevc` and for each clip. Each run sweeps P4 and P7, each under CQ
(the product default) and VBR, at 4 rate-control points — enough for a BD-rate curve fit.
Writes `<output>.csv` (raw data) and `<output>.md` (human-readable table).

## Result storage

File results under `docs/development/quality-results/<date>-<gpu>-<codec>-<clip>.md` (matching
the `--output` path above) — tracked in git so results are diffable across runs. The `<gpu>` in
the filename is manual: the generated report logs the `ffmpeg` version but not the GPU name,
driver version, or the probe binary's commit hash, so note those in the filename or a line at the
top of the `.md` before committing a result if the comparison will ever cross hardware or driver
versions.

## Computing BD-rate between two runs

`bd_rate()` in `scripts/dev/encoder_quality_matrix.py` takes two (bitrate, VMAF) curves — the
baseline and the candidate — and returns the percent bitrate delta at equal quality (negative =
candidate is better). Each curve must be **exactly 4** (bitrate, VMAF) points — matching the
4 CQ / 4 VBR points `default_matrix()` sweeps per preset — and raises `ValueError` for any other
count; the fit-quality check that catches ill-conditioned input only holds at exactly 4 points
(see `bd_rate`'s and `_polyfit3`'s docstrings for why). Import it directly from a small script, or
extend `encoder_quality_matrix.py` with a `--compare` mode when a concrete before/after comparison
is needed (not built speculatively here — kept out of scope for this measurement-only pass, not
comparison tooling).

## Gate rule for shipping an encoder-quality change

A future encoder-quality change (enabling B-frames/lookahead/temporal AQ, or changing an
existing default) may become a shipped default when, over the full reference clip set:

1. Median BD-rate improves by at least 5% in the target rate-control mode, and
2. No single clip regresses by more than 2% BD-rate, and
3. p99 encode latency (measurement infrastructure already shipped) still stays inside the
   frame budget for the target configuration (60 fps -> under ~16 ms).

These thresholds are a deliberate, revisable choice, not a law of nature — changing them is
cheap; not having any threshold at all would be the real mistake.

## What this does not cover

- 10-bit/HDR clips (8-bit only in this pass).
- Multipass/`lookaheadLevel`/UHQ tuning (no measurement basis yet for these newer SDK
  features).
- Cross-vendor comparison (AMD/Intel) — that is the separate, later 1.0 roadmap gate this
  harness is a down payment on.
