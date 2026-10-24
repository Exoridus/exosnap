#include "WebcamService.h"
#include "WebcamReadCallback.h"

#include "../diagnostics/StructuredLog.h"

#include <QCoreApplication>
#include <QMetaObject>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <strmif.h>

#include <exosnap/engine/util/com_apartment.h>
#include <exosnap/engine/util/mf_session.h>

#include <windows.h>
#include <winrt/base.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <thread>

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

// Asks the reader to deliver `subtype`. For a subtype the current native type
// already has this is a pass-through; otherwise MF inserts its decoder/video
// processor (MJPEG decode, colour conversion) on the CPU.
bool TrySetOutputSubtype(IMFSourceReader* reader, const GUID& subtype, HRESULT* out_hr = nullptr) {
    winrt::com_ptr<IMFMediaType> type;
    HRESULT hr = MFCreateMediaType(type.put());
    if (FAILED(hr)) {
        if (out_hr)
            *out_hr = hr;
        return false;
    }
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, subtype);
    hr = reader->SetCurrentMediaType(kFirstVideoStream, nullptr, type.get());
    if (out_hr)
        *out_hr = hr;
    return SUCCEEDED(hr);
}

// MFVideoFormat_* subtype GUIDs encode a FourCC in Data1 (e.g. 'MJPG', 'NV12',
// 'YUY2', 'H264') against a fixed base GUID suffix — readable/loggable without a
// lookup table for the common camera formats this matters for.
std::string SubtypeFourCc(const GUID& g) {
    char buf[5] = {static_cast<char>(g.Data1 & 0xFF), static_cast<char>((g.Data1 >> 8) & 0xFF),
                   static_cast<char>((g.Data1 >> 16) & 0xFF), static_cast<char>((g.Data1 >> 24) & 0xFF), 0};
    for (char& c : buf) {
        if (c != 0 && (c < 0x20 || c > 0x7E))
            return "non_fourcc";
    }
    return buf;
}

WebcamNativeEncoding NativeEncodingOf(const GUID& subtype) noexcept {
    if (subtype == MFVideoFormat_NV12)
        return WebcamNativeEncoding::Nv12;
    if (subtype == MFVideoFormat_YUY2)
        return WebcamNativeEncoding::Yuy2;
    return WebcamNativeEncoding::Other;
}

const char* PixelFormatName(exosnap::engine::WebcamPixelFormat format) noexcept {
    switch (format) {
    case exosnap::engine::WebcamPixelFormat::Nv12:
        return "NV12";
    case exosnap::engine::WebcamPixelFormat::Yuy2:
        return "YUY2";
    case exosnap::engine::WebcamPixelFormat::Bgra:
        return "BGRA";
    }
    return "unknown";
}

// MF leaves both attributes unset on most UVC types. BT.601 limited range is
// what such cameras send, and what this reader assumed before it read them.
void ReadYuvColorDescription(IMFMediaType* type, exosnap::engine::MatrixCoefficients& matrix,
                             exosnap::engine::ColorRange& range) {
    matrix = exosnap::engine::MatrixCoefficients::Bt601;
    range = exosnap::engine::ColorRange::Limited;
    UINT32 value = 0;
    if (SUCCEEDED(type->GetUINT32(MF_MT_YUV_MATRIX, &value)) && value == MFVideoTransferMatrix_BT709)
        matrix = exosnap::engine::MatrixCoefficients::Bt709;
    if (SUCCEEDED(type->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &value)) && value == MFNominalRange_0_255)
        range = exosnap::engine::ColorRange::Full;
}

// Where a UVC control lives on a Windows capture source: the processing unit
// behind IAMVideoProcAmp or the camera terminal behind IAMCameraControl, and
// the KS property id there. The DirectShow enums stop at gain; the power-line
// frequency is the next KS id of the same property set and is reached through
// the same interface.
struct ControlBinding {
    bool camera_terminal = false;
    long id = 0;
};

ControlBinding BindingOf(WebcamControl control) noexcept {
    switch (control) {
    case WebcamControl::Brightness:
        return {false, VideoProcAmp_Brightness};
    case WebcamControl::Contrast:
        return {false, VideoProcAmp_Contrast};
    case WebcamControl::Hue:
        return {false, VideoProcAmp_Hue};
    case WebcamControl::Saturation:
        return {false, VideoProcAmp_Saturation};
    case WebcamControl::Sharpness:
        return {false, VideoProcAmp_Sharpness};
    case WebcamControl::Gamma:
        return {false, VideoProcAmp_Gamma};
    case WebcamControl::WhiteBalance:
        return {false, VideoProcAmp_WhiteBalance};
    case WebcamControl::BacklightCompensation:
        return {false, VideoProcAmp_BacklightCompensation};
    case WebcamControl::Gain:
        return {false, VideoProcAmp_Gain};
    case WebcamControl::PowerLineFrequency:
        return {false, 13}; // KSPROPERTY_VIDEOPROCAMP_POWERLINE_FREQUENCY
    case WebcamControl::Exposure:
        return {true, CameraControl_Exposure};
    case WebcamControl::Focus:
        return {true, CameraControl_Focus};
    case WebcamControl::Zoom:
        return {true, CameraControl_Zoom};
    case WebcamControl::Pan:
        return {true, CameraControl_Pan};
    case WebcamControl::Tilt:
        return {true, CameraControl_Tilt};
    }
    return {};
}

// Both interfaces share one flag encoding: 1 = automatic, 2 = manual.
constexpr long kControlFlagAuto = 0x1;
constexpr long kControlFlagManual = 0x2;

