# ADR 0058: Qt Quick D3D11 Preview Scene Bridge

## Status

Accepted with follow-up.

## Context

ADR 0057 selected the real preview path as the go/no-go gate for further Record-area migration. The
shipping Widgets path presents through a D3D11 swap chain owned by a native child HWND. That path is
fast, but the child window owns the preview pixels and prevents the top-level Quick scene from
stacking, clipping, hit-testing, or animating content over them.

The recording engine and the idle capture hubs publish an NT-handle shared D3D11 texture guarded by
a keyed mutex. DXGI Output Duplication supplies the normal monitor path; a WGC hub supplies window
targets and the monitor fallback required when Output Duplication cannot serve the selected source.
The Quick bridge must consume either resource without moving Qt concepts into `engine`,
without CPU readback, and without changing the shipping renderer.

## Decision

Add `ExoPreviewItem`, a `QQuickItem` whose render-thread node opens the existing NT shared handle on
Qt Quick's D3D11 device. The node acquires consumer key 1 without waiting, copies the shared resource
to a cached private texture, releases producer key 0 immediately, and performs any required GPU
format pass after releasing the mutex.

The source and Qt Quick use distinct D3D11 devices:

```text
idle DXGI/WGC capture-hub device OR recording-engine D3D11 device
        |
        | NT shared handle, keyed mutex (producer 0 -> consumer 1)
        v
Qt Quick scene-graph D3D11 device
        |
        | OpenSharedResource1 + cached GPU CopyResource
        v
private same-format texture
        |
        | FP16 tone map or BGRA/RGB10 -> RGBA GPU pass when required
        v
RGBA8 display texture
        |
        | QNativeInterface::QSGD3D11Texture::fromNative
        v
QSGImageNode under rounded QSGClipNode
```

The private copy is intentional. It decouples the producer mutex lifetime from scene composition and
allows the producer to continue immediately. There is no staging texture, map, `QImage`, CPU
readback, or CPU frame copy. Resource allocation occurs at source adoption or replacement, not per
frame.

`QQuickWindow::beginExternalCommands()` and `endExternalCommands()` delimit the bridge's D3D work.
The conversion helper clears inherited immediate-context state before its off-screen pass because the
existing tone mapper normally owns its context; otherwise Qt's scissor/blend state can leak into the
conversion.

The bridge uses only public Qt 6.9 APIs: `QQuickItem`, public scene-graph nodes,
`QSGRendererInterface`, `QQuickWindow` external-command boundaries, and
`QNativeInterface::QSGD3D11Texture::fromNative`. No Qt private headers or types are used. The native
interface is public but intentionally platform/version specific and does not carry source or binary
compatibility guarantees, so Qt upgrades require a focused bridge build and runtime check.

## Composition and application boundary

`RecordPreviewAdapter` is the only preview transport object visible to QML. `QuickApplication` owns
the real idle hubs and recording coordinator integration and centrally switches the adapter from the
idle shared texture to the recording engine's WYSIWYG texture and back. Record workflow state and
commands use the separate narrow `RecordViewModelAdapter`. Neither the coordinator nor a service
registry is exposed to QML.

`RecordPage.qml` places ordinary interactive QML content above `ExoPreviewItem`. The old Widgets
preview OSD is not present: the Quick path bypasses `PreviewSurface`, `DxgiPreviewRenderer`, and its
`QImage`/bitmap OSD sprites. Cursor and webcam pixels that are part of the recording result remain
video content; application status and metrics are Quick items.

The item owns no HWND. Rounded clipping is scene-graph geometry, and pointer ownership remains with
the Quick scene.

## One-shot Ready-frame capture

Ready-state WYSIWYG capture duplicates the exact retained NT handle currently feeding the Quick
item. A user-triggered worker opens it on a dedicated D3D11 device, waits for the existing consumer
key only for that bounded request, makes a GPU-local copy, releases the producer promptly, and uses
the existing tone-map/compositor/cursor helpers to produce the configured crop and overlay result.
Only the final composed still is staged to CPU memory for PNG encoding. This does not change the
continuous preview path, introduce screenshot semantics, or start a parallel source capture.

Recording-state capture continues through the engine-owned snapshot request. While paused, the
engine reads the last completed encoder-owned real-frame slot on demand, because no new producer
frame is expected until resume. This adds no per-frame copy and keeps Ready, Recording, and Paused
capture aligned with the real source/recording paths.

## Lifetime and synchronization

- The GUI thread owns adapter activation, source generations, and a retained duplicate-capable NT
  handle.
- The capture hub or recording engine thread/device publishes frames with producer key 0 and releases
  key 1.
- The Quick render thread opens resources, polls key 1 with a zero timeout, copies/converts, and
  releases key 0. It never blocks the GUI thread or producer.
- Every queued render-state update carries a source generation; stale updates after stop or
  replacement are discarded.
- Scene-graph invalidation clears readiness. Initialization duplicates the retained handle and
  rebuilds the render resources without requiring the producer to recreate its texture.
- Conversion failure does not mark a frame consumed or ready and remains visible until a successful
  conversion clears it.
- Source size/format changes replace the generation and rebuild the cached resources. Shutdown first
  destroys QML/scene-graph consumers, then composition services and engine-owned producer resources.

## Consequences

The architecture removes the native-child-window composition barrier and is suitable as the
production Record preview bridge. A per-consumed-frame GPU copy and, for non-RGBA sources, a GPU
format/tone-map pass remain. The current bridge polls at the scene cadence because the existing
shared-texture protocol has no frame-available notification. This produced stable results but many
zero-timeout mutex misses; producer-driven or adaptive scheduling remains a useful optimization but
does not block Record-area cutover.

The validation was limited to the available 2560x1440 144 Hz display and NVIDIA GeForce RTX 5070 Ti.
The broader 1080p and 4K matrix, GPU counters, and a QML-profiler trace remain follow-up work.
