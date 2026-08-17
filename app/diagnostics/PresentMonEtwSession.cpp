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

#include <QtCore/QtGlobal> // qWarning -> the installed Qt handler -> exosnap.log

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
    // Fresh accounting per session so a Stop()->Start() cycle does not carry over a prior
    // session's discarded/flip totals. SetTargetProcessId resets it again per recording.
    accumulator_.Reset();
    auto signal = std::make_shared<FinishSignal>();
    {
        std::lock_guard lk(sample_mutex_);
        impl_ = s; // shared_ptr<SessionImpl> -> shared_ptr<void>
        finish_ = signal;
    }
    open_.store(true, std::memory_order_release);
    // The thread body captures ONLY shared owners -- never `this`. That is what makes
    // the detach path in Stop() safe: a consumer that outlives this object still has
    // everything it touches alive in its own hands.
    worker_ = std::thread([sp = std::static_pointer_cast<void>(s), signal]() {
        auto* impl = static_cast<SessionImpl*>(sp.get());
        if (impl != nullptr) {
            // ProcessTrace blocks, routing events into impl->pm (TraceSession set up
            // the callback), until TraceSession::Stop() -> CloseTrace unblocks it.
            // The captured sp keeps SessionImpl alive for the whole call.
            ::ProcessTrace(&impl->session.mTraceHandle, 1, nullptr, nullptr);
        }
        // Published BEFORE `finished`, and unconditionally: ProcessTrace returning is
        // the end of consumption regardless of whether Stop() asked for it. This is
        // the only writer that can tell the session a trace died on its own.
        signal->consuming.store(false, std::memory_order_release);
        if (!signal->stop_requested.load(std::memory_order_acquire)) {
            // Nobody asked. Another process stopped the named session, a driver reset
            // tore it down, or ETW hit a buffer error -- and present diagnostics are
            // now unavailable for the rest of this run. Until IsOpen() consulted this
            // flag, the same event silently froze the last sample as "current".
            qWarning("[presentmon] the ETW trace ended without a stop request; "
                     "present diagnostics are unavailable until the session is restarted");
        }
        {
            std::lock_guard lk(signal->mutex);
            signal->finished = true;
        }
        signal->cv.notify_all();
    });
    return true;
}

void PresentMonEtwSession::SetTargetProcessId(unsigned long pid) {
    target_pid_.store(pid, std::memory_order_relaxed);
    accumulator_.Reset();
    last_present_qpc_ = 0;
    std::lock_guard lk(sample_mutex_);
    latest_ = PresentSample{};
    target_death_logged_ = false;
    if (target_handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(target_handle_));
        target_handle_ = nullptr;
    }
    if (pid != 0) {
        // SYNCHRONIZE only: this is a liveness question, never an inspection.
        target_handle_ = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    }
}

bool PresentMonEtwSession::TargetAlive() const {
    std::lock_guard lk(sample_mutex_);
    if (target_pid_.load(std::memory_order_relaxed) == 0) {
        return true;
    }
    if (target_handle_ == nullptr) {
        // The process could not be opened at all -- most often because it is more
        // privileged than this one. That is UNKNOWN, and unknown must not read as
        // dead: blanking present diagnostics for a program that is plainly running
        // would be the same lie, pointed the other way.
        return true;
    }
    if (::WaitForSingleObject(static_cast<HANDLE>(target_handle_), 0) == WAIT_TIMEOUT) {
        return true;
    }
    if (!target_death_logged_) {
        target_death_logged_ = true;
        qWarning("[presentmon] the attributed process (pid %lu) exited; present diagnostics stop here "
                 "rather than keep reporting its last totals",
                 target_pid_.load(std::memory_order_relaxed));
    }
    return false;
}

bool PresentMonEtwSession::IsOpen() const {
    if (!open_.load(std::memory_order_acquire)) {
        return false;
    }
    std::shared_ptr<FinishSignal> signal;
    {
        std::lock_guard lk(sample_mutex_);
        signal = finish_;
    }
    return signal && signal->consuming.load(std::memory_order_acquire);
}

