#include "diagnostics/PresentMonEtwSession.h"
#include "diagnostics/PresentModeMapping.h"

#ifdef EXOSNAP_HAS_PRESENTMON

#define WIN32_LEAN_AND_MEAN
// clang-format off
// Order matters: evntrace.h depends on TRACEHANDLE et al. from windows.h.
#include <windows.h>
#include <evntrace.h>
// clang-format on
#include <vector>
// Vendored PresentMon PresentData (pinned v1.10.0):
#include "PresentMonTraceConsumer.hpp"
#include "TraceSession.hpp"

namespace exosnap::diagnostics {

namespace {
constexpr wchar_t kSessionName[] = L"ExoSnapPresentMon";

// Build a RawPresentEvent from a completed PresentEvent + the prior present's QPC and
// the session's QPC frequency. interval_ms is the inter-present gap; 0 for the first.
RawPresentEvent ToRaw(const PresentEvent& pe, uint64_t prev_qpc, int64_t qpc_freq) {
    RawPresentEvent ev;
    ev.valid = true;
    ev.present_mode_code = static_cast<int>(pe.PresentMode);
    ev.sync_interval = pe.SyncInterval;
    ev.tearing_flag = pe.SupportsTearing;
    ev.interval_ms = (prev_qpc != 0 && pe.PresentStartTime > prev_qpc && qpc_freq != 0)
                         ? static_cast<double>(pe.PresentStartTime - prev_qpc) * 1000.0 / static_cast<double>(qpc_freq)
                         : 0.0;
    return ev;
}

struct SessionImpl {
    PMTraceConsumer pm;   // default ctor: mTrackDisplay=true, mTrackGPU/Input=false
    TraceSession session; // PresentMon's session helper (open + enable + OpenTrace)
};
} // namespace

PresentMonEtwSession::PresentMonEtwSession() = default;

bool PresentMonEtwSession::Start() {
    if (open_.load(std::memory_order_acquire))
        return true;
    auto s = std::make_shared<SessionImpl>();
    // A stale session from a previous crashed instance would block Start; clear it first.
    TraceSession::StopNamedSession(kSessionName);
    // realtime: etlPath=nullptr; no WinMR: mrConsumer=nullptr.
    const ULONG st = s->session.Start(&s->pm, nullptr, nullptr, kSessionName);
    if (st != ERROR_SUCCESS) { // ERROR_ACCESS_DENIED when not elevated -> graceful degrade
        return false;          // s drops here; SessionImpl destroyed.
    }
    // Reset drain state so a Stop()->Start() cycle does not compute a bogus first
    // interval against the previous session's QPC epoch.
    last_present_qpc_ = 0;
    qpc_freq_ = 0;
    {
        std::lock_guard lk(sample_mutex_);
        impl_ = s; // shared_ptr<SessionImpl> -> shared_ptr<void>
    }
    open_.store(true, std::memory_order_release);
    worker_ = std::thread(&PresentMonEtwSession::ConsumeLoop, this);
    return true;
}

void PresentMonEtwSession::ConsumeLoop() {
    std::shared_ptr<void> sp;
    {
        std::lock_guard lk(sample_mutex_);
        sp = impl_;
    }
    auto* s = static_cast<SessionImpl*>(sp.get());
    if (s == nullptr)
        return;
    // ProcessTrace blocks, routing events into s->pm (TraceSession set up the callback),
    // until Stop() calls TraceSession::Stop() -> CloseTrace. The snapshot sp keeps
    // SessionImpl alive for the lifetime of this call.
    ::ProcessTrace(&s->session.mTraceHandle, 1, nullptr, nullptr);
}

PresentSample PresentMonEtwSession::Latest() const {
    // Snapshot impl_ under the lock, then drain on the snapshot. The held snapshot keeps
    // SessionImpl alive even if Stop() resets impl_ mid-drain (a DequeuePresentEvents
    // after session.Stop() is safe and just returns empty).
    std::shared_ptr<void> sp;
    {
        std::lock_guard lk(sample_mutex_);
        sp = impl_;
    }
    if (sp) {
        auto* s = static_cast<SessionImpl*>(sp.get());
        // mTimestampFrequency is constant for the session; read it before the drain loop
        // so the first batch computes real intervals.
        qpc_freq_ = s->session.mTimestampFrequency.QuadPart;
        // Drain on the reader side (DequeuePresentEvents is thread-safe). Keep the most
        // recent present that matches the target PID filter (0 = any non-composed-dominant).
        std::vector<std::shared_ptr<PresentEvent>> presents;
        s->pm.DequeuePresentEvents(presents);
        const unsigned long want_pid = target_pid_.load(std::memory_order_relaxed);
        for (const auto& p : presents) {
            if (want_pid != 0 && p->ProcessId != want_pid)
                continue;
            const RawPresentEvent raw = ToRaw(*p, last_present_qpc_, qpc_freq_);
            last_present_qpc_ = p->PresentStartTime;
            const PresentSample mapped = MapPresentEvent(raw);
            std::lock_guard lk(sample_mutex_);
            latest_ = mapped;
        }
    }
    std::lock_guard lk(sample_mutex_);
    return latest_;
}

void PresentMonEtwSession::Stop() {
    if (!open_.exchange(false))
        return;
    std::shared_ptr<void> sp;
    {
        std::lock_guard lk(sample_mutex_);
        sp = impl_;
    }
    if (sp) {
        auto* s = static_cast<SessionImpl*>(sp.get());
        s->session.Stop(); // CloseTrace -> unblocks ProcessTrace
    }
    if (worker_.joinable())
        worker_.join();
    {
        std::lock_guard lk(sample_mutex_);
        impl_.reset();
    }
}

PresentMonEtwSession::~PresentMonEtwSession() {
    Stop();
}

} // namespace exosnap::diagnostics

#else // !EXOSNAP_HAS_PRESENTMON — graceful no-op build

namespace exosnap::diagnostics {
PresentMonEtwSession::PresentMonEtwSession() = default;
PresentMonEtwSession::~PresentMonEtwSession() = default;
bool PresentMonEtwSession::Start() {
    return false;
}
void PresentMonEtwSession::Stop() {
}
PresentSample PresentMonEtwSession::Latest() const {
    return PresentSample{};
}
} // namespace exosnap::diagnostics

#endif
