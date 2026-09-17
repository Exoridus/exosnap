// WARP-backed GPU tests for the per-frame luminance compute pass
// (gpu_frame_luminance.*). Every assertion compares the pass against a CPU
// reference computed over the exact same pixels: the extremes, the frame mean,
// and the full histogram. The pure evaluation of those numbers is pinned
// separately in test_frame_luminance.cpp.

#include <gtest/gtest.h>

#include "frame_luminance.h"
#include "gpu_frame_luminance.h"
#include "hdr_pq.h"

#include <d3d11.h>
#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

namespace {

using exosnap::engine::FrameLuminanceAnalyzer;
using exosnap::engine::FrameLuminanceHistogramBin;
using exosnap::engine::FrameLuminanceStats;
using exosnap::engine::kFrameLuminanceHistogramBins;
using exosnap::engine::kHdrReferenceWhiteNits;
using exosnap::engine::kLuminanceWeightB;
using exosnap::engine::kLuminanceWeightG;
using exosnap::engine::kLuminanceWeightR;

struct D3DTestDevice {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
};

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

// --- half <-> float (IEEE 754 binary16) ------------------------------------

float HalfToFloat(uint16_t h) {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            exp = 127 - 15 + 1;
            while ((mant & 0x400u) == 0) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3FFu;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

uint16_t FloatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, sizeof(x));
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t exp = static_cast<int32_t>((x >> 23) & 0xFFu) - 127 + 15;
    const uint32_t mant = x & 0x7FFFFFu;
    if (exp <= 0) {
        return static_cast<uint16_t>(sign);
    }
    if (exp >= 31) {
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13));
}

float RoundTrip(float v) {
    return HalfToFloat(FloatToHalf(v));
}

struct Rgb {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

winrt::com_ptr<ID3D11Texture2D> CreateFp16Surface(ID3D11Device* device, UINT width, UINT height,
                                                  const std::vector<Rgb>& pixels) {
    std::vector<uint16_t> halfs(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < pixels.size(); ++i) {
        halfs[i * 4 + 0] = FloatToHalf(pixels[i].r);
        halfs[i * 4 + 1] = FloatToHalf(pixels[i].g);
        halfs[i * 4 + 2] = FloatToHalf(pixels[i].b);
        halfs[i * 4 + 3] = FloatToHalf(1.0f);
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = halfs.data();
    init.SysMemPitch = width * 4 * 2;
    winrt::com_ptr<ID3D11Texture2D> tex;
    EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, &init, tex.put())));
    return tex;
}

winrt::com_ptr<ID3D11Texture2D> CreateR10G10B10A2Surface(ID3D11Device* device, UINT width, UINT height,
                                                         const std::vector<uint32_t>& packed) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = packed.data();
    init.SysMemPitch = width * 4;
    winrt::com_ptr<ID3D11Texture2D> tex;
    EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, &init, tex.put())));
    return tex;
}

// The pass never blocks, so the test has to submit the work itself and then poll
// for the result. A bounded number of attempts keeps a broken fence from hanging
// the suite.
bool DispatchAndTake(FrameLuminanceAnalyzer& analyzer, ID3D11DeviceContext* context, ID3D11Texture2D* src,
                     FrameLuminanceStats* out) {
    std::string err;
    if (!analyzer.Dispatch(src, err)) {
        ADD_FAILURE() << "Dispatch failed: " << err;
        return false;
    }
    context->Flush();
    for (int attempt = 0; attempt < 2000; ++attempt) {
        if (analyzer.TryTakeResult(out)) {
            return true;
        }
    }
    ADD_FAILURE() << "no measurement landed within the poll budget";
    return false;
}

// --- CPU reference ----------------------------------------------------------

struct CpuReference {
    float min_nits = 0.0f;
    float max_nits = 0.0f;
    double mean_nits = 0.0;
    std::array<uint32_t, kFrameLuminanceHistogramBins> histogram{};
};

