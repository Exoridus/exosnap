// WARP-backed GPU tests for the editor-playback colour conversion pass
// (edit_frame_gpu_converter.*). The CPU converters are the source of truth:
//
//   - SDR (all three DecodedPixelFormat values) is pinned against
//     ConvertFullPlanarYuv420ToBgraScalar / ConvertFullPlanar444ToBgraScalar,
//     so the shader's constant buffer cannot drift from ComputeCoefs' matrix /
//     range / bit-depth math.
//   - HDR10 (is_pq_source) is pinned against P010PqPixelToMonitorBgr, the exact
//     (non-tabulated) monitoring chain in hdr_preview.h. The shipping
//     P010PqMonitorConverter samples that same chain into 1024-entry tables, so
//     it is cross-checked separately against a bound derived from the table's
//     own error rather than a fixed tolerance (see the last PQ test).
//
// Tolerance is +-2 codes per channel, matching test_gpu_hdr_tonemap.cpp: the CPU
// reference rounds 16.16 fixed-point where the GPU rounds a float UNORM write.

#include <gtest/gtest.h>

#include "hdr_preview.h"
#include "yuv_to_bgra.h"

#include <recorder_core/edit_frame_gpu_converter.h>

#include <d3d11.h>
#include <winrt/base.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace {

using recorder_core::ColorRange;
using recorder_core::DecodedPixelFormat;
using recorder_core::EditFrameGpuConverter;
using recorder_core::MatrixCoefficients;
using recorder_core::RawDecodedVideoFrame;

constexpr uint32_t kWidth = 16;
constexpr uint32_t kHeight = 16;
constexpr uint32_t kChromaW = kWidth / 2;
constexpr uint32_t kChromaH = kHeight / 2;

// Deliberately padded row pitches: a decoder's planes are padded, and the upload
// path has to honour the stride rather than assume a tight plane.
constexpr uint32_t kLumaPad = 8;
constexpr uint32_t kChromaPad = 4;

struct D3DTestDevice {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
};

// Same WARP-device construction as test_gpu_hdr_tonemap.cpp -- no real GPU
// needed, deterministic in CI.
D3DTestDevice CreateWarpDevice() {
    D3DTestDevice out;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selected{};
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                         levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                         out.device.put(), &selected, out.context.put());
    EXPECT_TRUE(SUCCEEDED(hr));
    return out;
}

winrt::com_ptr<ID3D11Texture2D> CreateBgraTarget(ID3D11Device* device, uint32_t width, uint32_t height) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    winrt::com_ptr<ID3D11Texture2D> tex;
    EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, tex.put())));
    return tex;
}

// Tightly-packed BGRA readback of a render target.
std::vector<uint8_t> ReadBgra(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    winrt::com_ptr<ID3D11Texture2D> staging;
    EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, staging.put())));
    context->CopyResource(staging.get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    EXPECT_TRUE(SUCCEEDED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)));
    std::vector<uint8_t> out(static_cast<size_t>(desc.Width) * desc.Height * 4u);
    for (UINT row = 0; row < desc.Height; ++row) {
        std::memcpy(out.data() + static_cast<size_t>(row) * desc.Width * 4u,
                    static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(row) * mapped.RowPitch,
                    static_cast<size_t>(desc.Width) * 4u);
    }
    context->Unmap(staging.get(), 0);
    return out;
}

// --- synthetic source planes ------------------------------------------------
//
// Seeded the way probe_edit_playback's StepC/StepC2 seed theirs (a linear index
// times a small odd stride), sized so the 8-bit luma plane walks all 256 codes
// and the 10-bit luma plane walks the whole [0, 1020] range.

struct Planes8 {
    std::vector<uint8_t> y, u, v;
    uint32_t y_stride = 0;
    uint32_t c_stride = 0;
};

