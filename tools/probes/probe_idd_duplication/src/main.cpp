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
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>

namespace {

constexpr int kDefaultFramesPerOutput = 30;
constexpr UINT kDefaultAcquireTimeoutMs = 250;
constexpr ULONGLONG kDefaultOutputProbeBudgetMs = 5000;

struct ProbeOptions {
    int framesPerOutput = kDefaultFramesPerOutput;
    UINT acquireTimeoutMs = kDefaultAcquireTimeoutMs;
    ULONGLONG budgetMs = kDefaultOutputProbeBudgetMs;
    bool nonBlockingAcquire = false;
    bool resetResourceBeforeRelease = false;
    ULONGLONG holdAfterAcquireMs = 0;
};

struct TimerStats {
    std::vector<double> valuesUs;

    void AddSample(double valueUs) {
        valuesUs.push_back(valueUs);
    }

    bool Empty() const {
        return valuesUs.empty();
    }

    size_t Count() const {
        return valuesUs.size();
    }

    double Min() const {
        if (valuesUs.empty()) {
            return 0.0;
        }
        return *std::min_element(valuesUs.begin(), valuesUs.end());
    }

    double Max() const {
        if (valuesUs.empty()) {
            return 0.0;
        }
        return *std::max_element(valuesUs.begin(), valuesUs.end());
    }

    double Mean() const {
        if (valuesUs.empty()) {
            return 0.0;
        }
        double sum = 0.0;
        for (double value : valuesUs) {
            sum += value;
        }
        return sum / static_cast<double>(valuesUs.size());
    }

    double Percentile(double percentile) const {
        if (valuesUs.empty()) {
            return 0.0;
        }
        std::vector<double> sorted(valuesUs);
        std::sort(sorted.begin(), sorted.end());
        const double index = percentile * (sorted.size() - 1);
        const size_t idx = static_cast<size_t>(index + 0.5);
        return sorted[std::min(idx, sorted.size() - 1)];
    }
};

struct ProbeMeasurements {
    int successfulAcquires = 0;
    int acquiredFrames = 0;
    int attempts = 0;
    int timeouts = 0;
    int accessLostCount = 0;
    int recreateCount = 0;
    int accumulatedFrames = 0;
    int accumulatedFramesSum = 0;
    int accumulatedFramesMax = 0;
    int successfulAcquiresWithLastPresent = 0;
    int duplicateLastPresentCount = 0;
    long long lastPresentGapUs = -1;
    long long minLastPresentGapUs = -1;
    long long maxLastPresentGapUs = -1;
    int nonZeroLastPresent = 0;
    int resetResourceCount = 0;
    int resourcesReleasedByHold = 0;
    HRESULT lastError = S_OK;
    TimerStats acquireTimings;
    TimerStats releaseTimings;
};

bool StartsWith(std::string_view value, std::string_view prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool ParseUnsignedInt(std::string_view text, unsigned long long& output) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const char* begin = text.data();
    const unsigned long long parsed = std::strtoull(begin, &end, 10);
    if (end == begin || *end != '\0') {
        return false;
    }
    output = parsed;
    return true;
}

void AppendUsage() {
    printf(
        "probe_idd_duplication [options]\n"
        "\n"
        "Output-duplication probe with optional high-cadence DXGI diagnostics.\n"
        "\n"
        "Options:\n"
        "  --frames=<n>                 Frames required before success.\n"
        "  --acquire-timeout-ms=<n>     Timeout in AcquireNextFrame.\n"
        "  --duration-ms=<n>            Per-output probe budget in milliseconds.\n"
        "  --nonblocking-acquire         Use AcquireNextFrame(0).\n"
        "  --resource-reset-before-release\n"
        "                               Release the duplicated resource before ReleaseFrame.\n"
        "  --hold-ms=<n>                Hold the duplicated resource for N ms before ReleaseFrame.\n"
        "  --help                       Show this help.\n");
}

std::string FormatDouble(double value) {
    char out[64];
    snprintf(out, sizeof(out), "%.3f", value);
    return out;
}

std::string TimerToJson(const TimerStats& stats, const char* key) {
    (void)key;
    char out[300];
    snprintf(out, sizeof(out),
             "{\"count\":%zu,\"min\":%s,\"p50\":%s,\"p95\":%s,\"p99\":%s,\"max\":%s,\"mean\":%s}",
             stats.Count(),
             FormatDouble(stats.Min()).c_str(),
             FormatDouble(stats.Percentile(0.50)).c_str(),
             FormatDouble(stats.Percentile(0.95)).c_str(),
             FormatDouble(stats.Percentile(0.99)).c_str(),
             FormatDouble(stats.Max()).c_str(),
             FormatDouble(stats.Mean()).c_str());
    return out;
}

void WaitMicroseconds(ULONGLONG microseconds) {
    if (microseconds == 0) {
        return;
    }

    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    LARGE_INTEGER now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    const ULONGLONG target = start.QuadPart + static_cast<ULONGLONG>(
                                (frequency.QuadPart * static_cast<LONGLONG>(microseconds)) / 1000000ull);
    while (QueryPerformanceCounter(&now), now.QuadPart < static_cast<LONGLONG>(target)) {
        const ULONGLONG remaining = target - now.QuadPart;
        if (remaining > static_cast<ULONGLONG>(frequency.QuadPart) / 1000ull) {
            Sleep(0);
        }
    }
}

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

std::string DxgiProbeWideToUtf8(const wchar_t* value) {
    if (value == nullptr || *value == L'\0') {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }

    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

std::string DxgiProbeJsonEscape(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (ch < 0x20) {
                char escaped[7];
                snprintf(escaped, sizeof(escaped), "\\u%04X", static_cast<unsigned int>(ch));
                result += escaped;
            } else {
                result += static_cast<char>(ch);
            }
            break;
        }
    }
    return result;
}

