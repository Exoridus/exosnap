#include "WebcamService.h"

#include <QCoreApplication>
#include <QMetaObject>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <windows.h>
#include <winrt/base.h>

#include <algorithm>
#include <cassert>

// MF libs are listed in CMakeLists.txt target_link_libraries and in the
// exosnap_device_service_test_support support target; no redundant pragmas here.
// /DELAYLOAD:mfplat.dll, /DELAYLOAD:mf.dll, /DELAYLOAD:mfreadwrite.dll are set
// on the exosnap exe target so MF DLLs are not loaded until first use. The
// IsMfPresent() probe below must be called before any MF entry point.

namespace exosnap {

namespace {

// Avoid signed/unsigned mismatch warnings with the MF enum constant.
static constexpr DWORD kFirstVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

// Tiny RAII wrapper for CoTaskMem strings from MF attributes.
struct CoStringGuard {
    WCHAR* p = nullptr;
    CoStringGuard() = default;
    CoStringGuard(const CoStringGuard&) = delete;
    CoStringGuard& operator=(const CoStringGuard&) = delete;
    ~CoStringGuard() {
        CoTaskMemFree(p);
    }
};

// Convert WCHAR* to std::string (UTF-8).
std::string WcharToString(const WCHAR* w) {
    if (!w)
        return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1)
        return {};
    std::string s(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}

// Returns the smallest MFVideoFormat_* GUID that maps to BGRA output via MF.
// We prefer MFVideoFormat_ARGB32 (= BGRA in memory on little-endian Windows).
// Falls back to YUY2 when ARGB32 is unavailable; YUY2 is then converted to BGRA below.

bool TrySetBgraOutputType(IMFSourceReader* reader) {
    // Request ARGB32 output (BGRA in memory).
    winrt::com_ptr<IMFMediaType> type;
    HRESULT hr = MFCreateMediaType(type.put());
    if (FAILED(hr))
        return false;
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
    hr = reader->SetCurrentMediaType(kFirstVideoStream, nullptr, type.get());
    return SUCCEEDED(hr);
}

bool TrySetYuy2OutputType(IMFSourceReader* reader) {
    winrt::com_ptr<IMFMediaType> type;
    HRESULT hr = MFCreateMediaType(type.put());
    if (FAILED(hr))
        return false;
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_YUY2);
    hr = reader->SetCurrentMediaType(kFirstVideoStream, nullptr, type.get());
    return SUCCEEDED(hr);
}

// Convert YUY2 → BGRA (row-major, in-place output buffer).
void Yuy2ToBgra(const uint8_t* src, int width, int height, int src_stride, uint8_t* dst, int dst_stride) {
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = src + y * src_stride;
        uint8_t* out = dst + y * dst_stride;
        for (int x = 0; x < width; x += 2) {
            // YUY2: Y0 U0 Y1 V0
            const int y0 = row[0], u = row[1], y1 = row[2], v = row[3];
            row += 4;

            auto clamp = [](int v) -> uint8_t { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); };
            auto pixel = [&](int yy) -> std::tuple<uint8_t, uint8_t, uint8_t> {
                const int c = yy - 16, d = u - 128, e = v - 128;
                uint8_t r = clamp((298 * c + 409 * e + 128) >> 8);
                uint8_t g = clamp((298 * c - 100 * d - 208 * e + 128) >> 8);
                uint8_t b = clamp((298 * c + 516 * d + 128) >> 8);
                return {r, g, b};
            };

            auto [r0, g0, b0] = pixel(y0);
            out[0] = b0;
            out[1] = g0;
            out[2] = r0;
            out[3] = 255;
            out += 4;
            if (x + 1 < width) {
                auto [r1, g1, b1] = pixel(y1);
                out[0] = b1;
                out[1] = g1;
                out[2] = r1;
                out[3] = 255;
                out += 4;
            }
        }
    }
}

