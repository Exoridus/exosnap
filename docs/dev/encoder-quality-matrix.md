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

Scoring sets libvmaf's thread count to the available CPU count minus two, with a minimum of one. The campaign records this budget as scoring identity. It does not change the model, sampled frames or quality gates.

Two normalization rules are load-bearing:

- Pair frames by index, with matching timestamps, rather than letting container timestamp quantization shift frame pairing.
- Normalize color descriptions on both inputs so FFmpeg does not insert a conversion on only one side and score that conversion instead of encoder loss.

Preserve frame count, FFmpeg/libvmaf versions, model, command, source/encoded file hashes, GPU/driver and probe binary identity in the run evidence. The generated report does not automatically capture every host/probe fact; add missing facts to the evidence, not a permanent historical report under `docs/`.

## Read the result

Read mean and tail statistics together: median, p10, p5, p1, worst-1%-mean and minimum. Screen/text content can saturate a mean or median while a short scrolling interval visibly degrades. Minimum is sensitive to one outlier; worst-1%-mean summarizes a short bad interval. Use at least 200 frames for a meaningful percentile tail rather than treating a single frame as an independent distribution.

VMAF is a relative ranking within the same clip and qualified harness, not an absolute screen-quality certification. Keep SSIM/PSNR and visual inspection alongside it. Do not infer one codec's universal superiority from one clip, one GPU or unmatched bitrate/latency points.

`exo-dev encoder-quality-matrix`'s BD-rate compares two bitrate/quality curves. It uses shape-preserving PCHIP interpolation of natural-log actual bitrate against quality, with exactly four measured points per curve. The analytic piecewise integral uses only the common quality interval; no extrapolation is permitted. Negative bitrate delta means fewer bits at equal measured quality. Non-finite values, non-positive rates, duplicate quality and numerically insufficient overlap are rejected. Points are sorted by quality without removing noisy measurements. Non-monotonic measured rates are preserved locally and flagged. The overlap must be meaningful; saturated curves and small overlap remain limitations even when interpolation succeeds. The slope construction follows [SciPy PchipInterpolator](https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.PchipInterpolator.html), validated against independently calculated reference values.

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

## Resumable advanced-tuning campaigns

`exo-dev encoder-quality-campaign --manifest <inputs.json> --output <local evidence root>` runs the fixed P4 advanced-tuning curves without changing the historical matrix. It interleaves BASE and candidates at each CQ/bitrate point, continues independent failed cells and preserves each attempt. Reusing the output root resumes completed cells only when source, probe, references, scoring tools and tuning identities match. A different identity requires a new root. The source tree must be committed and clean, and the frozen Release probe needs a JSON build receipt containing `head`, `configuration` and `probe_sha256`.

The input manifest contains absolute local `probe`, `build_receipt`, `ffmpeg` and `ffprobe` paths, `nvenc_sdk`, `libvmaf`, `model`, `reserve_gib` and `clips`. The model is `vmaf_v0.6.1 (FFmpeg embedded default)`. Each clip contains `name`, `class` (`gameplay`, `stable` or `desktop`), `path`, `provenance`, `source_path`, `source_sha256`, `production` and `qualified`. Use `qualified: false` when the clip does not honestly establish its required content class or provenance. A partial reference qualification blocks a default verdict while retaining useful measurements. Optional `editor_probe` selects the existing headless `probe_edit_playback`; optional `archive` selects the final evidence destination.

The campaign records probe TIMING as encoder service cost including slot waits. ENCODER_LATENCY records submit-to-packet residency separately. The existing frame-budget gate protects realtime service, not the intentional residence of buffered pictures. The campaign uses an 8 ms service screen to reserve over half of a 60 fps frame interval. This is a conservative measurement screen, not a replacement project acceptance threshold or proof of live recording performance.

Only the best qualifying or narrowly borderline candidate per codec/rate-control mode receives a second independent curve and alternating BASE curve. Both phases must preserve the quality and service conclusion. Confirmed quality candidates then receive production MKV samples, H.264/HEVC MP4 delivery, full decode, packet timestamp inspection, seek and headless editor checks. Missing compatibility evidence leaves the result inconclusive. No defaults change automatically.

The runner requests process-scoped system wakefulness and restores it on exit. It holds the existing host device lock, estimates retained-media space at 100 Mbps per cell and checks a 60 GiB reserve between cells. Successful local media remain retained. The compact archive includes reports, complete per-frame metrics, hashes, logs, CQ24 BASE samples, failed media and winner compatibility samples. Every copied file is SHA-256 verified before archive success is reported. References and prior evidence are never deleted.
`exo-dev encoder-quality-campaign --manifest <inputs.json> --output <existing campaign root> --reanalyze` recomputes saved curves without running the main matrix. Analysis outputs go into an identity-specific `analyses/pchip-*` directory. The analysis identity records the method, analysis source/dirty state, runner hash, hashes of input cell results and the unchanged raw measurement environment. Historical analysis is retained separately and cannot drive the corrected verdict. Per-reference evidence includes overlap, monotonicity flags, relative curve difference, measured tail regressions and log-rate/RCD plots. Duplicate-quality points are rejected rather than merged. Common overlap must exceed `1e-9 * max(1, abs(overlap endpoints))` to avoid a numerically meaningless integration interval; this is numerical validation, not a new quality threshold.

Add `--confirm-qualified` only to run missing winner-confirmation curves for at most the two best qualifying candidates per codec/rate-control pair. This never launches main cells. Complete saved confirmation curves are reused with their original measurement identity. The frozen probe, references, scoring executable, thread budget and GPU/driver must match before additional measurements. Analysis-only confirmation does not establish container/editor compatibility and does not change defaults. No qualifying candidate means no additional encodes.