HRESULT CreateOutputDuplicationForProbe(ID3D11Device* device, IDXGIOutput* output,
                                        IDXGIOutputDuplication** duplication) {
    *duplication = nullptr;

    // Both formats offered, and the order matters: an HDR desktop duplicates as
    // scRGB FP16, and a probe that asked only for BGRA would report an output that
    // cannot be duplicated when the truth is that it cannot be duplicated AS BGRA.
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                   DXGI_FORMAT_R10G10B10A2_UNORM};

    HRESULT hr = E_FAIL;
    IDXGIOutput5* output5 = nullptr;
    if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput5), reinterpret_cast<void**>(&output5)))) {
        hr = output5->DuplicateOutput1(device, 0, ARRAYSIZE(formats), formats, duplication);
        SafeRelease(output5);
    }
    if (FAILED(hr)) {
        IDXGIOutput1* output1 = nullptr;
        if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1)))) {
            hr = output1->DuplicateOutput(device, duplication);
            SafeRelease(output1);
        }
    }
    return hr;
}
OutputReport ProbeOutput(ID3D11Device* device, IDXGIOutput* output, UINT adapterIndex, UINT outputIndex,
                        const ProbeOptions& options) {
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

    IDXGIOutputDuplication* duplication = nullptr;
    HRESULT hr = CreateOutputDuplicationForProbe(device, output, &duplication);
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

    ProbeMeasurements measurements;
    constexpr int kMaxAccessLostRecreates = 5;
    measurements.lastError = S_OK;
    const ULONGLONG probeDeadline = GetTickCount64() + options.budgetMs;

    LARGE_INTEGER qpcFrequency{};
    QueryPerformanceFrequency(&qpcFrequency);

    LARGE_INTEGER lastPresentTime{};
    lastPresentTime.QuadPart = 0;
    const UINT acquireTimeout = options.nonBlockingAcquire ? 0 : options.acquireTimeoutMs;

    while (measurements.acquiredFrames < options.framesPerOutput && GetTickCount64() < probeDeadline) {
        ++measurements.attempts;
        DXGI_OUTDUPL_FRAME_INFO info{};
        IDXGIResource* resource = nullptr;
        LARGE_INTEGER acquireStart{};
        LARGE_INTEGER acquireEnd{};
        QueryPerformanceCounter(&acquireStart);
        HRESULT acquired = duplication->AcquireNextFrame(acquireTimeout, &info, &resource);
        QueryPerformanceCounter(&acquireEnd);
        if (acquireEnd.QuadPart >= acquireStart.QuadPart) {
            measurements.acquireTimings.AddSample((static_cast<double>(acquireEnd.QuadPart - acquireStart.QuadPart)
                                                   * 1000000.0)
                                                  / qpcFrequency.QuadPart);
        }

        if (acquired == DXGI_ERROR_WAIT_TIMEOUT) {
            ++measurements.timeouts;
            continue;
        }
        if (acquired == DXGI_ERROR_ACCESS_LOST) {
            ++measurements.accessLostCount;
            measurements.lastError = acquired;
            if (measurements.recreateCount >= kMaxAccessLostRecreates) {
                break;
            }

            SafeRelease(duplication);
            Sleep(100);

            hr = CreateOutputDuplicationForProbe(device, output, &duplication);
            if (FAILED(hr) || duplication == nullptr) {
                measurements.lastError = hr;
                break;
            }

            ++measurements.recreateCount;
            duplication->GetDesc(&duplDesc);

            continue;
        }
        if (FAILED(acquired)) {
            measurements.lastError = acquired;
            break;
        }
        measurements.lastError = S_OK;
        ++measurements.successfulAcquires;
        // LastPresentTime zero means the desktop did not change and only the mouse
        // moved. Counting those as frames would report a duplicating output on a
        // machine where nothing renders at all.
        if (info.LastPresentTime.QuadPart != 0) {
            ++measurements.acquiredFrames;
        }

        const int accumulatedNow = static_cast<int>(info.AccumulatedFrames);
        measurements.accumulatedFrames = accumulatedNow;
        measurements.accumulatedFramesSum += accumulatedNow;
        if (accumulatedNow > measurements.accumulatedFramesMax) {
            measurements.accumulatedFramesMax = accumulatedNow;
        }

        if (info.AccumulatedFrames != 0) {
            ++measurements.nonZeroLastPresent;
        }
        if (info.LastPresentTime.QuadPart != 0 && lastPresentTime.QuadPart != 0 &&
            info.LastPresentTime.QuadPart == lastPresentTime.QuadPart) {
            ++measurements.duplicateLastPresentCount;
        }
        if (info.LastPresentTime.QuadPart != 0) {
            if (lastPresentTime.QuadPart != 0) {
                const long long gapUs =
                    (info.LastPresentTime.QuadPart - lastPresentTime.QuadPart) / 10;
                if (gapUs > 0) {
                    if (measurements.minLastPresentGapUs < 0 || gapUs < measurements.minLastPresentGapUs) {
                        measurements.minLastPresentGapUs = gapUs;
                    }
                    if (measurements.maxLastPresentGapUs < 0 || gapUs > measurements.maxLastPresentGapUs) {
                        measurements.maxLastPresentGapUs = gapUs;
                    }
                }
            }
            lastPresentTime = info.LastPresentTime;
        }

        if (options.holdAfterAcquireMs > 0) {
            WaitMicroseconds(options.holdAfterAcquireMs * 1000ull);
        }

        if (options.resetResourceBeforeRelease) {
            SafeRelease(resource);
            ++measurements.resetResourceCount;
        }

        LARGE_INTEGER releaseStart{};
        LARGE_INTEGER releaseEnd{};
        QueryPerformanceCounter(&releaseStart);
        const HRESULT released = duplication->ReleaseFrame();
        QueryPerformanceCounter(&releaseEnd);
        if (releaseEnd.QuadPart >= releaseStart.QuadPart) {
            measurements.releaseTimings.AddSample((static_cast<double>(releaseEnd.QuadPart - releaseStart.QuadPart)
                                                  * 1000000.0)
                                                 / qpcFrequency.QuadPart);
        }
        if (FAILED(released)) {
            measurements.lastError = released;
            SafeRelease(resource);
            break;
        }
        SafeRelease(resource);
    }

    char result[1536];
    std::string acquireTimingsJson = TimerToJson(measurements.acquireTimings, "acquireUs");
    std::string releaseTimingsJson = TimerToJson(measurements.releaseTimings, "releaseUs");
    const long long minPresentGapUs = measurements.minLastPresentGapUs < 0 ? 0 : measurements.minLastPresentGapUs;
    const long long maxPresentGapUs = measurements.maxLastPresentGapUs < 0 ? 0 : measurements.maxLastPresentGapUs;
    const bool okResult = measurements.acquiredFrames >= options.framesPerOutput;
    snprintf(result, sizeof(result),
             "\"duplication\":{\"ok\":%s,\"format\":\"%s\",\"desktopWidth\":%u,\"desktopHeight\":%u,"
             "\"frames\":%d,\"attempts\":%d,\"timeouts\":%d,\"accumulatedFrames\":%d,"
             "\"accumulatedFramesSum\":%d,\"accumulatedFramesMax\":%d,"
             "\"acquireTimings\":%s,\"releaseTimings\":%s,"
             "\"presentGapUs\":{\"count\":%d,\"min\":%lld,\"max\":%lld},"
             "\"resources\":{\"resetBeforeRelease\":%d,\"releaseCalls\":%d},"
             "\"acquireTimeoutMs\":%u,\"nonBlockingAcquire\":%s,\"holdAfterAcquireMs\":%llu,"
             "\"lastHresult\":\"0x%08lX\"}}",
             okResult ? "true" : "false",
             FormatName(duplDesc.ModeDesc.Format), duplDesc.ModeDesc.Width, duplDesc.ModeDesc.Height,
             measurements.acquiredFrames, measurements.attempts, measurements.timeouts, measurements.accumulatedFrames,
             measurements.accumulatedFramesSum, measurements.accumulatedFramesMax, acquireTimingsJson.c_str(),
             releaseTimingsJson.c_str(),
             measurements.nonZeroLastPresent,
             minPresentGapUs, maxPresentGapUs,
             measurements.resetResourceCount, measurements.successfulAcquires,
             options.acquireTimeoutMs, options.nonBlockingAcquire ? "true" : "false", options.holdAfterAcquireMs,
             static_cast<unsigned long>(measurements.lastError));
    report.json += result;
    if (const size_t close = report.json.rfind("}}"); close != std::string::npos) {
        char diagnostics[128];
        snprintf(diagnostics, sizeof(diagnostics), ",\"accessLost\":%d,\"recreates\":%d,\"duplicateLastPresent\":%d",
                 measurements.accessLostCount, measurements.recreateCount, measurements.duplicateLastPresentCount);
        report.json.insert(close, diagnostics);
    }
    report.frames = measurements.acquiredFrames;

    SafeRelease(duplication);
    return report;
}

} // namespace

