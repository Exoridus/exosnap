#include "diagnostics/PresentMonEtwSession.h"
#include "diagnostics/PresentModeMapping.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h> // OpenProcess / WaitForSingleObject / CloseHandle -- target liveness only.

#include <utility>

#include <QtCore/QtGlobal> // qWarning -> the installed Qt handler -> exosnap.log

namespace exosnap::diagnostics {

PresentMonEtwSession::PresentMonEtwSession() : backend_factory_(&MakePresentTraceBackend) {
}

PresentMonEtwSession::PresentMonEtwSession(std::function<std::shared_ptr<IPresentTraceBackend>()> backend_factory)
    : backend_factory_(std::move(backend_factory)) {
}

bool PresentMonEtwSession::Start() {
    if (open_.load(std::memory_order_acquire))
        return true;
    if (!backend_factory_)
        return false;
    auto backend = backend_factory_();
    if (!backend) {
        // No trace backend in this build. Identical outcome to a trace that refused to
        // open, and deliberately not a separate code path.
        return false;
    }
    if (!backend->Open()) {
        return false; // ERROR_ACCESS_DENIED when not elevated -> graceful degrade
    }
    // Reset drain state so a Stop()->Start() cycle does not compute a bogus first
    // interval against the previous session's QPC epoch.
    last_present_qpc_ = 0;
    qpc_freq_ = 0;
    // Fresh accounting per session so a Stop()->Start() cycle does not carry over a prior
    // session's discarded/flip totals. SetTargetProcessId resets it again per recording.
    accumulator_.Reset();
    {
        std::lock_guard lk(sample_mutex_);
        latest_ = PresentSample{};
    }
    auto signal = std::make_shared<FinishSignal>();
    {
        std::lock_guard lk(sample_mutex_);
        backend_ = backend;
        finish_ = signal;
    }
    open_.store(true, std::memory_order_release);
    // The thread body captures ONLY shared owners -- never `this`. That is what makes
    // the detach path in Stop() safe: a consumer that outlives this object still has
    // everything it touches alive in its own hands.
    worker_ = std::thread([backend, signal]() {
        // Published from inside the thread, not by Start(): "a consumer is consuming"
        // is a fact about this thread, and asserting it from the caller would make
        // IsOpen() a restatement of Start()'s return value.
        signal->consuming.store(true, std::memory_order_release);
        backend->Consume();
        // Cleared unconditionally: Consume() returning is the end of consumption
        // regardless of whether Stop() asked for it. This is the only writer that can
        // tell the session a trace died on its own.
        signal->consuming.store(false, std::memory_order_release);
        if (!signal->stop_requested.load(std::memory_order_acquire)) {
            // Nobody asked. Another process stopped the named session, a driver reset
            // tore it down, or ETW hit a buffer error -- and present diagnostics are
            // now unavailable for the rest of this run. Until IsOpen() consulted this
            // flag, the same event silently froze the last sample as "current".
            qWarning("[presentmon] the trace ended without a stop request; "
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
        // SYNCHRONIZE only: this is a liveness question, never an inspection. Taken ONCE,
        // here, and held -- see the header for why re-opening by pid later is unsafe.
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
    // Snapshot the backend under the lock, then drain on the snapshot. The held snapshot
    // keeps it alive even if Stop() releases it mid-drain (a drain after Close() is safe
    // and just returns empty).
    std::shared_ptr<IPresentTraceBackend> backend;
    {
        std::lock_guard lk(sample_mutex_);
        backend = backend_;
    }
    if (backend) {
        // The frequency is constant for the trace; read it before the drain loop so the
        // first batch computes real intervals.
        qpc_freq_ = backend->TimestampFrequency();
        const unsigned long want_pid = target_pid_.load(std::memory_order_relaxed);
        // Counted, not just skipped. A target that presents but is never counted and
        // a target that does not present at all look identical from the outside --
        // both report zero -- and telling them apart is the difference between a
        // broken attribution and a quiet application. Observed once already: a
        // process holding the display in TRUE exclusive fullscreen produced no
        // counted presents at all, and nothing in the diagnostics could say whether
        // its events reached the trace under a different id or never arrived.
        size_t drained = 0;
        size_t attributed = 0;
        for (const TracePresentEvent& present : backend->Drain()) {
            ++drained;
            if (want_pid != 0 && present.process_id != want_pid)
                continue;
            ++attributed;
            RawPresentEvent raw;
            raw.valid = true;
            raw.present_mode_code = present.present_mode_code;
            raw.sync_interval = present.sync_interval;
            raw.tearing_flag = present.tearing_flag;
            // The inter-present gap; 0 for the first present of an attribution window,
            // and 0 whenever the frequency is unknown -- never a fabricated number.
            raw.interval_ms = (last_present_qpc_ != 0 && present.present_qpc > last_present_qpc_ && qpc_freq_ != 0)
                                  ? static_cast<double>(present.present_qpc - last_present_qpc_) * 1000.0 /
                                        static_cast<double>(qpc_freq_)
                                  : 0.0;
            last_present_qpc_ = present.present_qpc;
            PresentSample mapped = MapPresentEvent(raw);
            // ADR 0033 extra-checks: fold into the per-recording aggregates. The accumulator
            // counts flips on the CLASSIFIED mode so sub-variant changes (e.g. Composed_Flip
            // -> Composed_Copy) do not register as a present-mode flip.
            accumulator_.Observe(mapped.mode, present.discarded);
            mapped.present_count = static_cast<uint32_t>(accumulator_.present_count);
            mapped.discarded_count = static_cast<uint32_t>(accumulator_.discarded_count);
            mapped.mode_flip_count = static_cast<uint32_t>(accumulator_.mode_flip_count);
            std::lock_guard lk(sample_mutex_);
            latest_ = mapped;
        }
        // Logged only when the trace carried presents and NONE of them were the
        // attributed process: the state that is otherwise indistinguishable from a
        // silent target. Once per attribution window, so a long recording cannot
        // fill the log with it.
        if (drained > 0 && attributed == 0 && want_pid != 0 && !unattributed_logged_) {
            unattributed_logged_ = true;
            qInfo("[presentmon] %llu present(s) in this batch, none from the attributed process (pid %lu)",
                  static_cast<unsigned long long>(drained), want_pid);
        } else if (attributed > 0) {
            unattributed_logged_ = false;
        }
    }
    std::lock_guard lk(sample_mutex_);
    return latest_;
}

void PresentMonEtwSession::Stop() {
    if (!open_.exchange(false))
        return;
    std::shared_ptr<IPresentTraceBackend> backend;
    std::shared_ptr<FinishSignal> signal;
    {
        std::lock_guard lk(sample_mutex_);
        backend = backend_;
        signal = finish_;
    }
    // Before Close(), so the consumer cannot observe its own return without also
    // observing that it was asked for.
    if (signal) {
        signal->stop_requested.store(true, std::memory_order_release);
    }
    if (backend) {
        backend->Close(); // unblocks Consume()
    }
    if (worker_.joinable()) {
        // BOUNDED. Closing the trace is documented to make the consume call return once
        // it has drained its buffers, and in practice that takes well under a second.
        // But this wait runs while the application is shutting down, and an unbounded
        // join on an OS call means one misbehaving trace session leaves a process the
        // user asked to quit alive, with its window gone and only Task Manager left to
        // end it. A quit that does not quit is a worse failure than a thread that
        // outlives its owner, so the wait has a ceiling.
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
            qWarning("[presentmon] the consumer did not return within 5s of closing the trace; "
                     "detaching the consumer thread so shutdown can finish");
            worker_.detach();
        }
    }
    {
        std::lock_guard lk(sample_mutex_);
        backend_.reset();
        finish_.reset();
        // The trace is gone, so the last sample describes something that is over. A
        // Stop() that left it in place would let a re-Start()ed session serve the
        // previous trace's present as its first reading.
        latest_ = PresentSample{};
    }
    // Drain state belongs to the trace generation, not to the object.
    last_present_qpc_ = 0;
    qpc_freq_ = 0;
    accumulator_.Reset();
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
