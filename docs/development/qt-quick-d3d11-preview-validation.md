# Qt Quick D3D11 Preview Validation

- Date: 2026-08-09
- Verdict: **GO WITH FOLLOW-UP**

## Scope and result

This slice added a real Record/Preview development page to the existing `QuickApplication` and
`ApplicationWindow`. It uses ExoSnap's live DXGI capture hub while idle and the recording engine's
real shared preview texture during recording. The shipping Widgets preview remains unchanged.

The architectural question is answered positively: a real ExoSnap D3D11 texture can participate in
the Qt Quick scene with stable frame pacing, no CPU readback, no child HWND, correct overlay/pointer
composition, and no observed recording drops. Follow-up is required for render-loop scheduling,
broader hardware coverage, and QML-profiler enablement.

## Architecture

### Resource path

The idle source texture is owned by the D3D11 device in `DxgiCaptureHubService`. During recording,
the source texture is owned by the recording engine's D3D11 path. Qt Quick renders with the separate
D3D11 device exposed by `QSGRendererInterface`.

`ExoPreviewItem` opens the producer's NT handle with `ID3D11Device1::OpenSharedResource1`. Producer
and consumer synchronize through the existing keyed-mutex contract: producer acquire 0/release 1;
Quick acquire 1/release 0. Both use a zero timeout and drop/retain instead of blocking.

Quick performs one `CopyResource` into a cached same-format private texture per successfully consumed
frame. The copy is required to release the producer mutex before scene composition. FP16 scRGB input
uses the existing GPU `HdrToneMapper`; BGRA8/RGB10 input uses a cached fullscreen D3D11 conversion
pass to RGBA8. Qt then wraps that display texture with
`QNativeInterface::QSGD3D11Texture::fromNative`. Qt performs no additional application-requested
copy. No staging resource, map, `QImage`, CPU readback, or CPU frame copy exists in this path.

All GPU textures, views, shaders, geometry, and wrappers are created at source adoption/replacement.
No per-frame GPU resource creation was observed in code or through the single source-generation
counter during the recording benchmark.

### Threads

| Thread | Work |
|---|---|
| GUI | QML state/navigation, adapter activation, source-generation handoff, 250 ms metric snapshots |
| Qt Quick render | open shared resource, non-blocking keyed-mutex acquire, GPU copy/conversion, node/clip update |
| DXGI capture hub | idle Output Duplication capture and publication |
| recording engine video path | capture/composite/encode and recording preview publication |

There is no per-frame synchronous GUI call and no blocking cross-thread wait. Render-state changes are
queued and generation checked.

### Qt APIs

Public Qt APIs used: `QQuickItem::updatePaintNode`, `QSGImageNode`, `QSGClipNode`, `QSGGeometry`,
`QSGRendererInterface`, `QQuickWindow::beginExternalCommands/endExternalCommands`, and
`QNativeInterface::QSGD3D11Texture::fromNative` from the public platform texture header.

No private Qt API is used. `QNativeInterface` is public but Qt documents it as platform/version
specific without source or binary compatibility guarantees. The bridge contains that coupling in
`ExoPreviewItem.cpp`; it does not leak into adapters, QML, or `engine`.

## UI and composition proof

`RecordPage.qml` contains the real preview, real source/recording status, a QML metrics panel and
toggle over the live texture, a render-thread `ScaleAnimator`, source/path cards, and normal shell
navigation. The overlay receives pointer events and remains above the preview during resize and
navigation.

Rounded corners use a `QSGClipNode` with cached rounded-rectangle geometry. Visual inspection and
corner pixel sampling confirmed the preview is clipped rather than covered by a native mask. The
Quick HWND audit exits 0 with no child HWND; the Widgets reference audit exits 1 and identifies the
existing `ExoSnapDxgiPreviewChild` over the preview pixels.

The actual Quick input is the raw hub texture while idle or the engine WYSIWYG video texture while
recording. It never passes through `PreviewSurface::setStatusText`, `DxgiPreviewRenderer::RenderOsdSprites`,
or the Widgets `QImage` OSD sprite path. Code-path inspection and live-frame visual inspection
confirmed that the old bottom status text is absent from the imported pixels. Equivalent status and
metrics are normal QML content.

Visual checks covered the normal window, the minimum-size scenario, 150% scaling, preview resize,
overlay stacking, rounded clipping, Record/About navigation, minimize/restore, and overlay pointer
interaction.

## Lifecycle

