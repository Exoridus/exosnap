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

## Recording a reference clip with the product itself

`--auto-record --cq <n>` takes a recording at a canonical CQ the shipped ladder
does not offer, which is what a reference clip needs: at CQ 1 the encode is close
enough to lossless that the measurement is about the candidate rather than about
the reference. Everything else about the run is the normal harness path.

```
exosnap.exe --auto-record --enable-preview --target monitor --duration 30             --frame-rate 60 --cq 1 --container mkv --video-codec av1             --audio-codec opus --chroma 420 --bit-depth 8 --hdr off --audio-rows sys
```

`--auto-record` is only compiled into a Release build configured with
`EXOSNAP_BUILD_BENCHMARK_HARNESS=ON`; a Debug build always has it. Point
`EXOSNAP_OUTPUT_DIR` at a scratch directory — a reference clip is a large file
and never belongs in the user's output folder.

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

Sweep selection is optional: `--presets`, `--cq-values` and `--vbr-values` replace the baseline
sweep when an exploration needs different points. Omit all three and the baseline matrix runs
unchanged, so the invocation above keeps producing the same cells and the 4-point BD-rate contract
below still holds.

## Trusting the numbers before trusting the encoder

```bash
python scripts/dev/encoder_quality_matrix.py --metric-sanity --clip desktop-scroll.y4m
```

Builds four candidates from the clip whose ordering is known in advance — a lossless copy, a mildly
and a severely degraded encode, and the clip shifted by one frame — and verifies that VMAF, SSIM and
PSNR all rank them `identity > mild > severe` with the shifted copy far below identity. Run it after
any change to the scoring path, and on a new clip before a sweep it will be used for. It carries no
absolute thresholds: those depend on the clip, and pinning them turns a content change into a false
failure.

Two things the scoring path does unconditionally, both of which this suite exists to keep honest:

- **Frames are paired by index**, by re-stamping both inputs. Metric filters pair by presentation
  time, and a muxed candidate carries container timestamps a raw Y4M reference does not — Matroska
  quantises to its 1 ms timecode scale, so 60 fps lands on 0/16/33/50 ms while the Y4M sits on exact
  1/60 s. Left alone, a bit-exact lossless copy scores PSNR-Y 21 dB with a third of its frames at
  VMAF 0.
- **Colour descriptions are normalised on both inputs.** The encodes carry one (the encoder writes
  BT.709 into the bitstream); a Y4M reference does not. ffmpeg then auto-inserts a colour conversion
  on one input only and the metric scores that conversion — measured at 14 dB of PSNR-Y and 4.3 VMAF
  on a 1440p60 AV1 encode whose pixels were untouched, which is more than enough to invert a
  comparison between two encoders.

Reports name the ffmpeg version, the libvmaf version, the VMAF model and the scored frame count, and
list VMAF **median, p10, p5, p1, worst-1%-mean and minimum** next to the mean. On screen content the
mean hides the answer: over a scrolling small-text clip the median stays at exactly 100.0000 across
an entire CQ sweep while p10 travels 8 points, and over a *real* browser scroll even p10 pegs at
100.0000 and only the extreme tail moves. Absolute VMAF is not a screen-quality grade — only the
ordering within one clip and one harness is.

**Which tail statistic to read.** `min` finds the single worst frame and is the most sensitive, but a
single frame is also the easiest thing to move by an outlier. `worst-1%-mean` averages the worst
percentile instead, which keeps a short visible scroll or rasterizer failure legible without resting
a verdict on one frame. The two coincide on windows shorter than 200 frames, where one per cent is
one frame — sweep at least 200 frames when the tail is what decides.

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
