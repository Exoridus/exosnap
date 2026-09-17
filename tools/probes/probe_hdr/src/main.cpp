// probe_hdr — HDR-readiness spike (see CMakeLists.txt).
//
// Part 1: per-display HDR status + mastering info via IDXGIOutput6::GetDesc1.
// Part 2: can the D3D11 VideoProcessor convert scRGB FP16 -> PQ/BT.2020 P010
//         directly (ID3D11VideoProcessorEnumerator1::CheckVideoProcessorFormatConversion)?

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <exosnap/engine/hdr_color_space.h>
#include <exosnap/engine/sdr_white_level.h>

#include <cstdio>
#include <cwchar>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

// Mirrors SdrPaperWhiteScale in the engine's private hdr_tonemap.h, which a probe
// cannot include, so the printed scale is the one the engine derives.
float SdrPaperWhiteScaleForProbe(float sdr_white_level_nits) {
    const float nits = exosnap::engine::EffectiveOverlayReferenceWhiteNits(sdr_white_level_nits);
    const float scale = nits / 80.0f;
    return scale > 1.0f ? scale : 1.0f;
}

// The OS "SDR content brightness" of a monitor, in nits. The same query the
// capture backend makes (DISPLAYCONFIG_SDR_WHITE_LEVEL, raw 1000 == 80 nits);
// 0 means the monitor could not be matched or the query failed.
float QuerySdrWhiteLevelNits(HMONITOR hmonitor) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hmonitor, &mi) == FALSE) {
        return 0.0f;
    }
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
        return 0.0f;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) !=
        ERROR_SUCCESS) {
        return 0.0f;
    }
    paths.resize(pathCount);
    for (const auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
            continue;
        }
        if (wcscmp(source.viewGdiDeviceName, mi.szDevice) != 0) {
            continue;
        }
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = path.targetInfo.adapterId;
        white.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) != ERROR_SUCCESS) {
            return 0.0f;
        }
        return exosnap::engine::SdrWhiteLevelRawToNits(white.SDRWhiteLevel);
    }
    return 0.0f;
}

const char* ColorSpaceName(DXGI_COLOR_SPACE_TYPE cs) {
    switch (cs) {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        return "RGB_FULL_G22_NONE_P709 (SDR sRGB/Rec.709, full)";
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        return "RGB_FULL_G10_NONE_P709 (scRGB linear, FP16 HDR)";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709:
        return "RGB_STUDIO_G22_NONE_P709 (SDR, limited)";
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        return "RGB_FULL_G2084_NONE_P2020 (HDR10 PQ/BT.2020, full) <-- HDR MODE ON";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
        return "RGB_STUDIO_G2084_NONE_P2020 (HDR10 PQ/BT.2020, studio) <-- HDR MODE ON";
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020:
        return "YCBCR_STUDIO_G2084_LEFT_P2020 (PQ/BT.2020 studio)";
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709:
        return "YCBCR_STUDIO_G22_LEFT_P709 (SDR Rec.709 studio)";
    case DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709:
        return "YCBCR_FULL_G22_LEFT_P709 (SDR Rec.709 full)";
    default:
        return "(other)";
    }
}

void DumpDisplays() {
    printf("==== Part 1: per-display HDR status (IDXGIOutput6::GetDesc1) ====\n");
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        printf("  CreateDXGIFactory1 failed\n");
        return;
    }
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            ComPtr<IDXGIOutput6> out6;
            if (FAILED(output.As(&out6))) {
                printf("  output %u: IDXGIOutput6 not available\n", o);
                output.Reset();
                continue;
            }
            DXGI_OUTPUT_DESC1 d{};
            if (SUCCEEDED(out6->GetDesc1(&d))) {
                printf("  display \"%ls\":\n", d.DeviceName);
                printf("    colorSpace      = %d  %s\n", static_cast<int>(d.ColorSpace), ColorSpaceName(d.ColorSpace));
                printf("    HDR mode        = %s\n",
                       exosnap::engine::IsHdrColorSpace(d.ColorSpace) ? "ON" : "off (SDR)");
                printf("    bitsPerColor    = %u\n", d.BitsPerColor);
                printf("    luminance       = min %.4f / max %.1f / maxFullFrame %.1f nits\n",
                       static_cast<double>(d.MinLuminance), static_cast<double>(d.MaxLuminance),
                       static_cast<double>(d.MaxFullFrameLuminance));
                const float sdr_white = QuerySdrWhiteLevelNits(d.Monitor);
                printf("    sdrWhiteLevel   = %.1f nits (paper_white_scale %.3f, effective %.1f nits)\n",
                       static_cast<double>(sdr_white), static_cast<double>(SdrPaperWhiteScaleForProbe(sdr_white)),
                       static_cast<double>(exosnap::engine::EffectiveOverlayReferenceWhiteNits(sdr_white)));
                printf("    primaries  R(%.3f,%.3f) G(%.3f,%.3f) B(%.3f,%.3f) W(%.3f,%.3f)\n",
                       static_cast<double>(d.RedPrimary[0]), static_cast<double>(d.RedPrimary[1]),
                       static_cast<double>(d.GreenPrimary[0]), static_cast<double>(d.GreenPrimary[1]),
                       static_cast<double>(d.BluePrimary[0]), static_cast<double>(d.BluePrimary[1]),
                       static_cast<double>(d.WhitePoint[0]), static_cast<double>(d.WhitePoint[1]));
            }
            output.Reset();
        }
        adapter.Reset();
    }
}