std::optional<WebcamControlRange> QueryControl(IMFMediaSource* source, WebcamControl control) {
    const ControlBinding binding = BindingOf(control);
    WebcamControlRange range;
    range.control = control;
    long caps = 0;
    long flags = 0;
    HRESULT hr = E_NOINTERFACE;
    if (binding.camera_terminal) {
        winrt::com_ptr<IAMCameraControl> camera;
        if (source == nullptr || FAILED(source->QueryInterface(IID_PPV_ARGS(camera.put()))))
            return std::nullopt;
        hr = camera->GetRange(binding.id, &range.minimum, &range.maximum, &range.step, &range.default_value, &caps);
        if (SUCCEEDED(hr) && FAILED(camera->Get(binding.id, &range.current, &flags)))
            range.current = range.default_value;
    } else {
        winrt::com_ptr<IAMVideoProcAmp> proc_amp;
        if (source == nullptr || FAILED(source->QueryInterface(IID_PPV_ARGS(proc_amp.put()))))
            return std::nullopt;
        hr = proc_amp->GetRange(binding.id, &range.minimum, &range.maximum, &range.step, &range.default_value, &caps);
        if (SUCCEEDED(hr) && FAILED(proc_amp->Get(binding.id, &range.current, &flags)))
            range.current = range.default_value;
    }
    if (FAILED(hr) || range.maximum < range.minimum)
        return std::nullopt;
    range.step = std::max(1L, range.step);
    range.can_automatic = (caps & kControlFlagAuto) != 0;
    range.can_manual = (caps & kControlFlagManual) != 0 || caps == 0;
    range.current_automatic = (flags & kControlFlagAuto) != 0;
    return range;
}

std::vector<WebcamControlRange> QueryControls(IMFMediaSource* source) {
    std::vector<WebcamControlRange> ranges;
    for (const WebcamControl control : kAllWebcamControls) {
        if (auto range = QueryControl(source, control))
            ranges.push_back(*range);
    }
    return ranges;
}

bool ApplyControl(IMFMediaSource* source, const WebcamControlRange& range, bool automatic, long value) {
    const ControlBinding binding = BindingOf(range.control);
    const bool use_auto = automatic && range.can_automatic;
    const long flags = use_auto ? kControlFlagAuto : kControlFlagManual;
    const long clamped = ClampWebcamControlValue(range, value);
    if (binding.camera_terminal) {
        winrt::com_ptr<IAMCameraControl> camera;
        return SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(camera.put()))) &&
               SUCCEEDED(camera->Set(binding.id, clamped, flags));
    }
    winrt::com_ptr<IAMVideoProcAmp> proc_amp;
    return SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(proc_amp.put()))) &&
           SUCCEEDED(proc_amp->Set(binding.id, clamped, flags));
}

// Brings the camera from `applied` to `desired`. A control that left the list
// returns to the camera's default, automatic where the camera offers it.
void ApplyControlChanges(IMFMediaSource* source, const std::vector<WebcamControlRange>& ranges,
                         const std::vector<WebcamControlSetting>& applied,
                         const std::vector<WebcamControlSetting>& desired) {
    if (source == nullptr)
        return;
    for (const WebcamControlRange& range : ranges) {
        const WebcamControlSetting* wanted = FindWebcamControlSetting(desired, range.control);
        const WebcamControlSetting* before = FindWebcamControlSetting(applied, range.control);
        if (wanted != nullptr) {
            if (before == nullptr || !(*before == *wanted))
                ApplyControl(source, range, wanted->automatic, wanted->value);
        } else if (before != nullptr) {
            ApplyControl(source, range, range.can_automatic, range.default_value);
        }
    }
}

struct ReaderContext {
    ReaderContext() = default;
    ReaderContext(ReaderContext&&) = default;
    ReaderContext(const ReaderContext&) = delete;
    ReaderContext& operator=(const ReaderContext&) = delete;
    ~ReaderContext() {
        if (callback)
            callback->Close();
        if (reader)
            reader->Flush(kFirstVideoStream);
        if (source)
            source->Shutdown();
    }
    winrt::com_ptr<IMFSourceReader> reader;
    Microsoft::WRL::ComPtr<WebcamReadCallback> callback;
    // The device itself, for the UVC controls the reader does not expose.
    winrt::com_ptr<IMFMediaSource> source;
    std::vector<WebcamControlRange> control_ranges;
    int width = 0;
    int height = 0;
    int fps = 0; // negotiated frame rate (rounded), 0 if unknown
    exosnap::engine::WebcamPixelFormat format = exosnap::engine::WebcamPixelFormat::Bgra;
    exosnap::engine::MatrixCoefficients matrix = exosnap::engine::MatrixCoefficients::Bt601;
    exosnap::engine::ColorRange range = exosnap::engine::ColorRange::Limited;
    std::string native_fourcc;
};

