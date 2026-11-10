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

During capture-loss recovery, hold the composed picture. On a healthy static display or window, sample the host visual scene before comparing visual keys. Both CFR and VFR can recompose an overlay change over the retained desktop at the existing output cadence. PTS remains monotonic. A capture-recovery hold never touches invalidated display resources to update an overlay.

## Overlay composition

The recording compositor draws in source coordinates: desktop, cursor, then the accepted host scene items in their declared order. CPU images and gamepad atlases use the existing overlay shader, sRGB handling, straight-alpha blending and HDR overlay reference white. Shared placement, opacity, output visibility and chroma properties apply once at the item boundary.

`RecorderConfig::visual_scene` retains passive host data through worker shutdown. The recording thread never calls an application provider or QObject to obtain frame content. Each consumer prepares its own scene lease, image cursors and gamepad scratch outside frame acquisition and composition. Sampling makes one try-lock attempt at each data boundary. A busy scene/image boundary defers the whole new composition and retains the last completed output. A revoked binding removes that contribution after a successful read; contention never authorizes an old GPU cache. Frame release drops pins without reclaiming payloads. Control-side collection owns reclamation and resource-budget release.

A same-camera live control change retains the previously accepted image with its original request stamp until a new image arrives. Device or capture-mode changes clear accepted content. New requests never relabel old pixels or admit old producer callbacks.

CPU image storage admits three frame slots and at most two resource generations by default. A configured byte ceiling includes staged, active and retiring payloads and sampler scratch. Image upload caches identify the source incarnation, request, resource and accepted publication sequence. A repeated sequence from a different incarnation is a new image. Cache maintenance removes absent sources outside the frame path and invalidates the corresponding reader cursor before an image can be reused.

A sprite layer is an immutable atlas plus prepared quads, defined in [overlay sprites](../../libs/engine/include/exosnap/engine/overlay_sprites.h). Each item has independent scratch and sampler fallback state, even when items share the same source. Changes to atlas identity, quads, masks, placement or opacity dirty the existing visual key. Stick movement does not copy or re-upload atlas pixels.

The experimental Stage A cache preserves prepared scene items and sampler scratch across property-only publications when ordered item identities, binding indexes and all source token/resource/kind identities match. The replacement scene lease pins the immutable resources before retained samplers run again. Live placement, opacity, visibility, chroma settings and frame content still come from the current publication. Changed topology or resources rebuild the plan. Within a single sprite-layer draw, invariant shader, topology, sampler and constant-buffer bindings can be reused between quads. Reuse never crosses a public compositor call because other immediate-context users can change that state. Dynamic constants, texture bindings and render-target/resource unbinding retain their existing behavior. `EXOSNAP_STAGE_A_CACHE=1` enables this experiment. It is disabled by default pending native performance and output qualification.

The independent `EXOSNAP_DEMAND_COMPOSITION=1` pilot can compose at the reduced output size for a single same-aspect BGRA8 SDR recording. Eligibility excludes crop, cursor, 10-bit, 4:4:4 and HDR/tone-mapping paths. An attached preview consumer must publish a known physical-pixel demand that fits the output; with no preview consumer, only recording demand constrains the size. Preview demand includes device pixel ratio, zoom and visibility. Increased or unknown preview demand promotes composition to the retained source-size compositor once per recording. Later reductions wait for a new recording. The source-size fallback and any reduced compositor remain allocated through the session, and the reduced path still copies the full source before scaling. A smaller raster alone therefore establishes neither a memory saving nor a performance gain. This pilot is disabled by default pending native performance and output qualification. General multi-destination and color-branch planning remains in the [accepted demand-composition design](../dev/compositor-demand-aware-rendering.md).

The existing recording diagnostics cadence samples runtime-lifetime CPU publication and host try-lock counters. Recording-session history counters distinguish attempted samples, busy reads and missing input. The GUI copies provider status and exact source/request identities from the accepted scene into diagnostics without exposing provider ownership to the engine. Missing observations remain unavailable; an observed zero is reported as zero. The final session snapshot retains sampled runtime totals.

