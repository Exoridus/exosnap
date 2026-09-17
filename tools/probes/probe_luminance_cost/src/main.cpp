// probe_luminance_cost -- what the per-frame luminance analysis costs on the GPU.
//
// The tone-map knee and the HDR10 content-light metadata both want a per-frame
// luminance measurement, and the only open question before wiring one into the
// recording path is what it costs relative to the work already being done there.
// This probe answers that on the real adapter, for the surface sizes the product
// actually records at, by measuring GPU execution time of:
//
//   * the existing scRGB -> SDR tone-map pass, which is the reference cost
//   * the luminance analysis dispatch on its own
//   * both together, which is what an HDR recording would actually run
//
// Output is JSON so the numbers can be quoted without re-reading prose.

#include "frame_luminance.h"
#include "gpu_frame_luminance.h"

#include <exosnap/engine/gpu_hdr_tonemap.h>

#include <d3d11.h>
#include <dxgi1_6.h>
#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using exosnap::engine::FrameLuminanceAnalyzer;
using exosnap::engine::FrameLuminanceStats;
using exosnap::engine::HdrToneMapper;

namespace {

constexpr int kWarmupFrames = 60;
constexpr int kMeasuredFrames = 400;

struct Resolution {
    UINT width;
    UINT height;
    const char* name;
};

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

// A gradient rather than a flat fill: a constant surface would let the histogram
// atomics collide on one bin every time, which is the cheap case rather than the
// representative one.
winrt::com_ptr<ID3D11Texture2D> CreateSource(ID3D11Device* device, UINT width, UINT height) {
    std::vector<uint16_t> texels(static_cast<size_t>(width) * height * 4);
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            const float t = static_cast<float>(x) / static_cast<float>(width);
            const float u = static_cast<float>(y) / static_cast<float>(height);
            const float base = std::pow(2.0f, -8.0f + 14.0f * t);
            const size_t i = (static_cast<size_t>(y) * width + x) * 4;
            texels[i + 0] = FloatToHalf(base * (0.5f + u));
            texels[i + 1] = FloatToHalf(base);
            texels[i + 2] = FloatToHalf(base * (1.5f - u));
            texels[i + 3] = FloatToHalf(1.0f);
        }
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
    init.pSysMem = texels.data();
    init.SysMemPitch = width * 4 * 2;
    winrt::com_ptr<ID3D11Texture2D> tex;
    if (FAILED(device->CreateTexture2D(&desc, &init, tex.put()))) {
        return nullptr;
    }
    return tex;
}

winrt::com_ptr<ID3D11Texture2D> CreateSdrTarget(ID3D11Device* device, UINT width, UINT height) {
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
    if (FAILED(device->CreateTexture2D(&desc, nullptr, tex.put()))) {
        return nullptr;
    }
    return tex;
}

struct Summary {
    double mean_ms = 0.0;
    double median_ms = 0.0;
    double p95_ms = 0.0;
    size_t samples = 0;
};

Summary Summarise(std::vector<double> samples) {
    Summary s;
    s.samples = samples.size();
    if (samples.empty()) {
        return s;
    }
    std::sort(samples.begin(), samples.end());
    double sum = 0.0;
    for (const double v : samples) {
        sum += v;
    }
    s.mean_ms = sum / static_cast<double>(samples.size());
    s.median_ms = samples[samples.size() / 2];
    s.p95_ms = samples[static_cast<size_t>(static_cast<double>(samples.size()) * 0.95)];
    return s;
}

enum class Workload { ToneMapOnly, LuminanceOnly, Both };

// Each frame is bracketed by its own disjoint + timestamp pair and read back
// synchronously. A probe may block where the recording path may not, and the
// non-blocking ring the engine uses drops exactly the samples that make a small
// pass look free.
Summary Measure(ID3D11Device* device, ID3D11DeviceContext* context, HdrToneMapper& tone_mapper,
                FrameLuminanceAnalyzer& analyzer, ID3D11Texture2D* src, ID3D11Texture2D* dst, Workload workload) {
    D3D11_QUERY_DESC disjoint_desc{};
    disjoint_desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    D3D11_QUERY_DESC stamp_desc{};
    stamp_desc.Query = D3D11_QUERY_TIMESTAMP;
    winrt::com_ptr<ID3D11Query> disjoint;
    winrt::com_ptr<ID3D11Query> begin_ts;
    winrt::com_ptr<ID3D11Query> end_ts;
    if (FAILED(device->CreateQuery(&disjoint_desc, disjoint.put())) ||
        FAILED(device->CreateQuery(&stamp_desc, begin_ts.put())) ||
        FAILED(device->CreateQuery(&stamp_desc, end_ts.put()))) {
        return Summary{};
    }

    std::vector<double> samples;
    samples.reserve(kMeasuredFrames);
    std::string err;
    FrameLuminanceStats stats;

    for (int frame = 0; frame < kWarmupFrames + kMeasuredFrames; ++frame) {
        context->Begin(disjoint.get());
        context->End(begin_ts.get());
        if (workload != Workload::LuminanceOnly) {
            tone_mapper.Convert(src, dst, err);
        }
        if (workload != Workload::ToneMapOnly) {
            analyzer.Dispatch(src, err);
        }
        context->End(end_ts.get());
        context->End(disjoint.get());
        context->Flush();

        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        while (context->GetData(disjoint.get(), &dj, sizeof(dj), 0) != S_OK) {
        }
        uint64_t t0 = 0;
        uint64_t t1 = 0;
        while (context->GetData(begin_ts.get(), &t0, sizeof(t0), 0) != S_OK) {
        }
        while (context->GetData(end_ts.get(), &t1, sizeof(t1), 0) != S_OK) {
        }
        analyzer.TryTakeResult(&stats);
        if (frame >= kWarmupFrames && dj.Disjoint == FALSE && dj.Frequency != 0 && t1 > t0) {
            samples.push_back(static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(dj.Frequency));
        }
    }
    return Summarise(std::move(samples));
}