// Try to open an IMFSourceReader on device_id (or first device if empty).
// fail_reason names the exact step that bailed (with its HRESULT and, for the
// output-type negotiation, the camera's actual native format) so a failure log
// distinguishes "no device enumerated" from "device found but
// ActivateObject/CreateSourceReader/output-type negotiation failed" instead of
// a single undifferentiated "open_reader.failed" — this is what found the
// 2026-07-15 NV12-only-camera bug (see MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING
// below) and is worth keeping for the next camera-specific failure.
std::optional<ReaderContext> OpenReader(const std::string& device_id, int want_w, int want_h, int want_fps,
                                        std::string* fail_reason = nullptr) {
    const auto fail = [&](const char* step, HRESULT hr) {
        if (fail_reason) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s hr=0x%08lX", step, static_cast<unsigned long>(hr));
            *fail_reason = buf;
        }
        return std::nullopt;
    };

    // Create device attributes filter.
    winrt::com_ptr<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(attrs.put(), 1);
    if (FAILED(hr))
        return fail("MFCreateAttributes", hr);
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    hr = MFEnumDeviceSources(attrs.get(), &devices, &count);
    if (FAILED(hr) || count == 0) {
        if (devices)
            CoTaskMemFree(devices);
        return fail(FAILED(hr) ? "MFEnumDeviceSources" : "MFEnumDeviceSources:no_devices", hr);
    }

    // Find matching device or pick first.
    IMFActivate* selected = nullptr;
    for (UINT32 i = 0; i < count; ++i) {
        CoStringGuard sym;
        UINT32 len = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &sym.p,
                                                     &len))) {
            if (!device_id.empty() && WcharToString(sym.p) == device_id) {
                selected = devices[i];
                selected->AddRef();
                break;
            }
        }
    }
    // No silent devices[0] fallback: an empty/unmatched device_id opens nothing.
    for (UINT32 i = 0; i < count; ++i)
        devices[i]->Release();
    CoTaskMemFree(devices);
    if (!selected)
        return fail("no_matching_device", E_FAIL);

    winrt::com_ptr<IMFMediaSource> source;
    hr = selected->ActivateObject(IID_PPV_ARGS(source.put()));
    selected->Release();
    if (FAILED(hr))
        return fail("ActivateObject", hr);

    // Reader attributes: enable the ADVANCED video processor MFT so
    // SetCurrentMediaType can convert an NV12-native camera (the frame-server
    // default for most UVC devices at their configured resolution — confirmed
    // live via an isolated probe against a real device: GetNativeMediaType
    // returns NV12 only) to ARGB32 output below. The plain
    // MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING attribute only covers YUV->RGB32
    // conversion, NOT YUV->ARGB32 (the alpha channel is the difference) — the
    // same probe confirmed ARGB32 negotiation still fails with only that
    // attribute set, which is why an NV12-only camera opened fine in Windows'
    // own Camera app (a different, wider MF topology) but failed here.
    winrt::com_ptr<IMFAttributes> readerAttrs;
    hr = MFCreateAttributes(readerAttrs.put(), 1);
    if (FAILED(hr))
        return fail("MFCreateAttributes:readerAttrs", hr);
    readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    auto callback = Microsoft::WRL::Make<WebcamReadCallback>();
    if (!callback)
        return fail("CreateSourceReaderCallback", E_OUTOFMEMORY);
    hr = readerAttrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback.Get());
    if (FAILED(hr))
        return fail("SetSourceReaderCallback", hr);

    winrt::com_ptr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromMediaSource(source.get(), readerAttrs.get(), reader.put());
    if (FAILED(hr))
        return fail("MFCreateSourceReaderFromMediaSource", hr);

    // Captures what the native-format enumeration actually found (or didn't) so a
    // failed BGRA/YUY2 negotiation below is explained by the real native format
    // instead of a bare HRESULT. Logged only on failure.
    bool native_match_found = false;
    std::string native_match_fourcc;
    UINT32 native_enum_count = 0;
    HRESULT native_set_hr = S_OK;
    int negotiated_fps = 0;

    // Enumerate every native type this device offers, collecting (width, height,
    // frame rate) so the requested frame rate can actually be honoured. A
    // resolution is typically exposed at more than one native frame rate (that is
    // exactly what EnumerateFormats/the Settings resolution combo lists), so
    // taking the FIRST width/height match -- as this used to do -- picks whatever
    // rate MF's enumeration order happens to put first, not the one the caller
    // (and the UI) asked for.
    std::vector<WebcamNativeFormat> native_formats;
    std::vector<std::string> native_fourccs; // parallel to native_formats; diagnostics only
    for (DWORD i = 0;; ++i) {
        winrt::com_ptr<IMFMediaType> native;
        if (FAILED(reader->GetNativeMediaType(kFirstVideoStream, i, native.put())))
            break;
        ++native_enum_count;
        GUID sub{};
        native->GetGUID(MF_MT_SUBTYPE, &sub);
        UINT64 sz = 0;
        native->GetUINT64(MF_MT_FRAME_SIZE, &sz);
        UINT64 fr = 0;
        native->GetUINT64(MF_MT_FRAME_RATE, &fr);
        WebcamNativeFormat fmt;
        fmt.width = static_cast<int>(sz >> 32);
        fmt.height = static_cast<int>(sz & 0xFFFFFFFF);
        fmt.fps_num = static_cast<int>(fr >> 32);
        fmt.fps_den = static_cast<int>(fr & 0xFFFFFFFF);
        fmt.encoding = NativeEncodingOf(sub);
        native_formats.push_back(fmt);
        native_fourccs.push_back(SubtypeFourCc(sub));
    }

    // Try every width/height match in closest-fps-first order: the single
    // closest candidate can fail to negotiate for a reason unrelated to frame
    // rate (e.g. a pixel format this particular native type advertises that
    // the reader can't convert), even though a same-resolution neighbor a
    // little further from the requested fps would succeed. A single-shot
    // attempt on only the closest match would then silently fall through to
    // whatever the reader's default native type already was -- possibly the
    // wrong resolution entirely -- instead of the next-best candidate that
    // would have worked.
    const std::vector<int> ranked = RankWebcamNativeFormats(native_formats, want_w, want_h, want_fps);
    for (const int candidate_index : ranked) {
        native_match_found = true;
        native_match_fourcc = native_fourccs[static_cast<size_t>(candidate_index)];
        winrt::com_ptr<IMFMediaType> chosen;
        native_set_hr =
            reader->GetNativeMediaType(kFirstVideoStream, static_cast<DWORD>(candidate_index), chosen.put());
        if (FAILED(native_set_hr))
            continue;
        native_set_hr = reader->SetCurrentMediaType(kFirstVideoStream, nullptr, chosen.get());
        if (SUCCEEDED(native_set_hr)) {
            const auto& picked = native_formats[static_cast<size_t>(candidate_index)];
            negotiated_fps = RoundWebcamFps(picked.fps_num, picked.fps_den);
            break; // this candidate's native type is now current; stop retrying
        }
        // This candidate failed to negotiate: native_set_hr/native_match_fourcc
        // above already reflect it for the failure diagnostic below (in case
        // every candidate fails and BGRA/YUY2 negotiation also fails); try the
        // next-closest candidate.
    }

    // Output type. The current native type's own encoding goes first, so an NV12
    // or YUY2 camera is read untouched: offering NV12 to a YUY2 camera would
    // succeed too, through a CPU video processor this path exists to avoid.
    // Anything else (MJPEG, ...) is decoded by MF, preferably straight to NV12.
    GUID native_subtype{};
    {
        winrt::com_ptr<IMFMediaType> native_type;
        if (SUCCEEDED(reader->GetCurrentMediaType(kFirstVideoStream, native_type.put())))
            native_type->GetGUID(MF_MT_SUBTYPE, &native_subtype);
    }
    struct OutputCandidate {
        GUID subtype{};
        exosnap::engine::WebcamPixelFormat format = exosnap::engine::WebcamPixelFormat::Nv12;
    };
    std::vector<OutputCandidate> outputs;
    if (NativeEncodingOf(native_subtype) == WebcamNativeEncoding::Yuy2)
        outputs.push_back({MFVideoFormat_YUY2, exosnap::engine::WebcamPixelFormat::Yuy2});
    outputs.push_back({MFVideoFormat_NV12, exosnap::engine::WebcamPixelFormat::Nv12});
    if (NativeEncodingOf(native_subtype) != WebcamNativeEncoding::Yuy2)
        outputs.push_back({MFVideoFormat_YUY2, exosnap::engine::WebcamPixelFormat::Yuy2});
    // MFVideoFormat_ARGB32 is BGRA in memory on little-endian Windows.
    outputs.push_back({MFVideoFormat_ARGB32, exosnap::engine::WebcamPixelFormat::Bgra});

    std::optional<exosnap::engine::WebcamPixelFormat> output_format;
    std::string output_failures;
    for (const OutputCandidate& candidate : outputs) {
        HRESULT output_hr = S_OK;
        if (TrySetOutputSubtype(reader.get(), candidate.subtype, &output_hr)) {
            output_format = candidate.format;
            break;
        }
        char part[48];
        snprintf(part, sizeof(part), " %s_hr=0x%08lX", PixelFormatName(candidate.format),
                 static_cast<unsigned long>(output_hr));
        output_failures += part;
    }
    if (!output_format) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "no_usable_output_type%s native_matches=%u/%lu native_fourcc=%s native_set_hr=0x%08lX",
                 output_failures.c_str(), native_match_found ? 1u : 0u, static_cast<unsigned long>(native_enum_count),
                 native_match_found ? native_match_fourcc.c_str() : "(no exact match)",
                 static_cast<unsigned long>(native_set_hr));
        if (fail_reason)
            *fail_reason = buf;
        return std::nullopt;
    }

    // Read actual output dimensions.
    winrt::com_ptr<IMFMediaType> currentType;
    hr = reader->GetCurrentMediaType(kFirstVideoStream, currentType.put());
    if (FAILED(hr))
        return fail("GetCurrentMediaType", hr);
    UINT64 sz = 0;
    currentType->GetUINT64(MF_MT_FRAME_SIZE, &sz);
    const int actual_w = static_cast<int>(sz >> 32);
    const int actual_h = static_cast<int>(sz & 0xFFFFFFFF);
    if (exosnap::engine::WebcamFramePayloadBytes(*output_format, actual_w, actual_h) == 0)
        return fail("invalid_output_dimensions", E_FAIL);

    ReaderContext ctx;
    ctx.reader = std::move(reader);
    ctx.callback = std::move(callback);
    ctx.source = source;
    ctx.control_ranges = QueryControls(source.get());
    ctx.width = actual_w;
    ctx.height = actual_h;
    ctx.fps = negotiated_fps;
    ctx.format = *output_format;
    ReadYuvColorDescription(currentType.get(), ctx.matrix, ctx.range);
    ctx.native_fourcc = SubtypeFourCc(native_subtype);
    return ctx;
}

} // namespace

