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
cargo exo-dev encoder-quality-matrix --metric-sanity --clip desktop-scroll.y4m
cargo exo-dev encoder-quality-matrix --clip desktop-scroll.y4m --vcodec av1 --output '<evidence directory>/av1-desktop'
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

`exo-dev encoder-quality-matrix`'s BD-rate compares two bitrate/quality curves. Its cubic fit requires exactly four points per curve and rejects other counts. Negative bitrate delta means fewer bits at equal measured quality. The fit and overlap must be meaningful; extrapolating disjoint or ill-conditioned curves is not useful evidence.

The command writes CSV and Markdown beneath the chosen output prefix. Keep those results in untracked evidence or attached review/release artifacts. Durable docs hold the procedure and any current default's rationale, not dated benchmark campaigns.

## Gate for changing a shipped encoder-quality default

For the full reference set, a proposed default must improve median BD-rate by at least 5% in the target rate-control mode, regress no individual clip by more than 2%, and keep p99 encode latency inside the target frame budget. At 60 fps the nominal frame interval is about 16.67 ms; reserve practical pipeline headroom rather than spending the entire interval on encode alone.

These are explicit project acceptance criteria, not physical constants. Revisit them deliberately with evidence. A preset increase, spatial/temporal AQ flag or deeper asynchronous queue does not earn a default change without measured user benefit and reliable output.

The current workflow does not establish 10-bit/HDR quality, every advanced SDK tuning feature or cross-vendor equivalence. Add the relevant reference formats and hardware before making those claims. [Soak testing](soak-and-recovery-drills.md) independently checks endurance/synchronization; a quality sweep is not a reliability gate.

## Advanced-feature scouts

`probe_encode_file` applies explicit `--bframes`, `--b-ref off|each|middle`, `--lookahead`, `--lookahead-depth`, `--spatial-aq`, `--temporal-aq` and `--multipass single|quarter|full` through the production NVENC wrapper. Invalid codec/hardware combinations fail at encoder initialization. `--capabilities` prints authoritative per-adapter facts without encoding. Input slot count comes from the configured backend, not a probe constant. The resolved summary records actual features, input slots and output depth. The backlog summary and submitted/encoded/drained count check expose incomplete EOS drain. TIMING measures encoder service cost including slot waits, excluding reference reads and CPU uploads. It does not measure a live capture tick or desktop presentation.

The same advanced flags are available on `exo-dev encoder-quality-matrix` and apply identically to every cell in a run. Use one output prefix per feature variant. Select P4 with a small explicit point set for scouting, then use four points only for candidates that warrant BD-rate confirmation. The baseline invocation remains unchanged. CQ spatial AQ remains opt-in and has not earned a default recommendation. Multipass is meaningful for VBR/CBR and is rejected for CQ. Use the probe directly for targeted CBR compatibility and timing checks.

The matrix preserves each probe log and encoded file in its artifact directory. `<output>.evidence.json` records requested tuning, reference/probe/encoded SHA-256 digests and artifact paths. The normal report includes FFmpeg/libvmaf versions and quality tails. Add actual GPU, driver and SDK identity to the evidence. Requested flags alone never establish what ran.

`probe_encode_file --mkv-output <path>` also writes the packets through the production Matroska writer, including actual codec-private extraction and decode-order interleaving. Inspect this file with ffprobe and full FFmpeg decode, then exercise production MP4 delivery, seeks, trim and split boundaries. Elementary-stream scoring alone cannot prove recording-container compatibility.

Feature availability and default policy are separate decisions. Hardware support plus correct engine/mux behavior can qualify an Expert control. Changing defaults still requires the full representative-set gate above. If motion, stable content and scrolling text references are unavailable, report the default measurement as blocked and retain conservative defaults.
`probe_encode_file --vfr` separately exercises VFR encoder initialization. The quality matrix uses the reference clip's CFR cadence; it does not measure static-source VFR capture behavior.
