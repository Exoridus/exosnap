# Soak, A/V synchronization and recovery drills

This runbook operates the endurance, signal-analysis and interruption tools. [Recording pipeline](../architecture/recording-pipeline.md) and [timing](../architecture/timing-and-cfr.md) own the system contracts. The [release checklist](../release-checklist.md#long-duration-audiocapture-gate) owns mandatory acceptance thresholds; other tool budgets are diagnostic unless explicitly adopted there.

## Engine soak

`exosnap-soak` drives `RecorderSession`, samples engine/process metrics into JSONL and writes a report. A separate engine tool avoids mixing frontend state into an engine endurance measurement. A full-product acceptance run instead controls the official executable through [Live Verify](live-verify.md).

```powershell
cmake --preset windows-x64-debug
cmake --build build/windows-x64-debug --config Debug --target exosnap-soak
exosnap-soak --minutes 120 --vcodec av1 --acodec opus --container mkv --out '<scratch>/run.mkv' --report-dir '<scratch>'
```

Real capture needs NVIDIA NVENC and a suitable display/audio environment. Use repeatable source content, stable power/display settings and adequate output storage. Stop by the configured duration or Ctrl+C for graceful finalization. Exit 0 is a completed run, 1 session failure, 2 target/validation failure and 3 a diagnostic abort budget. Inspect the report rather than equating process exit with release eligibility.

Samples include capture/emission/drop/duplicate counters, A/V drift availability, duration skew, audio discontinuities, mux queue, disk ETA, memory/handles and pipeline health. Reports aggregate percentiles and memory/handle slopes. Evaluate sustained growth after a baseline window, not a one-sample working-set fluctuation. Record the output volume; flush cost and host load can affect results.

The synthetic form exercises real audio encode/mux/finalize/report plumbing with deterministic video input:

```powershell
exosnap-soak --synthetic --seconds 60 --realtime --out '<scratch>/synthetic.mkv'
```

It cannot measure physical device-clock drift, hardware capture loss or real GPU resource growth. It qualifies the instrument and deterministic pipeline behavior, never hardware A/V acceptance.

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
python scripts/dev/av-sync-check.py '<recording>' --max-drift-ms 20 --expected-markers 20
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

The committed golden clip and generator under `tests/fixtures/av-sync` / `scripts/dev/gen-av-sync-fixture.py` test the analysis path against a declared synthetic timeline. That guards the analyzer, not a user's hardware synchronization.

## Release soak inspection

For the durations/profile/device setup and numeric acceptance budgets, follow the release checklist. Inspect every audio stream's packet span and sample continuity, not only optional container duration tags or one track's metadata. Listen at multiple points and compare the session report to the independently inspected file.

Silent gap filling preserves elapsed time but cannot recreate missing sound. Judge discontinuities by lost duration and maximum gap, not count alone. Slaving within its envelope can remove sustained rate error; saturation beyond the cap can leave growing residual. A report showing unavailable drift must not be read as zero.

Keep report JSON, timeline, stimulus schedule, analyzer output, exact binary/media/tool identities and environment facts together outside durable docs. Changed binaries or materially changed environments need new evidence.

## Recovery drills

Exercise recording, finalization and remux phases under ordered stop, process kill and controlled power-loss simulation. Use scratch data and a disposable VM for destructive interruption, never a normal user's ongoing work.

Deterministic recovery tests use real synthetic-pipeline Matroska files and truncation. They establish that repair returns safely, preserves original data on failure and salvages complete clusters where possible. Truncating a file is a missing-tail model, not proof of a physical storage controller's flush behavior or the coordinator's entire live manifest choreography.

For live process-kill drills, interrupt the actual app in each phase, relaunch and inspect the recovery offer and Finish result. Check exact original format/destination, atomic final publication and valuable partial preservation. Verify missing/zero-byte artifacts are not offered indefinitely. A manifest-write failure must be disclosed and cannot create a nonexistent recovery guarantee.

Remux/repair writes a sibling temporary on the destination volume, then atomically replaces the final path only after success. During a running remux, the final path must not hold a partially published result. Cancel/failure preserves the source; a completed MP4 session may have a retained edit master, which is distinct from an unfinished recovery artifact.

A controlled forced-off VM can exercise OS interruption, but a real storage/power-loss claim needs evidence for that environment. The durability model includes flush interval, reorder/buffering and complete-cluster boundaries; it is not a universal promise that every last second of a recording survives power loss.