// ---------------------------------------------------------------------------
// Native webcam format selection — pure, MF-call-free
// ---------------------------------------------------------------------------

int SelectBestWebcamNativeFormat(const std::vector<WebcamNativeFormat>& formats, int want_w, int want_h,
                                 int want_fps) noexcept {
    const std::vector<int> ranked = RankWebcamNativeFormats(formats, want_w, want_h, want_fps);
    return ranked.empty() ? -1 : ranked.front();
}

std::vector<int> RankWebcamNativeFormats(const std::vector<WebcamNativeFormat>& formats, int want_w, int want_h,
                                         int want_fps) noexcept {
    struct Candidate {
        int index;
        long long distance;
        WebcamNativeEncoding encoding;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(formats.size());
    for (size_t i = 0; i < formats.size(); ++i) {
        const WebcamNativeFormat& f = formats[i];
        if (f.width != want_w || f.height != want_h)
            continue;

        long long distance = 0;
        if (want_fps > 0) {
            // Compare in milli-fps units so integer division loses no precision
            // (e.g. 30000/1001 ~= 29.97 fps must stay distinguishable from 30 fps).
            const long long den = (f.fps_den > 0) ? f.fps_den : 1;
            const long long native_milli_fps = (static_cast<long long>(f.fps_num) * 1000) / den;
            const long long want_milli_fps = static_cast<long long>(want_fps) * 1000;
            const long long diff = native_milli_fps - want_milli_fps;
            distance = (diff < 0) ? -diff : diff;
        }
        // want_fps <= 0: every candidate's distance stays 0, so the stable
        // sort below leaves them in original enumeration order (first
        // width/height match first) -- "no frame-rate preference".
        candidates.push_back({static_cast<int>(i), distance, f.encoding});
    }

    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.distance != b.distance)
            return a.distance < b.distance;
        return static_cast<int>(a.encoding) < static_cast<int>(b.encoding);
    });

    std::vector<int> result;
    result.reserve(candidates.size());
    for (const auto& c : candidates)
        result.push_back(c.index);
    return result;
}