Planes8 MakePlanes8(uint32_t chroma_w, uint32_t chroma_h) {
    Planes8 p;
    p.y_stride = kWidth + kLumaPad;
    p.c_stride = chroma_w + kChromaPad;
    p.y.assign(static_cast<size_t>(p.y_stride) * kHeight, 0u);
    p.u.assign(static_cast<size_t>(p.c_stride) * chroma_h, 0u);
    p.v.assign(static_cast<size_t>(p.c_stride) * chroma_h, 0u);
    for (uint32_t r = 0; r < kHeight; ++r) {
        for (uint32_t c = 0; c < kWidth; ++c) {
            p.y[static_cast<size_t>(r) * p.y_stride + c] = static_cast<uint8_t>((r * kWidth + c) & 0xFFu);
        }
    }
    for (uint32_t r = 0; r < chroma_h; ++r) {
        for (uint32_t c = 0; c < chroma_w; ++c) {
            const uint32_t i = r * chroma_w + c;
            p.u[static_cast<size_t>(r) * p.c_stride + c] = static_cast<uint8_t>((i * 3u) & 0xFFu);
            p.v[static_cast<size_t>(r) * p.c_stride + c] = static_cast<uint8_t>((i * 7u + 5u) & 0xFFu);
        }
    }
    return p;
}

struct Planes10 {
    std::vector<uint16_t> y, u, v;
    uint32_t y_stride_bytes = 0;
    uint32_t c_stride_bytes = 0;
};

// `pq` picks codes that make sense as HDR10 limited-range codes (luma spanning
// 64..940, chroma inside 64..960); otherwise the planes walk the raw [0, 1023]
// range the SDR 10-bit path has to handle.
Planes10 MakePlanes10(bool pq) {
    Planes10 p;
    const uint32_t y_stride_samples = kWidth + kLumaPad;
    const uint32_t c_stride_samples = kChromaW + kChromaPad;
    p.y_stride_bytes = y_stride_samples * 2u;
    p.c_stride_bytes = c_stride_samples * 2u;
    p.y.assign(static_cast<size_t>(y_stride_samples) * kHeight, 0u);
    p.u.assign(static_cast<size_t>(c_stride_samples) * kChromaH, 0u);
    p.v.assign(static_cast<size_t>(c_stride_samples) * kChromaH, 0u);
    for (uint32_t r = 0; r < kHeight; ++r) {
        for (uint32_t c = 0; c < kWidth; ++c) {
            const uint32_t i = r * kWidth + c; // 0..255
            const uint32_t code = pq ? (64u + (i * 876u) / 255u) : ((i * 4u) & 1023u);
            p.y[static_cast<size_t>(r) * y_stride_samples + c] = static_cast<uint16_t>(code);
        }
    }
    for (uint32_t r = 0; r < kChromaH; ++r) {
        for (uint32_t c = 0; c < kChromaW; ++c) {
            const uint32_t i = r * kChromaW + c; // 0..63
            const uint32_t u_code = pq ? (128u + i * 12u) : ((i * 16u + 3u) & 1023u);
            const uint32_t v_code = pq ? (940u - i * 12u) : ((i * 16u + 11u) & 1023u);
            p.u[static_cast<size_t>(r) * c_stride_samples + c] = static_cast<uint16_t>(u_code);
            p.v[static_cast<size_t>(r) * c_stride_samples + c] = static_cast<uint16_t>(v_code);
        }
    }
    return p;
}

// Runs one frame through the converter and returns the tightly-packed BGRA.
std::vector<uint8_t> ConvertOnGpu(D3DTestDevice& d3d, EditFrameGpuConverter& converter, const RawDecodedVideoFrame& f,
                                  float peak_scale = 1.0f) {
    auto dst = CreateBgraTarget(d3d.device.get(), f.width, f.height);
    std::string err;
    EXPECT_TRUE(converter.Convert(f, dst.get(), peak_scale, err)) << err;
    return ReadBgra(d3d.device.get(), d3d.context.get(), dst.get());
}

std::vector<uint8_t> ConvertOnGpu(D3DTestDevice& d3d, const RawDecodedVideoFrame& f, float peak_scale = 1.0f) {
    EditFrameGpuConverter converter;
    std::string err;
    EXPECT_TRUE(converter.Init(d3d.device.get(), d3d.context.get(), err)) << err;
    return ConvertOnGpu(d3d, converter, f, peak_scale);
}