- Initial activation acquires the real idle hub lease and publishes one source generation.
- The first successful GPU conversion alone sets `frameReady`.
- Stop clears readiness synchronously and invalidates queued state through a generation token.
- Restart and source replacement close superseded handles and rebuild cached resources once.
- Recording switches from the raw hub texture to the engine texture and returns to the hub after the
  terminal recording result; the benchmark observed one engine texture generation.
- Resize changes node and clip geometry without source-resource recreation.
- Minimize/restore was exercised by the lifecycle harness.
- The lifecycle harness explicitly calls `QQuickWindow::releaseResources()` while minimized. The
  restored scene reopens a duplicate of the retained NT handle and receives frames without producer
  recreation.
- Scene-graph invalidation marks the GUI state not ready. Stale queued callbacks are discarded.
- Conversion failures no longer count as consumed/ready and remain reported until recovery.
- Clean process shutdown completed after idle, lifecycle, and recording runs without a stale callback,
  access violation, or D3D teardown error.

## Performance methodology

Both paths ran Debug builds on Windows 11, Qt 6.9.0, an NVIDIA GeForce RTX 5070 Ti, and one
2560x1440 144 Hz display. `PresentBench` supplied the same continuously changing desktop workload.
Each application recorded the primary monitor for 10 seconds at requested 144 fps while its real
preview and representative UI workload were active. CPU is process CPU normalized across logical
processors; the external workload is excluded.

Native timing is measured immediately after its DXGI swap-chain `Present`. Quick scene timing is the
render-thread preprocess interval. They describe the respective presentation loops but are not the
same OS-present event. Source delivery and submit timing use equivalent successful keyed-mutex
consumption points. Percentile windows contain up to the latest 1,024 samples.

### Results

| Scenario / metric | Native HWND | Qt Quick | Delta |
|---|---:|---:|---:|
| 1440p144 recording + UI, successful | yes | yes | — |
| Recording frames dropped | 0 | 0 | 0 |
| Preview/scene loop | 62.51 Hz | 144.02 Hz | +81.51 Hz |
| Loop interval p50 | 16.03 ms | 6.94 ms | -9.09 ms |
| Loop interval p95 | 16.27 ms | 7.33 ms | -8.94 ms |
| Loop interval p99 | 20.03 ms | 7.74 ms | -12.29 ms |
| Worst loop interval | 26.81 ms | 8.45 ms | -18.37 ms |
| Successful source delivery | 27.44 fps | 28.53 fps | +1.09 fps |
| Source interval p95 | 48.20 ms | 41.76 ms | -6.44 ms |
| Source interval p99 | 48.28 ms | 42.17 ms | -6.10 ms |
| GPU submit p95 | 133.7 us | 170.8 us | +37.1 us |
| GPU submit p99 | 257.4 us | 216.5 us | -40.9 us |
| Zero-timeout mutex misses | 344 | 1,163 | +819 |
| Process CPU | 12.20% | 5.68% | -6.52 points |
| Native preview child HWND | yes | no | removed |

The Quick recording produced 1,451 captured frames and 1,451 encoded packets in 10.076 seconds with
zero recording drops. The source format during recording was `DXGI_FORMAT_B8G8R8A8_UNORM` (87), so
this run exercised the GPU BGRA-to-RGBA pass. Separate idle runs exercised the FP16 scRGB tone-map
path. Quick used one source texture generation during recording, showing that no hidden per-frame
resource recreation occurred.

The higher Quick mutex-miss count follows from polling at 144 Hz while the content-driven shared
preview publisher delivered about 28.5 updates per second. Acquires use a zero timeout, frame pacing
remained stable, and recording was unaffected, but producer-driven/coalesced scheduling should remove
the avoidable polling.

The machine had no 1080p or 4K display/source modes available, so 1080p60, 4K60, and 4K144 were not
fabricated. In-process GPU usage was unavailable. PresentBench was the changing source workload, not
a GPU counter. These omissions prevent an unconditional GO for the full hardware matrix.

## Validation performed

- Completed the full all-target Debug build, including `exosnap_quick_spike`, shipping `exosnap`,
  native instrumentation, and every test target.
- Full repository test run through `scripts/run-tests.ps1`: 276/276 passed.
- Focused repository test run: 5/5 passed (`auto_record_harness_tests`,
  `dxgi_preview_pushed_source_tests`, `record_viewmodel_qml_adapter_tests`, and both live Quick
  preview smoke/lifecycle entries).