bool PackWebcamFrame(exosnap::engine::WebcamPixelFormat format, int width, int height, const uint8_t* scan0, long pitch,
                     std::vector<uint8_t>& out) {
    using exosnap::engine::WebcamPixelFormat;
    const size_t total = exosnap::engine::WebcamFramePayloadBytes(format, width, height);
    if (total == 0 || scan0 == nullptr)
        return false;
    const size_t row_bytes = static_cast<size_t>(width) * (format == WebcamPixelFormat::Bgra   ? 4u
                                                           : format == WebcamPixelFormat::Yuy2 ? 2u
                                                                                               : 1u);
    const long long abs_pitch = pitch < 0 ? -static_cast<long long>(pitch) : pitch;
    if (static_cast<unsigned long long>(abs_pitch) < row_bytes)
        return false;
    if (pitch < 0 && format != WebcamPixelFormat::Bgra)
        return false;
    // NV12's chroma plane: height / 2 rows of `width` interleaved bytes.
    const int rows = format == WebcamPixelFormat::Nv12 ? height + height / 2 : height;
    out.resize(total);
    for (int row = 0; row < rows; ++row) {
        std::memcpy(out.data() + static_cast<size_t>(row) * row_bytes, scan0 + static_cast<long long>(row) * pitch,
                    row_bytes);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Webcam read-result classification (loss detection) — pure, MF-call-free
// ---------------------------------------------------------------------------

WebcamReadAction ClassifyWebcamReadResult(HRESULT hr, DWORD reader_flags, bool has_sample) noexcept {
    if (FAILED(hr)) {
        // ReadSample itself failed (e.g. MF_E_HW_MFT_FAILED_START_STREAMING /
        // device invalidated): the reader is unusable — reopen the device.
        return WebcamReadAction::Reconnect;
    }
    if (reader_flags & MF_SOURCE_READERF_ERROR) {
        // Reader signalled a fatal stream error (device removed mid-stream).
        return WebcamReadAction::Reconnect;
    }
    if (reader_flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        // A live capture stream ending means the device went away; try to reopen.
        return WebcamReadAction::Reconnect;
    }
    if (!has_sample) {
        // Healthy read with no payload (streaming tick / spurious wake): hold the
        // last frame and read again.
        return WebcamReadAction::Skip;
    }
    return WebcamReadAction::Deliver;
}

// Returns true if a freshly-read sample timestamp should be delivered — see the
// header comment for the full rationale (drops stale frames replayed by a
// reopened MF reader).
bool ShouldDeliverWebcamSample(long long last_delivered_100ns, long long sample_100ns) noexcept {
    if (sample_100ns <= 0)
        return true;
    if (last_delivered_100ns < 0)
        return true;
    return sample_100ns > last_delivered_100ns;
}

bool WaitForWebcamReconnect(std::stop_token stop, std::chrono::milliseconds delay) {
    std::mutex mutex;
    std::condition_variable_any wake;
    std::unique_lock lock(mutex);
    wake.wait_for(lock, stop, delay, [] { return false; });
    return !stop.stop_requested();
}

// ---------------------------------------------------------------------------
// Webcam device selection / preview policy — pure, MF-call-free
// ---------------------------------------------------------------------------

bool ShouldEnableWebcamToggle(bool has_device, bool transport_locked) noexcept {
    return has_device && !transport_locked;
}

bool ShouldOpenWebcamPreview(bool webcam_enabled, bool has_device) noexcept {
    return webcam_enabled && has_device;
}

std::string ResolveWebcamDeviceId(const std::string& configured_id, const std::vector<WebcamDeviceInfo>& devices) {
    if (!configured_id.empty())
        return configured_id;
    if (!devices.empty())
        return devices.front().id;
    return {};
}

QString FriendlyWebcamOpenFailure(const QString& raw_reason) {
    // No compatible pixel format: the camera exposes neither a BGRA nor a YUY2
    // output type the reader can convert (the NV12-only-camera failure class).
    if (raw_reason.contains(QStringLiteral("no_usable_output_type"), Qt::CaseInsensitive))
        return QStringLiteral("Camera doesn't offer a compatible video format");

    // Device busy / already opened by another application. Media Foundation
    // surfaces this as a sharing-violation / busy HRESULT while activating the
    // media source or starting the reader; the raw reason embeds it as "hr=0x…".
    static constexpr const char* kBusyHresults[] = {
        "0x80070020", // HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) — camera opened elsewhere
        "0x800700AA", // HRESULT_FROM_WIN32(ERROR_BUSY)
        "0xC00D3704", // MF_E_HW_MFT_FAILED_START_STREAMING — capture hardware already streaming
    };
    for (const char* code : kBusyHresults) {
        if (raw_reason.contains(QLatin1String(code), Qt::CaseInsensitive))
            return QStringLiteral("Camera is in use by another application");
    }

    // Unknown reason: hand back the raw string so the caller shows it verbatim.
    return raw_reason;
}

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

WebcamDeviceProbe WebcamService::ProbeDevice(const std::string& device_id) {
    if (!IsMfPresent())
        return {};
    // Callers run this on worker threads that have not joined COM.
    exosnap::engine::ComApartment com_apartment(COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);

    winrt::com_ptr<IMFAttributes> attrs;
    MFCreateAttributes(attrs.put(), 1);
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    WebcamDeviceProbe result;

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
            if (!device_id.empty() && WcharToString(sym.p) == device_id) {
                selected = devices[i];
                selected->AddRef();
                break;
            }
        }
    }
    // No silent devices[0] fallback: an empty/unmatched device_id opens nothing.
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
                        for (const auto& f : result.formats)
                            if (f.width == w && f.height == h && f.fps_num == fn && f.fps_den == fd) {
                                dup = true;
                                break;
                            }
                        if (!dup)
                            result.formats.push_back({w, h, fn, fd});
                    }
                }
            }
            result.controls = QueryControls(source.get());
            // Releases the camera for the capture reader; a source left to its
            // final Release keeps the device open until COM gets round to it.
            source->Shutdown();
        }
        selected->Release();
    }

    MFShutdown();
    return result;
}

