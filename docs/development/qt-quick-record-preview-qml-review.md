## QML Code Review Report

- **Scope**: files: `RecordPage.qml`, `PreviewMetricsOverlay.qml`, `AppShell.qml`, `Main.qml`
- **Files reviewed**: 4 (plus C++ boundary sources for lifecycle tracing)
- **Issues found**: 19 (11 from lint, 8 from deep analysis; 18 addressed, 1 required follow-up)
- **qmllint**: ran; zero final warnings

---

### Lint findings

#### [L-001] Root declarations followed assignments
- **File**: `app/quick/ExoSnap/Quick/AppShell.qml:8`
- **Rule**: ORD-1
- **Finding**: Required properties followed `objectName`.
- **Mitigation**: Declarations now precede assignments.

#### [L-002] Stack index followed attached properties
- **File**: `app/quick/ExoSnap/Quick/AppShell.qml:98`
- **Rule**: ORD-1
- **Finding**: `currentIndex` appeared after `Layout.*` assignments.
- **Mitigation**: The ordinary assignment now precedes attached properties.

#### [L-003] Customized control used the abstract style import
- **File**: `app/quick/ExoSnap/Quick/PreviewMetricsOverlay.qml:4`
- **Rule**: IMP-3
- **Finding**: A customized Button used `QtQuick.Controls`.
- **Mitigation**: The component imports `QtQuick.Controls.Basic`.

#### [L-004] Record root declarations followed assignments
- **File**: `app/quick/ExoSnap/Quick/RecordPage.qml:10`
- **Rule**: ORD-1
- **Finding**: Required properties followed `objectName`.
- **Mitigation**: Declarations now precede assignments.

#### [L-005] Overlay handler followed a grouped child
- **File**: `app/quick/ExoSnap/Quick/RecordPage.qml:185`
- **Rule**: ORD-1
- **Finding**: `onToggled` followed the grouped anchors object.
- **Mitigation**: The handler now precedes child/group objects.

#### [L-006] Font group used repeated dot notation
- **File**: `app/quick/ExoSnap/Quick/RecordPage.qml:271`
- **Rule**: STY-3
- **Finding**: Three `font.*` values used dot notation.
- **Mitigation**: They now use one `font {}` group.

#### [L-007] Attached layout property followed a font child
- **File**: `app/quick/ExoSnap/Quick/RecordPage.qml:270`
- **Rule**: ORD-1
- **Finding**: `Layout.fillWidth` followed a grouped child.
- **Mitigation**: The attached property now precedes the group.

#### [L-008] Source-size label ordering
- **File**: `app/quick/ExoSnap/Quick/RecordPage.qml:279`
- **Rule**: ORD-1
- **Finding**: Font/layout attributes were out of canonical order.
- **Mitigation**: Ordinary properties now precede the attached layout property.

#### [L-009] Recording-state label ordering
- **File**: `app/quick/ExoSnap/Quick/RecordPage.qml:292`
- **Rule**: ORD-1
- **Finding**: Font/layout attributes were out of canonical order.
- **Mitigation**: Ordinary properties now precede the attached layout property.

#### [L-010] Resource-path label ordering
- **File**: `app/quick/ExoSnap/Quick/RecordPage.qml:329`
- **Rule**: ORD-1
- **Finding**: Font/layout attributes were out of canonical order.
- **Mitigation**: Ordinary properties now precede the attached layout property.

#### [L-011] No-copy label ordering
- **File**: `app/quick/ExoSnap/Quick/RecordPage.qml:338`
- **Rule**: ORD-1
- **Finding**: Font/layout attributes were out of canonical order.
- **Mitigation**: Ordinary properties now precede the attached layout property.

---

### Deep analysis findings

#### [D-001] Child write removed the owner's overlay binding
- **File**: `app/quick/ExoSnap/Quick/PreviewMetricsOverlay.qml:35`
- **Category**: Bindings & Properties
- **Confidence**: 96/100
- **Finding**: The child assigned its bound `expanded` input before notifying the owner.
- **Trace**: `RecordPage.showMetricsOverlay` bound `expanded`; the button overwrote it and then emitted `toggled`.
- **Mitigation**: The child now emits the requested value only; the owner remains the single state authority. The QML test also verifies a later owner change still propagates.

#### [D-002] Metrics panel overflowed narrow portrait previews
- **File**: `app/quick/ExoSnap/Quick/PreviewMetricsOverlay.qml:60`
- **Category**: Layout & Anchoring
- **Confidence**: 88/100
- **Finding**: A fixed 220-pixel panel could exceed the fitted preview width.
- **Trace**: The real source aspect ratio can make the overlay narrower than the panel at minimum window size.
- **Mitigation**: Panel and button widths are constrained to the available parent width; labels elide.