- Direct Qt Quick Test: 4/4 passed, including owner-controlled overlay toggling and binding retention.
- CMake-generated `qmllint` JSON: every packaged QML file succeeded with zero warnings.
- Deterministic 47+ rule QML lint: clean after fixes.
- Six-pass QML review found and drove fixes for handle retention across scene-graph recreation,
  conversion-error readiness, generationless callbacks, owner/child overlay state, narrow-preview
  overflow, metric semantics, and render-thread animation.
- Quick HWND audit: exit 0. Widgets reference audit: exit 1 with the expected preview child HWND.
- Live preview smoke, navigation, resize, minimize/restore, forced scene-graph resource release,
  shutdown, and real recording benchmark: passed.
- Qt QML Profiler was attempted with the installed Qt 6.9 tool. The target rejected the profiler
  connection because it is not built with `QT_QML_DEBUG`; no valid trace was produced. Existing
  in-process render/source/submit percentile instrumentation supplied the results above. A dedicated
  profiling build is a required follow-up.
- Slice-specific cppcheck (`warning`, `performance`, and `portability`) passed. The repository-wide
  static-only wrapper remains non-green solely for pre-existing return-by-value findings in the
  untracked About and legacy Record Quick adapters; the preview slice findings were corrected.

## Files

Added for this slice:

- `app/quick/ExoSnap/Quick/ExoPreviewItem.h/.cpp`
- `app/quick/ExoSnap/Quick/QuickPreviewRgbaConverter.h/.cpp`
- `app/quick/ExoSnap/Quick/RecordPreviewAdapter.h/.cpp`
- `app/quick/ExoSnap/Quick/RecordPage.qml`
- `app/quick/ExoSnap/Quick/PreviewMetricsOverlay.qml`
- `app/quick/ExoSnap/Quick/tests/tst_PreviewMetricsOverlay.qml`
- `docs/decisions/0058-qt-quick-d3d11-preview-scene-bridge.md`
- `docs/development/qt-quick-d3d11-preview-validation.md`
- `docs/development/qt-quick-record-preview-qml-review.md`

Modified for Quick composition/build and validation:

- `app/quick/ExoSnap/Quick/AppShell.qml`
- `app/quick/ExoSnap/Quick/Main.qml`
- `app/quick/ExoSnap/Quick/QuickApplication.h/.cpp`
- `app/quick/ExoSnap/Quick/main.cpp`
- `app/quick/ExoSnap/Quick/CMakeLists.txt`
- `app/CMakeLists.txt`
- `app/services/DxgiCaptureHubService.h/.cpp`
- `app/services/DxgiPreviewRenderer.h/.cpp`
- `app/ui/widgets/PreviewSurface.h/.cpp`
- `app/pages/RecordPage.h/.cpp`
- `app/auto_record/AutoRecordHarness.h/.cpp`
- `app/auto_record/AutoRecordOptions.cpp`

## Remaining issues

### Migration blockers

None found for the bridge architecture.

### Required follow-ups

1. Replace or adapt the 144 Hz zero-timeout poll loop with producer-driven/coalesced notification or
   a measured adaptive cadence. Preserve non-blocking semantics and re-run latency percentiles.
2. Add a `QT_QML_DEBUG` profiling configuration and capture a valid QML Profiler trace during the
   real recording path.
3. Run equivalent 1080p60, 4K60, and, where supported, 4K144 measurements with GPU engine counters
   and an OS-present tool on suitable hardware.
4. Revalidate the contained `QNativeInterface::QSGD3D11Texture` bridge on every Qt upgrade.

### Desirable optimizations

- Avoid allocating and sorting three metric vectors on every 250 ms adapter tick; compute summaries
  incrementally or only while metrics/benchmark collection is requested.
- Rename the development target's remaining `spike` terminology when the parallel frontend becomes
  a normal product target.

### Unrelated pre-existing issues

- The shipping HWND audit still reports the known native-child/window-chrome ownership barrier.
- The Widgets build emits the existing `RecordPage.moc` automoc warning.
- Repository-wide cppcheck reports pre-existing `returnByReference` findings in
  `AboutViewModelAdapter` and `RecordViewModelAdapter`; they are outside this slice.

## Final recommendation

The production-oriented scene bridge is viable, preserves the engine/UI boundary, eliminates the
native-child composition barrier, survives forced scene-graph recreation, and performed without
recording interference on the strongest available hardware mode. Proceed toward the real Quick
RecordPage, but complete the required scheduling, profiler, and hardware-matrix follow-ups before
calling that migration finished.

**GO WITH FOLLOW-UP**
