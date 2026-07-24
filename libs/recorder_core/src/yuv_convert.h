#pragma once

// Pure CPU-side I420 (planar 4:2:0) -> NV12 (semi-planar 4:2:0) conversion.
// NV12 is what the D3D11 encode texture ring uses everywhere else in this
// codebase (see video_thread.cpp's nv12Textures ring), and its Y-plane-then-
// interleaved-UV-plane layout is exactly what a single
// ID3D11DeviceContext::UpdateSubresource call with row pitch = width expects
// — the same upload pattern already used by tools/probes/probe_nvenc_async.

#include <cstdint>
#include <vector>

namespace recorder_core {

// Converts one I420 frame (width*height Y bytes, then (width/2)*(height/2) U
// bytes, then (width/2)*(height/2) V bytes — see Y4mFrame/I420FrameSize) into
// one NV12 frame (width*height Y bytes, then interleaved U/V at half
// resolution: U0 V0 U1 V1 ...). `width` and `height` must both be even.
// `out_nv12` is resized to exactly the NV12 frame size and fully overwritten.
void ConvertI420ToNv12(const uint8_t* i420, uint32_t width, uint32_t height, std::vector<uint8_t>& out_nv12);

} // namespace recorder_core