void ExpectBgraNear(const std::vector<uint8_t>& gpu, const std::vector<uint8_t>& cpu, int tolerance, const char* what) {
    ASSERT_EQ(gpu.size(), cpu.size());
    for (uint32_t row = 0; row < kHeight; ++row) {
        for (uint32_t col = 0; col < kWidth; ++col) {
            const size_t i = (static_cast<size_t>(row) * kWidth + col) * 4u;
            for (int ch = 0; ch < 3; ++ch) { // B, G, R -- alpha is a constant 255
                EXPECT_NEAR(static_cast<int>(gpu[i + static_cast<size_t>(ch)]),
                            static_cast<int>(cpu[i + static_cast<size_t>(ch)]), tolerance)
                    << what << " at (" << col << "," << row << ") channel " << ch;
            }
            EXPECT_EQ(gpu[i + 3], 255u) << what << " alpha at (" << col << "," << row << ")";
        }
    }
}

// --- SDR: 4:2:0 8-bit -------------------------------------------------------

std::vector<uint8_t> Cpu420(const Planes8& p, MatrixCoefficients matrix, ColorRange range) {
    recorder_core::FullPlanarYuv420Frame src;
    src.y_plane = p.y.data();
    src.y_stride_bytes = p.y_stride;
    src.u_plane = p.u.data();
    src.u_stride_bytes = p.c_stride;
    src.v_plane = p.v.data();
    src.v_stride_bytes = p.c_stride;
    src.width = kWidth;
    src.height = kHeight;
    src.bits_per_sample = 8;
    recorder_core::YuvToBgraParams params{matrix, range};
    std::vector<uint8_t> out(static_cast<size_t>(kWidth) * kHeight * 4u);
    recorder_core::ConvertFullPlanarYuv420ToBgraScalar(src, params, out.data(), kWidth * 4u);
    return out;
}

RawDecodedVideoFrame Frame420P8(const Planes8& p, MatrixCoefficients matrix, ColorRange range) {
    RawDecodedVideoFrame f;
    f.width = kWidth;
    f.height = kHeight;
    f.format = DecodedPixelFormat::Yuv420P8;
    f.y_stride_bytes = p.y_stride;
    f.u_stride_bytes = p.c_stride;
    f.v_stride_bytes = p.c_stride;
    f.y_plane = p.y.data();
    f.u_plane = p.u.data();
    f.v_plane = p.v.data();
    f.matrix = matrix;
    f.range = range;
    return f;
}

TEST(EditFrameGpuConverterTest, InitSucceedsAgainstRealDevice) {
    D3DTestDevice warp = CreateWarpDevice();
    ASSERT_NE(warp.device, nullptr);
    ASSERT_NE(warp.context, nullptr);

    EditFrameGpuConverter converter;
    std::string err;
    EXPECT_TRUE(converter.Init(warp.device.get(), warp.context.get(), err)) << err;
}

TEST(EditFrameGpuConverterTest, Yuv420P8MatchesCpuReference) {
    D3DTestDevice warp = CreateWarpDevice();
    ASSERT_NE(warp.device, nullptr);

    const Planes8 p = MakePlanes8(kChromaW, kChromaH);
    const auto gpu = ConvertOnGpu(warp, Frame420P8(p, MatrixCoefficients::Bt709, ColorRange::Limited));
    ExpectBgraNear(gpu, Cpu420(p, MatrixCoefficients::Bt709, ColorRange::Limited), 2, "420p8 bt709 limited");
}

TEST(EditFrameGpuConverterTest, Yuv420P8FullRangeMatchesCpuReference) {
    D3DTestDevice warp = CreateWarpDevice();
    ASSERT_NE(warp.device, nullptr);

    const Planes8 p = MakePlanes8(kChromaW, kChromaH);
    const auto gpu = ConvertOnGpu(warp, Frame420P8(p, MatrixCoefficients::Bt709, ColorRange::Full));
    ExpectBgraNear(gpu, Cpu420(p, MatrixCoefficients::Bt709, ColorRange::Full), 2, "420p8 bt709 full");
}