struct ReaderContext {
    winrt::com_ptr<IMFSourceReader> reader;
    int width = 0;
    int height = 0;
    bool is_bgra = true; // false = YUY2
};

// Try to open an IMFSourceReader on device_id (or first device if empty).
std::optional<ReaderContext> OpenReader(const std::string& device_id, int want_w, int want_h, int /*want_fps*/) {
    // Create device attributes filter.
    winrt::com_ptr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(attrs.put(), 1)))
        return std::nullopt;
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attrs.get(), &devices, &count)) || count == 0) {
        if (devices)
            CoTaskMemFree(devices);
        return std::nullopt;
    }

    // Find matching device or pick first.
    IMFActivate* selected = nullptr;
    for (UINT32 i = 0; i < count; ++i) {
        CoStringGuard sym;
        UINT32 len = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &sym.p,
                                                     &len))) {
            if (device_id.empty() || WcharToString(sym.p) == device_id) {
                selected = devices[i];
                selected->AddRef();
                break;
            }
        }
    }
    if (!selected && count > 0) {
        selected = devices[0];
        selected->AddRef();
    }
    for (UINT32 i = 0; i < count; ++i)
        devices[i]->Release();
    CoTaskMemFree(devices);
    if (!selected)
        return std::nullopt;

    winrt::com_ptr<IMFMediaSource> source;
    HRESULT hr = selected->ActivateObject(IID_PPV_ARGS(source.put()));
    selected->Release();
    if (FAILED(hr))
        return std::nullopt;

    // Reader attributes: enable hardware decoding.
    winrt::com_ptr<IMFAttributes> readerAttrs;
    MFCreateAttributes(readerAttrs.put(), 1);

    winrt::com_ptr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromMediaSource(source.get(), readerAttrs.get(), reader.put());
    if (FAILED(hr))
        return std::nullopt;

    // Try to set desired resolution on the native media type first.
    // Enumerate and pick the best matching format.
    for (DWORD i = 0;; ++i) {
        winrt::com_ptr<IMFMediaType> native;
        if (FAILED(reader->GetNativeMediaType(kFirstVideoStream, i, native.put())))
            break;
        GUID sub{};
        native->GetGUID(MF_MT_SUBTYPE, &sub);
        UINT64 sz = 0;
        native->GetUINT64(MF_MT_FRAME_SIZE, &sz);
        const UINT32 nw = static_cast<UINT32>(sz >> 32);
        const UINT32 nh = static_cast<UINT32>(sz & 0xFFFFFFFF);
        if (static_cast<int>(nw) == want_w && static_cast<int>(nh) == want_h) {
            reader->SetCurrentMediaType(kFirstVideoStream, nullptr, native.get());
            break;
        }
    }

    // Set output type to BGRA or YUY2.
    bool is_bgra = TrySetBgraOutputType(reader.get());
    if (!is_bgra && !TrySetYuy2OutputType(reader.get()))
        return std::nullopt;

    // Read actual output dimensions.
    winrt::com_ptr<IMFMediaType> currentType;
    if (FAILED(reader->GetCurrentMediaType(kFirstVideoStream, currentType.put())))
        return std::nullopt;
    UINT64 sz = 0;
    currentType->GetUINT64(MF_MT_FRAME_SIZE, &sz);
    const int actual_w = static_cast<int>(sz >> 32);
    const int actual_h = static_cast<int>(sz & 0xFFFFFFFF);
    if (actual_w <= 0 || actual_h <= 0)
        return std::nullopt;

    ReaderContext ctx;
    ctx.reader = std::move(reader);
    ctx.width = actual_w;
    ctx.height = actual_h;
    ctx.is_bgra = is_bgra;
    return ctx;
}

} // namespace

// ---------------------------------------------------------------------------
// S4: MF presence probe (once-per-process, cached)
// ---------------------------------------------------------------------------

