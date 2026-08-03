#pragma once

// Hardware-decode readback support for EditPlayerEngine's D3D11VA path
// (docs/superpowers/specs/2026-08-03-editor-playback-hw-decode-design.md).
//
// FFmpeg's D3D11VA hwaccel hands decoded frames back (after
// av_hwframe_transfer_data copies them to system memory) as semi-planar
// AV_PIX_FMT_NV12 (8-bit 4:2:0) or AV_PIX_FMT_P010LE (10-bit, samples
// left-justified into the high 10 bits of each 16-bit word -- the transfer
// does not undo this DXGI convention, and it applies to BOTH the luma and
// chroma planes, not just chroma). This codebase's own DecodedPixelFormat
// convention is fully planar YUV420P/YUV420P10LE, with 10-bit codes as PLAIN
// values in [0, 1023] and no P010 left-justification (see
// recorder_core::DecodedPixelFormat::Yuv420P10's own contract, and
// EditFrameGpuConverter's SDR scale factors / hdr_pq.h's PQ dequant, which
// both assume that range). A readback frame therefore needs both a
// semi-planar-to-planar split AND, for 10-bit, a >>6 rescale before it can
// flow through the existing WrapRawDecodedFrame/ConvertToDecodedFrame path
// unchanged.

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace recorder_core {

// True for the two hardware readback pixel formats DeinterleaveHwReadbackFrame
// accepts. 4:4:4 hardware readback is deliberately not supported here yet --
// the design doc's "Not yet verified" note requires confirming the exact
// DXGI surface layout on real hardware before this grows a third format.
[[nodiscard]] bool IsSupportedHwReadbackFormat(int av_pix_fmt) noexcept;

// Converts a semi-planar hardware readback frame (`src`, already transferred
// to system memory) into a freshly allocated, fully-planar AVFrame matching
// AV_PIX_FMT_YUV420P (from NV12) or AV_PIX_FMT_YUV420P10LE (from P010LE,
// every sample rescaled from P010's <<6 left-justified range down to a plain
// 10-bit code). Returns nullptr when `src` is null, its format is not one
// IsSupportedHwReadbackFormat accepts, its dimensions are non-positive, or
// allocation fails. The caller owns the result (av_frame_free).
[[nodiscard]] AVFrame* DeinterleaveHwReadbackFrame(const AVFrame* src);

} // namespace recorder_core
