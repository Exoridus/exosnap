# Destination-demand composition and render-plan caching

> **Status:** Stage A and a bounded single-output SDR demand pilot are implemented and disabled by default. Native performance and output qualification remain open. General multi-destination and color-branch planning is accepted, not implemented.
> [Capture and preview](../architecture/capture-and-preview.md) owns the implemented flags, eligibility and lifetime contracts. This document describes remaining design and qualification work without claiming performance gains or multi-destination support.

## Decisions and scope

- Qualify **Stage A** independently of demand sizing: keep Direct3D 11, reuse scene topology and immutable render state, bind changing source frames and per-item parameters dynamically.
- Test whether composition can run at the *effective physical-pixel resolution demanded by active consumers* rather than always at capture-source dimensions. Resolution and color are separate dimensions of a render profile.
- Treat the **Preview as a consumer of the composed image** and the editor of scene intent. It does not capture its own Qt UI or use an encoded/decompressed video image as an input. Selection handles and editing chrome are not recorded.
- Do not implement D3D11 deferred-command-list replay (Stage B), D3D12, Vulkan, or wgpu ports as part of this work. Revisit them only if measurements establish a meaningful bottleneck.
- Do not replace `VisualSource`, `VisualSceneRuntime`, the existing recorder/encoder, or the host-owned compositor with an extension-owned scene renderer.
- Keep a prospective sandboxed browser/HTML surface for **external plugins after V3**, rather than building native chat text as a mandatory substitute for custom HTML/CSS/emotes now.

## Current baseline

1. `libs/engine/src/video_thread.cpp`: default SDR capture is composited in source coordinates/size, then `VideoProcessorBlt` applies crop, contain-fit output geometry, letterboxing, scaling, and RGB-to-NV12/P010 conversion. The native HDR and 4:4:4 paths have distinct conversions. The opt-in demand pilot moves scaling before composition for its eligible subset and retains the source-size fallback.
2. `libs/engine/src/gpu_compositor.cpp`: D3D11 shaders, samplers and blend states are created in `Init` and reused; video/camera texture and sprite-atlas uploads are cached using generation, dimensions, token and resource identity where applicable. The opt-in Stage A cache adds prepared scene reuse and bounded invariant sprite-state reuse.
3. `libs/engine/src/visual_scene_consumer.cpp`: the consumer refreshes prepared item/binding state when publication sequence/invalidation requires it. `app/extensions/VisualSourceManager.cpp` publishes scene items in order. Existing source/scene revision and lease contracts must remain authoritative.
4. `app/quick/ExoSnap/Quick/ExoPreviewItem.cpp`: the preview has change-driven updates, a separate scene consumer and cached visual resources. Idle preview may compose independently; recording preview consumes the engine-composited feed. Both must keep the same accepted scene intent and color semantics.
5. `docs/architecture/color-management.md`: SDR and native-HDR paths have explicitly different working/output representations. Source-specific HDR conversion and 80-nit scRGB normalization/overlay reference white must not be lost.
6. The demand pilot handles one reduced, same-aspect SDR recording with compatible known preview demand or no preview consumer. A general multi-destination render graph, mixed color branches and live resizing back to smaller targets remain unimplemented. Both experiment flags default off.

## Logical render demand and resolution policy

The existing **RenderDemand** planner handles compatible full-scene SDR requests; the recording pilot supplies one output demand and separately checks preview pixels. The future general consumer contract below adds broader output, color and live reconfiguration policy. A consumer publishes demand at a stable control boundary, including scene/view identity, physical output dimensions, zoom/crop, cadence, color and alpha requirements, visibility and transformation policy. Outputs may be one-shot (screenshot) or continuous.