// static
bool WebcamService::IsMfPresent() noexcept {
    // Probe mfplat.dll via LoadLibraryW — safe even with /DELAYLOAD because we
    // never call a delay-loaded symbol here (LoadLibraryW is a kernel32 export).
    // The result is cached: a Windows-N machine without the Media Feature Pack
    // will always return false; a normal Windows install always returns true.
    static const bool s_present = []() noexcept -> bool {
        HMODULE h = LoadLibraryW(L"mfplat.dll");
        if (h) {
            FreeLibrary(h);
            return true;
        }
        return false;
    }();
    return s_present;
}

// ---------------------------------------------------------------------------
// WebcamService public API
// ---------------------------------------------------------------------------

WebcamService::~WebcamService() {
    Stop();
}

std::vector<WebcamDeviceInfo> WebcamService::EnumerateDevices() {
    if (!IsMfPresent())
        return {};
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);

    winrt::com_ptr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(attrs.put(), 1))) {
        MFShutdown();
        return {};
    }
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    std::vector<WebcamDeviceInfo> result;

    if (SUCCEEDED(MFEnumDeviceSources(attrs.get(), &devices, &count))) {
        for (UINT32 i = 0; i < count; ++i) {
            WebcamDeviceInfo info;
            CoStringGuard sym, name;
            UINT32 len = 0;
            if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                                         &sym.p, &len)))
                info.id = WcharToString(sym.p);
            len = 0;
            if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name.p, &len)))
                info.name = WcharToString(name.p);
            if (info.name.empty())
                info.name = "Camera " + std::to_string(i + 1);
            result.push_back(std::move(info));
            devices[i]->Release();
        }
        CoTaskMemFree(devices);
    }

    MFShutdown();
    return result;
}

std::vector<WebcamFormat> WebcamService::EnumerateFormats(const std::string& device_id) {
    if (!IsMfPresent())
        return {};
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);

    winrt::com_ptr<IMFAttributes> attrs;
    MFCreateAttributes(attrs.put(), 1);
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    std::vector<WebcamFormat> result;

    if (FAILED(MFEnumDeviceSources(attrs.get(), &devices, &count)) || count == 0) {
        if (devices)
            CoTaskMemFree(devices);
        MFShutdown();
        return {};
    }

    IMFActivate* selected = nullptr;
    for (UINT32 i = 0; i < count; ++i) {
        CoStringGuard sym;
        UINT32 len = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &sym.p,
                                                     &len))) {
            if (device_id.empty() || WcharToString(sym.p) == device_id) {
                selected = devices[i];
                selected->AddRef();
                break;
            }
        }
    }
    if (!selected && count > 0) {
        selected = devices[0];
        selected->AddRef();
    }
    for (UINT32 i = 0; i < count; ++i)
        devices[i]->Release();
    CoTaskMemFree(devices);

    if (selected) {
        winrt::com_ptr<IMFMediaSource> source;
        if (SUCCEEDED(selected->ActivateObject(IID_PPV_ARGS(source.put())))) {
            winrt::com_ptr<IMFSourceReader> reader;
            if (SUCCEEDED(MFCreateSourceReaderFromMediaSource(source.get(), nullptr, reader.put()))) {
                for (DWORD i = 0;; ++i) {
                    winrt::com_ptr<IMFMediaType> type;
                    if (FAILED(reader->GetNativeMediaType(kFirstVideoStream, i, type.put())))
                        break;
                    UINT64 sz = 0;
                    type->GetUINT64(MF_MT_FRAME_SIZE, &sz);
                    UINT64 fps = 0;
                    type->GetUINT64(MF_MT_FRAME_RATE, &fps);
                    const int w = static_cast<int>(sz >> 32);
                    const int h = static_cast<int>(sz & 0xFFFFFFFF);
                    const int fn = static_cast<int>(fps >> 32);
                    const int fd = static_cast<int>(fps & 0xFFFFFFFF);
                    if (w > 0 && h > 0 && fn > 0 && fd > 0) {
                        // deduplicate
                        bool dup = false;
                        for (const auto& f : result)
                            if (f.width == w && f.height == h && f.fps_num == fn && f.fps_den == fd) {
                                dup = true;
                                break;
                            }
                        if (!dup)
                            result.push_back({w, h, fn, fd});
                    }
                }
            }
        }
        selected->Release();
    }

    MFShutdown();
    return result;
}