std::vector<WebcamFormat> WebcamService::EnumerateFormats(const std::string& device_id) {
    return ProbeDevice(device_id).formats;
}

void WebcamService::SetFrameCallback(FrameCallback cb) {
    auto delivery = std::make_shared<FrameDelivery>();
    delivery->callback = std::move(cb);
    std::lock_guard lk(delivery_mutex_);
    frame_delivery_ = std::move(delivery);
}

void WebcamService::SetFrameCallback(QObject* receiver, FrameCallback cb) {
    auto delivery = std::make_shared<FrameDelivery>();
    delivery->callback = std::move(cb);
    delivery->receiver = receiver;
    delivery->receiver_bound = true;
    std::lock_guard lk(delivery_mutex_);
    frame_delivery_ = std::move(delivery);
}

void WebcamService::SetStatusCallback(QObject* receiver, StatusCallback cb) {
    auto delivery = std::make_shared<StatusDelivery>();
    delivery->callback = std::move(cb);
    delivery->receiver = receiver;
    std::lock_guard lk(delivery_mutex_);
    status_delivery_ = std::move(delivery);
}

bool WebcamService::Start(const std::string& device_id, int width, int height, int fps) {
    std::lock_guard lk(control_mutex_);
    StopLocked();
    running_.store(true);
    thread_ = std::jthread([this, device_id, width, height, fps](std::stop_token st) {
        ThreadMain(device_id, width, height, fps, std::move(st));
    });
    return true;
}

void WebcamService::Stop() {
    std::lock_guard lk(control_mutex_);
    StopLocked();
}

void WebcamService::StopLocked() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
    running_.store(false);
    {
        std::lock_guard lk(frame_mutex_);
        const auto metrics = frame_mailbox_.ReadMetrics();
        if (metrics.generations > 0) {
            diagnostics::logEvent(diagnostics::LogSeverity::Info, "webcam", "webcam.frame_copies",
                                  {{"generations", std::to_string(metrics.generations)},
                                   {"snapshots", std::to_string(metrics.snapshots)},
                                   {"legacy_copies", std::to_string(metrics.legacy_copies)},
                                   {"payload_bytes", std::to_string(metrics.payload_bytes)},
                                   {"buffers_created", std::to_string(metrics.buffers_created)}});
        }
        frame_mailbox_.Reset();
    }
}

bool WebcamService::IsRunning() const noexcept {
    return running_.load();
}

bool WebcamService::TryGetFrame(int& out_width, int& out_height, std::vector<uint8_t>& out_bgra,
                                uint64_t& out_generation) {
    std::lock_guard lk(frame_mutex_);
    return frame_mailbox_.Copy(out_width, out_height, out_bgra, out_generation);
}

std::shared_ptr<const exosnap::engine::WebcamFrameSnapshot> WebcamService::Snapshot() {
    std::lock_guard lk(frame_mutex_);
    return frame_mailbox_.Snapshot();
}