float LuminanceNits(const Rgb& c) {
    const float r = c.r > 0.0f ? c.r : 0.0f;
    const float g = c.g > 0.0f ? c.g : 0.0f;
    const float b = c.b > 0.0f ? c.b : 0.0f;
    return (r * kLuminanceWeightR + g * kLuminanceWeightG + b * kLuminanceWeightB) * kHdrReferenceWhiteNits;
}

CpuReference Reference(const std::vector<Rgb>& pixels) {
    CpuReference ref;
    double sum = 0.0;
    for (size_t i = 0; i < pixels.size(); ++i) {
        const float lum = LuminanceNits(pixels[i]);
        if (i == 0 || lum < ref.min_nits) {
            ref.min_nits = lum;
        }
        if (lum > ref.max_nits) {
            ref.max_nits = lum;
        }
        sum += lum;
        ++ref.histogram[static_cast<size_t>(FrameLuminanceHistogramBin(lum))];
    }
    ref.mean_nits = pixels.empty() ? 0.0 : sum / static_cast<double>(pixels.size());
    return ref;
}

// A luminance whose log2 sits within this fraction of a bin edge can be binned
// differently by the shader than by the CPU, because the two evaluate log2 with
// different precision. Steering the test pixels away from an edge is not
// weakening the test: what is under test is the reduction, not the tie-break at a
// boundary no picture depends on.
bool ClearOfABinEdge(float nits) {
    if (!(nits > 0.0f)) {
        return true; // black is binned by an exact comparison, not by log2
    }
    const float t = (std::log2(nits) - exosnap::engine::kFrameLuminanceHistogramMinLog2Nits) /
                    exosnap::engine::kFrameLuminanceHistogramLog2Step;
    const float frac = t - std::floor(t);
    return frac > 0.05f && frac < 0.95f;
}

// A spread of scRGB colours wide enough to populate most of the histogram, with
// each pixel round-tripped through FP16 so the CPU reference sees exactly the
// values the shader samples.
std::vector<Rgb> MakeSpreadPixels(UINT width, UINT height) {
    const size_t count = static_cast<size_t>(width) * height;
    std::vector<Rgb> pixels;
    pixels.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        // Geometric sweep from deep shadow to well past reference white, with a
        // per-pixel channel tilt so the luminance weights are exercised rather
        // than a grey ramp any weighting would reproduce.
        const double t = static_cast<double>(i) / static_cast<double>(count - 1);
        const float tilt = 0.25f + 0.75f * static_cast<float>(i % 7) / 6.0f;
        float base = static_cast<float>(std::pow(2.0, -8.0 + 16.0 * t));
        Rgb c;
        for (int attempt = 0; attempt < 64; ++attempt) {
            c.r = RoundTrip(base * tilt);
            c.g = RoundTrip(base);
            c.b = RoundTrip(base * (2.0f - tilt));
            if (ClearOfABinEdge(LuminanceNits(c))) {
                break;
            }
            base *= 1.013f;
        }
        pixels.push_back(c);
    }
    return pixels;
}

// --- tests ------------------------------------------------------------------

TEST(GpuFrameLuminanceTest, NoResultBeforeAnyDispatch) {
    D3DTestDevice dev = CreateWarpDevice();
    ASSERT_NE(dev.device, nullptr);
    FrameLuminanceAnalyzer analyzer;
    std::string err;
    ASSERT_TRUE(analyzer.Init(dev.device.get(), dev.context.get(), 32, 32, false, err)) << err;
    FrameLuminanceStats stats;
    EXPECT_FALSE(analyzer.TryTakeResult(&stats));
}

