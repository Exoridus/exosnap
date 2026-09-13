#pragma once

// Pure CPU-side I420 (planar 4:2:0) -> NV12 (semi-planar 4:2:0) conversion.
// NV12 is what the D3D11 encode texture ring uses everywhere else in this
// codebase (see video_thread.cpp's nv12Textures ring), and its Y-plane-then-
// interleaved-UV-plane layout is exactly what a single
// ID3D11DeviceContext::UpdateSubresource call with row pitch = width expects
// — the same upload pattern already used by tools/probes/probe_nvenc_async.

#include <cstdint>
#include <vector>

namespace exosnap::engine {

// Converts one I420 frame (width*height Y bytes, then (width/2)*(height/2) U
// bytes, then (width/2)*(height/2) V bytes — see Y4mFrame/I420FrameSize) into
// one NV12 frame (width*height Y bytes, then interleaved U/V at half
// resolution: U0 V0 U1 V1 ...). `out_nv12` is resized to exactly the NV12 frame
// size and fully overwritten.
//
// Precondition: `width` and `height` are both even and positive. Both layouts
// halve the dimensions for chroma, so the arithmetic here is exact for an even
// pair and loses the last row or column for an odd one -- and the caller would
// read past the end of the chroma planes it was promised. This is not checked at
// runtime: every caller obtains its dimensions from ParseY4mHeader, which rejects
// odd and zero dimensions at the boundary where they first become known, so a
// check here would be unreachable code in every build that exists. A new caller
// that gets its dimensions elsewhere owns that validation.
void ConvertI420ToNv12(const uint8_t* i420, uint32_t width, uint32_t height, std::vector<uint8_t>& out_nv12);

} // namespace exosnap::engine