void WebcamService::ThreadMain(const std::string& device_id, int width, int height, int fps, std::stop_token stop) {
    if (!IsMfPresent()) {
        diagnostics::logEvent(diagnostics::LogSeverity::Warning, "webcam", "webcam.mf_not_present");
        running_.store(false);
        return;
    }
    // Both guards release in reverse declaration order at every exit path of
    // this function (return via the outer while loop finishing): mfSession's
    // destructor only calls MFShutdown() if MFStartup() actually succeeded,
    // and comApartment's destructor only calls CoUninitialize() if this call
    // owned the reference (S_OK/S_FALSE) -- never on RPC_E_CHANGED_MODE.
    exosnap::engine::ComApartment comApartment(COINIT_MULTITHREADED);
    if (!comApartment.usable()) {
        diagnostics::logEvent(diagnostics::LogSeverity::Warning, "webcam", "webcam.com_init_failed");
        running_.store(false);
        return;
    }
    exosnap::engine::MfSession mfSession;
    if (!mfSession.usable()) {
        diagnostics::logEvent(diagnostics::LogSeverity::Warning, "webcam", "webcam.mf_startup_failed");
        running_.store(false);
        return;
    }

    // How long to wait before re-opening the reader after a device loss (or a
    // failed initial open). The retry is UNBOUNDED: a webcam unplugged mid-recording
    // keeps its last frame served by TryGetFrame (frozen) and resumes live if it
    // returns, rather than vanishing — the user decides when to stop. Mirrors the
    // DXGI OD in-place recovery. Only Stop()/stop_token ends the loop.
    constexpr auto kReconnectDelay = std::chrono::milliseconds{500};

    // Timestamp of the last sample accepted as fresh (MF 100ns units; see
    // ShouldDeliverWebcamSample). Reset to "no frame yet" on every (re)open below
    // because a new MF source restarts its own timeline — its first sample must
    // always be accepted even if its timestamp is smaller than the old reader's.
    long long last_delivered_ts = -1;

    // Streak state for the open-reader transition log and status callback: the
    // failed/succeeded events (and PostStatus emits) fire once per streak, on the
    // first failed open and the recovery to a successful open — never on every
    // 500ms retry, which would spam engine.jsonl. first_open makes the very first
    // successful open of a run a recovery transition too (a harmless ok=true).
    bool open_failure_logged = false;
    bool first_open = true;

    // Outer loop: (re)open the device and drain it; on loss, fall back here to poll
    // a reopen. The last stored frame is NEVER cleared here, so it stays frozen for
    // the whole gap; has_frame_ is reset only by Stop().
    while (!stop.stop_requested()) {
        std::string fail_reason;
        auto ctx = OpenReader(device_id, width, height, fps, &fail_reason);
        if (!ctx) {
            if (!open_failure_logged) {
                diagnostics::logEvent(
                    diagnostics::LogSeverity::Warning, "webcam", "webcam.open_reader.failed",
                    {{"device_id_empty", device_id.empty() ? "true" : "false"}, {"reason", fail_reason}});
                open_failure_logged = true;
                PostStatus(false, QString::fromStdString(fail_reason));
            }
            // Device not (yet) available: hold whatever frame we have and retry.
            WaitForWebcamReconnect(stop, kReconnectDelay);
            continue;
        }
        const bool recovered = open_failure_logged || first_open;
        open_failure_logged = false;
        first_open = false;
        diagnostics::logEvent(
            diagnostics::LogSeverity::Info, "webcam", "webcam.open_reader.succeeded",
            {{"negotiated_width", std::to_string(ctx->width)},
             {"negotiated_height", std::to_string(ctx->height)},
             {"negotiated_fps", std::to_string(ctx->fps)},
             {"native_fourcc", ctx->native_fourcc},
             {"output_format", PixelFormatName(ctx->format)},
             {"yuv_matrix", ctx->matrix == exosnap::engine::MatrixCoefficients::Bt709 ? "bt709" : "bt601"},
             {"yuv_range", ctx->range == exosnap::engine::ColorRange::Full ? "full" : "limited"}});
        if (recovered)
            PostStatus(true, {});
        last_delivered_ts = -1; // fresh reader: its first sample always passes.

        const WebcamFrameLayout layout{ctx->width, ctx->height, ctx->format, ctx->matrix, ctx->range};
        const size_t expected = exosnap::engine::WebcamFramePayloadBytes(layout.format, layout.width, layout.height);
        // Only for buffers whose rows are padded; a tightly packed buffer is
        // published straight from the locked memory.
        std::vector<uint8_t> repacked;

        {
            std::string supported;
            for (const WebcamControlRange& range : ctx->control_ranges) {
                if (!supported.empty())
                    supported += ',';
                supported += WebcamControlKey(range.control);
            }
            diagnostics::logEvent(diagnostics::LogSeverity::Info, "webcam", "webcam.controls",
                                  {{"supported", supported.empty() ? std::string("none") : supported}});
        }
        // A freshly opened device holds whatever it held before; every control
        // the user set is applied again.
        std::vector<WebcamControlSetting> applied_controls;
        uint64_t applied_controls_generation = 0;

        // Inner loop: drain frames until the reader dies or a stop is requested.
        bool reader_lost = false;
        while (!stop.stop_requested() && !reader_lost) {
            // Between reads, on the thread that owns the source, so a control
            // change never races the reader.
            if (const uint64_t wanted = controls_generation_.load(); wanted != applied_controls_generation) {
                applied_controls_generation = wanted;
                std::vector<WebcamControlSetting> desired;
                {
                    std::lock_guard lk(controls_mutex_);
                    desired = controls_;
                }
                ApplyControlChanges(ctx->source.get(), ctx->control_ranges, applied_controls, desired);
                applied_controls = std::move(desired);
            }

            DWORD flags = 0;
            LONGLONG ts = 0;
            winrt::com_ptr<IMFSample> sample;
            HRESULT hr = ctx->reader->ReadSample(kFirstVideoStream, 0, nullptr, nullptr, nullptr, nullptr);
            std::optional<MediaEventType> failed_event;
            if (SUCCEEDED(hr)) {
                WebcamReadCallback::Sample next;
                if (!ctx->callback->Wait(stop, next))
                    break;
                hr = next.status;
                flags = next.flags;
                ts = next.timestamp;
                sample.attach(next.sample.Detach());
                failed_event = next.event;
            }

            switch (ClassifyWebcamReadResult(hr, flags, sample != nullptr)) {
            case WebcamReadAction::Reconnect:
                // Reader dead — drop it and let the outer loop poll a reopen.
                diagnostics::logEvent(
                    diagnostics::LogSeverity::Warning, "webcam", "webcam.reader_lost",
                    {{"hresult", std::format("0x{:08X}", static_cast<std::uint32_t>(hr))},
                     {"flags", std::format("0x{:X}", flags)},
                     {"source", failed_event ? "event_" + std::to_string(*failed_event) : std::string("read")}});
                reader_lost = true;
                continue;
            case WebcamReadAction::Skip:
                // No sample this read but the stream is healthy: keep the last frame.
                continue;
            case WebcamReadAction::Deliver:
                break; // fall through to store the sample below
            }

            if (!ShouldDeliverWebcamSample(last_delivered_ts, ts))
                continue; // stale frame replayed by a reopened reader — drop it
            last_delivered_ts = ts;

            winrt::com_ptr<IMFMediaBuffer> buf;
            if (FAILED(sample->ConvertToContiguousBuffer(buf.put())))
                continue;

            // Every path trusts the locked buffer only as far as it says it goes.
            // The media type promises W x H, but a driver can hand back a shorter
            // sample (partial frame, stride quirk, a renegotiation in flight), and
            // reading the promised size out of it walks off the end of MF-owned
            // memory on this thread, every frame, with no recovery. A short frame
            // keeps the previous one instead.
            uint64_t generation = 0;
            if (const auto buffer_2d = buf.try_as<IMF2DBuffer2>()) {
                BYTE* scan0 = nullptr;
                LONG pitch = 0;
                BYTE* start = nullptr;
                DWORD length = 0;
                if (FAILED(buffer_2d->Lock2DSize(MF2DBuffer_LockFlags_Read, &scan0, &pitch, &start, &length)))
                    continue;
                const size_t rows = layout.format == exosnap::engine::WebcamPixelFormat::Nv12
                                        ? static_cast<size_t>(layout.height) * 3u / 2u
                                        : static_cast<size_t>(layout.height);
                const long long abs_pitch = pitch < 0 ? -static_cast<long long>(pitch) : pitch;
                const bool fits = static_cast<unsigned long long>(abs_pitch) * rows <= length;
                if (fits && static_cast<size_t>(pitch) * rows == expected && pitch > 0) {
                    generation = StoreFrame(layout, scan0, expected);
                } else if (fits &&
                           PackWebcamFrame(layout.format, layout.width, layout.height, scan0, pitch, repacked)) {
                    generation = StoreFrame(layout, repacked.data(), repacked.size());
                }
                buffer_2d->Unlock2D();
            } else {
                BYTE* src = nullptr;
                DWORD max_length = 0;
                DWORD current_length = 0;
                if (FAILED(buf->Lock(&src, &max_length, &current_length)))
                    continue;
                if (current_length >= expected)
                    generation = StoreFrame(layout, src, expected);
                buf->Unlock();
            }
            if (generation != 0)
                PostFrame(generation);
        }

        // Reader released as ctx goes out of scope. Wait before reopening so a
        // permanently-gone device does not spin.
        if (!WaitForWebcamReconnect(stop, kReconnectDelay))
            break;
    }

    running_.store(false);
}