void CheckConversion(ID3D11VideoProcessorEnumerator1* en1, const char* label, DXGI_FORMAT inFmt,
                     DXGI_COLOR_SPACE_TYPE inCS, DXGI_FORMAT outFmt, DXGI_COLOR_SPACE_TYPE outCS) {
    BOOL supported = FALSE;
    const HRESULT hr = en1->CheckVideoProcessorFormatConversion(inFmt, inCS, outFmt, outCS, &supported);
    printf("  [%-3s %s] %-46s -> %s\n", label, SUCCEEDED(hr) ? (supported ? "SUPPORTED" : "  no     ") : " ERROR   ",
           ColorSpaceName(inCS), ColorSpaceName(outCS));
}

void CheckVideoProcessor() {
    printf("\n==== Part 2: VideoProcessor format/colour-space conversion support ====\n");
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    const D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fls, 2, D3D11_SDK_VERSION, &dev,
                                 nullptr, &ctx))) {
        printf("  D3D11CreateDevice failed\n");
        return;
    }
    ComPtr<ID3D11VideoDevice> vdev;
    if (FAILED(dev.As(&vdev))) {
        printf("  ID3D11VideoDevice not available\n");
        return;
    }
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = 3840;
    cd.InputHeight = 2160;
    cd.OutputWidth = 3840;
    cd.OutputHeight = 2160;
    cd.InputFrameRate = {60, 1};
    cd.OutputFrameRate = {60, 1};
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    ComPtr<ID3D11VideoProcessorEnumerator> en;
    if (FAILED(vdev->CreateVideoProcessorEnumerator(&cd, &en))) {
        printf("  CreateVideoProcessorEnumerator failed\n");
        return;
    }
    ComPtr<ID3D11VideoProcessorEnumerator1> en1;
    if (FAILED(en.As(&en1))) {
        printf("  ID3D11VideoProcessorEnumerator1 not available — cannot query conversion support\n");
        return;
    }

    // Baseline sanity: the SDR path ExoSnap uses today (must be supported).
    CheckConversion(en1.Get(), "sdr", DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
                    DXGI_FORMAT_NV12, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);

    // The HDR question: scRGB FP16 desktop -> PQ/BT.2020 P010.
    CheckConversion(en1.Get(), "HDR", DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,
                    DXGI_FORMAT_P010, DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020);
    // If the captured HDR surface is already PQ-encoded (some capture paths).
    CheckConversion(en1.Get(), "HDR", DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020,
                    DXGI_FORMAT_P010, DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020);
    // 10-bit RGB input variant.
    CheckConversion(en1.Get(), "HDR", DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020,
                    DXGI_FORMAT_P010, DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020);

    // 10 bpc SDR desktop (fix/od-10bit-desktop): DWM composes SDR at 10 bit /
    // FP16 scRGB. Can the VP convert those SDR sources to the SDR encode
    // formats directly (gamma + matrix + range)?
    CheckConversion(en1.Get(), "sdr", DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
                    DXGI_FORMAT_NV12, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    CheckConversion(en1.Get(), "sdr", DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
                    DXGI_FORMAT_P010, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    CheckConversion(en1.Get(), "sdr", DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,
                    DXGI_FORMAT_NV12, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    CheckConversion(en1.Get(), "sdr", DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,
                    DXGI_FORMAT_NV12, DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709);
    CheckConversion(en1.Get(), "sdr", DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,
                    DXGI_FORMAT_P010, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
}

} // namespace

int main() {
    DumpDisplays();
    CheckVideoProcessor();
    printf("\n[probe_hdr] done.\n");
    return 0;
}
