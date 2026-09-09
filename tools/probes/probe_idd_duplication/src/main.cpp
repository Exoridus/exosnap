// probe_idd_duplication -- can the desktop be duplicated on this machine's outputs?
//
// A GPU-partitioned guest has no display of its own: the partition carries no
// scanout, so the only monitor is whatever indirect display driver was installed, and
// the whole capture side of the product reads its frames through DXGI Output
// Duplication on that virtual monitor. Two things can go wrong that look identical
// from a failing gate. Duplication can be unsupported on the output entirely, and it
// can be supported but never produce a frame, because nothing composes to a desktop
// nobody is looking at.
//
// So this asks per output, on every adapter: which mode is it in, which colour space
// does it report, does duplication start, and do 30 frames actually arrive. Timeouts
// are counted rather than treated as errors -- an idle desktop legitimately produces
// none, and "30 frames, 0 timeouts" and "0 frames, 30 timeouts" are different answers
// to different questions.
//
// Runs on the host as well, where its answer is the reference for the guest's.

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_6.h>

#include "exosnap/engine/hdr_color_space.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int kFramesPerOutput = 30;
constexpr UINT kAcquireTimeoutMs = 1000;

template <typename T> void SafeRelease(T*& pointer) {
    if (pointer != nullptr) {
        pointer->Release();
        pointer = nullptr;
    }
}

std::string JsonEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04X", static_cast<unsigned>(static_cast<unsigned char>(c)));
            out += buf;
        } else {
            out += c;
        }
    }
    return out;
}

std::string ToUtf8(const wchar_t* text) {
    if (text == nullptr) {
        return {};
    }
    int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), length, nullptr, nullptr);
    return out;
}

const char* ColorSpaceName(DXGI_COLOR_SPACE_TYPE space) {
    switch (space) {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        return "RGB_FULL_G22_NONE_P709";
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        return "RGB_FULL_G10_NONE_P709";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709:
        return "RGB_STUDIO_G22_NONE_P709";
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        return "RGB_FULL_G2084_NONE_P2020";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
        return "RGB_STUDIO_G2084_NONE_P2020";
    default:
        return "other";
    }
}

const char* FormatName(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return "R16G16B16A16_FLOAT";
    default:
        return "other";
    }
}

struct OutputReport {
    std::string json;
    bool duplicated = false;
    int frames = 0;
};

// The current mode, from the display device rather than from DXGI. DXGI knows the
// desktop rectangle but not the refresh rate the output is running at, and the refresh
// rate is half of what a fixed-mode virtual monitor is configured for.
void AppendCurrentMode(const wchar_t* deviceName, std::string& json) {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(deviceName, ENUM_CURRENT_SETTINGS, &mode) == 0) {
        json += "\"mode\":null";
        return;
    }
    char buffer[160];
    snprintf(buffer, sizeof(buffer), "\"mode\":{\"width\":%lu,\"height\":%lu,\"refreshHz\":%lu,\"bitsPerPixel\":%lu}",
             static_cast<unsigned long>(mode.dmPelsWidth), static_cast<unsigned long>(mode.dmPelsHeight),
             static_cast<unsigned long>(mode.dmDisplayFrequency), static_cast<unsigned long>(mode.dmBitsPerPel));
    json += buffer;
}