// The constant buffer must follow the clip's tagged matrix, not a hard-coded
// BT.709 -- the exact bug yuv_to_bgra.h exists to have fixed on the CPU side.
// One converter instance runs both matrices in a row, so this also proves the
// per-frame constant cache invalidates when the tag changes.
TEST(EditFrameGpuConverterTest, MatrixTagSelectsCoefficients) {
    D3DTestDevice warp = CreateWarpDevice();
    ASSERT_NE(warp.device, nullptr);

    const Planes8 p = MakePlanes8(kChromaW, kChromaH);
    EditFrameGpuConverter converter;
    std::string err;
    ASSERT_TRUE(converter.Init(warp.device.get(), warp.context.get(), err)) << err;

    const auto gpu709 = ConvertOnGpu(warp, converter, Frame420P8(p, MatrixCoefficients::Bt709, ColorRange::Limited));
    const auto gpu601 = ConvertOnGpu(warp, converter, Frame420P8(p, MatrixCoefficients::Bt601, ColorRange::Limited));
    // ...and back again, to catch a cache that only ever updates once.
    const auto gpu709b = ConvertOnGpu(warp, converter, Frame420P8(p, MatrixCoefficients::Bt709, ColorRange::Limited));

    ExpectBgraNear(gpu709, Cpu420(p, MatrixCoefficients::Bt709, ColorRange::Limited), 2, "420p8 bt709");
    ExpectBgraNear(gpu601, Cpu420(p, MatrixCoefficients::Bt601, ColorRange::Limited), 2, "420p8 bt601");
    ExpectBgraNear(gpu709b, Cpu420(p, MatrixCoefficients::Bt709, ColorRange::Limited), 2, "420p8 bt709 again");
    EXPECT_NE(gpu709, gpu601) << "BT.601 and BT.709 must not render identically";
}

// --- SDR: 4:2:0 10-bit ------------------------------------------------------

std::vector<uint8_t> Cpu420_10(const Planes10& p, MatrixCoefficients matrix, ColorRange range) {
    recorder_core::FullPlanarYuv420Frame src;
    src.y_plane = reinterpret_cast<const uint8_t*>(p.y.data());
    src.y_stride_bytes = p.y_stride_bytes;
    src.u_plane = reinterpret_cast<const uint8_t*>(p.u.data());
    src.u_stride_bytes = p.c_stride_bytes;
    src.v_plane = reinterpret_cast<const uint8_t*>(p.v.data());
    src.v_stride_bytes = p.c_stride_bytes;
    src.width = kWidth;
    src.height = kHeight;
    src.bits_per_sample = 10;
    recorder_core::YuvToBgraParams params{matrix, range};
    std::vector<uint8_t> out(static_cast<size_t>(kWidth) * kHeight * 4u);
    recorder_core::ConvertFullPlanarYuv420ToBgraScalar(src, params, out.data(), kWidth * 4u);
    return out;
}

RawDecodedVideoFrame Frame420P10(const Planes10& p, MatrixCoefficients matrix, ColorRange range, bool is_pq) {
    RawDecodedVideoFrame f;
    f.width = kWidth;
    f.height = kHeight;
    f.format = DecodedPixelFormat::Yuv420P10;
    f.y_stride_bytes = p.y_stride_bytes;
    f.u_stride_bytes = p.c_stride_bytes;
    f.v_stride_bytes = p.c_stride_bytes;
    f.y_plane = reinterpret_cast<const uint8_t*>(p.y.data());
    f.u_plane = reinterpret_cast<const uint8_t*>(p.u.data());
    f.v_plane = reinterpret_cast<const uint8_t*>(p.v.data());
    f.is_pq_source = is_pq;
    f.matrix = matrix;
    f.range = range;
    return f;
}

TEST(EditFrameGpuConverterTest, Yuv420P10MatchesCpuReference) {
    D3DTestDevice warp = CreateWarpDevice();
    ASSERT_NE(warp.device, nullptr);

    const Planes10 p = MakePlanes10(/*pq=*/false);
    const auto gpu =
        ConvertOnGpu(warp, Frame420P10(p, MatrixCoefficients::Bt709, ColorRange::Limited, /*is_pq=*/false));
    ExpectBgraNear(gpu, Cpu420_10(p, MatrixCoefficients::Bt709, ColorRange::Limited), 2, "420p10 bt709 limited");
}

TEST(EditFrameGpuConverterTest, Yuv420P10Bt601MatchesCpuReference) {
    D3DTestDevice warp = CreateWarpDevice();
    ASSERT_NE(warp.device, nullptr);

    const Planes10 p = MakePlanes10(/*pq=*/false);
    const auto gpu =
        ConvertOnGpu(warp, Frame420P10(p, MatrixCoefficients::Bt601, ColorRange::Limited, /*is_pq=*/false));
    ExpectBgraNear(gpu, Cpu420_10(p, MatrixCoefficients::Bt601, ColorRange::Limited), 2, "420p10 bt601 limited");
}