// The whole point of the pass: the same pixels, reduced on the GPU, agree with a
// straight CPU pass over them. The surface size is deliberately not a multiple of
// the threadgroup edge, so the bounds guard is under test at the same time.
TEST(GpuFrameLuminanceTest, MatchesTheCpuReferenceOverAWideSpread) {
    constexpr UINT kWidth = 37;
    constexpr UINT kHeight = 23;
    D3DTestDevice dev = CreateWarpDevice();
    ASSERT_NE(dev.device, nullptr);

    const std::vector<Rgb> pixels = MakeSpreadPixels(kWidth, kHeight);
    const CpuReference ref = Reference(pixels);
    winrt::com_ptr<ID3D11Texture2D> src = CreateFp16Surface(dev.device.get(), kWidth, kHeight, pixels);
    ASSERT_NE(src, nullptr);

    FrameLuminanceAnalyzer analyzer;
    std::string err;
    ASSERT_TRUE(analyzer.Init(dev.device.get(), dev.context.get(), kWidth, kHeight, false, err)) << err;

    FrameLuminanceStats stats;
    ASSERT_TRUE(DispatchAndTake(analyzer, dev.context.get(), src.get(), &stats));

    EXPECT_NEAR(stats.max_nits, ref.max_nits, ref.max_nits * 1e-5f);
    EXPECT_NEAR(stats.min_nits, ref.min_nits, 1e-4f);
    EXPECT_NEAR(stats.mean_nits, static_cast<float>(ref.mean_nits),
                (std::max)(0.1f, static_cast<float>(ref.mean_nits) * 1e-4f));

    uint64_t total = 0;
    for (int bin = 0; bin < kFrameLuminanceHistogramBins; ++bin) {
        EXPECT_EQ(stats.histogram[static_cast<size_t>(bin)], ref.histogram[static_cast<size_t>(bin)]) << "bin=" << bin;
        total += stats.histogram[static_cast<size_t>(bin)];
    }
    // No thread outside the analysed region may have contributed: the histogram
    // counts exactly the pixels the surface has.
    EXPECT_EQ(total, static_cast<uint64_t>(kWidth) * kHeight);
}

// A downscale before the extremes would average the brightest pixel away, which
// is exactly what MaxCLL must not do. One bright pixel in an otherwise dark frame
// has to survive at full value.
TEST(GpuFrameLuminanceTest, ASingleBrightPixelSurvivesIntoTheMaximum) {
    constexpr UINT kWidth = 64;
    constexpr UINT kHeight = 64;
    D3DTestDevice dev = CreateWarpDevice();
    ASSERT_NE(dev.device, nullptr);

    std::vector<Rgb> pixels(static_cast<size_t>(kWidth) * kHeight, Rgb{0.05f, 0.05f, 0.05f});
    const float bright = RoundTrip(48.0f); // 48 * 80 == 3840 cd/m^2
    pixels[1234] = Rgb{bright, bright, bright};

    winrt::com_ptr<ID3D11Texture2D> src = CreateFp16Surface(dev.device.get(), kWidth, kHeight, pixels);
    FrameLuminanceAnalyzer analyzer;
    std::string err;
    ASSERT_TRUE(analyzer.Init(dev.device.get(), dev.context.get(), kWidth, kHeight, false, err)) << err;

    FrameLuminanceStats stats;
    ASSERT_TRUE(DispatchAndTake(analyzer, dev.context.get(), src.get(), &stats));

    const float expected_max = LuminanceNits(pixels[1234]);
    EXPECT_NEAR(stats.max_nits, expected_max, expected_max * 1e-5f);
    // and the frame average is nowhere near it, which is the difference MaxCLL
    // and MaxFALL exist to express.
    EXPECT_LT(stats.mean_nits, 20.0f);
}