// Desktop Duplication is scoped to the desktop/session visible to the calling
// thread. Unattended launch mechanisms may start a process on a different
// desktop, so attach to the current input desktop before probing.
//
// This only removes desktop attachment as a variable. DXGI_ERROR_ACCESS_LOST
// may still occur when the display topology, mode, or underlying virtual display
// path invalidates the duplication interface.
static bool AttachToInputDesktop() {
    HDESK desktop = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (desktop == nullptr) {
        return false;
    }
    const bool attached = SetThreadDesktop(desktop) != FALSE;
    if (!attached) {
        CloseDesktop(desktop);
    }
    // A desktop that is attached stays open for the lifetime of the thread.
    return attached;
}

int main(int argc, char* argv[]) {
    ProbeOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            AppendUsage();
            return 0;
        }
        if (StartsWith(arg, "--frames=")) {
            unsigned long long value = 0;
            if (!ParseUnsignedInt(arg.substr(strlen("--frames=")), value) || value == 0 || value > 100000ULL) {
                AppendUsage();
                return 1;
            }
            options.framesPerOutput = static_cast<int>(value);
            continue;
        }
        if (StartsWith(arg, "--acquire-timeout-ms=")) {
            unsigned long long value = 0;
            if (!ParseUnsignedInt(arg.substr(strlen("--acquire-timeout-ms=")), value) || value > 60000ULL) {
                AppendUsage();
                return 1;
            }
            options.acquireTimeoutMs = static_cast<UINT>(value);
            continue;
        }
        if (StartsWith(arg, "--duration-ms=")) {
            unsigned long long value = 0;
            if (!ParseUnsignedInt(arg.substr(strlen("--duration-ms=")), value) || value == 0) {
                AppendUsage();
                return 1;
            }
            options.budgetMs = value;
            continue;
        }
        if (StartsWith(arg, "--hold-ms=")) {
            unsigned long long value = 0;
            if (!ParseUnsignedInt(arg.substr(strlen("--hold-ms=")), value) || value > 100000ULL) {
                AppendUsage();
                return 1;
            }
            options.holdAfterAcquireMs = value;
            continue;
        }
        if (arg == "--nonblocking-acquire") {
            options.nonBlockingAcquire = true;
            options.acquireTimeoutMs = 0;
            continue;
        }
        if (arg == "--resource-reset-before-release") {
            options.resetResourceBeforeRelease = true;
            continue;
        }

        AppendUsage();
        return 1;
    }

    if (options.nonBlockingAcquire) {
        options.acquireTimeoutMs = 0;
    }

    const bool onInputDesktop = AttachToInputDesktop();
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
            OutputReport report = ProbeOutput(device, output, adapterIndex, outputIndex, options);
            if (!report.json.empty() && report.json.front() == '{') {
                const std::string description = DxgiProbeJsonEscape(DxgiProbeWideToUtf8(adapterDesc.Description));
                char metadata[768];
                snprintf(metadata, sizeof(metadata),
                         "\"adapterDescription\":\"%s\","
                         "\"adapterVendorId\":%u,"
                         "\"adapterDeviceId\":%u,"
                         "\"adapterLuidHigh\":%ld,"
                         "\"adapterLuidLow\":%lu,",
                         description.c_str(),
                         adapterDesc.VendorId,
                         adapterDesc.DeviceId,
                         static_cast<long>(adapterDesc.AdapterLuid.HighPart),
                         static_cast<unsigned long>(adapterDesc.AdapterLuid.LowPart));
                report.json.insert(1, metadata);
            }
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
    printf("{\"ok\":true,\"onInputDesktop\":%s,\"outputs\":%zu,\"duplicating\":%d,\"producingFrames\":%d,"
           "\"framesRequested\":%d,\"acquireTimeoutMs\":%u,\"nonBlockingAcquire\":%s,\"resourceResetBeforeRelease\":%s,"
           "\"holdAfterAcquireMs\":%llu,\"budgetMs\":%llu,\"detail\":[%s]}\n",
           onInputDesktop ? "true" : "false", reports.size(), duplicated, producing, options.framesPerOutput,
           options.acquireTimeoutMs, options.nonBlockingAcquire ? "true" : "false",
           options.resetResourceBeforeRelease ? "true" : "false", options.holdAfterAcquireMs, options.budgetMs,
           joined.c_str());
    fflush(stdout);
    return reports.empty() ? 1 : 0;
}
