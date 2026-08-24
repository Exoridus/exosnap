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

#include <exosnap/engine/color_metadata.h>

#include <cstdint>

namespace exosnap::engine {

// The subset of ColorMetadata that affects the YUV<->RGB matrix.
// NOTE: both call sites (video_thread.cpp CaptureFrame + live preview tap)
// always explicitly assign `.range` from the session's live ColorMetadata
// before calling ConvertYuv420ToBgra, so this member default is never actually
// relied upon in production — kept in sync with ColorMetadata's default
// (fix/color-range-signaling) purely for hygiene/consistency.
struct YuvToBgraParams {
    MatrixCoefficients matrix = MatrixCoefficients::Bt709;
    ColorRange range = ColorRange::Limited;
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

// Describes one fully-planar 4:2:0 YUV frame: separate Y, U, V planes (no
// chroma interleaving). This is the layout FFmpeg's software decoders use
// (AV_PIX_FMT_YUV420P for 8-bit, AV_PIX_FMT_YUV420P10LE for 10-bit) -- as
// opposed to PlanarYuv420Frame's semi-planar interleaved-UV layout, which
// models the DXGI capture/encode surfaces (NV12/P010).
//
// *_stride_bytes are the row pitch and may exceed width/2 (chroma) or width
// (luma) * bytes-per-sample when the source buffer is padded.
//
// 10-bit (YUV420P10LE) samples are plain 16-bit little-endian values in
// [0, 1023] -- unlike P010, there is no <<6 left-justification.
struct FullPlanarYuv420Frame {
    const uint8_t* y_plane = nullptr;
    uint32_t y_stride_bytes = 0;
    const uint8_t* u_plane = nullptr;
    uint32_t u_stride_bytes = 0;
    const uint8_t* v_plane = nullptr;
    uint32_t v_stride_bytes = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bits_per_sample = 8; // 8 (YUV420P) or 10 (YUV420P10LE)
};

// Converts one fully-planar 4:2:0 YUV frame to top-down BGRA8888 (B, G, R, A
// byte order; alpha always 255/opaque). Same matrix/range semantics as
// ConvertYuv420ToBgra -- only the input memory layout differs.
//
// out_bgra must have at least `height * out_stride_bytes` bytes available;
// out_stride_bytes must be >= src.width * 4. Does nothing if src.width,
// src.height, a plane pointer, or out_bgra is 0/null.
void ConvertFullPlanarYuv420ToBgra(const FullPlanarYuv420Frame& src, const YuvToBgraParams& params, uint8_t* out_bgra,
                                   uint32_t out_stride_bytes);

// --- Implementation seam (exposed for tests, not for callers) --------------
//
// ConvertFullPlanarYuv420ToBgra above picks between these two at runtime. The
// 8-bit path is the editor player's hot loop -- measured at 13.3 ms per
// 2560x1440 frame scalar against a 16.7 ms budget for 60 fps -- so it has a
// hand-written SIMD implementation that is 4.6x faster and bit-for-bit
// identical. Callers should keep using the dispatching function; these exist
// so a test can assert that "bit-for-bit identical" directly, on a machine
// where the dispatcher would only ever exercise one of them.

// The portable reference implementation. Always available, always correct,
// handles 8- and 10-bit.
void ConvertFullPlanarYuv420ToBgraScalar(const FullPlanarYuv420Frame& src, const YuvToBgraParams& params,
                                         uint8_t* out_bgra, uint32_t out_stride_bytes);

// True when this CPU has the SSE4.1 instructions the SIMD path below needs
// (Intel from 2008, AMD from 2011). Result is queried once and cached.
[[nodiscard]] bool CpuSupportsYuvToBgraSimd() noexcept;

// 8-bit-only SIMD implementation, 8 pixels per iteration. Falls back to the
// scalar routine above for 10-bit input and for the columns of a width that
// is not a multiple of 8. MUST NOT be called unless
// CpuSupportsYuvToBgraSimd() returned true.
void ConvertFullPlanarYuv420ToBgraSimd(const FullPlanarYuv420Frame& src, const YuvToBgraParams& params,
                                       uint8_t* out_bgra, uint32_t out_stride_bytes);

// Describes one fully-planar 4:4:4 YUV frame: separate Y, U, V planes, each
// at the FULL frame resolution (no chroma subsampling at all -- every pixel
// carries its own U and V sample, unlike FullPlanarYuv420Frame's row/2,col/2
// chroma addressing). This is the layout FFmpeg's software H.264/HEVC
// decoders produce for High 4:4:4 Predictive / HEVC RExt bitstreams
// (AV_PIX_FMT_YUV444P).
//
// 8-bit only, deliberately: the product's 4:4:4 recording option (Settings ->
// Video, Expert, docs/product-spec.md "Chroma") is gated to H.264/HEVC at
// 8-bit only -- no AV1 4:4:4, no 10-bit 4:4:4, no 4:4:4 with HDR10 (which
// requires 10-bit). AV_PIX_FMT_YUV444P10LE can therefore never appear in a
// clip this product ever wrote, so there is no 10-bit branch here (contrast
// FullPlanarYuv420Frame's bits_per_sample field, which the 4:2:0 path needs
// because 4:2:0 recording DOES ship at both 8- and 10-bit).
//
// *_stride_bytes are the row pitch and may exceed width when the source
// buffer is padded.
struct FullPlanar444Frame {
    const uint8_t* y_plane = nullptr;
    uint32_t y_stride_bytes = 0;
    const uint8_t* u_plane = nullptr;
    uint32_t u_stride_bytes = 0;
    const uint8_t* v_plane = nullptr;
    uint32_t v_stride_bytes = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Converts one fully-planar 4:4:4 YUV frame (AV_PIX_FMT_YUV444P) to top-down
// BGRA8888 (B, G, R, A byte order; alpha always 255/opaque). Same
// matrix/range semantics as ConvertYuv420ToBgra, and the same 8-bit fixed-
// point math ConvertAyuvToBgra uses for its packed 4:4:4 layout -- only the
// memory layout differs (three separate planes here vs. one packed
// [V,U,Y,A] plane there). No chroma upsampling: each pixel carries its own
// U, V, so this loop has no pair/block handling at all, unlike the 4:2:0
// converters above.
//
// out_bgra must have at least `height * out_stride_bytes` bytes available;
// out_stride_bytes must be >= src.width * 4. Does nothing if src.width,
// src.height, a plane pointer, or out_bgra is 0/null.
void ConvertFullPlanar444ToBgra(const FullPlanar444Frame& src, const YuvToBgraParams& params, uint8_t* out_bgra,
                                uint32_t out_stride_bytes);

// --- Implementation seam (exposed for tests, not for callers) --------------
// Same dispatch convention as ConvertFullPlanarYuv420ToBgra above: a portable
// scalar reference, and an SSE4.1 implementation selected at runtime via
// CpuSupportsYuvToBgraSimd() when the CPU supports it.

void ConvertFullPlanar444ToBgraScalar(const FullPlanar444Frame& src, const YuvToBgraParams& params, uint8_t* out_bgra,
                                      uint32_t out_stride_bytes);

// 8 pixels per iteration, falls back to the scalar routine above for the
// columns of a width that is not a multiple of 8. MUST NOT be called unless
// CpuSupportsYuvToBgraSimd() returned true.
void ConvertFullPlanar444ToBgraSimd(const FullPlanar444Frame& src, const YuvToBgraParams& params, uint8_t* out_bgra,
                                    uint32_t out_stride_bytes);

// Describes one packed 4:4:4 AYUV frame (DXGI_FORMAT_AYUV): a single plane,
// 4 bytes per pixel, memory byte order [V, U, Y, A] (matching the AYUV encode
// surface the RGB->AYUV compute shader writes — see gpu_rgb_to_ayuv.cpp).
// `stride_bytes` is the mapped RowPitch and may exceed width * 4 when padded.
struct PackedAyuvFrame {
    const uint8_t* data = nullptr;
    uint32_t stride_bytes = 0; // row pitch
    uint32_t width = 0;
    uint32_t height = 0;
};

// Converts one packed 4:4:4 AYUV frame to top-down BGRA8888 (B, G, R, A byte
// order; alpha always 255/opaque). Uses the same 8-bit matrix/range semantics
// as ConvertYuv420ToBgra (with BT.709 and the session's range this is the exact
// inverse of the RGB->AYUV encoder shader) but with no chroma upsampling: each
// pixel carries its own V, U, Y. The source alpha byte is ignored. Pure,
// thread-safe, no GPU dependency.
//
// out_bgra must have at least `height * out_stride_bytes` bytes available;
// out_stride_bytes must be >= src.width * 4. Does nothing if src.width,
// src.height, src.data, or out_bgra is 0/null.
void ConvertAyuvToBgra(const PackedAyuvFrame& src, const YuvToBgraParams& params, uint8_t* out_bgra,
                       uint32_t out_stride_bytes);

} // namespace exosnap::engine