// Wide-gamut scRGB carries negative channel values. They are clamped, not
// reinterpreted: a negative must not become an enormous positive through the bit
// mapping, and must not drag the minimum below zero.
TEST(GpuFrameLuminanceTest, WideGamutNegativesClampInsteadOfCorruptingTheReduction) {
    constexpr UINT kWidth = 32;
    constexpr UINT kHeight = 32;
    D3DTestDevice dev = CreateWarpDevice();
    ASSERT_NE(dev.device, nullptr);

    std::vector<Rgb> pixels(static_cast<size_t>(kWidth) * kHeight, Rgb{0.5f, 0.5f, 0.5f});
    pixels[0] = Rgb{-4.0f, -4.0f, -4.0f};
    pixels[1] = Rgb{-2.0f, 0.5f, -1.0f};

    winrt::com_ptr<ID3D11Texture2D> src = CreateFp16Surface(dev.device.get(), kWidth, kHeight, pixels);
    FrameLuminanceAnalyzer analyzer;
    std::string err;
    ASSERT_TRUE(analyzer.Init(dev.device.get(), dev.context.get(), kWidth, kHeight, false, err)) << err;

    FrameLuminanceStats stats;
    ASSERT_TRUE(DispatchAndTake(analyzer, dev.context.get(), src.get(), &stats));

    EXPECT_FLOAT_EQ(stats.min_nits, 0.0f);
    EXPECT_NEAR(stats.max_nits, 40.0f, 0.01f); // 0.5 * 80
    EXPECT_GT(stats.histogram[0], 0u);
}

// The tone-map path also meets a PQ-encoded BT.2020 surface when duplication
// offers no FP16 one. The pass has to decode it, or every measurement on that
// sub-path describes the code values instead of the light.
TEST(GpuFrameLuminanceTest, DecodesAPqSourceIntoRealLuminance) {
    constexpr UINT kWidth = 16;
    constexpr UINT kHeight = 16;
    constexpr float kTargetNits = 1000.0f;
    D3DTestDevice dev = CreateWarpDevice();
    ASSERT_NE(dev.device, nullptr);

    // A neutral BT.2020 grey at the target luminance: equal channels, so the
    // BT.2020 -> BT.709 matrix leaves it neutral and the luminance is the level
    // itself.
    const float code = exosnap::engine::PqOetf(kTargetNits / exosnap::engine::kPqPeakNits);
    const uint32_t q = static_cast<uint32_t>(std::lround(static_cast<double>(code) * 1023.0));
    const uint32_t packed = q | (q << 10) | (q << 20) | (3u << 30);
    const std::vector<uint32_t> texels(static_cast<size_t>(kWidth) * kHeight, packed);

    winrt::com_ptr<ID3D11Texture2D> src = CreateR10G10B10A2Surface(dev.device.get(), kWidth, kHeight, texels);
    FrameLuminanceAnalyzer analyzer;
    std::string err;
    ASSERT_TRUE(analyzer.Init(dev.device.get(), dev.context.get(), kWidth, kHeight, true, err)) << err;

    FrameLuminanceStats stats;
    ASSERT_TRUE(DispatchAndTake(analyzer, dev.context.get(), src.get(), &stats));

    // 10-bit PQ quantisation alone is worth about half a percent at this level.
    EXPECT_NEAR(stats.max_nits, kTargetNits, kTargetNits * 0.02f);
    EXPECT_NEAR(stats.mean_nits, kTargetNits, kTargetNits * 0.02f);
}