// The only 10-bit branch neither test above reaches: ComputeCoefs' full-range
// 10-bit scales are 255/1023 for BOTH luma and chroma, where limited range uses
// 255/876 and 255/896. A shader that hard-coded either the 8-bit full-range
// scales (1.0) or the limited-range 10-bit ones passes every other SDR test in
// this file and fails only here.
TEST(EditFrameGpuConverterTest, Yuv420P10FullRangeMatchesCpuReference) {
    D3DTestDevice warp = CreateWarpDevice();
    ASSERT_NE(warp.device, nullptr);

    const Planes10 p = MakePlanes10(/*pq=*/false);
    const auto gpuFull = ConvertOnGpu(warp, Frame420P10(p, MatrixCoefficients::Bt709, ColorRange::Full,
                                                        /*is_pq=*/false));
    ExpectBgraNear(gpuFull, Cpu420_10(p, MatrixCoefficients::Bt709, ColorRange::Full), 2, "420p10 bt709 full");

    // ...and the two ranges must not collapse to the same output, or the
    // comparison above would hold for a shader that ignored range entirely.
    const auto gpuLimited = ConvertOnGpu(warp, Frame420P10(p, MatrixCoefficients::Bt709, ColorRange::Limited,
                                                           /*is_pq=*/false));
    EXPECT_NE(gpuFull, gpuLimited) << "full and limited range 10-bit must not render identically";
}

// The SDR path's remaining two MatrixCoefficients values. Bt2020Ncl is only
// otherwise exercised on the PQ tone-map path (where the matrix constants are
// not even used the same way), and Unspecified must resolve to BT.709 -- the
// documented fallback in BOTH yuv_to_bgra.cpp and edit_frame_gpu_converter.cpp,
// so a shader table that dropped the fallback would silently render garbage for
// any clip whose container omits the tag.
TEST(EditFrameGpuConverterTest, Bt2020AndUnspecifiedMatricesMatchCpuReferenceOnTheSdrPath) {
    D3DTestDevice warp = CreateWarpDevice();
    ASSERT_NE(warp.device, nullptr);

    const Planes8 p = MakePlanes8(kChromaW, kChromaH);

    const auto gpu2020 = ConvertOnGpu(warp, Frame420P8(p, MatrixCoefficients::Bt2020Ncl, ColorRange::Limited));
    ExpectBgraNear(gpu2020, Cpu420(p, MatrixCoefficients::Bt2020Ncl, ColorRange::Limited), 2,
                   "420p8 bt2020ncl limited");

    const auto gpuUnspec = ConvertOnGpu(warp, Frame420P8(p, MatrixCoefficients::Unspecified, ColorRange::Limited));
    ExpectBgraNear(gpuUnspec, Cpu420(p, MatrixCoefficients::Unspecified, ColorRange::Limited), 2,
                   "420p8 unspecified limited");

    // Unspecified IS BT.709 by both implementations' documented fallback, and
    // BT.2020 must be a genuinely different matrix from it.
    const auto gpu709 = ConvertOnGpu(warp, Frame420P8(p, MatrixCoefficients::Bt709, ColorRange::Limited));
    EXPECT_EQ(gpuUnspec, gpu709) << "Unspecified must fall back to BT.709";
    EXPECT_NE(gpu2020, gpu709) << "BT.2020-NCL and BT.709 must not render identically";
}

// --- SDR: 4:4:4 8-bit -------------------------------------------------------

std::vector<uint8_t> Cpu444(const Planes8& p, MatrixCoefficients matrix, ColorRange range) {
    recorder_core::FullPlanar444Frame src;
    src.y_plane = p.y.data();
    src.y_stride_bytes = p.y_stride;
    src.u_plane = p.u.data();
    src.u_stride_bytes = p.c_stride;
    src.v_plane = p.v.data();
    src.v_stride_bytes = p.c_stride;
    src.width = kWidth;
    src.height = kHeight;
    recorder_core::YuvToBgraParams params{matrix, range};
    std::vector<uint8_t> out(static_cast<size_t>(kWidth) * kHeight * 4u);
    recorder_core::ConvertFullPlanar444ToBgraScalar(src, params, out.data(), kWidth * 4u);
    return out;
}