#### [D-003] Scene-graph recreation lost the one-shot handle
- **File**: `app/quick/ExoSnap/Quick/ExoPreviewItem.cpp:470`
- **Category**: Component Loading & Lifecycle
- **Confidence**: 96/100
- **Finding**: The first node consumed and closed the only NT handle, leaving no resource to reopen after scene-graph destruction.
- **Trace**: Handle handoff was destructive while the producer announced only resource generations, not scene-graph rebuilds.
- **Mitigation**: GUI-owned state retains the source handle and duplicates it for each node. Forced `QQuickWindow::releaseResources()` is now part of the lifecycle test.

#### [D-004] Conversion failure could mark the first frame ready
- **File**: `app/quick/ExoSnap/Quick/ExoPreviewItem.cpp:216`
- **Category**: Component Loading & Lifecycle
- **Confidence**: 93/100
- **Finding**: Failed tone-map/format conversion still advanced success metrics and readiness.
- **Trace**: Error assignment was followed by unconditional `has_frame_ = true` and a success callback.
- **Mitigation**: Conversion failure returns unsuccessful, does not advance metrics/readiness, and remains reported until a later successful frame.

#### [D-005] Queued state could resurrect a stale source
- **File**: `app/quick/ExoSnap/Quick/ExoPreviewItem.cpp:650`
- **Category**: Component Loading & Lifecycle
- **Confidence**: 89/100
- **Finding**: Render-thread state callbacks carried no source epoch.
- **Trace**: A queued source-N success could run after GUI-thread stop or source-N+1 replacement.
- **Mitigation**: Every callback carries a generation and is discarded unless it matches the active source; clear is synchronously authoritative.

#### [D-006] Scene loop continuously polls the keyed mutex
- **File**: `app/quick/ExoSnap/Quick/ExoPreviewItem.cpp:270`
- **Category**: Performance & Quality
- **Confidence**: 99/100
- **Finding**: The render loop schedules another update after every preprocess, including mutex misses.
- **Trace**: Final benchmark: 1,448 scene passes, 285 consumes, and 1,163 zero-timeout misses in 10 seconds.
- **Mitigation**: Retained as a documented required follow-up because the transport currently has no frame-available notification. It is non-blocking and produced stable pacing/zero recording drops.

#### [D-007] Delivery label showed scene-loop rate
- **File**: `app/quick/ExoSnap/Quick/PreviewMetricsOverlay.qml:101`
- **Category**: Performance & Quality
- **Confidence**: 98/100
- **Finding**: The label described scene polling as source delivery.
- **Trace**: Scene rate and successful consume rate are independently measured and differ substantially.
- **Mitigation**: The adapter now exposes both rates and QML labels them `Delivery` and `scene` explicitly.

#### [D-008] Diagnostic animation ran on the GUI animation path
- **File**: `app/quick/ExoSnap/Quick/RecordPage.qml:211`
- **Category**: Performance & Quality
- **Confidence**: 90/100
- **Finding**: `NumberAnimation` could contaminate GUI-stall measurements.
- **Trace**: The animation is deliberately active during benchmark interaction.
- **Mitigation**: The marker now uses `ScaleAnimator`, which advances on the render thread.

---

### Investigation targets (human verification needed)

#### [I-001] Periodic metric percentile allocations
- **File**: `app/quick/ExoSnap/Quick/ExoPreviewItem.cpp:505`
- **Category**: Performance & Quality
- **Confidence**: 74/100
- **Finding**: A 250 ms timer copies and sorts up to three 1,024-sample vectors even while the panel is collapsed.
- **Unverified because**: The installed app target is not built with `QT_QML_DEBUG`, so no valid QML-profiler trace isolated the timer cost.
- **How to verify**: Enable a dedicated profiling configuration and inspect GUI-thread samples around metric timer firings; move to incremental/on-demand summaries if measurable.

---

### Summary

| Category | Lint | Deep | Investigate | Total |
|---|---:|---:|---:|---:|
| Imports/style/ordering | 11 | 0 | 0 | 11 |
| Bindings & properties | 0 | 1 | 0 | 1 |
| Layout & anchoring | 0 | 1 | 0 | 1 |
| Component lifecycle | 0 | 3 | 0 | 3 |
| Delegates | 0 | 0 | 0 | 0 |
| States & structure | 0 | 0 | 0 | 0 |
| Performance & quality | 0 | 3 | 1 | 4 |
| **Total** | **11** | **8** | **1** | **20** |

Findings below confidence 60 are suppressed entirely. Final deterministic lint and generated `qmllint` are clean;
the producer-notification scheduling item remains the one confirmed deep-analysis follow-up.