### Gamepad source and artwork

The Qt-free gamepad library uses GameInput v4 reading and device callbacks. Each application-local device ID has
its own bounded timestamp history. IDs distinguish identical VID/PID pairs. Partial auxiliary readings merge only
with that same device's state at or before the reading timestamp. Disconnect publishes a neutral state.
The initial GameInput microsecond/QPC-100ns anchor is sampled between two QPC reads. The video thread queries
history at its actual frame time, without a UI timer in the input-to-recording path.

The Qt-free `GamepadState` exposes 30 canonical controls through `ReadControl`.
Its populated canonical array is authoritative. Raw GameInput fields remain a compatibility input for older
callers, converted centrally when `controls_normalized` is false. Renderers consume `ControlValue`, not provider
bit positions or SVG identifiers. `Released` and `Active` carry valid values. `Unavailable` means no usable
reading for a potentially supported control, while `Unsupported` means the provider/device does not expose it.
Buttons use x = 0 or 1, triggers x in [0, 1], and sticks x/y in [-1, 1] with positive y up.
Unknown values are not evidence of released controls. Presentation deadzones do not alter captured input.

`NormalizeGameInputControls` explicitly translates standard GameInput button positions and its paddle fields.
The live provider consults `GetGamepadInfo().supportedLayout` and `GetSupportedSystemButtons()` before exposing
controls. Guide/share use system readings, with the existing DualSense PS/touchpad-click path retained.
Touchpad click is independent of touch contact. GameInput trigger-button flags do not by themselves prove a
separate mechanical end click. The live provider does not qualify them as `misc3`/`misc4`.
A qualified provider can supply these states through the canonical array or explicitly enable separate-click
normalization. Selecting GameCube artwork does not identify or remap an adapter. Its physical B/X wiring and
separate end-click reporting remain unqualified.

