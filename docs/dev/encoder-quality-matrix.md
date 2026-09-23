# Encoder-quality measurement

This developer workflow measures NVENC quality per bitrate through ExoSnap's actual encoder path. It does not add a product feature or establish universal quality claims. The encoder configuration contract is in [encoding and containers](../architecture/encoding-and-containers.md).

## Prerequisites and reference clips

Use an NVIDIA NVENC GPU, `probe_encode_file` and a full system FFmpeg with `libvmaf`. The application's small shared FFmpeg build is not the external analysis tool.

```powershell
ffmpeg -filters | Select-String libvmaf
cmake --preset windows-x64-debug -DEXOSNAP_BUILD_PROBES=ON
cmake --build build/windows-x64-debug --target probe_encode_file --config Debug
```

Use representative 8-bit 4:2:0 Y4M clips, normally 10–30 seconds each: fast motion, slow/stable content and desktop/text scrolling. Keep the exact reference bytes and the same scored interval across comparisons. A synthetic pattern can test the instrument but is not the complete product workload.

```powershell
ffmpeg -i '<recording>.mkv' -ss '<start>' -t '<duration>' -pix_fmt yuv420p -vf scale=1920:1080 '<reference>.y4m'
```

A harness-enabled ExoSnap can capture a high-quality reference using `--auto-record --cq 1`. That is still an encoded reference, not a mathematically lossless ground truth. Record its provenance and residual reference loss when interpreting results. Keep reference capture outside the normal user output directory.

## Run and qualify the measurement

```powershell
python scripts/dev/encoder_quality_matrix.py --metric-sanity --clip desktop-scroll.y4m
python scripts/dev/encoder_quality_matrix.py --clip desktop-scroll.y4m --vcodec av1 --output '<evidence directory>/av1-desktop'
```

Repeat for H.264/HEVC and each reference. The baseline sweep compares P4/P7 under CQ/VBR at four rate-control points. `--presets`, `--cq-values` and `--vbr-values` intentionally change that selection; do not compare differently selected matrices as though only the encoder changed.

Metric sanity constructs identity, mild/severe degradation and a one-frame temporal shift. Identity must rank above progressively degraded candidates, and the shifted sequence must not look equivalent. Run it when the scoring path or reference changes. A tool that produces a number is not necessarily measuring aligned frames.

Two normalization rules are load-bearing:

- Pair frames by index, with matching timestamps, rather than letting container timestamp quantization shift frame pairing.
- Normalize color descriptions on both inputs so FFmpeg does not insert a conversion on only one side and score that conversion instead of encoder loss.

Preserve frame count, FFmpeg/libvmaf versions, model, command, source/encoded file hashes, GPU/driver and probe binary identity in the run evidence. The generated report does not automatically capture every host/probe fact; add missing facts to the evidence, not a permanent historical report under `docs/`.

## Read the result

Read mean and tail statistics together: median, p10, p5, p1, worst-1%-mean and minimum. Screen/text content can saturate a mean or median while a short scrolling interval visibly degrades. Minimum is sensitive to one outlier; worst-1%-mean summarizes a short bad interval. Use at least 200 frames for a meaningful percentile tail rather than treating a single frame as an independent distribution.

VMAF is a relative ranking within the same clip and qualified harness, not an absolute screen-quality certification. Keep SSIM/PSNR and visual inspection alongside it. Do not infer one codec's universal superiority from one clip, one GPU or unmatched bitrate/latency points.

`bd_rate()` in `scripts/dev/encoder_quality_matrix.py` compares two bitrate/quality curves. Its cubic fit requires exactly four points per curve and rejects other counts. Negative bitrate delta means fewer bits at equal measured quality. The fit and overlap must be meaningful; extrapolating disjoint or ill-conditioned curves is not useful evidence.

The script writes CSV and Markdown beneath the chosen output prefix. Keep those results in untracked evidence or attached review/release artifacts. Durable docs hold the procedure and any current default's rationale, not dated benchmark campaigns.

## Gate for changing a shipped encoder-quality default

For the full reference set, a proposed default must improve median BD-rate by at least 5% in the target rate-control mode, regress no individual clip by more than 2%, and keep p99 encode latency inside the target frame budget. At 60 fps the nominal frame interval is about 16.67 ms; reserve practical pipeline headroom rather than spending the entire interval on encode alone.

These are explicit project acceptance criteria, not physical constants. Revisit them deliberately with evidence. A preset increase, spatial/temporal AQ flag or deeper asynchronous queue does not earn a default change without measured user benefit and reliable output.

The current workflow does not establish 10-bit/HDR quality, every advanced SDK tuning feature or cross-vendor equivalence. Add the relevant reference formats and hardware before making those claims. [Soak testing](soak-and-recovery-drills.md) independently checks endurance/synchronization; a quality sweep is not a reliability gate.