- One active output and no more demanding visible preview: compose only the pixels that output needs, subject to source quality and format correctness.
- Multiple **compatible** consumers: calculate the cheapest shared composition representation that satisfies all of them, usually the largest required pixel dimensions for a common aspect, scene layout, timebase, and working color representation. Downsample after shared composition for smaller outputs.
- A Preview demand counts while the Preview is **actually visible**, including its *physical* viewport size (logical Qt size times device pixel ratio), crop and zoom. Do not select based on full desktop resolution or on a hidden preview. Preserve editable/zoomed detail and WYSIWYG placement. A small preview need not force source-resolution composition when the only output is 720p.
- Different aspect ratios, different crop/layout/visibility rules, different output colors or materially different cadence cannot always share a single final frame. Group compatible demands and branch as required. A single maximum width/height or pixel count is **not** a universal solver; an aspect-conversion policy or separate composition may be necessary.
- SDR and HDR sinks should share a correctly tagged scene-linear working representation *only where correct and economical*, then receive distinct tone-map, color-space and encoder-format conversion branches. Do not tone-map away HDR information merely because an SDR preview exists.
- Avoid unnecessary upscaling of a low-resolution fully composed frame to a newly enabled larger output: render a sufficiently detailed composition or create a compatible higher-resolution branch. Source image detail is still bounded by the original source.
- No unconditional 1080p minimum. If quality comparisons establish a useful minimum/floor, make it a reasoned, bounded policy rather than an arbitrary hidden promise. An optional stable-preview quality floor may be evaluated as UX behavior, not assumed.
- During capture handoff and live destination addition/removal, do not expose black frames, stale scene revisions, wrong crop or HDR metadata, or reduce recording resolution unexpectedly. Build/validate a replacement render target off the hot path and publish it atomically after readiness. If live reconfiguration is not supported, defer changes explicitly rather than altering an accepted recording silently.
- Source texture resolution and scene render-target resolution are different: clip/NDC coordinates are resolution-independent, but rasterization, shader invocation, filtering, edge quality and bandwidth depend on output pixels.

### Example demands (same 16:9 scene and compatible color policy)

| Active consumers | Proposed composition size | Note |
|---|---|---|
| File 720p; hidden preview | 1280x720 | Potential large saving relative to 4K source; must validate pre-compose source scaling |
| File 720p; visible Preview 800x450 physical px | 1280x720 | Preview fits without upscaling |
| File 720p; visible Preview 2560x1440 physical px / zoom | At least preview-equivalent detail, e.g. 2560x1440 or a separately resolved preview branch | Do not blur the preview just to save file-output work |
| File 1080p + live 720p destination | Shared 1920x1080, then downsample | Only when scene/cadence/color are compatible |
| File 4K HDR + Virtual Camera 1080p SDR | Share correct scene data where practical; distinct HDR/SDR output conversion | Do not make SDR preview define the HDR working precision |

These are **design scenarios**, not current behavior or guaranteed optimal performance.

## Stage A render-plan cache

A **scene plan** stores ordering/grouping, source identity and roles, node presentation policy, static pipeline state and resource layout requirements. It does not store a frozen image. Physical GPU resources and frame-dependent binding should be managed independently from the plan.

- Source contents change (game, webcam, animated controller, chat, cursor): reuse the plan; update/rebind the frame/surface and dynamic constants. Do not rebuild on every timestamp or texture pixel update.
- Position, scale, opacity and per-node transform: update dynamic arguments; avoid unnecessary graph rebuild. In particular, an interaction should not need to reconstruct the whole draw sequence every mouse movement.
- Add/remove nodes, modify order, effect topology or blend/shader variant: invalidate only affected plan structures and refresh the stable ordering/grouping as needed.
- Source image format, texture identity/generation, device loss, render-target resolution or color working format: recreate or rebind **affected GPU resources**. These are not necessarily full scene-topology changes.
- Preserve `SourceToken`, `ConfigRevision`, leases, host-owned scene publication, cache retirement, and on-thread D3D11 immediate-context ownership. No extension may retain mutable producer-owned frames.
- Prefer retained D3D11 state objects + a compact precomputed draw schedule over `ID3D11CommandList`. Deferred command lists can carry recorded resource bindings and add driver-dependent playback overhead; **do not** add them without separate evidence.

### Remaining qualification and extension work