The SDL3 symbol correspondence and additive mapping schema are documented with the
[controller artwork contract](../../app/assets/gamepad/controllers/README.md#logical-controls).
SDL3 supplies the semantic convention only and is not a runtime dependency. SDL enum values, GameInput masks
and browser indices are separate domains. The same state contract is available to other renderers without
requiring any artwork identifier or provider mask.

GamepadOverlay owns input capture and app-side artwork. SVG groups rasterize only when artwork or settled
recording-content size changes. A resize temporarily transforms existing quads, then debounces rasterization.
Raster work runs on one joined worker. Stale results are discarded before GUI delivery.
The immutable atlas stores straight-alpha BGRA8, transparent gutters and edge RGB for bilinear filtering.
Knockout tiles are prepared with the artwork. The body `SpriteQuad` carries up to two `SpriteMask` entries,
each referencing the same atlas and a destination in content coordinates. Each mask follows its cap's radial
movement. The GPU multiplies only the body's alpha by the uncovered fraction of both masks before blending.
It neither erases the framebuffer background nor masks other control quads. Caps are drawn normally afterward.
Stick movement changes quad/mask destinations, without copying, rasterizing or uploading atlas pixels.
Button, trigger, opacity and placement changes also reuse the atlas. Unbound controls remain static artwork.

The GUI compares input state only to request preview updates. Recording and preview consumers sample retained `HistoryReadView` data with the existing Qt-free gamepad math and canonical masks. A history view owns data independently of the GameInput driver. The sample point is the CFR output slot or the existing VFR timestamp. A new request recreates sampler fallback state, preventing a prior device's pose from surviving an identity change. Busy or missing history uses only the current binding's safe historical or neutral output.

The x64 GameInputRedist.dll and GameInputRawInputProxy.exe ship beside the executable through the common CMake
install tree used by both package formats. Runtime terms, Microsoft notices and the MIT loader notice ship together.
The loader uses the newest supported local/system runtime. No GameInput MSI is installed.
Microsoft recommends retaining the process-wide GameInput instance until application shutdown.
With the 4.0.302 runtime, final IGameInput release can block after successful callback unregistration.
One process reference is therefore retained and reused across source restarts. Each source unregisters both
callbacks before freeing its context and releases its own interface reference. This bounds retention to one
runtime instance, rather than leaking one instance per recording. Omitting the final release is a workaround,
not a documented GameInput requirement. Clean final runtime release remains unqualified.

## Exclusive fullscreen and stalls

Window shape alone cannot distinguish borderless content from legacy exclusive fullscreen. Pre-flight classification combines shape, WGC evidence and corroborating fullscreen signals. No-frame evidence is not a pixel analysis of whether a picture is black. A failed evidence probe supplies no evidence, never proof of a blocked capture.

The executable remedy is a confirmed change to monitor capture, with the wider recording scope and changed application-audio eligibility named before application. There is no silent window-to-monitor fallback and no capture injection or hook path.

The mid-session monitor watches frame production, not emitted CFR packets. Ten seconds without source progress can produce a standing stall notice for a fullscreen-shaped, live, visible window. A still borderless picture can meet that rule, so the message remains conditional. Display/region starvation needs corroboration such as a disconnected or sleeping display to raise the standing notice. The diagnosis never fabricates an exclusive-fullscreen cause from starvation alone. Fullscreen behavior remains hardware/title dependent and needs live evidence.

## Recording preview transport

The recording tap publishes the composited source image with cursor and webcam before encoder-format conversion. It is not a CPU copy of NV12/P010 and it is not a second capture of the desktop. Native HDR uses a shared linear HDR image that the preview tone-maps for SDR presentation. A desktop captured already in PQ/BT.2020 is linearized into the same FP16 image first, so overlays and the preview tap work on every capture format.

An NT shared handle and a keyed mutex transfer access between D3D11 devices. The producer takes key 0 and publishes key 1. The Quick render thread attempts key 1 with zero wait, copies into a cached private texture and releases key 0 promptly. Conversion and scene composition then use the private copy. Holding the producer's mutex across a full scene render would couple capture latency to UI latency.

The scene-graph node allocates on source adoption or replacement, not on every frame. Native D3D commands are bracketed by `beginExternalCommands()` and `endExternalCommands()`. Conversion establishes the state it needs rather than inheriting Qt scissor or blend state. The imported display texture uses the public, platform-specific Qt D3D11 native interface. Qt upgrades require focused bridge verification.

An idle hub source carries no overlay. The Quick render node consumes the host scene and composites over its retained clean desktop copy using the recording compositor and content mapping. A source-only update can recompose without acquiring another desktop frame or consuming transport presentation debt. Idle preview includes output-hidden editing items at reduced opacity. A recording tap already contains output-visible items, so preview samples only hidden editing items and otherwise bypasses the extra compositor pass. A camera drag supplies a preview-only draft rectangle until the existing release gesture commits the item edit. Ready-frame capture uses recording visibility and the accepted item geometry, excluding the uncommitted draft.

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

The recording tap has its own 30 ms minimum publication interval and only receives newly composed frames. Pure CFR repeats do not publish another preview frame. At a regular 60 fps composition cadence this gate permits about 30 publications per second; transport contention or absent new compositions can lower that rate. The 60 and 120 fps UI choices remain consumption caps and do not raise this publisher cadence. Keep the requested cap, gate passes, successful publications and actual presentations distinct when diagnosing or measuring preview behavior.

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
| Webcam | The camera's native NV12 or YUY2 bytes (or BGRA, or MF-decoded NV12 for MJPEG) are copied once into an immutable mailbox generation, uploaded once per changed generation on each consuming D3D11 device, and converted to RGB on the GPU |
| Hardware editor decode | GPU-to-CPU readback and normalization followed by renderer upload remain the compatibility bridge between independently owned FFmpeg and Qt devices |

The surface inventory attributes logical owned texels by subsystem across adapters. It excludes external capture/decoder pools and driver overhead, and is not DXGI residency. Provider sampling stays off time-sensitive workers. Required lifetime copies and encoder slot counts must not be reduced merely to lower these numbers.
