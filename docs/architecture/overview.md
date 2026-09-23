# Architecture overview

This document owns subsystem boundaries and process composition. User-visible behavior and defaults belong to the [product specification](../product-spec.md). Operational commands belong to [developer documentation](../README.md#developer-workflows).

## Processes and layers

ExoSnap is a Windows x64 application with a C++20 recording engine and a Qt Quick frontend. There is one main application, `exosnap.exe`, and a separate update application, `exosnap-updater.exe`. The main application uses `QApplication` because its system-tray integration needs `QSystemTrayIcon`. Its rendered application surfaces are QML and scene-graph items, not a parallel Widgets frontend. The updater owns its separate Widgets window.

| Owner | Responsibility | Must not own |
|---|---|---|
| Process bootstrap | DLL search policy, install self-heal, application identity, logging lifetime, clean-exit bookkeeping and relaunch | Page state or recording policy |
| `QuickApplication` | Composition, service lifetime, persistence, propagation between models and adapters | A second encoder, mixer or compatibility resolver |
| QObject adapters and list models | Typed presentation state, narrow actions, transient input state | Independently derived capabilities, track resolution or storage formats |
| Application services | Recording orchestration, diagnostics, edit/export, notifications, update and recovery workflows | QML layout or engine-thread UI access |
| Capability and model libraries | Validation, compatible combinations, format resolution and source-to-track planning | Widgets, scene-graph objects or speculative hardware availability |
| Recording engine | Capture, GPU processing, clocks, encoding, packet queues and container writing | Dialogs, navigation, user prompts or Windows administration |
| Verification tools | Artifact-bound observation, test stimuli, environment setup and independent analysis | A bypass of the product's admission or security checks |

The practical dependency direction is presentation → application services/models → capability/engine contracts. Some application services use Qt Core for signals, files and asynchronous work. That does not make them part of the QML layer. Build layering checks and focused tests enforce the distinctions.

## Main data flows

A settings gesture reaches `SettingsAdapter`, then the existing reconciliation and sanitization functions. The composition root persists the resulting live configuration and supplies it to the recording coordinator. A start snapshots its inputs before crossing into a worker. The worker validates the destination and resolved configuration, acquires resources, and enters `RecorderSession`. Engine workers produce packets and measurements. The mux worker owns file serialization, while queued application callbacks update the presentation models.

The preview is a separate consumer of GPU images, not the capture or encode clock. Idle preview uses capture hubs. Recording preview consumes the composited engine image. An unavailable or slow preview must not hold the recorder's producer or encoder resources indefinitely.

Edit opens a recording as a temporary in-memory recipe. Playback decodes the media; export independently copies encoded streams. Closing the workspace releases playback resources but does not cancel an already-committed export. See [edit and export](edit-and-export.md).

## Startup and shutdown

`ProductionBootstrap::RunPreApplicationPhase()` runs before constructing the Qt application. DLL-search hardening must precede plugin loading, and interrupted-update repair must precede resolving installation-relative resources. Application metadata and logging initialization have separate phases so their ownership and measured startup costs remain distinguishable.

The frontend reads the previous session's crash context before beginning a new session. Otherwise a new clean context could overwrite the evidence needed by the next-launch crash surface. Recovery and crash-report prompts are separate concerns.

QML loads the main window hidden. C++ applies final native style and resolved geometry before the first show. A correction after the first rendered frame cannot satisfy the first-frame geometry contract. Shutdown destroys scene-graph consumers before the producer services and resources they reference. The bootstrap's RAII cleanup detaches logging, records clean termination and handles an approved relaunch.

A worker that outlives a bounded join must retain everything it can still access. Engine workers hold shared session state rather than borrowing stack or destroyed coordinator storage. Timeouts report incomplete work; they do not authorize destroying live resources or claiming a complete file.

## Cross-cutting invariants

There is one owner for each policy. The source rows are editable inputs, the resolved tracks are engine results, and neither is reconstructed in JavaScript. Hardware availability is measured, not inferred from a product name. Unknown metrics remain unavailable rather than becoming zero. Files become final only after the operation that makes them valid succeeds. An update offer, its signed manifest, its installed version and its release notes refer to the same target.

Errors cross boundaries as typed outcomes with a phase and actionable detail. A diagnostic notice is not a blocker. A verification infrastructure error is not a product failure. A UI indication that a command was accepted is not evidence that an asynchronous operation completed.

## Implementation and tests

Start with [application composition](../../app/quick/ExoSnap/Quick/QuickApplication.h), [bootstrap](../../app/bootstrap/ProductionBootstrap.h), [recording coordinator](../../app/services/RecordingCoordinator.h), [session API](../../libs/engine/include/exosnap/engine/recorder_session.h), and the [layering rules](../../cmake/exosnap_layering.cmake). The [build and test guide](../dev/build-and-test.md) explains how to exercise those boundaries.
