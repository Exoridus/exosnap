# ADR 0004: Output Scaling And Format Controls

## Status

Accepted.

## Context

The MVP now exposes real output resolution, frame-rate, timing, container, and codec controls. These settings must affect the encoded output, not just UI summaries, while preserving the engine/UI boundary.

Before this change the recorder encoded the captured source size, or the Region crop size, aligned down to even NV12 dimensions. Fixed output scaling and letterboxing did not exist. Frame-rate and CFR/VFR fields existed in lower layers, but Settings still presented frame rate as a disabled 60 fps placeholder.

## Decision

`OutputSettingsModel` owns canonical output-size intent:

- `Native`
- `UHD2160`
- `QHD1440`
- `FHD1080`
- `HD720`
- optional `Custom` fields in the schema, not exposed in UI yet
- `OutputFitMode::Contain`

`Native` means the effective capture content size frozen at recording start:

- Display: selected display texture dimensions
- Window: selected window capture texture dimensions
- Region: selected Region dimensions after monitor-local clipping

If a Window capture texture changes size during a session, recording fails honestly and asks for a restart rather than silently feeding mismatched textures into a fixed encoder configuration.

The recorder computes one shared contain-fit rectangle with `recorder_core::ResolveContainRect`. The video path uses it for GPU source scaling and letterbox placement. Webcam placement remains relative to the captured content rectangle, so PiP stays out of letterbox bars. DXGI cursor composition remains above webcam in the compositor before scaling.

The GPU path remains:

```text
capture BGRA texture
-> optional Region source rect
-> source-space webcam/cursor composition
-> D3D11 VideoProcessorBlt source rect to contain-fit output rect
-> black video-processor background for letterbox
-> NV12 encoder input
-> NVENC
-> muxer
```

No CPU full-frame scaling is introduced.

Frame rate remains rational (`num/den`) in the canonical video model. Settings exposes 24/25/30/50/60. 120 fps is visible as unavailable until source and encoder capability can be proven for the active pipeline.

CFR uses the existing QPC scheduler and duplicates/drops deterministically. VFR uses captured frame arrival timestamps and remains available for MKV/WebM. MP4 is reconciled to CFR because the current Media Foundation mux path is fixed-rate.

## Consequences

- Preset schema version is incremented.
- Old incompatible preset data resets under the pre-v1 schema policy.
- Codec/container reconciliation is shared through the preset reconciliation helper and coordinator runtime reconciliation.
- Effective result metadata reports actual output dimensions, timing, container, and codecs.
- Visual Test Harness scenarios carry deterministic requested/effective output and format fields without performing real recordings.

## Non-goals

- Custom output-size UI.
- Crop-to-fill or stretch modes.
- Scene graphs, multiple outputs, streaming, rotation, AI upscaling, or post-record transcoding.
- WGC manual cursor composition; existing WGC cursor capture behavior is preserved.
