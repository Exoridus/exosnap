# Diagnostics and support data

This document owns measurement provenance, diagnosis, aggregation and support artifacts. The [product specification](../product-spec.md#11-diagnostics-and-fix-actions) owns visible card/tile/notification behavior.

## Measurement before diagnosis

Every live metric has a producer, meaning, cadence, presentation consumer and log consumer. Hot-path instrumentation is bounded; aggregation and UI updates are lower frequency. Per-frame image-quality analysis does not belong in ordinary recording diagnostics.

Keep source progress, emitted CFR cadence, GPU elapsed time and CPU submission time distinct. A CPU bracket around a GPU command is not GPU execution time. A repeated held picture is not necessarily lost video. Drift measured from a device clock is not queue delay. A missing/stopped/refused producer withdraws its reading instead of leaving a stale number labeled live.

Engine snapshots carry availability alongside values. The application must not turn unavailable into zero, count an invalid snapshot twice, or carry deltas across session generations. Real-drop summaries consistently count backpressure, processing failures and unemitted ring eviction, not benign coalescing or pre-first-frame slots.

## Diagnosis and presentation policy

Severity and tier are orthogonal. Pass/Notice/Blocker determines admission significance. Each diagnosis also declares its own tier:

| Tier | Meaning | Presentation rule |
|---|---|---|
| Blocker | Recording cannot be admitted | Always visible, blocks start |
| Measured problem | Observed runtime/environment issue | Always represented, attention tone |
| Optimization | A supported configuration could be improved | Quiet bundled tip, never makes readiness amber by itself |
| Fact | Capability/environment reference | Neutral reference information, excluded from verdict counts |

The check declares its tier at creation. A downstream allowlist of diagnostic IDs must not recreate that knowledge. Environment facts use the same value vocabulary without becoming recommendations or blockers.

The application controller owns verdict construction and session-aware accounting. Adapters expose the result. Blocking probes, filesystem checks and self-tests execute asynchronously. An output-path write test is not repeated per frame or at the recording's telemetry rate.

## Fix actions

Fix actions describe intent, safety class, reversibility and consequences. Auto means an executable, reviewable change, not silent application. The confirmation path is structural: requesting a fix emits its summary; acceptance reaches the dispatcher. A target/scope or track-structure change always needs confirmation naming those consequences.

Assisted actions reveal an existing control/location; External actions direct the user to an operation the application cannot perform. A fix never acquires additional authority because it came from Diagnostics rather than Settings. No generic shell or Windows-administration operation is introduced through a fix.

## Present and latency observation

The optional in-process PresentMon consumer and the separate DPC/ISR trace require both session opt-in and elevation. The in-depth switch is off at each normal launch and is not a persisted setting. Turning it on in a standard process offers an explicit elevated relaunch; it does not itself raise UAC or change the process token.

Display cadence is measured from duplication timestamps. WGC delivery timestamps supply the window/region cadence path and have different provenance/jitter. PresentMon adds presentation-mode and related attribution; it does not replace the unprivileged monitor-cadence source. Attribute a window measurement to the actual process instance and reset totals at each recording boundary even if the PID is unchanged.

Process exit, trace termination or unavailable decoding clears the live reading. A generic fullscreen signal is not proof of a capture failure. Mid-session stall reporting claims absence of frame progress and names exclusive fullscreen only as a corroborated possibility.

DPC/ISR attribution is best effort. An unresolved driver remains unidentified rather than being guessed. Compiling the provider or passing pure mapping tests is not evidence that elevated ETW delivery or driver attribution works on a particular machine.

## Session ledger and reports

The session ledger records persistent measured problems from distinct consecutive observations, not a single transient spike. Active entries retain duration and worst values; quiet entries remain available until the session ends. Configuration properties already true before start do not become runtime incidents just because they are still true.

A finished session freezes its ledger into the report. Individual frame-drop counts do not imply a timestamped drop-event history; only measured ledger occurrences can be located on its timeline. Use media duration when linking a session observation into Edit and clamp a tail observation to the actual file.

The engine JSON-lines log appends and rotates across launches. Its launch/session identity correlates with the human-readable application stream. A recording identifier is separate from the launch identifier and remains stable across its split segments; it must not depend on whether recovery-manifest protection was available.

A per-recording JSON report is written atomically alongside the logs and retains the ten newest reports. It includes resolved configuration/format, encoder initialization, duration/drop/drift/discontinuity information, segment status, failure phase and the frozen ledger. Unmeasured values stay unavailable. A failure that aborts before packet accounting must be reported by its error rather than a misleading zero metric.

## Structured application logging

`AppLog` stores timestamp, severity, category, message and sequence in a bounded, mutex-protected history. Qt delivery coalesces queued appends. The Logs model uses incremental append/eviction notifications and a proxy for filtering, not parsing severity out of formatted strings. Copy exports the visible filtered rows; Export includes the current in-memory history. Rotated disk logs are a different, longer-lived source.

Source/window labels shown in the UI need not be safe to log. Window capture targets use a placeholder at the logging source. Additional bundle scrubbing removes recognized user paths, usernames, machine names and capture-target fields. Do not rely solely on a support operation to sanitize a log already written locally.

## Support bundles

Support bundles are user-created local ZIP files. They include rotated logs, recent session reports and allowlisted structured capability/configuration facts, not raw settings/presets/history or recordings. Crash dumps are a separate consent-controlled channel and are not included.

Bundle work runs off the GUI thread. A successful write is reported truthfully; reveal/open behavior belongs to the product action that follows. Scrubbing known fields and path shapes is not a proof that arbitrary user-authored text can contain no personal information. Users should review a bundle before sharing it. Nothing uploads it automatically.

The [privacy review](../privacy-review.md) owns the egress inventory and checks. The [verification boundaries](verification-boundaries.md) document explains what each test instrument can establish.

## Implementation and tests

See [recommendation engine](../../app/diagnostics/RecommendationEngine.h), [controller](../../app/diagnostics/DiagnosticsController.cpp), [present provider](../../app/diagnostics/PresentMonProvider.h), [session report](../../app/diagnostics/SessionReport.cpp), [bundle](../../app/diagnostics/SupportBundle.cpp) and [pipeline snapshot](../../libs/engine/include/exosnap/engine/pipeline_diagnostics.h). Their tests cover tier requirements, stale-sample withdrawal, session resets, scrubbing and report availability.