// Every dispatch starts from a cleared accumulator: a dark frame after a bright
// one must not inherit the bright one's maximum.
TEST(GpuFrameLuminanceTest, EachDispatchMeasuresOnlyItsOwnFrame) {
    constexpr UINT kWidth = 32;
    constexpr UINT kHeight = 32;
    D3DTestDevice dev = CreateWarpDevice();
    ASSERT_NE(dev.device, nullptr);

    const std::vector<Rgb> bright(static_cast<size_t>(kWidth) * kHeight, Rgb{20.0f, 20.0f, 20.0f});
    const std::vector<Rgb> dark(static_cast<size_t>(kWidth) * kHeight, Rgb{0.25f, 0.25f, 0.25f});
    winrt::com_ptr<ID3D11Texture2D> bright_tex = CreateFp16Surface(dev.device.get(), kWidth, kHeight, bright);
    winrt::com_ptr<ID3D11Texture2D> dark_tex = CreateFp16Surface(dev.device.get(), kWidth, kHeight, dark);

    FrameLuminanceAnalyzer analyzer;
    std::string err;
    ASSERT_TRUE(analyzer.Init(dev.device.get(), dev.context.get(), kWidth, kHeight, false, err)) << err;

    FrameLuminanceStats first;
    ASSERT_TRUE(DispatchAndTake(analyzer, dev.context.get(), bright_tex.get(), &first));
    EXPECT_NEAR(first.max_nits, 1600.0f, 1.0f);

    FrameLuminanceStats second;
    ASSERT_TRUE(DispatchAndTake(analyzer, dev.context.get(), dark_tex.get(), &second));
    EXPECT_NEAR(second.max_nits, 20.0f, 0.1f);
    EXPECT_NEAR(second.mean_nits, 20.0f, 0.1f);
}

// A consumer that stops collecting must not make the pass block or grow: the
// ring saturates, later dispatches are skipped, and the results that are there
// are still readable.
TEST(GpuFrameLuminanceTest, AFullReadbackRingSkipsRatherThanBlocks) {
    constexpr UINT kWidth = 32;
    constexpr UINT kHeight = 32;
    D3DTestDevice dev = CreateWarpDevice();
    ASSERT_NE(dev.device, nullptr);

    const std::vector<Rgb> pixels(static_cast<size_t>(kWidth) * kHeight, Rgb{1.0f, 1.0f, 1.0f});
    winrt::com_ptr<ID3D11Texture2D> src = CreateFp16Surface(dev.device.get(), kWidth, kHeight, pixels);

    FrameLuminanceAnalyzer analyzer;
    std::string err;
    ASSERT_TRUE(analyzer.Init(dev.device.get(), dev.context.get(), kWidth, kHeight, false, err)) << err;

    for (int i = 0; i < 64; ++i) {
        ASSERT_TRUE(analyzer.Dispatch(src.get(), err)) << err;
    }
    dev.context->Flush();

    int taken = 0;
    FrameLuminanceStats stats;
    for (int attempt = 0; attempt < 5000 && taken < 8; ++attempt) {
        if (analyzer.TryTakeResult(&stats)) {
            ++taken;
            EXPECT_NEAR(stats.max_nits, 80.0f, 0.1f);
        }
    }
    EXPECT_GT(taken, 0);
    EXPECT_LE(taken, 3) << "the ring holds three measurements; the rest were skipped, not queued";
}

TEST(GpuFrameLuminanceTest, ResetDropsEverythingInFlight) {
    constexpr UINT kWidth = 32;
    constexpr UINT kHeight = 32;
    D3DTestDevice dev = CreateWarpDevice();
    ASSERT_NE(dev.device, nullptr);

    const std::vector<Rgb> pixels(static_cast<size_t>(kWidth) * kHeight, Rgb{1.0f, 1.0f, 1.0f});
    winrt::com_ptr<ID3D11Texture2D> src = CreateFp16Surface(dev.device.get(), kWidth, kHeight, pixels);

    FrameLuminanceAnalyzer analyzer;
    std::string err;
    ASSERT_TRUE(analyzer.Init(dev.device.get(), dev.context.get(), kWidth, kHeight, false, err)) << err;
    ASSERT_TRUE(analyzer.Dispatch(src.get(), err)) << err;
    dev.context->Flush();

    analyzer.Reset();
    EXPECT_FALSE(analyzer.Initialised());
    FrameLuminanceStats stats;
    EXPECT_FALSE(analyzer.TryTakeResult(&stats));
    EXPECT_FALSE(analyzer.Dispatch(src.get(), err));
}

} // namespace