1. **Measurement:** capture CPU submission, GPU composition/video-processor timings, upload counts, render sizes, scene refresh and actual preview publication cadence. Record real output receipts and separate each experiment from its baseline.
2. **Stage A qualification:** exercise topology and resource invalidation, live transforms and changing source frames. Validate recorded output and Preview before enabling the cache by default.
3. **Demand-pilot qualification:** compare eligible SDR cases with the source-size compositor, including preview growth, zoom and promotion. Include retained fallback allocations and full-source copy/scaling costs in the comparison. Native-HDR, 4:4:4 and mixed-format cases retain their existing paths.
4. **Additional consumers:** extend demand aggregation only for concrete multi-resolution destinations and color branches, with independent compatibility and lifetime tests. Recorded artifacts and on-screen preview must both pass before rollout.

## Benchmark and visual acceptance

Compare baseline ExoSnap D3D11 with (A) render-plan caching and (B) the *separate* target-resolution pilot. Keep these two contributions measurable independently.

- Source: 4K60 game, 1080p webcam; source-stable, moving scene and high-motion cases. Output 720p/1080p/1440p/4K with compatible and conflicting aspect ratios.
- Visuals: 0, 1, 10 and 100 overlays; 2D/3D gamepad source, mouse pointer, transparency, transformed bounds, edges/text, alpha blending, animation. Browser/chat output is optional **synthetic** texture stimulus for now, not a V3 runtime dependency.
- Consumers: recording alone, visible Preview at small/large DPI-correct viewport, zoom/dragging, hidden Preview, two outputs with different resolutions, one-shot screenshot, HDR+SDR demands.
- Measure CPU submit cost (p50/p95/p99), GPU composition and scale/color pass durations separately, total frame/encode latency, source uploads, resource allocations/peak VRAM, duplication/drops and temporal stability. Avoid synchronous GPU query readback in hot loops; identify unmeasured quantities as unavailable.
- Validate reference imagery (crop/Contain, orientation, placement, opacity, pixel/alpha edges, text legibility, SDR/HDR luminance and signaling); decoded final file versus Preview and snapshot; recording FPS and exact source/scene identity; avoid asserting GPU utilization from combined GPU-timestamp spans.
- Establish an A/B acceptance threshold **after baseline measurements**, not as a claimed expected speedup. Revert/disable when savings do not exceed the complexity/regression cost.
- Verify no UI/Preview blurring during handoff or destination additions and no unintended scene-node reallocations on ordinary frame updates.

## Deferred external-browser/plugin design (V3+)

A future Twitch/custom-chat plugin should support existing HTML/CSS/JavaScript overlays, animated emotes and platform-supplied overlay URLs. A native chat renderer may be an optional alternative, **not** a required replacement or immediate feature.

Prefer investigating a **host-managed, sandboxed optional WebSurface service** shared by plugins. The plugin provides approved URL/content and parameters and receives a video/VisualSource surface or a bounded host-issued output capability. Host retains lifetime, GPU texture publication, output sync, network/permission policy, resource limits and teardown. A simple iframe alone does not provide sufficient process/resource isolation or permission control; use a dedicated browser engine/process with explicit origin/network and storage permissions. Provider login/secrets must not be exposed to arbitrary web content.

Do not ship Chromium/Qt WebEngine as an unconditional dependency until a real V3 plugin and measured start/memory overhead justify it. A plugin-bundled browser renderer is a possible evaluated alternative, not an architectural requirement.

Twitch's native chat embed and third-party browser overlays are different products with different customization and host/origin rules. Validate actual URLs, allowed embeds, transparent background behavior and third-party emotes in a future spike; do not assume arbitrary cross-origin CSS access or unrestricted local iframe embedding.

## Out of scope and integration guardrails

No migration to D3D12/Vulkan, cross-GPU transport, new codec backend, plugin SDK, broad recorder rewrite, user-facing GPU selector change, or automatic enabling of a future output graph is authorized by this plan. Keep source-scene semantics, preview capture exclusion, HDR correctness and current tests stable.

Implement in small focused PRs against `next`. Keep implementation PRs Draft through independent correctness, CI, decoded-file and real Preview acceptance. Do not auto-merge without explicit approval.

References: [capture and preview](../architecture/capture-and-preview.md), [encoding and containers](../architecture/encoding-and-containers.md), [color management](../architecture/color-management.md), [architecture overview](../architecture/overview.md), [roadmap](../roadmap.md).
