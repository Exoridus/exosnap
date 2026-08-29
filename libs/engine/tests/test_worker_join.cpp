#include <gtest/gtest.h>

#include "worker_join.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <windows.h>

namespace {

using exosnap::engine::WaitForWorkersThenJoin;
using exosnap::engine::WorkerJoinResult;

// A controllable stand-in for the real video/audio/mux worker threads. It loops
// observing `stop` cooperatively (like the engine's threads), then optionally
// lingers for `drain` to simulate shutdown work before exiting. `self_exit_after`
// (0 = never) lets a worker exit on its own without a stop, to exercise the
// Phase-1 fall-through guard.
class FakeWorker {
  public:
    void Start(const std::atomic<bool>* stop, std::chrono::milliseconds drain,
               std::chrono::milliseconds self_exit_after = std::chrono::milliseconds(0)) {
        thread_ = std::thread([stop, drain, self_exit_after] {
            const auto start = std::chrono::steady_clock::now();
            while (!stop->load()) {
                if (self_exit_after.count() > 0 && (std::chrono::steady_clock::now() - start) >= self_exit_after) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (drain.count() > 0) {
                std::this_thread::sleep_for(drain);
            }
        });
    }

    HANDLE NativeHandle() {
        return thread_.joinable() ? thread_.native_handle() : nullptr;
    }
    void Join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    ~FakeWorker() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

  private:
    std::thread thread_;
};

// A worker's exit, expressed as the thing WaitForMultipleObjects actually
// observes: a signalable kernel handle. A thread handle IS one, so the wait
// cannot tell the two apart -- but a thread reaches its signaled state when the
// scheduler gets to it, and an event reaches it when the test says so.
//
// That difference is the whole point. Asserting "this worker finished within the
// budget" against a thread makes the assertion a statement about how busy the
// machine is: under a parallel CTest run a thread that only has to observe a
// flag and return can miss a 100 ms budget, and the test fails while the code is
// correct. Every case below that pins a worker to a SPECIFIC signaled state uses
// an event, so the state is a fact the test established rather than an outcome
// it hoped for. FakeWorker stays where a real thread handle is what is under
// test.
class FakeWorkerEvent {
  public:
    FakeWorkerEvent() : handle_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
    }
    ~FakeWorkerEvent() {
        if (handle_ != nullptr)
            CloseHandle(handle_);
    }
    FakeWorkerEvent(const FakeWorkerEvent&) = delete;
    FakeWorkerEvent& operator=(const FakeWorkerEvent&) = delete;

    [[nodiscard]] HANDLE Handle() const noexcept {
        return handle_;
    }
    // The worker exited. Manual-reset, so it stays exited.
    void SignalExit() const {
        SetEvent(handle_);
    }

  private:
    HANDLE handle_ = nullptr;
};

using namespace std::chrono_literals;

} // namespace

// REGRESSION (max-recording-duration defect): a recording that runs far longer
// than the join budget must NOT be reported as a timeout. Phase 1 waits for the
// stop signal without consuming the budget; only Phase 2 (after stop) is bounded.
// Pre-fix, the 50 ms budget would have elapsed ~350 ms before the stop and all
// workers would have been reported as TIMEOUT (the actual crash signature:
// "join timeout: v=TIMEOUT a0=TIMEOUT m=TIMEOUT", hr=0x800705B4).
//
// The elapsed assertion is a LOWER bound, which is the only kind a wall clock
// can carry honestly: a busy machine can make the wait return later, never
// earlier, so the measurement means the same thing under any load. What the
// workers do is not left to the scheduler either -- they exit exactly when the
// stopper says so.
TEST(WorkerJoinTest, LongRunBeforeStopIsNotATimeout) {
    std::atomic<bool> stop{false};
    const FakeWorkerEvent w0;
    const FakeWorkerEvent w1;
    const FakeWorkerEvent w2;
    ASSERT_NE(w0.Handle(), nullptr);
    ASSERT_NE(w1.Handle(), nullptr);
    ASSERT_NE(w2.Handle(), nullptr);
    const std::vector<HANDLE> handles{w0.Handle(), w1.Handle(), w2.Handle()};

    const auto t0 = std::chrono::steady_clock::now();
    std::thread stopper([&] {
        // A long recording: nothing is signaled and no stop is pending, so the
        // wait has nothing to return for.
        std::this_thread::sleep_for(400ms);
        // Drained first, then the stop, which is the order the engine produces:
        // whichever of the two Phase 1 observes, it observes it now and not at
        // the budget.
        w0.SignalExit();
        w1.SignalExit();
        w2.SignalExit();
        stop.store(true);
    });

    // Tiny join budget on purpose: it must NOT bound Phase 1.
    const WorkerJoinResult r =
        WaitForWorkersThenJoin(handles, stop, /*join_budget_ms=*/50, /*stop_poll_interval_ms=*/10);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    stopper.join();

    EXPECT_TRUE(r.all_signaled);
    EXPECT_FALSE(r.wait_failed);
    // The wait returned only after the ~400 ms stop, proving it was unbounded
    // and did not trip the 50 ms budget.
    EXPECT_GE(elapsed, 350);
}