RawDecodedVideoFrame Frame444P8(const Planes8& p, MatrixCoefficients matrix, ColorRange range) {
    RawDecodedVideoFrame f = Frame420P8(p, matrix, range);
    f.format = DecodedPixelFormat::Yuv444P8;
    return f;
}

TEST(EditFrameGpuConverterTest, Yuv444P8MatchesCpuReference) {
    D3DTestDevice warp = CreateWarpDevice();
    ASSERT_NE(warp.device, nullptr);

    // Full-resolution chroma planes -- the whole difference from the 4:2:0 case.
    const Planes8 p = MakePlanes8(kWidth, kHeight);
    const auto gpu = ConvertOnGpu(warp, Frame444P8(p, MatrixCoefficients::Bt709, ColorRange::Limited));
    ExpectBgraNear(gpu, Cpu444(p, MatrixCoefficients::Bt709, ColorRange::Limited), 2, "444p8 bt709 limited");
}

TEST(EditFrameGpuConverterTest, Yuv444P8Bt601MatchesCpuReference) {
    D3DTestDevice warp = CreateWarpDevice();
    ASSERT_NE(warp.device, nullptr);

    const Planes8 p = MakePlanes8(kWidth, kHeight);
    const auto gpu = ConvertOnGpu(warp, Frame444P8(p, MatrixCoefficients::Bt601, ColorRange::Limited));
    ExpectBgraNear(gpu, Cpu444(p, MatrixCoefficients::Bt601, ColorRange::Limited), 2, "444p8 bt601 limited");
}

// A converter instance must survive a format switch (the plane textures change
// size AND element format between 4:2:0 8-bit and 4:4:4 8-bit / 10-bit).
TEST(EditFrameGpuConverterTest, SurvivesFormatSwitchOnOneInstance) {
    D3DTestDevice warp = CreateWarpDevice();
    ASSERT_NE(warp.device, nullptr);

    EditFrameGpuConverter converter;
    std::string err;
    ASSERT_TRUE(converter.Init(warp.device.get(), warp.context.get(), err)) << err;

    const Planes8 p420 = MakePlanes8(kChromaW, kChromaH);
    const Planes8 p444 = MakePlanes8(kWidth, kHeight);
    const Planes10 p10 = MakePlanes10(/*pq=*/false);

    const auto gpu420 = ConvertOnGpu(warp, converter, Frame420P8(p420, MatrixCoefficients::Bt709, ColorRange::Limited));
    const auto gpu444 = ConvertOnGpu(warp, converter, Frame444P8(p444, MatrixCoefficients::Bt709, ColorRange::Limited));
    const auto gpu10 = ConvertOnGpu(warp, converter,
                                    Frame420P10(p10, MatrixCoefficients::Bt709, ColorRange::Limited, /*is_pq=*/false));
    const auto gpu420b =
        ConvertOnGpu(warp, converter, Frame420P8(p420, MatrixCoefficients::Bt709, ColorRange::Limited));

    ExpectBgraNear(gpu420, Cpu420(p420, MatrixCoefficients::Bt709, ColorRange::Limited), 2, "switch 420p8");
    ExpectBgraNear(gpu444, Cpu444(p444, MatrixCoefficients::Bt709, ColorRange::Limited), 2, "switch 444p8");
    ExpectBgraNear(gpu10, Cpu420_10(p10, MatrixCoefficients::Bt709, ColorRange::Limited), 2, "switch 420p10");
    ExpectBgraNear(gpu420b, Cpu420(p420, MatrixCoefficients::Bt709, ColorRange::Limited), 2, "switch back to 420p8");
}

// --- HDR10 PQ ---------------------------------------------------------------