OutputReport ProbeOutput(ID3D11Device* device, IDXGIOutput* output, UINT adapterIndex, UINT outputIndex) {
    OutputReport report;

    DXGI_OUTPUT_DESC desc{};
    output->GetDesc(&desc);

    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    UINT bitsPerColor = 8;
    bool haveDesc1 = false;
    IDXGIOutput6* output6 = nullptr;
    if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&output6)))) {
        DXGI_OUTPUT_DESC1 desc1{};
        if (SUCCEEDED(output6->GetDesc1(&desc1))) {
            colorSpace = desc1.ColorSpace;
            bitsPerColor = desc1.BitsPerColor;
            haveDesc1 = true;
        }
    }

    char header[512];
    snprintf(header, sizeof(header),
             "{\"adapter\":%u,\"output\":%u,\"name\":\"%s\",\"attachedToDesktop\":%s,"
             "\"desktop\":{\"left\":%ld,\"top\":%ld,\"right\":%ld,\"bottom\":%ld},",
             adapterIndex, outputIndex, JsonEscape(ToUtf8(desc.DeviceName)).c_str(),
             desc.AttachedToDesktop != 0 ? "true" : "false",
             static_cast<long>(desc.DesktopCoordinates.left), static_cast<long>(desc.DesktopCoordinates.top),
             static_cast<long>(desc.DesktopCoordinates.right), static_cast<long>(desc.DesktopCoordinates.bottom));
    report.json = header;

    AppendCurrentMode(desc.DeviceName, report.json);

    char colour[256];
    snprintf(colour, sizeof(colour), ",\"colorSpace\":\"%s\",\"bitsPerColor\":%u,\"hdr\":%s,\"colorSpaceKnown\":%s,",
             ColorSpaceName(colorSpace), bitsPerColor,
             exosnap::engine::IsHdrColorSpace(colorSpace) ? "true" : "false", haveDesc1 ? "true" : "false");
    report.json += colour;

    // Both formats offered, and the order matters: an HDR desktop duplicates as
    // scRGB FP16, and a probe that asked only for BGRA would report an output that
    // cannot be duplicated when the truth is that it cannot be duplicated AS BGRA.
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                   DXGI_FORMAT_R10G10B10A2_UNORM};

    IDXGIOutputDuplication* duplication = nullptr;
    HRESULT hr = E_FAIL;
    IDXGIOutput5* output5 = nullptr;
    if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput5), reinterpret_cast<void**>(&output5)))) {
        hr = output5->DuplicateOutput1(device, 0, ARRAYSIZE(formats), formats, &duplication);
        SafeRelease(output5);
    }
    if (FAILED(hr)) {
        IDXGIOutput1* output1 = nullptr;
        if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1)))) {
            hr = output1->DuplicateOutput(device, &duplication);
            SafeRelease(output1);
        }
    }
    SafeRelease(output6);

    if (FAILED(hr) || duplication == nullptr) {
        char failure[160];
        snprintf(failure, sizeof(failure), "\"duplication\":{\"ok\":false,\"hresult\":\"0x%08lX\"}}",
                 static_cast<unsigned long>(hr));
        report.json += failure;
        return report;
    }
    report.duplicated = true;

    DXGI_OUTDUPL_DESC duplDesc{};
    duplication->GetDesc(&duplDesc);

    int frames = 0;
    int timeouts = 0;
    int accumulatedFrames = 0;
    HRESULT lastError = S_OK;
    for (int attempt = 0; attempt < kFramesPerOutput * 2 && frames < kFramesPerOutput; ++attempt) {
        DXGI_OUTDUPL_FRAME_INFO info{};
        IDXGIResource* resource = nullptr;
        HRESULT acquired = duplication->AcquireNextFrame(kAcquireTimeoutMs, &info, &resource);
        if (acquired == DXGI_ERROR_WAIT_TIMEOUT) {
            timeouts++;
            continue;
        }
        if (FAILED(acquired)) {
            lastError = acquired;
            break;
        }
        // LastPresentTime zero means the desktop did not change and only the mouse
        // moved. Counting those as frames would report a duplicating output on a
        // machine where nothing renders at all.
        if (info.LastPresentTime.QuadPart != 0) {
            frames++;
        }
        accumulatedFrames = static_cast<int>(info.AccumulatedFrames);
        SafeRelease(resource);
        duplication->ReleaseFrame();
    }

    char result[320];
    snprintf(result, sizeof(result),
             "\"duplication\":{\"ok\":true,\"format\":\"%s\",\"desktopWidth\":%u,\"desktopHeight\":%u,"
             "\"frames\":%d,\"timeouts\":%d,\"accumulatedFrames\":%d,\"lastHresult\":\"0x%08lX\"}}",
             FormatName(duplDesc.ModeDesc.Format), duplDesc.ModeDesc.Width, duplDesc.ModeDesc.Height, frames, timeouts,
             accumulatedFrames, static_cast<unsigned long>(lastError));
    report.json += result;
    report.frames = frames;

    SafeRelease(duplication);
    return report;
}

} // namespace

int main() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) ||
        factory == nullptr) {
        printf("{\"ok\":false,\"error\":\"CreateDXGIFactory1 failed\"}\n");
        return 1;
    }

    std::vector<std::string> reports;
    int duplicated = 0;
    int producing = 0;

    for (UINT adapterIndex = 0;; ++adapterIndex) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 adapterDesc{};
        adapter->GetDesc1(&adapterDesc);
        if ((adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            SafeRelease(adapter);
            continue;
        }

        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                                       &device, nullptr, &context);
        if (FAILED(hr) || device == nullptr) {
            SafeRelease(adapter);
            continue;
        }

        for (UINT outputIndex = 0;; ++outputIndex) {
            IDXGIOutput* output = nullptr;
            if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            OutputReport report = ProbeOutput(device, output, adapterIndex, outputIndex);
            if (report.duplicated) {
                duplicated++;
            }
            if (report.frames > 0) {
                producing++;
            }
            reports.push_back(report.json);
            SafeRelease(output);
        }

        SafeRelease(context);
        SafeRelease(device);
        SafeRelease(adapter);
    }

    SafeRelease(factory);

    std::string joined;
    for (size_t i = 0; i < reports.size(); ++i) {
        if (i != 0) {
            joined += ",";
        }
        joined += reports[i];
    }

    // ok is about the probe, not about the desktop: an output that produced no frames
    // is a reported result, and the caller decides whether that is acceptable for the
    // gate it is about to run.
    printf("{\"ok\":true,\"outputs\":%zu,\"duplicating\":%d,\"producingFrames\":%d,\"framesRequested\":%d,"
           "\"detail\":[%s]}\n",
           reports.size(), duplicated, producing, kFramesPerOutput, joined.c_str());
    fflush(stdout);
    return reports.empty() ? 1 : 0;
}
