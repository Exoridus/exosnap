#include "diagnostics/DpcLatencyProvider.h"

#ifdef EXOSNAP_HAS_PRESENTMON

#define WIN32_LEAN_AND_MEAN
// clang-format off
// Order matters: evntrace.h / evntcons.h depend on TRACEHANDLE et al. from windows.h.
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
// clang-format on

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace exosnap::diagnostics {

namespace {
constexpr wchar_t kSessionName[] = L"ExoSnapDpcLatency";

// --- Well-known kernel trace GUIDs (defined locally to avoid pulling in INITGUID,
//     which would materialise every DEFINE_GUID in the SDK headers in this TU) --------
//
// SystemTraceControlGuid {9e814aad-3204-11d2-9a82-006008a86939} — the named system
// logger control GUID (Win8.1+ allows multiple named system loggers via
// EVENT_TRACE_SYSTEM_LOGGER_MODE; this is NOT the singleton NT Kernel Logger).
constexpr GUID kSystemTraceControlGuid = {0x9e814aad, 0x3204, 0x11d2, {0x9a, 0x82, 0x00, 0x60, 0x08, 0xa8, 0x69, 0x39}};
// PerfInfoGuid {ce1dbfb4-137e-4da6-87b0-3f59aa102cbc} — DPC / ISR events.
constexpr GUID kPerfInfoGuid = {0xce1dbfb4, 0x137e, 0x4da6, {0x87, 0xb0, 0x3f, 0x59, 0xaa, 0x10, 0x2c, 0xbc}};
// Image kernel-provider GUID {2cb15d1d-5fc1-11d2-abe1-00a0c911f518} — image load/unload.
constexpr GUID kImageLoadGuid = {0x2cb15d1d, 0x5fc1, 0x11d2, {0xab, 0xe1, 0x00, 0xa0, 0xc9, 0x11, 0xf5, 0x18}};

bool GuidEq(const GUID& a, const GUID& b) {
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

// Read a pointer-sized field from a raw kernel event payload. The traced kernel's
// pointer width comes from the logfile header (PointerSize); a 32-bit field on a
// 64-bit consumer is still read as 4 bytes.
uint64_t ReadPtr(const uint8_t* p, uint32_t ptr_size) {
    if (ptr_size >= 8) {
        uint64_t v = 0;
        std::memcpy(&v, p, 8);
        return v;
    }
    uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return static_cast<uint64_t>(v);
}

struct ImageRange {
    uint64_t base = 0;
    uint64_t size = 0;
    std::string name; // basename (ASCII), e.g. "nvlddmkm.sys"; empty if unresolved
};

struct SessionImpl {
    TRACEHANDLE control_handle = 0;
    TRACEHANDLE trace_handle = INVALID_PROCESSTRACE_HANDLE;
    int64_t qpc_freq = 0;
    uint32_t pointer_size = sizeof(void*);
    std::vector<uint8_t> props_storage; // EVENT_TRACE_PROPERTIES + name; reused by ControlTrace(STOP)

    std::mutex acc_mutex; // guards everything below
    double max_us = 0.0;
    double sum_us = 0.0;
    uint64_t count = 0;
    uint64_t worst_routine = 0;
    std::vector<ImageRange> images;
};

// =====================================================================================
// DECODE SECTION — fiddly, NOT headless-verifiable. Opcodes/struct offsets below are the
// documented values; runtime correctness is dev-machine-verified (recording under load).
// Uncertainties are flagged inline. Everything here is bounds-checked and degrades to an
// empty/zero reading rather than misbehaving on an unexpected layout.
// =====================================================================================

// Best-effort kernel-image basename extraction. The Image_Load FileName is a trailing
// NUL-terminated wide string whose exact offset varies by event version (V2 vs V3 differ
// in the Signature fields), so instead of hard-coding a fragile offset we scan the
// payload for the path — kernel image paths always begin with a backslash (e.g.
// "\SystemRoot\System32\drivers\nvlddmkm.sys" or "\Device\HarddiskVolumeN\...") and run
// to the buffer end. Robust to layout drift; returns "" if not found.  [DEV-VERIFY]
std::string ExtractImageBasename(const uint8_t* data, uint32_t len, uint32_t ptr_size) {
    const size_t wcount = len / sizeof(wchar_t);
    const wchar_t* w = reinterpret_cast<const wchar_t*>(data);
    // Skip at least ImageBase+ImageSize before scanning for the path.
    const size_t start = (2u * ptr_size) / sizeof(wchar_t);
    for (size_t i = start; i < wcount; ++i) {
        if (w[i] != L'\\')
            continue;
        std::wstring path;
        bool ok = true;
        for (size_t j = i; j < wcount && w[j] != L'\0'; ++j) {
            if (w[j] < 0x20 || w[j] > 0x7e) { // non-ASCII/control => not the path we want
                ok = false;
                break;
            }
            path.push_back(w[j]);
        }
        if (!ok || path.size() < 4)
            continue;
        // Reduce to the basename (component after the last backslash).
        const size_t slash = path.find_last_of(L'\\');
        const std::wstring base = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
        std::string narrow; // path is guaranteed ASCII (0x20..0x7e) by the scan above
        narrow.reserve(base.size());
        for (const wchar_t c : base)
            narrow.push_back(static_cast<char>(c));
        return narrow;
    }
    return {};
}

void WINAPI DpcEventRecordCallback(EVENT_RECORD* record) {
    auto* s = static_cast<SessionImpl*>(record->UserContext);
    if (s == nullptr || record->UserData == nullptr)
        return;
    const GUID& provider = record->EventHeader.ProviderId;
    const uint8_t opcode = record->EventHeader.EventDescriptor.Opcode;
    const auto* data = static_cast<const uint8_t*>(record->UserData);
    const uint32_t len = record->UserDataLength;
    const uint32_t ps = s->pointer_size;

    if (GuidEq(provider, kPerfInfoGuid)) {
        // DPC/ISR variants (DPC, ThreadedDPC, TimerDPC, ISR). Opcodes 66..69 [DEV-VERIFY:
        // exact opcode->variant mapping]. All of these share the same leading payload
        // layout { InitialTime: uint64 QPC; Routine: pointer }, so we treat the whole
        // band uniformly and only read that common prefix — robust to the exact opcode.
        if (opcode < 66 || opcode > 69)
            return;
        if (len < 8u + ps)
            return;
        uint64_t initial = 0;
        std::memcpy(&initial, data, 8); // InitialTime (QPC) — start of the DPC/ISR
        const uint64_t routine = ReadPtr(data + 8, ps);
        const int64_t end = record->EventHeader.TimeStamp.QuadPart; // QPC at event emit
        if (s->qpc_freq <= 0 || initial == 0 || static_cast<uint64_t>(end) < initial)
            return;
        const double us =
            static_cast<double>(end - static_cast<int64_t>(initial)) * 1e6 / static_cast<double>(s->qpc_freq);
        std::lock_guard<std::mutex> lk(s->acc_mutex);
        s->sum_us += us;
        ++s->count;
        if (us > s->max_us) {
            s->max_us = us;
            s->worst_routine = routine;
        }
        return;
    }

    if (GuidEq(provider, kImageLoadGuid)) {
        // Load=10, DCStart=3, DCEnd=4 (3/4 are rundown — images already loaded when the
        // session started). Leading payload: { ImageBase: ptr; ImageSize: ptr; ... }.
        if (opcode != 10 && opcode != 3 && opcode != 4)
            return;
        if (len < 2u * ps)
            return;
        const uint64_t base = ReadPtr(data, ps);
        const uint64_t size = ReadPtr(data + ps, ps);
        std::string name = ExtractImageBasename(data, len, ps);
        std::lock_guard<std::mutex> lk(s->acc_mutex);
        s->images.push_back(ImageRange{base, size, std::move(name)});
        return;
    }
}

// Build the EVENT_TRACE_PROPERTIES blob (struct + trailing session name) into `out`.
EVENT_TRACE_PROPERTIES* BuildProps(std::vector<uint8_t>& out) {
    const size_t name_bytes = (std::wcslen(kSessionName) + 1) * sizeof(wchar_t);
    const size_t total = sizeof(EVENT_TRACE_PROPERTIES) + name_bytes;
    out.assign(total, 0);
    auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(out.data());
    props->Wnode.BufferSize = static_cast<ULONG>(total);
    props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props->Wnode.ClientContext = 1; // 1 = QPC timestamps
    props->Wnode.Guid = kSystemTraceControlGuid;
    props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
    props->EnableFlags = EVENT_TRACE_FLAG_DPC | EVENT_TRACE_FLAG_INTERRUPT | EVENT_TRACE_FLAG_IMAGE_LOAD;
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    return props;
}

// Stop a stale named session left behind by a previously crashed instance.
void StopStaleSession() {
    std::vector<uint8_t> buf;
    EVENT_TRACE_PROPERTIES* props = BuildProps(buf);
    ::ControlTraceW(0, kSessionName, props, EVENT_TRACE_CONTROL_STOP);
}
} // namespace

DpcLatencyProvider::DpcLatencyProvider() = default;

bool DpcLatencyProvider::Start() {
    if (open_.load(std::memory_order_acquire))
        return true;
    auto s = std::make_shared<SessionImpl>();
    EVENT_TRACE_PROPERTIES* props = BuildProps(s->props_storage);

    // A stale session from a crashed prior instance would block StartTrace; clear it.
    StopStaleSession();

    ULONG st = ::StartTraceW(&s->control_handle, kSessionName, props);
    if (st == ERROR_ALREADY_EXISTS) {
        StopStaleSession();
        props = BuildProps(s->props_storage); // StartTrace mutates props; rebuild clean
        st = ::StartTraceW(&s->control_handle, kSessionName, props);
    }
    if (st != ERROR_SUCCESS) {
        // ERROR_ACCESS_DENIED when not elevated -> graceful degrade (no throw).
        return false; // s drops here; SessionImpl destroyed.
    }

    EVENT_TRACE_LOGFILEW log = {};
    log.LoggerName = const_cast<wchar_t*>(kSessionName);
    log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    log.EventRecordCallback = &DpcEventRecordCallback;
    log.Context = s.get(); // raw ptr; impl_ keeps SessionImpl alive for the worker's life
    const TRACEHANDLE trace = ::OpenTraceW(&log);
    if (trace == INVALID_PROCESSTRACE_HANDLE) {
        ::ControlTraceW(s->control_handle, nullptr, props, EVENT_TRACE_CONTROL_STOP);
        return false;
    }
    s->trace_handle = trace;
    s->qpc_freq = log.LogfileHeader.PerfFreq.QuadPart;
    if (s->qpc_freq <= 0) {
        LARGE_INTEGER f{};
        ::QueryPerformanceFrequency(&f);
        s->qpc_freq = f.QuadPart;
    }
    if (log.LogfileHeader.PointerSize != 0)
        s->pointer_size = log.LogfileHeader.PointerSize;

    {
        std::lock_guard<std::mutex> lk(impl_mutex_);
        impl_ = s; // shared_ptr<SessionImpl> -> shared_ptr<void>
    }
    open_.store(true, std::memory_order_release);
    worker_ = std::thread(&DpcLatencyProvider::ConsumeLoop, this);
    return true;
}

void DpcLatencyProvider::ConsumeLoop() {
    std::shared_ptr<void> sp;
    {
        std::lock_guard<std::mutex> lk(impl_mutex_);
        sp = impl_;
    }
    auto* s = static_cast<SessionImpl*>(sp.get());
    if (s == nullptr)
        return;
    // ProcessTrace blocks, routing events into DpcEventRecordCallback, until Stop() calls
    // CloseTrace. The snapshot sp keeps SessionImpl alive for the lifetime of this call.
    ::ProcessTrace(&s->trace_handle, 1, nullptr, nullptr);
}

DpcLatencyReading DpcLatencyProvider::Read() const {
    std::shared_ptr<void> sp;
    {
        std::lock_guard<std::mutex> lk(impl_mutex_);
        sp = impl_;
    }
    if (!sp)
        return DpcLatencyReading{};
    auto* s = static_cast<SessionImpl*>(sp.get());
    std::lock_guard<std::mutex> lk(s->acc_mutex);
    DpcLatencyReading r;
    r.available = s->count > 0;
    r.max_latency_us = s->max_us;
    r.avg_latency_us = s->count != 0 ? s->sum_us / static_cast<double>(s->count) : 0.0;
    if (s->worst_routine != 0) {
        for (const auto& img : s->images) {
            if (img.size != 0 && s->worst_routine >= img.base && s->worst_routine < img.base + img.size) {
                r.worst_driver = img.name; // may be empty if the path was unresolved
                break;
            }
        }
    }
    return r;
}

void DpcLatencyProvider::Stop() {
    if (!open_.exchange(false))
        return;
    std::shared_ptr<void> sp;
    {
        std::lock_guard<std::mutex> lk(impl_mutex_);
        sp = impl_;
    }
    if (sp) {
        auto* s = static_cast<SessionImpl*>(sp.get());
        if (s->control_handle != 0) {
            auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(s->props_storage.data());
            ::ControlTraceW(s->control_handle, nullptr, props, EVENT_TRACE_CONTROL_STOP);
        }
        if (s->trace_handle != INVALID_PROCESSTRACE_HANDLE) {
            ::CloseTrace(s->trace_handle); // unblocks ProcessTrace
        }
    }
    if (worker_.joinable())
        worker_.join();
    {
        std::lock_guard<std::mutex> lk(impl_mutex_);
        impl_.reset();
    }
}

DpcLatencyProvider::~DpcLatencyProvider() {
    Stop();
}

} // namespace exosnap::diagnostics

#else // !EXOSNAP_HAS_PRESENTMON — graceful no-op build

namespace exosnap::diagnostics {
DpcLatencyProvider::DpcLatencyProvider() = default;
DpcLatencyProvider::~DpcLatencyProvider() = default;
bool DpcLatencyProvider::Start() {
    return false;
}
void DpcLatencyProvider::Stop() {
}
DpcLatencyReading DpcLatencyProvider::Read() const {
    return DpcLatencyReading{};
}
} // namespace exosnap::diagnostics

#endif