// The exact (non-tabulated) monitoring chain, applied per pixel with the same
// 4:2:0 chroma addressing the shader uses.
std::vector<uint8_t> CpuPqExact(const Planes10& p, float peak_scale) {
    const uint32_t y_stride_samples = p.y_stride_bytes / 2u;
    const uint32_t c_stride_samples = p.c_stride_bytes / 2u;
    std::vector<uint8_t> out(static_cast<size_t>(kWidth) * kHeight * 4u);
    for (uint32_t row = 0; row < kHeight; ++row) {
        for (uint32_t col = 0; col < kWidth; ++col) {
            const uint16_t y10 = p.y[static_cast<size_t>(row) * y_stride_samples + col];
            const uint16_t cb10 = p.u[static_cast<size_t>(row / 2u) * c_stride_samples + col / 2u];
            const uint16_t cr10 = p.v[static_cast<size_t>(row / 2u) * c_stride_samples + col / 2u];
            const recorder_core::MonitorBgr bgr = recorder_core::P010PqPixelToMonitorBgr(y10, cb10, cr10, peak_scale);
            uint8_t* px = out.data() + (static_cast<size_t>(row) * kWidth + col) * 4u;
            px[0] = bgr.b;
            px[1] = bgr.g;
            px[2] = bgr.r;
            px[3] = 255u;
        }
    }
    return out;
}

TEST(EditFrameGpuConverterTest, PqSourceMatchesCpuReferenceAtDisplayPeak) {
    D3DTestDevice warp = CreateWarpDevice();
    ASSERT_NE(warp.device, nullptr);

    const Planes10 p = MakePlanes10(/*pq=*/true);
    // 12.5 == 1000 cd/m^2 / 80 cd/m^2 reference white.
    constexpr float kPeak1000 = 12.5f;
    const auto gpu = ConvertOnGpu(
        warp, Frame420P10(p, MatrixCoefficients::Bt2020Ncl, ColorRange::Limited, /*is_pq=*/true), kPeak1000);
    ExpectBgraNear(gpu, CpuPqExact(p, kPeak1000), 2, "pq peak 12.5");
}

TEST(EditFrameGpuConverterTest, PqSourceMatchesCpuReferenceAtReferenceWhitePeak) {
    D3DTestDevice warp = CreateWarpDevice();
    ASSERT_NE(warp.device, nullptr);

    const Planes10 p = MakePlanes10(/*pq=*/true);
    constexpr float kPeak80 = 1.0f;
    const auto gpu =
        ConvertOnGpu(warp, Frame420P10(p, MatrixCoefficients::Bt2020Ncl, ColorRange::Limited, /*is_pq=*/true), kPeak80);
    ExpectBgraNear(gpu, CpuPqExact(p, kPeak80), 2, "pq peak 1.0");
}

// peak_scale must actually reach the shader: at a 1000-nit peak the highlights
// roll off, at reference white they clip, so the two renders cannot be equal.
TEST(EditFrameGpuConverterTest, PqPeakScaleChangesOutput) {
    D3DTestDevice warp = CreateWarpDevice();
    ASSERT_NE(warp.device, nullptr);

    const Planes10 p = MakePlanes10(/*pq=*/true);
    EditFrameGpuConverter converter;
    std::string err;
    ASSERT_TRUE(converter.Init(warp.device.get(), warp.context.get(), err)) << err;

    const auto f = Frame420P10(p, MatrixCoefficients::Bt2020Ncl, ColorRange::Limited, /*is_pq=*/true);
    const auto gpu1 = ConvertOnGpu(warp, converter, f, 1.0f);
    const auto gpu12 = ConvertOnGpu(warp, converter, f, 12.5f);
    EXPECT_NE(gpu1, gpu12) << "the display peak must change the tone-map result";
}

// The same 10-bit planes rendered with and without is_pq_source must differ:
// the PQ path is a different chain, not a re-parameterised matrix.
TEST(EditFrameGpuConverterTest, PqFlagSelectsToneMapPath) {
    D3DTestDevice warp = CreateWarpDevice();
    ASSERT_NE(warp.device, nullptr);

    const Planes10 p = MakePlanes10(/*pq=*/true);
    EditFrameGpuConverter converter;
    std::string err;
    ASSERT_TRUE(converter.Init(warp.device.get(), warp.context.get(), err)) << err;

    const auto sdr = ConvertOnGpu(
        warp, converter, Frame420P10(p, MatrixCoefficients::Bt2020Ncl, ColorRange::Limited, /*is_pq=*/false), 12.5f);
    const auto hdr = ConvertOnGpu(
        warp, converter, Frame420P10(p, MatrixCoefficients::Bt2020Ncl, ColorRange::Limited, /*is_pq=*/true), 12.5f);
    EXPECT_NE(sdr, hdr) << "is_pq_source must select the tone-map shader";
}