void PrintSummary(const char* key, const Summary& s, bool last) {
    std::printf("      \"%s\": { \"mean_ms\": %.4f, \"median_ms\": %.4f, \"p95_ms\": %.4f, \"samples\": %zu }%s\n", key,
                s.mean_ms, s.median_ms, s.p95_ms, s.samples, last ? "" : ",");
}

} // namespace

int main() {
    winrt::com_ptr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void()))) {
        std::fprintf(stderr, "CreateDXGIFactory1 failed\n");
        return 1;
    }
    winrt::com_ptr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(0, adapter.put()) != S_OK) {
        std::fprintf(stderr, "no adapter 0\n");
        return 1;
    }
    DXGI_ADAPTER_DESC1 adapter_desc{};
    adapter->GetDesc1(&adapter_desc);
    char adapter_name[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description, -1, adapter_name, sizeof(adapter_name) - 1, nullptr,
                        nullptr);

    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selected{};
    if (FAILED(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels,
                                 static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, device.put(), &selected,
                                 context.put()))) {
        std::fprintf(stderr, "D3D11CreateDevice failed\n");
        return 1;
    }

    const Resolution resolutions[] = {{1920, 1080, "1920x1080"}, {2560, 1440, "2560x1440"}, {3840, 2160, "3840x2160"}};

    std::printf("{\n");
    std::printf("  \"adapter\": \"%s\",\n", adapter_name);
    std::printf("  \"warmup_frames\": %d,\n", kWarmupFrames);
    std::printf("  \"measured_frames\": %d,\n", kMeasuredFrames);
    std::printf("  \"resolutions\": [\n");

    for (size_t r = 0; r < std::size(resolutions); ++r) {
        const Resolution& res = resolutions[r];
        winrt::com_ptr<ID3D11Texture2D> src = CreateSource(device.get(), res.width, res.height);
        winrt::com_ptr<ID3D11Texture2D> dst = CreateSdrTarget(device.get(), res.width, res.height);
        if (src == nullptr || dst == nullptr) {
            std::fprintf(stderr, "surface creation failed at %s\n", res.name);
            return 1;
        }
        std::string err;
        HdrToneMapper tone_mapper;
        if (!tone_mapper.Init(device.get(), context.get(), res.width, res.height, 12.5f, false, err, 2.5f, false)) {
            std::fprintf(stderr, "HdrToneMapper::Init failed: %s\n", err.c_str());
            return 1;
        }
        FrameLuminanceAnalyzer analyzer;
        if (!analyzer.Init(device.get(), context.get(), res.width, res.height, false, err)) {
            std::fprintf(stderr, "FrameLuminanceAnalyzer::Init failed: %s\n", err.c_str());
            return 1;
        }

        const Summary tone_map =
            Measure(device.get(), context.get(), tone_mapper, analyzer, src.get(), dst.get(), Workload::ToneMapOnly);
        const Summary luminance =
            Measure(device.get(), context.get(), tone_mapper, analyzer, src.get(), dst.get(), Workload::LuminanceOnly);
        const Summary both =
            Measure(device.get(), context.get(), tone_mapper, analyzer, src.get(), dst.get(), Workload::Both);

        std::printf("    {\n");
        std::printf("      \"resolution\": \"%s\",\n", res.name);
        PrintSummary("tone_map_only", tone_map, false);
        PrintSummary("luminance_only", luminance, false);
        PrintSummary("both", both, false);
        const double added = both.mean_ms - tone_map.mean_ms;
        std::printf("      \"luminance_added_ms\": %.4f,\n", added);
        std::printf("      \"luminance_added_percent_of_60fps_budget\": %.3f\n", added / (1000.0 / 60.0) * 100.0);
        std::printf("    }%s\n", r + 1 == std::size(resolutions) ? "" : ",");
    }

    std::printf("  ]\n}\n");
    return 0;
}
