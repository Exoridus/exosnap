# Qt Quick Record Page Migration

- Date: 2026-08-09
- Status: **Cutover ready**

## Scope and result

The opt-in Qt Quick frontend now provides the practical Record workflow over real ExoSnap state and
services. The existing Widgets Record page and shipping executable remain available and unchanged as
the reference path. No default frontend cutover was made.

The Quick page supports real target selection (display, window, and region), source enablement,
record readiness, countdown, start, pause, resume, stop/finalize, recording-time actions, live
statistics, notices/errors, result state, and a movable/resizable webcam overlay. Format and profile
editing remain in Settings because the Widgets Record page only displays the resolved format on this
surface.

## Reference audit

The Widgets page owns the source chip and picker, resolved-format strip, D3D preview, SYS/APP/MIC/CAM
source controls, countdown and transport, timer, frame/marker/split actions, live statistics,
warnings/results, and webcam picture-in-picture editing. Its preview is a native child HWND and its
presentation glue consumes the shared `RecordViewModel` and coordinator callbacks.

The Quick implementation preserves those product responsibilities without reproducing the QWidget
hierarchy. Business rules such as capability, container/codec, track, profile, and recording-state
resolution remain in their existing C++ owners.

## Architecture

```text
engine / capability and device services
                    |
                    v
RecordingCoordinator + pure RecordViewModel
                    |
                    v
QuickApplication composition and callback fan-out
                    |
          +---------+----------+
          |                    |
          v                    v
RecordViewModelAdapter   RecordPreviewAdapter
          |                    |
          +---------+----------+
                    v
              RecordPage.qml
```

`QuickApplication` centrally owns the adapters, coordinator, stores, device notifiers, target-change
notifier, countdown, meter delivery, webcam-frame provider, one-shot Ready-frame capture service, and
preview hubs. QML sees only the two narrow Record adapters.
It does not receive a coordinator, service registry, capability resolver, or application-service
object.

Coordinator callbacks have one central fan-out so the preview and workflow presentation cannot
silently replace each other's observer. Snapshot synchronization remains centralized. High-frequency
meter delivery is coalesced to about 30 Hz and uses a dedicated notification, avoiding invalidation of
unrelated bindings. Webcam preview delivery is likewise coalesced, page-activity gated, and scaled to
the requested PiP size before upload. The webcam provider works only on the coordinator's existing
CPU `QImage` preview callback; the main desktop preview never passes through it.

Monitor idle preview normally uses DXGI Output Duplication. Window targets and unsupported/cross-path
monitor cases use the WGC capture hub. Both publish the existing NT-handle/keyed-mutex contract to the
unchanged Quick scene-graph bridge.

`CaptureTargetNotifier` is shared app-layer discovery state. Qt screen notifications and Win32
top-level-window events are debounced into one coordinator enumeration, deduplicated, and applied
centrally. Stable monitor/window identity preserves the current selection when possible; an
unavailable selected source is not silently replaced. Refresh is deferred while recording state
locks the target and reconciled when it becomes editable again. QML does not poll or enumerate.

Ready-state frame capture starts with a duplicate of the exact NT shared handle currently feeding
`ExoPreviewItem`. A one-shot worker opens that texture on its own D3D11 device, observes the existing
keyed-mutex contract, and performs crop, tone mapping, cursor, and webcam composition through the
same app/engine helpers used to define the recording result. CPU readback occurs only after the final
composed still is complete so it can be encoded as PNG. It is neither a window screenshot nor a
continuous preview readback, and it does not create another capture source or producer.

## UI behavior

- The source picker uses stable, filtered display/window models and virtualizes the window list.
- A normalized region model survives preview resize and supports pointer and keyboard editing.
- Source toggles, transport actions, icon actions, navigation, notices, and source selection expose
  normal checked/disabled/focus/accessibility semantics.
- Recording, paused, preparing, stopping/finalizing, ready, unavailable, warning, error, and result
  presentation is driven by the real C++ state model rather than a QML-only state machine.
