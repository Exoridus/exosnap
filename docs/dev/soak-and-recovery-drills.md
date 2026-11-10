# Soak, A/V synchronization and recovery drills

This runbook operates the endurance, signal-analysis and interruption tools. [Recording pipeline](../architecture/recording-pipeline.md) and [timing](../architecture/timing-and-cfr.md) own the system contracts. The [release checklist](../release-checklist.md#long-duration-audiocapture-gate) owns the change-triggered selection criteria and acceptance thresholds when selected; other tool budgets are diagnostic unless explicitly adopted there.

## Engine soak

`exosnap-soak` drives `RecorderSession`, samples engine/process metrics into JSONL and writes a report. A separate engine tool avoids mixing frontend state into an engine endurance measurement. A full-product acceptance run instead controls the official executable through [Live Verify](live-verify.md).

```powershell
cmake --preset windows-x64-debug
cmake --build build/windows-x64-debug --config Debug --target exosnap-soak
exosnap-soak --minutes 120 --vcodec av1 --acodec opus --container mkv --out '<scratch>/run.mkv' --report-dir '<scratch>'
```

Real capture needs NVIDIA NVENC and a suitable display/audio environment. Use repeatable source content, stable power/display settings and adequate output storage. Stop by the configured duration or Ctrl+C for graceful finalization. Exit 0 requires a successful session, readable nonempty output and successfully written evidence. Exit 1 is session failure, 2 target/output/evidence failure and 3 a diagnostic abort budget. Inspect the report rather than equating process exit with release eligibility. Closing a console window is an OS termination event and does not prove that finalization completed.

Use MKV for engine-only integrity drills. The application owns the MP4 delivery remux from its transient Matroska source. Calling `RecorderSession` directly does not qualify that final-publication path. Exercise MP4 through the official application and Live Verify.

Samples include capture/emission/drop/duplicate counters, A/V drift availability, duration skew, audio discontinuities, mux queue, disk ETA, memory/handles and pipeline health. Reports aggregate percentiles and memory/handle slopes. Evaluate sustained growth after a baseline window, not a one-sample working-set fluctuation. Record the output volume; flush cost and host load can affect results.

The synthetic form exercises real audio encode/mux/finalize/report plumbing with deterministic video input:

```powershell
exosnap-soak --synthetic --seconds 60 --realtime --out '<scratch>/synthetic.mkv'
```

The synthetic executable still needs the Windows build. Its video packets are structural fixtures, not decodable captured video. The built-in file probe establishes demuxability and clean EOF only. It cannot measure physical device-clock drift, hardware capture loss or real GPU resource growth. It qualifies the instrument and deterministic pipeline behavior, never hardware A/V acceptance. Its PCM generator retains one borrowed packet rather than preallocating the entire future recording.

### Deterministic output failures

Inject a failure at a selected visit to an existing production writer boundary:

```powershell
exosnap-soak --synthetic --seconds 10 --output-fault trailer --fault-after 1 --out '<scratch>/trailer.mkv' --report-dir '<scratch>'
```

`--output-fault` accepts `payload`, `cluster`, `trailer`, `flush`, `close` and `durability`. `--fault-after` is a positive, one-based occurrence at that boundary. The point and occurrence define the replay completely. This schedule has no random seed or scheduler dependency. It records the matching visit count and whether the fault fired. A requested fault that never fires is evidence failure, not a successful negative test.

These controls belong to the test utility and use the existing `SessionState` writer failure callback. They do not add a product environment variable or change production recording defaults. Each run must use its own scratch path. They model failed operations, not physical disk removal, power loss, a driver reset or the timing of a partial storage-controller write.

Payload, cluster, trailer, CRT flush and close failures must remain visible to the recording result. An expected injected session failure still returns exit 1. The test orchestrator must assert the expected error and inspect retained media; it must not require exit 0. Durability flush follows the writer's existing best-effort contract: inspect its failure counter separately rather than assuming it is a fatal write error. A nonempty retained file is not automatically a complete or decodable recording.

The native synthetic suite checks the trailer failure through `AudioThread`, `MuxThread` and the real writer and checks that a sampler abort reaches the running session. The portable suite tests the actual schedule, sampler and stop control, but cannot execute those Windows owners.

### Profiles and test tiers

| Profile | Requested wall time | Required interpretation |
|---|---|---|
| Short | 10-30 seconds | Functional stop, report and media checks |
| Medium | 2-10 minutes | Early backpressure and resource trends |
| Extended | 30-60 minutes | Independently inspected streams and drift reference |
| Long | 2-4 hours | Hardware recording with a qualified A/V stimulus |
| Soak | 8 hours or more | Sustained growth and rare failures on a controlled host |

Use `--realtime` for a synthetic wall-time profile. An accelerated media timeline is not an equally long endurance run. Retain exact requested and observed duration, source/build identity, OS, driver, device configuration, codec/container, repeat count, output validation level and unmeasured metrics. Do not relabel a partial run as completion of its requested profile.

| Tier | Entry points | Scheduling and coverage |
|---|---|---|
| 1 | Portable synchronization/control CTest target, normal native non-live tests, deterministic sample regressions | Existing PR lanes; no added full Windows matrix |
| 2 | Repeated focused concurrency tests, native synthetic writer failures, media fixtures, session churn | Targeted or periodic runs on an available developer host |
| 3 | Real `exosnap-soak`, official-app Live Verify, physical devices and OS transitions | Manually controlled hardware runs; never inferred from Tier 1 or 2 |

The portable target is independently configurable:

```text
cmake -S libs/engine/tests/portable -B build/concurrency-portable -DCMAKE_BUILD_TYPE=Debug
cmake --build build/concurrency-portable
ctest --test-dir build/concurrency-portable --output-on-failure
```

The existing Linux Clang ThreadSanitizer lane also covers these controls. A normal build without `EXOSNAP_TSAN=ON` is not sanitizer evidence. Native worker tests still require the Windows build and the normal repository test runner.

Use the existing same-process DXGI lifecycle command for churn, with a unique output directory:

```powershell
cargo exo-verify dxgi-lifecycle --product '<official-executable>' --out '<scratch>/churn' --cycles 100 --same-process
```

This covers its declared presentation lifecycle, not hundreds of completed recordings. Recording churn must additionally start, stop, validate each output and return to Ready between sessions through Live Verify. Record per-cycle stop time and post-cleanup resource samples. Do not infer a recording churn pass from process exit or a swapchain cycle count.

### Output evidence levels

Report each level separately: file existence, successful demux, every A/V stream decoded, temporally consistent streams, and completeness against an independently declared recording interval. A clean EOF does not establish all five. Check stream count and codec parameters, packet DTS in decode order, decoded-frame presentation order, frame/sample counts, per-stream spans and gaps, and each track's end position. Do not sort timestamps before checking ordering. Packet PTS may reorder in valid B-frame video.

Use an external FFmpeg/FFprobe build for full decode. Decode every audio and video stream explicitly, including nondefault tracks, and retain the exit status and errors. An expected duration must come from the scenario or observed recording interval before finalization, not the same container duration being tested. Timestamp agreement alone does not establish content alignment; use the clapper analysis below for the decodable output.

The existing sample runner qualifies the media oracles independently:

```powershell
cargo exo-verify samples run 'media.*' --out '<scratch>/media-samples.json'
```

Passing generated fixtures qualifies the inspector for those fixtures. It is not a measurement of ExoSnap capture, NVENC, device recovery or hour-scale A/V drift.

## A/V clapper stimulus

The clapper emits a full-frame flash on the primary monitor and a beep. Capture that display with SYS loopback. It uses a separate stimulus process and no ExoSnap single-instance ownership. It is inherently visible/audible; coordinate it before running.

For a long run, declare the full schedule including margins:

```powershell
exosnap-soak --clapper --seconds 7200 --markers 20 --start-margin-seconds 10 --end-margin-seconds 10
```

`--print-clapper-schedule` prints a schedule without emitting it. Counts start at two, but at least three markers are needed for a qualified fitted-drift verdict. More can be necessary at a tight budget. Invalid zero/negative/nonnumeric/overflow controls are usage failures, not default schedules.

A microphone clap observed by a webcam is a separate source/capture path. Do not treat SYS loopback's emission timing as calibration for that path. HDR tone-mapping also changes the video stimulus and should be judged separately.

## Analyze and qualify the reference

Use a full external FFmpeg providing the analysis filters, not ExoSnap's bundled component set. Pin the tool version for an acceptance run.

```powershell
cargo exo-dev av-sync-check '<recording>' --max-drift-ms 20 --expected-markers 20
```

For a known schedule, also supply `--marker-times-seconds` with the exact comma-separated times printed by the stimulus. Auto mode without an expected schedule only accepts its bounded small marker set; extra disturbances must not silently become favorable pairs.

For each marker, the analyzer estimates `offset = flash PTS - beep PTS`, then fits offset against time using all markers and their uncertainty. **Fitted slope × observed span** is the drift quantity. Start/end difference, middle offsets and opposing segment drifts remain diagnostic outputs, not substitutes for the fit.

The reference must qualify before a budget verdict:

| Requirement | Why |
|---|---|
| At least three markers | Two points always form a line and cannot expose nonlinearity |
| Drift uncertainty no greater than the allowed budget fraction | A noisy estimate cannot distinguish acceptable drift from measurement noise |
| Residuals compatible with edge uncertainty | A nonlinear offset is not represented by one trustworthy drift rate |

Exit 0 means measured within budget, 2 over budget and 3 unmeasurable, including an unqualified reference. `--unqualified-reference` is diagnostic only and is not accepted for release qualification.

A longer recording alone does not improve total-drift uncertainty: improved rate precision is multiplied by the longer span. For evenly spaced markers with independent edge uncertainty σ, the drift uncertainty scales as `σ × sqrt(12(n−1)/(n(n+1)))`. More/sharper markers help. Let the analyzer's measured edge quality determine whether the chosen count is sufficient rather than permanently assuming three markers prove a 20 ms budget.

Absolute A/V offset includes the stimulus setup's display/audio emission skew. Report it, but the drift verdict concerns change over time, not an uncalibrated intercept. Large opposing segments are a reliability finding even when endpoint differences cancel.

The committed golden clip under `tests/samples/media` and `cargo exo-dev gen-av-sync-fixture` test the analysis path against a declared synthetic timeline. That guards the analyzer, not a user's hardware synchronization.

## Release soak inspection

The [v0.11 independent-clock and runtime obligations](../release-checklist.md#mandatory-v011-hardware-acceptance) are RELEASE-BLOCKING - NOT YET QUALIFIED. Use this infrastructure for the required 2-3-hour independent SYS/MIC recording and qualified 20 ms fitted-drift analysis. `audio.mixed-clock-soak` currently runs 30 minutes and cannot close that longer reference gate alone. Retain audio discontinuity/longest-gap, degradation/loss, resampler-drain, mux and comparable background-load observations with exact binary/hardware/OS/driver/tool/media identities. Physical recovery and rate variants follow the established change-triggered policy. Simulated clocks and one-source eight-track fan-out remain separately attributed.

For the durations/profile/device setup and numeric acceptance budgets, follow the release checklist. Inspect every audio stream's packet span and sample continuity, not only optional container duration tags or one track's metadata. Listen at multiple points and compare the session report to the independently inspected file.

Silent gap filling preserves elapsed time but cannot recreate missing sound. Judge discontinuities by lost duration and maximum gap, not count alone. Slaving within its envelope can remove sustained rate error; saturation beyond the cap can leave growing residual. A report showing unavailable drift must not be read as zero.

Keep report JSON, timeline, stimulus schedule, analyzer output, exact binary/media/tool identities and environment facts together outside durable docs. Changed binaries or materially changed environments need new evidence.

## Recovery drills

Exercise recording, finalization and remux phases under ordered stop, process kill and controlled power-loss simulation. Use scratch data and a disposable VM for destructive interruption, never a normal user's ongoing work.

Deterministic recovery tests use real synthetic-pipeline Matroska files and truncation. They establish that repair returns safely, preserves original data on failure and salvages complete clusters where possible. Truncating a file is a missing-tail model, not proof of a physical storage controller's flush behavior or the coordinator's entire live manifest choreography.

For live process-kill drills, interrupt the actual app in each phase, relaunch and inspect the recovery offer and Finish result. Check exact original format/destination, atomic final publication and valuable partial preservation. Verify missing/zero-byte artifacts are not offered indefinitely. A manifest-write failure must be disclosed and cannot create a nonexistent recovery guarantee.

Remux/repair writes a sibling temporary on the destination volume, then atomically replaces the final path only after success. During a running remux, the final path must not hold a partially published result. Cancel/failure preserves the source; a completed MP4 session may have a retained edit master, which is distinct from an unfinished recovery artifact.

A controlled forced-off VM can exercise OS interruption, but a real storage/power-loss claim needs evidence for that environment. The durability model includes flush interval, reorder/buffering and complete-cluster boundaries; it is not a universal promise that every last second of a recording survives power loss.
