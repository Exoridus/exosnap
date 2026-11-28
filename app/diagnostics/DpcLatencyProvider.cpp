#include "diagnostics/DpcLatencyProvider.h"

#ifdef EXOSNAP_HAS_PRESENTMON

#include "diagnostics/DpcTraceSample.h"

#define WIN32_LEAN_AND_MEAN
// clang-format off
// Order matters: evntrace.h / evntcons.h depend on TRACEHANDLE et al. from windows.h.
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
// clang-format on

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace exosnap::diagnostics {

namespace {
// --- Well-known kernel trace GUIDs (defined locally to avoid pulling in INITGUID,
//     which would materialise every DEFINE_GUID in the SDK headers in this TU) --------
//
// PerfInfoGuid {ce1dbfb4-137e-4da6-87b0-3f59aa102cbc} — DPC / ISR events.
constexpr GUID kPerfInfoGuid = {0xce1dbfb4, 0x137e, 0x4da6, {0x87, 0xb0, 0x3f, 0x59, 0xaa, 0x10, 0x2c, 0xbc}};
// Image kernel-provider GUID {2cb15d1d-5fc1-11d2-abe1-00a0c911f518} — image load/unload.
constexpr GUID kImageLoadGuid = {0x2cb15d1d, 0x5fc1, 0x11d2, {0xab, 0xe1, 0x00, 0xa0, 0xc9, 0x11, 0xf5, 0x18}};

bool GuidEq(const GUID& a, const GUID& b) {
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

struct ImageRange {
    uint64_t base = 0;
    uint64_t size = 0;
    std::string name; // basename (ASCII), e.g. "nvlddmkm.sys"; empty if unresolved
};

struct SessionImpl {
    std::wstring session_name;
    TRACEHANDLE control_handle = 0;
    TRACEHANDLE trace_handle = INVALID_PROCESSTRACE_HANDLE;
    int64_t qpc_freq = 0;
    uint32_t pointer_size = sizeof(void*);
    std::vector<uint8_t> props_storage; // EVENT_TRACE_PROPERTIES + name; reused by ControlTrace(STOP)
    // Set by the worker when it enters ProcessTrace and cleared when ProcessTrace
    // returns, for whatever reason it returned. Starts false on purpose: an open trace
    // nobody is consuming yet has measured nothing, and has nothing to report.
    // Lives in SessionImpl rather than in the provider because the worker holds the
    // shared_ptr for the whole call, so writing it after ProcessTrace can never touch
    // a destroyed object.
    std::atomic<bool> consuming{false};
    // The worker's "I have left ProcessTrace" handshake, so Stop() can wait with a
    // deadline instead of joining forever. It lives in SessionImpl, which the worker
    // itself holds a shared_ptr to, precisely so that a thread abandoned after the
    // deadline still owns everything it may touch.
    std::mutex finish_mutex;
    std::condition_variable finish_cv;
    bool finished = false; // guarded by finish_mutex

    std::mutex acc_mutex; // guards everything below
    std::chrono::steady_clock::time_point observed_at{};
    double max_us = 0.0;
    double sum_us = 0.0;
    uint64_t count = 0;
    uint64_t worst_routine = 0;
    std::vector<ImageRange> images;
};

// =====================================================================================
// Kernel image attribution is best effort. Keep the latency sample when the driver name is unavailable.
// =====================================================================================

// Best-effort kernel-image basename extraction. The Image_Load FileName is a trailing
// NUL-terminated wide string whose exact offset varies by event version (V2 vs V3 differ
// in the Signature fields), so instead of hard-coding a fragile offset we scan the
// payload for the path — kernel image paths always begin with a backslash (e.g.
// "\SystemRoot\System32\drivers\nvlddmkm.sys" or "\Device\HarddiskVolumeN\...") and run
// to the buffer end. Robust to layout drift; returns "" if not found.  [DEV-VERIFY]
std::string ExtractImageBasename(const uint8_t* data, uint32_t len, uint32_t ptr_size) {
    const size_t wcount = len / sizeof(wchar_t);
    const auto read_char = [data](size_t index) {
        wchar_t value{};
        std::memcpy(&value, data + index * sizeof(value), sizeof(value));
        return value;
    };
    // Skip at least ImageBase+ImageSize before scanning for the path.
    const size_t start = (2u * ptr_size) / sizeof(wchar_t);
    for (size_t i = start; i < wcount; ++i) {
        if (read_char(i) != L'\\')
            continue;
        std::wstring path;
        bool ok = true;
        bool terminated = false;
        for (size_t j = i; j < wcount && j - i < 1024; ++j) {
            const auto ch = read_char(j);
            if (ch == L'\0') {
                terminated = true;
                break;
            }
            if (ch < 0x20 || ch > 0x7e) {
                ok = false;
                break;
            }
            path.push_back(ch);
        }
        if (!ok || !terminated || path.size() < 4)
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
        const auto sample =
            DecodeDpcTraceSample(std::span(data, len), opcode, ps, record->EventHeader.TimeStamp.QuadPart, s->qpc_freq);
        if (!sample)
            return;
        std::lock_guard<std::mutex> lk(s->acc_mutex);
        s->sum_us += sample->elapsed_us;
        ++s->count;
        s->observed_at = std::chrono::steady_clock::now();
        if (sample->elapsed_us > s->max_us) {
            s->max_us = sample->elapsed_us;
            s->worst_routine = sample->routine;
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
        const uint64_t base = ReadTracePointer(data, ps);
        const uint64_t size = ReadTracePointer(data + ps, ps);
        std::string name = ExtractImageBasename(data, len, ps);
        std::lock_guard<std::mutex> lk(s->acc_mutex);
        if (s->images.size() < 4096)
            s->images.push_back(ImageRange{base, size, std::move(name)});
        return;
    }
}

// Build the EVENT_TRACE_PROPERTIES blob (struct + trailing session name) into `out`.
EVENT_TRACE_PROPERTIES* BuildProps(std::vector<uint8_t>& out, const wchar_t* session_name) {
    const size_t name_bytes = (std::wcslen(session_name) + 1) * sizeof(wchar_t);
    const size_t total = sizeof(EVENT_TRACE_PROPERTIES) + name_bytes;
    out.assign(total, 0);
    auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(out.data());
    props->Wnode.BufferSize = static_cast<ULONG>(total);
    props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props->Wnode.ClientContext = 1; // 1 = QPC timestamps
    props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
    props->EnableFlags = EVENT_TRACE_FLAG_DPC | EVENT_TRACE_FLAG_INTERRUPT | EVENT_TRACE_FLAG_IMAGE_LOAD;
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    return props;
}

} // namespace

DpcLatencyProvider::DpcLatencyProvider() = default;

bool DpcLatencyProvider::Start(std::wstring session_name, const GUID& session_id) {
    if (open_.load(std::memory_order_acquire))
        return true;
    auto s = std::make_shared<SessionImpl>();
    s->session_name = std::move(session_name);
    EVENT_TRACE_PROPERTIES* props = BuildProps(s->props_storage, s->session_name.c_str());
    props->Wnode.Guid = session_id;

    const ULONG st = ::StartTraceW(&s->control_handle, s->session_name.c_str(), props);
    if (st != ERROR_SUCCESS) {
        // ERROR_ACCESS_DENIED when not elevated -> graceful degrade (no throw).
        return false; // s drops here; SessionImpl destroyed.
    }

    EVENT_TRACE_LOGFILEW log = {};
    log.LoggerName = s->session_name.data();
    // ProcessTrace would otherwise convert the header timestamp to system time while InitialTime stays in QPC ticks.
    log.ProcessTraceMode =
        PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
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
    if (log.LogfileHeader.PointerSize == 4 || log.LogfileHeader.PointerSize == 8)
        s->pointer_size = log.LogfileHeader.PointerSize;

    {
        std::lock_guard<std::mutex> lk(impl_mutex_);
        impl_ = s; // shared_ptr<SessionImpl> -> shared_ptr<void>
    }
    open_.store(true, std::memory_order_release);
    worker_ = std::thread(&DpcLatencyProvider::ConsumeLoop, std::static_pointer_cast<void>(s));
    return true;
}

void DpcLatencyProvider::ConsumeLoop(std::shared_ptr<void> sp) {
    auto* s = static_cast<SessionImpl*>(sp.get());
    if (s == nullptr)
        return;
    // ProcessTrace blocks, routing events into DpcEventRecordCallback, until Stop() calls
    // CloseTrace. The snapshot sp keeps SessionImpl alive for the lifetime of this call.
    s->consuming.store(true, std::memory_order_release);
    ::ProcessTrace(&s->trace_handle, 1, nullptr, nullptr);
    s->consuming.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(s->finish_mutex);
        s->finished = true;
    }
    s->finish_cv.notify_all();
}

bool DpcLatencyProvider::IsOpen() const {
    if (!open_.load(std::memory_order_acquire))
        return false;
    std::shared_ptr<void> sp;
    {
        std::lock_guard<std::mutex> lk(impl_mutex_);
        sp = impl_;
    }
    if (!sp)
        return false;
    return static_cast<SessionImpl*>(sp.get())->consuming.load(std::memory_order_acquire);
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
    // A peak nothing is updating any more is not a measurement of this machine now, and
    // the numbers do not leave this function with it: an unavailable reading carries no
    // figure a caller could mistake for one.
    if (!s->consuming.load(std::memory_order_acquire))
        return DpcLatencyReading{};
    std::lock_guard<std::mutex> lk(s->acc_mutex);
    DpcLatencyReading r;
    r.available = s->count > 0;
    r.metadata.observed_at = s->observed_at;
    r.metadata.source = "DPC/ISR ETW";
    r.max_latency_us = s->max_us;
    r.avg_latency_us = s->count != 0 ? s->sum_us / static_cast<double>(s->count) : 0.0;
    if (s->worst_routine != 0) {
        for (const auto& img : s->images) {
            if (s->worst_routine >= img.base && s->worst_routine - img.base < img.size) {
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
    // A deadline rather than an unconditional join, and the reason is the process exit
    // path: ~DpcLatencyProvider() runs on the GUI thread, and CloseTrace only *asks*
    // ProcessTrace to return (ERROR_CTX_CLOSE_PENDING is a documented answer). Joining
    // forever would trade a hung shutdown for a guarantee nobody can see. Past the
    // ceiling the worker is abandoned instead: the thread body took its own shared_ptr
    // to SessionImpl as an argument and touches nothing else, so an abandoned worker
    // owns everything it can reach. Same trade, same reason as
    // PresentMonEtwSession::Stop().
    constexpr auto kShutdownCeiling = std::chrono::seconds(2);
    bool finished = true;
    if (sp && worker_.joinable()) {
        auto* s = static_cast<SessionImpl*>(sp.get());
        std::unique_lock<std::mutex> lk(s->finish_mutex);
        finished = s->finish_cv.wait_for(lk, kShutdownCeiling, [s]() { return s->finished; });
    }
    if (worker_.joinable()) {
        if (finished) {
            worker_.join();
        } else {
            worker_.detach();
        }
    }
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
bool DpcLatencyProvider::Start(std::wstring, const _GUID&) {
    return false;
}
void DpcLatencyProvider::Stop() {
}
bool DpcLatencyProvider::IsOpen() const {
    return false;
}
DpcLatencyReading DpcLatencyProvider::Read() const {
    return DpcLatencyReading{};
}
} // namespace exosnap::diagnostics

#endif
