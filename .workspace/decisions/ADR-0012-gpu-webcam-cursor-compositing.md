# ADR-0012: GPU Webcam and Cursor Compositing

## Status
Accepted

## Context

Webcam PiP and DXGI Output Duplication cursor compositing previously used CPU-side pixel work and
GPU readbacks. Webcam chroma key and OD cursor blending copied regions into staging textures, mapped
them for CPU reads, blended on the CPU, then uploaded pixels back to the GPU. That created avoidable
CPU/GPU synchronization in the video hot path and prevented live webcam placement/mirror/chroma edits
from matching the recorded output during a running session.

VideoProcessor multi-stream compositing was rejected. The required per-stream alpha behavior is not
portable enough for this recorder: Microsoft documents legacy feature-cap behavior where per-stream
planar alpha is unreliable, `VideoProcessorSetStreamAlpha` depends on optional
`D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ALPHA_STREAM`, and supported stream counts are driver-dependent.
The OD path also needs background + webcam + cursor, while chroma key still requires shader work.

Primary references:
- D3D11 video processor feature caps:
  <https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_video_processor_feature_caps>
- `ID3D11VideoContext::VideoProcessorSetStreamAlpha`:
  <https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11videocontext-videoprocessorsetstreamalpha>
- D3D11 video processor caps:
  <https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_video_processor_caps>
- NVENC programming guide:
  <https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/index.html>

## Decision

Use one D3D11 render-target pass for webcam and cursor overlays. `GpuCompositor` copies the BGRA
capture texture to a BGRA render target, draws the webcam quad with bilinear scaling, mirror, optional
chroma-key alpha, then draws the OD cursor quad with source alpha. The existing single-stream
`VideoProcessorBlt` then converts BGRA to NV12 for the existing NVENC slot ring.

Encoder input remains NV12. This keeps existing color conversion, slot registration, and CFR reference
duplication unchanged instead of switching to NVENC ARGB input.

Webcam frames remain CPU-resident in `WebcamService`. The live frame is uploaded one-way to a D3D11
texture; the D3D11 device/context stay VideoThread-exclusive per ADR-0009. Webcam GPU residency via
`IMFDXGIDeviceManager` is deferred.

Live overlay state is represented by `WebcamOverlayLive`, a sanitized mutable subset of
`WebcamConfig`. `RecorderSession::UpdateWebcamOverlay` updates that snapshot while recording; device,
resolution, and FPS remain restart-class settings and are not live-swapped.

Shader bytecode is compiled with runtime `D3DCompile` for this slice. Offline `fxc` bytecode would
avoid the runtime compiler DLL dependency, but the repository has no established `fxc` discovery or
shader asset pipeline yet. This choice should be revisited if shader count grows.

## Consequences

- Webcam/chroma/cursor compositing no longer performs D3D11 readback maps in `VideoThread`.
- WGC and DXGI OD share the same compositor path; OD draws webcam below cursor.
- OD region crop keeps the full monitor compositor target and lets the existing VideoProcessor source
  rect crop the final frame.
- Overlay placement, mirror, chroma, and enable state can update during Recording and Paused states.
- Device/resolution/FPS changes remain locked during active recording.

## Related

- ADR-0008: Live Preview in Record View
- ADR-0009: Engine Threading and Lifecycle
- ADR-0011: DXGI Output Duplication for Monitor Capture