- The timer, output size, delivery/scene metrics, encode/bitrate/drop/drift statistics, warnings, and
  source status are ordinary QML overlays over clean preview pixels.
- The idle webcam PiP supports mirror, opacity, drag, resize, arrow-key movement, and Shift+arrow-key
  resizing. During active recording the engine WYSIWYG texture already contains the webcam, so the
  separate PiP is hidden to prevent double composition.
- The page has deliberate minimum widths, wrapping, elision, and responsive `Flow` overlays. At the
  860 x 700 application minimum, all essential actions remain visible without relying on a large
  development window.

## Preview invariants

The existing scene-graph path is retained: NT shared handle, keyed mutex, cached GPU `CopyResource`,
GPU format conversion, public `QSGD3D11Texture::fromNative`, and `QSGImageNode`. It has no preview
child HWND, no main-preview CPU readback/copy, no private Qt API, and no Widgets bitmap/QImage status
OSD. Rounded clipping, overlays, and pointer ownership remain in the Quick scene.

## Tests and validation

Automated coverage includes adapter state mapping, target models, device and meter state, countdown,
region state, narrow command routing, capture-frame restrictions, webcam chroma/downscale behavior,
preview resource/lifecycle behavior, Ready-frame crop mapping, runtime target add/remove and event
deduplication, and Qt Quick control interaction. Qt Quick Tests exercise action and toggle
keyboard/disabled/authoritative-selection semantics and adapter-driven preview metrics.

Validated visual harness states: ready, recording, paused, warning, error, and unavailable at
860 x 700; ready at 860 x 700 with `QT_SCALE_FACTOR=1.5`; ready at 1280 x 820; and recording at
1600 x 1000. Checks covered text overlap/elision, source and action access, preview fit/clip, overlay
stacking, disabled state, and focus indicators. The 150% result uses Qt logical coordinates: the hard
860 x 700 logical minimum remains available while device pixels scale.

The HWND audit completed with zero Quick child HWNDs. Real Ready, Recording, and Paused stills were
saved from a high-change desktop source at 2560 x 1440; the associated recording completed with zero
recording drops and the Quick window had zero child HWNDs. Paused capture uses the last completed
encoder-owned real-frame slot, avoiding a second source capture while the paused producer is idle.

A real external top-level window was then added and removed while Quick was running. The filtered
window model changed from 10 to 11 and back to 10 without QML polling. Physical monitor cabling was
not changed during this session; monitor replacement/removal is covered through the notifier's
synthetic topology seam over the same debounce, deduplication, and reconciliation path.

An earlier 5-second real high-change desktop recording
at requested 2560 x 1440/60 completed with 302 captured and encoded frames, zero recording drops,
zero producer-contention drops, 583 expected non-blocking consumer mutex misses, and a 5.53% process
CPU sample. The Quick scene loop measured
143.56 Hz (p95 7.38 ms, p99 8.10 ms), while successful source delivery measured only 28.73 fps
(interval p95 48.69 ms, p99 55.22 ms). These are different measurement points and do not establish
144 unique preview frames per second, 4K60, or 4K144 capability.

## Remaining work

### Blockers before Quick frontend cutover

None identified in the Record area.

### Required follow-ups

- Run the broader 1080p, 4K60, and hardware-permitting 4K144 matrix with comparable present/GPU
  counters.
- Revalidate the public platform native interface on every Qt upgrade.

### Optimizations

- Capture a profiling-enabled Qt Quick trace and optimize only confirmed GUI/render hot spots.
- Consider producer-driven/adaptive preview invalidation to reduce zero-timeout scene-cadence
  polling.
- Consider a reusable native webcam texture path if profiling shows the coalesced/downscaled image
  provider is material.

No product specification changed: the migration supplies another frontend for the existing behavior.
ADR 0058 was updated only to record the WGC idle-source extension and the completed Record boundary.
