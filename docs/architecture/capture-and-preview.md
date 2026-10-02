# Capture and preview

This document owns source identity at runtime, frame-resource ownership, capture recovery, and the GPU preview transport. The [product specification](../product-spec.md#7-capture-targets-and-webcam) owns selection, webcam controls and visible recovery behavior.

## Capture sources

Normal display recording uses DXGI Output Duplication. Window recording uses Windows Graphics Capture (WGC). A region records a display with a physical-pixel crop. A developer-only backend override can measure WGC against a monitor; it is not a user-selectable recording backend.

The actual D3D11 capture adapter is the capture role. The persisted encoder-device preference names the preferred post-capture processing plus encoding device, and the resolver builds the full role assignment; it is not an independent hardcoded encoder pin. There is no implicit cross-adapter transfer: this build executes only the all-on-one-adapter topology, and a different explicit device is refused through the transport policy (`CrossAdapterUnsupported`) with an honest admission error, never silently redirected. A display driven by an incompatible adapter produces that error, not an encoder selector that cannot take effect. [Encoding and containers](encoding-and-containers.md#pipeline-adapter-roles) owns the role model.

Use the acquired texture's description for dimensions and format. A duplication mode description is not sufficient evidence of the actual texture layout. The encoder and conversion resources are configured for the admitted source dimensions and color state. A size or incompatible color-state change ends that session rather than interpreting new pixels using old resources or metadata.

WGC pool surfaces are borrowed. Copy their pixels into owned resources before the pool can recycle them. A retained COM pointer alone does not make the pool's contents immutable. Held frames, ring entries and encoder input slots must never refer to a surface whose producer may overwrite it.

## Capture hubs and leases

An idle preview owns a subscription, not an independent untracked capture lifetime. The DXGI and WGC hubs key producers by capture technology and source, share published resources and close producers when no consumer needs them. Display preview normally uses duplication. Window and region preview use WGC, with a WGC fallback where the preview cannot consume a display's duplication resource.

A plain display preview and a recording must not compete for duplication ownership. After validation and the cancellation checkpoint, the preparation worker requests the engine lease and waits for the preview producer to release it. This blocking handshake stays off the GUI thread. Returning to an idle/completed/error state releases the lease and permits preview capture again. A held preview image can survive this handoff without keeping the duplication producer alive.

The visible page controls idle capture lifetime. Requested navigation and displayed navigation can temporarily differ while a page is loading; stopping preview merely because a destination was requested would blank a page the user is still viewing.

Keeping duplication open can affect desktop presentation paths on some systems. Refcounting and visibility-based shutdown are therefore resource and desktop-behavior boundaries, not just memory optimizations.

## Recovery without retargeting

| Event | Engine response |
|---|---|
| Transient duplication access loss | Hold the last valid picture and reopen the same runtime display identity |
| Access loss before the first display frame | Bounded start hold, up to 15 seconds; suspend the ordinary first-frame guard while reopening, then restart that guard |
| GPU/device removal | Fail the recording and finalize what can be finalized |
| Captured window closes | End the recording; never select a replacement window |
| Window is minimized or has no new content | Preserve the last frame; lack of motion alone is not a failure. WGC keeps delivering frames of a minimized window at its iconic caption size; those frames, and stale ones of that size dequeued after restore, are held back rather than read as a resize |
| Source dimensions change | Stop with the old/new dimensions and require a new recording |
| Webcam disappears | Hold its last image and retry the same camera |

The normal mid-session duplication reopen has no give-up deadline. Waiting does not recover lost pictures, but it permits audio and a stable held picture to continue. Runtime reopening uses its runtime display identifier. The stronger persisted display matcher described in [configuration](configuration-and-persistence.md#display-identity) is a separate concern; it must not be advertised as an absolute guarantee of physical identity during every topology mutation.

During capture-loss recovery, hold the composed picture. On a static healthy display or window, observe the immutable webcam snapshot before comparing visual keys. Recompose only when a visual generation changes. Upload camera pixels only for a new generation or a replaced texture. The camera mailbox reuses unreferenced payload buffers and preserves snapshots still held by a consumer.

## Exclusive fullscreen and stalls

Window shape alone cannot distinguish borderless content from legacy exclusive fullscreen. Pre-flight classification combines shape, WGC evidence and corroborating fullscreen signals. No-frame evidence is not a pixel analysis of whether a picture is black. A failed evidence probe supplies no evidence, never proof of a blocked capture.

The executable remedy is a confirmed change to monitor capture, with the wider recording scope and changed application-audio eligibility named before application. There is no silent window-to-monitor fallback and no capture injection or hook path.

The mid-session monitor watches frame production, not emitted CFR packets. Ten seconds without source progress can produce a standing stall notice for a fullscreen-shaped, live, visible window. A still borderless picture can meet that rule, so the message remains conditional. Display/region starvation needs corroboration such as a disconnected or sleeping display to raise the standing notice. The diagnosis never fabricates an exclusive-fullscreen cause from starvation alone. Fullscreen behavior remains hardware/title dependent and needs live evidence.

## Recording preview transport

The recording tap publishes the composited source image with cursor and webcam before encoder-format conversion. It is not a CPU copy of NV12/P010 and it is not a second capture of the desktop. Native HDR uses a shared linear HDR image that the preview tone-maps for SDR presentation. The already-PQ desktop path lacks this shareable/compositable image and has explicit overlay/preview limitations.

An NT shared handle and a keyed mutex transfer access between D3D11 devices. The producer takes key 0 and publishes key 1. The Quick render thread attempts key 1 with zero wait, copies into a cached private texture and releases key 0 promptly. Conversion and scene composition then use the private copy. Holding the producer's mutex across a full scene render would couple capture latency to UI latency.

The scene-graph node allocates on source adoption or replacement, not on every frame. Native D3D commands are bracketed by `beginExternalCommands()` and `endExternalCommands()`. Conversion establishes the state it needs rather than inheriting Qt scissor or blend state. The imported display texture uses the public, platform-specific Qt D3D11 native interface. Qt upgrades require focused bridge verification.

Source generations reject stale queued callbacks after replacement or stop. The GUI-side adapter retains a duplicate-capable handle; scene-graph reinitialization can rebuild consumers without requiring the capture source to recreate its texture. Readiness follows successful conversion/presentation, not just receipt of a handle. A cross-adapter open failure does not falsely switch preview to the recording tap; independent preview may remain, and must not be described as unconditional WYSIWYG.

## Producer-driven presentation

Each successful publication emits a lightweight edge after the texture is ready. Failed publication due to contention emits no edge. The single-slot transport already holds the newest picture, so edges coalesce rather than queue images.

`PreviewUpdateScheduler` permits one queued wake at a time. The GUI handler disarms the wake **before** requesting the render. Disarming afterward can swallow the final publication of a burst and leave a stale picture indefinitely. Bounded redundant renders are preferable to losing that final image.

Presentation debt survives window exposure, screen changes and scene-graph reconstruction. If a publication has not been followed by a render, the next usable lifecycle transition requests one without requiring mouse movement or a second source frame. With no new picture and no ordinary UI invalidation, preview does not perpetually schedule another render. Preview rate and recording rate remain independent.

A one-shot Ready screenshot copies the retained preview source into its own bounded worker operation and reads back only the final still for PNG encoding. Recording and paused screenshots use the engine-owned snapshot route, including the last completed slot when paused. These explicit still-image readbacks do not change the continuous GPU-only preview contract.

## Implementation and tests

See [video worker](../../libs/engine/src/video_thread.cpp), [preview tap](../../libs/engine/include/exosnap/engine/preview_tap.h), [Quick preview item](../../app/quick/ExoSnap/Quick/ExoPreviewItem.h), [composition](../../app/quick/ExoSnap/Quick/QuickApplication.cpp), and the capture/preview tests under [engine tests](../../libs/engine/tests) and [Quick tests](../../app/quick/tests). Use the [tracing guide](../dev/harness-and-tracing.md) for presentation debt and native-window checks.

## Preview resource budget

The global preview rate is independent of recording cadence: Off, 15, 30, 60 or 120 fps, default 60. Idle hub workers gate acquisition, ownership copies and publication at that rate. Off releases the idle subscription. Engine-fed preview retains the recording producer and its pre-encode tap, while consumption follows the preview cap. A pending frame retains presentation debt until one deadline wake can deliver it. Disabling and re-enabling preview rebuilds the current source-generation view.

## Pixel transfers and ownership

The normal hardware recording path is D3D11 capture, owned held/ring surfaces, GPU composition and color processing, GPU encoder-format conversion, then native D3D11 hardware encoder inputs. The backend interface accepts native surfaces, including future AMF/QSV implementations, without requiring CPU planes.

| Transfer | Contract |
|---|---|
| Capture pool to held surface or phase ring | GPU ownership copy protects pixels from producer reuse |
| Composition, HDR and RGB/YUV conversion | GPU format and overlay processing |
| Encoder input slots | Retained GPU resources remain valid until encoder completion |
| Shared preview to private render texture | GPU copy releases the producer mutex before Qt rendering |
| HDR statistics | Small asynchronous GPU-to-CPU measurement, not full-frame readback |
| Screenshot | Requested one-shot readback for image encoding |
| Webcam | Camera-owned CPU payload is copied into an immutable mailbox generation, then uploaded once per changed generation |
| Hardware editor decode | GPU-to-CPU readback and normalization followed by renderer upload remain the compatibility bridge between independently owned FFmpeg and Qt devices |

The surface inventory attributes logical owned texels by subsystem across adapters. It excludes external capture/decoder pools and driver overhead, and is not DXGI residency. Provider sampling stays off time-sensitive workers. Required lifetime copies and encoder slot counts must not be reduced merely to lower these numbers.