void WebcamService::SetControls(std::vector<WebcamControlSetting> controls) {
    controls = NormalizeWebcamControlSettings(std::move(controls));
    {
        std::lock_guard lk(controls_mutex_);
        if (controls == controls_)
            return;
        controls_ = std::move(controls);
    }
    controls_generation_.fetch_add(1);
}

uint64_t WebcamService::StoreFrame(const WebcamFrameLayout& layout, const uint8_t* data, size_t size) {
    std::lock_guard lk(frame_mutex_);
    const uint64_t generation = ++frame_generation_;
    frame_mailbox_.Publish(layout, data, size, generation);
    return generation;
}

void WebcamService::PostFrame(uint64_t generation) {
    // Runs on the MF capture thread. Everything it needs is taken as one
    // immutable snapshot under the lock; the lock is released before anything
    // is posted, and no user code ever runs under it.
    std::shared_ptr<const FrameDelivery> delivery;
    {
        std::lock_guard lk(delivery_mutex_);
        delivery = frame_delivery_;
    }
    if (!delivery || !delivery->callback)
        return;

    // The receiver is unconditionally the application object -- a QObject that
    // outlives every registration -- and never the bound receiver: testing a
    // QPointer here and posting to what it returned raced the receiver's
    // destructor, which runs on the GUI thread. The QPointer is read below
    // instead, on the thread that owns the answer, where it cannot change under
    // the check. (Same shape as RecordPreviewAdapter's handle sink.)
    QObject* target = QCoreApplication::instance();
    if (target == nullptr)
        return;
    QMetaObject::invokeMethod(
        target,
        [delivery, generation]() {
            if (delivery->receiver_bound && delivery->receiver.isNull())
                return;
            delivery->callback(generation);
        },
        Qt::QueuedConnection);
}

void WebcamService::PostStatus(bool ok, QString reason) {
    std::shared_ptr<const StatusDelivery> delivery;
    {
        std::lock_guard lk(delivery_mutex_);
        delivery = status_delivery_;
    }
    if (!delivery || !delivery->callback)
        return;

    QObject* target = QCoreApplication::instance();
    if (target == nullptr)
        return;
    QMetaObject::invokeMethod(
        target,
        [delivery, ok, reason = std::move(reason)]() mutable {
            // Status delivery is always receiver-scoped: a receiver already gone
            // means there is nothing to deliver to, so drop the transition.
            if (delivery->receiver.isNull())
                return;
            delivery->callback(ok, std::move(reason));
        },
        Qt::QueuedConnection);
}

} // namespace exosnap