// The join budget still protects against a worker that hangs AFTER stop: it must
// be reported as not-signaled (so the caller emits the join-timeout detail and
// the destructor detaches it) instead of blocking forever.
//
// Both outcomes are established before the wait runs -- one worker has exited,
// the other never will -- so the only thing the budget decides here is when the
// wait gives up, which is what the test is about.
TEST(WorkerJoinTest, HungWorkerAfterStopReportedAsTimeout) {
    std::atomic<bool> stop{false};
    const FakeWorkerEvent prompt;
    const FakeWorkerEvent hung;
    ASSERT_NE(prompt.Handle(), nullptr);
    ASSERT_NE(hung.Handle(), nullptr);
    const std::vector<HANDLE> handles{prompt.Handle(), hung.Handle()};

    prompt.SignalExit(); // drained and exited
    stop.store(true);    // request stop; `hung` never exits

    const WorkerJoinResult r =
        WaitForWorkersThenJoin(handles, stop, /*join_budget_ms=*/100, /*stop_poll_interval_ms=*/10);

    EXPECT_FALSE(r.all_signaled);
    EXPECT_FALSE(r.wait_failed);
    ASSERT_EQ(r.signaled.size(), 2u);
    EXPECT_TRUE(r.signaled[0]);
    EXPECT_FALSE(r.signaled[1]);
}

// Edge case for the Phase-1 bWaitAll probe: a worker that exits on its own
// without stop ever being set must not hang the wait — the probe detects the
// exit and falls through to the (immediately satisfied) join.
//
// Real thread handles, deliberately: this is the one case where what is under
// test is a THREAD reaching its signaled state without anyone asking it to. No
// budget rides on how soon that happens -- Phase 1 is unbounded while stop stays
// false and only leaves once both handles are signaled, which leaves Phase 2
// already satisfied. The budget below is never reached at any load.
TEST(WorkerJoinTest, WorkersSelfExitingWithoutStopDoNotHang) {
    std::atomic<bool> stop{false};
    std::vector<FakeWorker> workers(2);
    std::vector<HANDLE> handles;
    for (auto& w : workers) {
        w.Start(&stop, /*drain=*/0ms, /*self_exit_after=*/40ms);
        handles.push_back(w.NativeHandle());
    }

    // stop stays false the whole time; the workers exit on their own.
    const WorkerJoinResult r =
        WaitForWorkersThenJoin(handles, stop, /*join_budget_ms=*/2000, /*stop_poll_interval_ms=*/10);

    for (auto& w : workers) {
        w.Join();
    }

    EXPECT_TRUE(r.all_signaled);
    EXPECT_FALSE(r.wait_failed);
}

// An empty handle set is a trivial success (no workers to wait on).
TEST(WorkerJoinTest, EmptyHandleSetSucceeds) {
    std::atomic<bool> stop{false};
    const std::vector<HANDLE> handles;
    const WorkerJoinResult r = WaitForWorkersThenJoin(handles, stop, 100, 10);
    EXPECT_TRUE(r.all_signaled);
    EXPECT_FALSE(r.wait_failed);
    EXPECT_TRUE(r.signaled.empty());
}