// Cross-check against the SHIPPING CPU converter (the one the editor used before
// this render path existed), not just the exact reference above.
//
// P010PqMonitorConverter samples the same chain into 1024-entry tables, and
// hdr_preview.h only promises it matches the exact chain "within table
// quantisation". That error is large in places -- the tables are indexed by
// normalised PQ linear luminance, where the bottom few entries span the whole
// identity part of the tone-map curve -- so a fixed tolerance here would either
// be meaninglessly loose or fail on the table, not on the shader.
//
// The bound is therefore self-calibrating: the shader may differ from the
// shipping converter by as much as the exact chain does, plus the same +-2
// rounding allowance used everywhere else. That still catches every error worth
// catching (swapped channels, a wrong matrix, a missing transfer stage all move
// output by tens of codes) without asserting anything about the table.
TEST(EditFrameGpuConverterTest, PqSourceAgreesWithShippingCpuConverterWithinTableError) {
    D3DTestDevice warp = CreateWarpDevice();
    ASSERT_NE(warp.device, nullptr);

    const Planes10 p = MakePlanes10(/*pq=*/true);
    constexpr float kPeak1000 = 12.5f;
    const auto gpu = ConvertOnGpu(
        warp, Frame420P10(p, MatrixCoefficients::Bt2020Ncl, ColorRange::Limited, /*is_pq=*/true), kPeak1000);
    const auto exact = CpuPqExact(p, kPeak1000);

    recorder_core::FullPlanarYuv420Frame src;
    src.y_plane = reinterpret_cast<const uint8_t*>(p.y.data());
    src.y_stride_bytes = p.y_stride_bytes;
    src.u_plane = reinterpret_cast<const uint8_t*>(p.u.data());
    src.u_stride_bytes = p.c_stride_bytes;
    src.v_plane = reinterpret_cast<const uint8_t*>(p.v.data());
    src.v_stride_bytes = p.c_stride_bytes;
    src.width = kWidth;
    src.height = kHeight;
    src.bits_per_sample = 10;
    std::vector<uint8_t> lut(static_cast<size_t>(kWidth) * kHeight * 4u);
    recorder_core::P010PqMonitorConverter(kPeak1000).Convert(src, lut.data(), kWidth * 4u);

    ASSERT_EQ(gpu.size(), lut.size());
    for (uint32_t row = 0; row < kHeight; ++row) {
        for (uint32_t col = 0; col < kWidth; ++col) {
            const size_t i = (static_cast<size_t>(row) * kWidth + col) * 4u;
            for (int ch = 0; ch < 3; ++ch) {
                const size_t k = i + static_cast<size_t>(ch);
                const int table_error = std::abs(static_cast<int>(exact[k]) - static_cast<int>(lut[k]));
                const int gpu_error = std::abs(static_cast<int>(gpu[k]) - static_cast<int>(lut[k]));
                EXPECT_LE(gpu_error, table_error + 2)
                    << "pq vs shipping LUT converter at (" << col << "," << row << ") channel " << ch;
            }
        }
    }
}

// --- guards -----------------------------------------------------------------

TEST(EditFrameGpuConverterTest, ConvertRejectsInvalidArguments) {
    D3DTestDevice warp = CreateWarpDevice();
    ASSERT_NE(warp.device, nullptr);

    const Planes8 p = MakePlanes8(kChromaW, kChromaH);
    auto dst = CreateBgraTarget(warp.device.get(), kWidth, kHeight);

    EditFrameGpuConverter uninitialized;
    std::string err;
    EXPECT_FALSE(
        uninitialized.Convert(Frame420P8(p, MatrixCoefficients::Bt709, ColorRange::Limited), dst.get(), 1.0f, err));
    EXPECT_FALSE(err.empty());

    EditFrameGpuConverter converter;
    ASSERT_TRUE(converter.Init(warp.device.get(), warp.context.get(), err)) << err;
    EXPECT_FALSE(converter.Convert(Frame420P8(p, MatrixCoefficients::Bt709, ColorRange::Limited), nullptr, 1.0f, err));

    RawDecodedVideoFrame no_planes = Frame420P8(p, MatrixCoefficients::Bt709, ColorRange::Limited);
    no_planes.u_plane = nullptr;
    EXPECT_FALSE(converter.Convert(no_planes, dst.get(), 1.0f, err));
}

} // namespace
