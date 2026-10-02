# Timing, clocks and frame pacing

This document owns media epochs, frame selection, pause accounting, sample timelines and drift measurement. The [recording pipeline](recording-pipeline.md) owns resources and container lifetime.

## Three different clocks

| Clock | Meaning |
|---|---|
| QPC | Monotonic capture/session reference; not wall-clock calendar time |
| Video media timeline | CFR frame index and interval, or VFR source timing, with session/pause normalization |
| Audio sample timeline | Accumulated output samples at the encoded sample rate |

The live session timer is not finished-file duration. It can include paused wall time. Completed media duration excludes paused time and finalization/remux work. Split-session duration is the sum of media segments. Do not derive one from the other just because both are displayed as a timer.

A first frame may carry a present timestamp from before recording started. Normalize/clamp the epoch rather than adding an artificial lead-in or negative time. Audio PTS zero is published from the path that first supplies the encoder timeline, including a recording that opens on synthetic silence. Waiting for the first audible packet would lose the placement race.

## CFR selection, not interpolation

CFR schedules output slots on QPC. It can repeat the held picture when the source is quiet, and discard surplus source frames when capture is faster than the configured output. These operations preserve cadence; they do not synthesize intermediate motion or guarantee smooth motion from an irregular source.

Phase-correct display capture keeps a bounded GPU texture ring and chooses by source presentation time rather than simply taking the newest frame at each tick. Ring sizing accounts for source/output-rate ratio with a bounded allocation (4–12, with a fallback where refresh is unknown). Selection never moves backward through source timestamps. Resource pressure can fall back to the supported non-ring path rather than fail an otherwise viable recording.

Nearest-time selection and newest-frame selection trade phase regularity against latency. The recording schedule and capture producer lifetime remain independent. A preview render or GUI timer must not pace the encoder.

A system that cannot meet the schedule needs bounded catch-up. The worker cannot replay an unbounded queue of past ticks or compress elapsed time by renumbering a late submission as an earlier one. Late-slot skipping and encoder pressure are accounted as real loss where appropriate.

## Pacing outcome measurements

The recording worker records scheduled CFR slots, selected DXGI present times, signed and absolute residuals, selected frame age, worker deadline lateness, submission cadence, duplicate runs and ring occupancy/misses. Fixed-capacity rolling windows provide p50/p95/p99 without per-frame allocations or synchronous GPU readback. Session totals and worst values survive window expiry; session generation resets them. Resume clears cadence/wake windows so a deliberate pause is not scheduler damage.

A fresh selection displaced by more than a full output period, or an explicitly skipped output slot, is coarse measured pacing impact. Sub-period motion quality has no calibrated warning threshold yet. Startup pictures predating the recording epoch and expected held frames (including 30-to-60 CFR) do not count as affected slots or encoder loss. Selection residuals require present timestamps; WGC/newest paths leave them unavailable. Submission cadence measures worker delivery to the encoder, not container PTS variation. Worker lateness includes scheduling and preceding recorder work; it does not prove CPU starvation by itself.

## Drop semantics

Real picture loss includes skipped output slots under backpressure, conversion/processing failure and undrained encoded output. A slot acquisition retry is pressure, not a lost frame. Phase-ring eviction is retained as source-selection evidence; it does not itself mean an output picture was lost. Deliberate source coalescing and empty CFR slots before any first frame exists are separate benign counters. Every product surface must use the same real-drop definition.

A held-frame duplicate is not necessarily a defect. A static desktop legitimately produces no new frame. Likewise an emitted rate at target does not prove that capture progressed: a stalled source can be duplicated at 60 fps indefinitely. Stall diagnosis reads capture progress and relevant environment facts, not the output rate alone.

## Audio clocks

WASAPI device-position/QPC pairs measure clock drift against the video reference. Normalize the baseline at capture start and smooth packet observations. Queue depth, encoder latency and guessed stream duration are not substitutes for this measurement.

Positive reported drift means audio leads video. The compensation layer reports its **actually applied** sample-count adjustment. Residual drift is raw device-clock drift minus that applied adjustment, not an integral of a requested control value.

