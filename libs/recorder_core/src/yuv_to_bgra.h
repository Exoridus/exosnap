#pragma once

// Pure CPU YUV (4:2:0 planar) -> BGRA8888 conversion, shared by the
// CaptureFrame snapshot path and the live preview tap (Strand 3 slice 1).
//
// Prior to this helper, video_thread.cpp's snapshot path hard-coded a
// BT.601 limited-range matrix regardless of what the session actually
// configured (ColorMetadata: BT.709 matrix, selectable Full/Limited range —
// see recorder_core/color_metadata.h). That mismatch is a real color bug:
// the encoder always writes BT.709-tagged output, so a BT.601 CPU readback
// looked visibly different (mild but real hue/saturation shift, most visible
// on skin tones and saturated UI colors) from what the encoded video
// actually contains. This helper takes the matrix/range as parameters so
// both call sites convert using the color space the session actually wrote.
//
// No D3D11/GPU dependency: pure math over caller-supplied mapped buffers, so
// it is unit-testable without a device and safe to call from any thread.

#include <recorder_core/color_metadata.h>

#include <cstdint>

namespace recorder_core {

// The subset of ColorMetadata that affects the YUV<->RGB matrix.
struct YuvToBgraParams {
    MatrixCoefficients matrix = MatrixCoefficients::Bt709;
    ColorRange range = ColorRange::Full;
};

// Describes one planar 4:2:0 YUV frame to convert:
//   - NV12  (bits_per_sample == 8):  Y plane 1 byte/sample; UV plane
//     interleaved U,V 1 byte/sample each, 2x2 subsampled.
//   - P010  (bits_per_sample == 10): Y plane 2 bytes/sample (16-bit
//     little-endian words, the active 10 bits left-justified in bits 15:6,
//     per the DXGI_FORMAT_P010 definition); UV plane interleaved U,V the
//     same way, 2x2 subsampled.
// *_stride_bytes are the mapped RowPitch values and may exceed
// width * bytes-per-sample when the source buffer is padded.
struct PlanarYuv420Frame {
    const uint8_t* y_plane = nullptr;
    uint32_t y_stride_bytes = 0;
    const uint8_t* uv_plane = nullptr;
    uint32_t uv_stride_bytes = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bits_per_sample = 8; // 8 (NV12) or 10 (P010)
};

// Converts one planar 4:2:0 YUV frame (NV12 or P010) to top-down BGRA8888
// (B, G, R, A byte order; alpha always 255/opaque).
//
// out_bgra must have at least `height * out_stride_bytes` bytes available;
// out_stride_bytes must be >= src.width * 4. Does nothing if src.width,
// src.height, a plane pointer, or out_bgra is 0/null.
void ConvertYuv420ToBgra(const PlanarYuv420Frame& src, const YuvToBgraParams& params, uint8_t* out_bgra,
                         uint32_t out_stride_bytes);

} // namespace recorder_core