void WebcamService::SetFrameCallback(FrameCallback cb) {
    frame_callback_ = std::move(cb);
}

bool WebcamService::Start(const std::string& device_id, int width, int height, int fps) {
    Stop();
    running_.store(true);
    thread_ = std::jthread([this, device_id, width, height, fps](std::stop_token st) {
        ThreadMain(device_id, width, height, fps, std::move(st));
    });
    return true;
}

void WebcamService::Stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
    running_.store(false);
    {
        std::lock_guard lk(frame_mutex_);
        has_frame_ = false;
        latest_bgra_.clear();
        frame_width_ = 0;
        frame_height_ = 0;
    }
}

bool WebcamService::IsRunning() const noexcept {
    return running_.load();
}

bool WebcamService::TryGetFrame(int& out_width, int& out_height, std::vector<uint8_t>& out_bgra) {
    std::lock_guard lk(frame_mutex_);
    if (!has_frame_)
        return false;
    out_width = frame_width_;
    out_height = frame_height_;
    out_bgra = latest_bgra_;
    return true;
}

void WebcamService::ThreadMain(const std::string& device_id, int width, int height, int fps, std::stop_token stop) {
    if (!IsMfPresent()) {
        running_.store(false);
        return;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);

    auto ctx = OpenReader(device_id, width, height, fps);
    if (!ctx) {
        running_.store(false);
        MFShutdown();
        CoUninitialize();
        return;
    }

    const int W = ctx->width;
    const int H = ctx->height;
    std::vector<uint8_t> scratch(static_cast<size_t>(W) * H * 4);

    while (!stop.stop_requested()) {
        DWORD flags = 0;
        winrt::com_ptr<IMFSample> sample;
        HRESULT hr = ctx->reader->ReadSample(kFirstVideoStream, 0, nullptr, &flags, nullptr, sample.put());
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR))
            break;
        if (!sample)
            continue;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;

        winrt::com_ptr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(buf.put())))
            continue;

        BYTE* src = nullptr;
        DWORD maxLen = 0, curLen = 0;
        if (FAILED(buf->Lock(&src, &maxLen, &curLen)))
            continue;

        if (ctx->is_bgra) {
            const size_t expected = static_cast<size_t>(W) * H * 4;
            if (curLen >= expected)
                scratch.assign(src, src + expected);
        } else {
            // YUY2 → BGRA
            const int srcStride = W * 2;
            Yuy2ToBgra(src, W, H, srcStride, scratch.data(), W * 4);
        }
        buf->Unlock();

        // Store latest frame.
        StoreFrame(W, H, scratch);

        // Post QImage preview to main thread.
        QImage img(scratch.data(), W, H, W * 4, QImage::Format_ARGB32);
        PostFrame(img.copy()); // copy needed: scratch is reused
    }

    running_.store(false);
    MFShutdown();
    CoUninitialize();
}

void WebcamService::StoreFrame(int w, int h, std::vector<uint8_t> bgra) {
    std::lock_guard lk(frame_mutex_);
    frame_width_ = w;
    frame_height_ = h;
    latest_bgra_ = std::move(bgra);
    has_frame_ = true;
}

void WebcamService::PostFrame(QImage img) {
    if (!frame_callback_)
        return;
    auto cb = frame_callback_;
    QMetaObject::invokeMethod(
        QCoreApplication::instance(), [cb, img = std::move(img)]() mutable { cb(std::move(img)); },
        Qt::QueuedConnection);
}

} // namespace exosnap