PresentSample PresentMonEtwSession::Latest() const {
    // A consumer that has stopped has no CURRENT present to report, and the last one
    // it saw is not a substitute -- that is precisely the sample that would sit on the
    // Diagnostics page describing a trace that ended minutes ago.
    if (!IsOpen()) {
        return PresentSample{};
    }
    // The presenter this session is attributed to has exited. Its totals describe a
    // source that no longer exists, and the filter guarantees no further present will
    // ever replace them -- so without this the last frame a closed game rendered would
    // sit on the Diagnostics page as the current present mode for the rest of the run.
    // Both checks run BEFORE sample_mutex_ is taken: each acquires it itself, and the
    // mutex is not recursive.
    if (!TargetAlive()) {
        std::lock_guard lk(sample_mutex_);
        latest_ = PresentSample{};
        return latest_;
    }
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
            PresentSample mapped = MapPresentEvent(raw);
            // ADR 0033 extra-checks: fold into the per-recording aggregates. The accumulator
            // counts flips on the CLASSIFIED mode so sub-variant changes (e.g. Composed_Flip
            // -> Composed_Copy) do not register as a present-mode flip.
            accumulator_.Observe(mapped.mode, p->FinalState == PresentResult::Discarded);
            mapped.present_count = static_cast<uint32_t>(accumulator_.present_count);
            mapped.discarded_count = static_cast<uint32_t>(accumulator_.discarded_count);
            mapped.mode_flip_count = static_cast<uint32_t>(accumulator_.mode_flip_count);
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
    std::shared_ptr<FinishSignal> signal;
    {
        std::lock_guard lk(sample_mutex_);
        sp = impl_;
        signal = finish_;
    }
    // Before CloseTrace, so the consumer cannot observe its own return without also
    // observing that it was asked for.
    if (signal) {
        signal->stop_requested.store(true, std::memory_order_release);
    }
    if (sp) {
        auto* s = static_cast<SessionImpl*>(sp.get());
        s->session.Stop(); // CloseTrace -> unblocks ProcessTrace
    }
    if (worker_.joinable()) {
        // BOUNDED. CloseTrace is documented to make ProcessTrace return once it has
        // drained its buffers, and in practice that takes well under a second. But
        // this wait runs while the application is shutting down, and an unbounded
        // join on an OS call means one misbehaving trace session leaves a process
        // the user asked to quit alive, with its window gone and only Task Manager
        // left to end it. A quit that does not quit is a worse failure than a thread
        // that outlives its owner, so the wait has a ceiling.
        bool finished = false;
        if (signal) {
            std::unique_lock lk(signal->mutex);
            finished = signal->cv.wait_for(lk, std::chrono::seconds(5), [&signal] { return signal->finished; });
        }
        if (finished) {
            worker_.join();
        } else {
            // Detached only after the ceiling was hit. The thread captured its own
            // owners for everything it touches, so it cannot reach this object.
            //
            // Logged because this is the one branch that trades a guarantee for a
            // deadline: from here on a thread is running that nothing will ever join,
            // and the process is a little less tidy at exit than the code claims.
            // Silent, it looks exactly like the clean path -- and the only evidence
            // that the ceiling was ever reached would be a shutdown that felt slow.
            qWarning("[presentmon] ProcessTrace did not return within 5s of CloseTrace; "
                     "detaching the consumer thread so shutdown can finish");
            worker_.detach();
        }
    }
    {
        std::lock_guard lk(sample_mutex_);
        impl_.reset();
        finish_.reset();
    }
}

PresentMonEtwSession::~PresentMonEtwSession() {
    Stop();
    // Not released in Stop(): the attribution outlives a Stop()/Start() cycle, so the
    // handle has to as well. The object's end is the only point at which it is
    // certainly no longer wanted.
    std::lock_guard lk(sample_mutex_);
    if (target_handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(target_handle_));
        target_handle_ = nullptr;
    }
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
bool PresentMonEtwSession::IsOpen() const {
    return false;
}
bool PresentMonEtwSession::TargetAlive() const {
    return true;
}
void PresentMonEtwSession::SetTargetProcessId(unsigned long pid) {
    // No session, so nothing to attribute -- but the field is still the record of what
    // was asked for, and the tests read it.
    target_pid_.store(pid, std::memory_order_relaxed);
    accumulator_.Reset();
    last_present_qpc_ = 0;
    std::lock_guard lk(sample_mutex_);
    latest_ = PresentSample{};
}
PresentSample PresentMonEtwSession::Latest() const {
    return PresentSample{};
}
} // namespace exosnap::diagnostics

#endif