Multi-source merged tracks have no single device clock and report drift unavailable. A single gain-adjusted source can forward its original timing through a mixer and remain measurable. Implausible device observations latch a measurement fault; they must not silently reappear as healthy measurements.

### Shared mixed-track intervals

Multi-source packets are normalized to 48 kHz stereo and assigned QPC sample positions before summation. A 30 ms arrival horizon bounds waiting for quiet or missing sources. Every emitted interval has one output position, including silence; late packets cannot append an already emitted interval. A bounded timestamp-tagged ring holds source data. Source mute changes amplitude, not placement. Rejoin uses the packet clock rather than the amount of queued data. Stop drains normalization tails to the latest occupied interval without adding another horizon.

The mixer owns silence during multi-source outages. The outer audio worker must not add wall-clock silence a second time. Presentation QPC remains available independently of device-drift measurement; several device clocks must not be reported as one physical clock.

### Clock slaving

Clock slaving is enabled by default and uses the existing output-format resampler, not a second resampler or periodic PTS jumps. Its fixed controller uses measured rate feed-forward plus proportional correction of residual error:

`requested_ppm = measured_rate_ppm + residual_ms / 60 * 1000`

The rate is the slope from a settled reference sample, not `drift / elapsed`, which mistakes a fixed startup offset for a changing clock. The reference begins after five seconds and needs at least sixty seconds of span. Engagement occurs if absolute drift exceeds 15 ms or the trustworthy rate predicts that threshold within ten minutes. Engagement latches for the current stream.

The controller updates at roughly one second, caps correction at ±500 ppm, slew-limits it to 125 ppm per second and suppresses changes below its minimum meaningful step. Feed-forward removes the steady rate burden from the proportional term; within the correction envelope the residual can converge near zero. At the rate cap, existing offset need not close. Beyond the cap, residual can continue growing. There is no universal bounded-drift promise for an arbitrarily bad device clock.

The expert opt-out preserves the uncorrected sample stream where the rest of the chosen capture/encoding path is also lossless. Once compensation engages, even PCM/FLAC output is resampled. "DSP off" alone does not mean bit-exact capture.

## Silence, gaps, pause and reactivation

A forward device-position discontinuity describes lost frames before the current packet. Insert the corresponding bounded silence before encoding that packet. Backward or pathological jumps are not instructions to allocate an unlimited buffer.

Healthy loopback may stop delivering packets entirely during silence. A lost endpoint also has no device observations. Both need wall-clock filling to preserve the track's elapsed sample count, but only endpoint failure is degradation. Account for wall-clock silence already inserted before processing a device-reported gap, so the same interval is not filled twice.

Pause drains/discards capture without adding media samples and continuously reanchors the silence clock. Resume must not fill the paused interval. A reopened device can restart its counter; reset the applicable drift baseline/controller rather than treating that discontinuity as clock error. The reopening operation itself consumes real unrecorded time and belongs in outage accounting.

## Measuring the result independently

Packet spans from ffprobe establish what the file contains; a duration tag cannot prove continuity of each audio track. An A/V clapper establishes changing offset from repeated flash/beep markers. The analyzer fits a slope over all qualified markers and checks reference uncertainty/nonlinearity before issuing a budget verdict. Start-to-end cancellation alone is not a pass.

For pixel/stimulus analysis, align timelines using independent wall-clock, in-band marker or QPC evidence. The observed behavior under test must not define its own alignment. The engine's logged epoch includes its provenance (`frame_timestamp`, `capture_observed`, or `session_start_floor`); a floor is a bound, not an exact anchor.

## Implementation and tests

See [clock controller](../../libs/engine/src/clock_slaving.h), [drift estimator](../../libs/engine/src/audio_clock_drift.h), [silence policy](../../libs/engine/src/audio_silence_fill.h), [video worker](../../libs/engine/src/video_thread.cpp), [clock tests](../../libs/engine/tests/test_clock_slaving.cpp), and the [soak/analysis workflow](../dev/soak-and-recovery-drills.md).
