#include <gtest/gtest.h>

#include "worker_join.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <windows.h>

namespace {

using recorder_core::WaitForWorkersThenJoin;
using recorder_core::WorkerJoinResult;

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

using namespace std::chrono_literals;

} // namespace

// REGRESSION (max-recording-duration defect): a recording that runs far longer
// than the join budget must NOT be reported as a timeout. Phase 1 waits for the
// stop signal without consuming the budget; only Phase 2 (after stop) is bounded.
// Pre-fix, the 50 ms budget would have elapsed ~350 ms before the stop and all
// workers would have been reported as TIMEOUT (the actual crash signature:
// "join timeout: v=TIMEOUT a0=TIMEOUT m=TIMEOUT", hr=0x800705B4).
TEST(WorkerJoinTest, LongRunBeforeStopIsNotATimeout) {
    std::atomic<bool> stop{false};
    std::vector<FakeWorker> workers(3);
    std::vector<HANDLE> handles;
    for (auto& w : workers) {
        w.Start(&stop, /*drain=*/0ms);
        handles.push_back(w.NativeHandle());
    }

    const auto t0 = std::chrono::steady_clock::now();
    std::thread stopper([&stop] {
        std::this_thread::sleep_for(400ms);
        stop.store(true);
    });

    // Tiny join budget on purpose: it must NOT bound Phase 1.
    const WorkerJoinResult r =
        WaitForWorkersThenJoin(handles, stop, /*join_budget_ms=*/50, /*stop_poll_interval_ms=*/10);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    stopper.join();
    for (auto& w : workers) {
        w.Join();
    }

    EXPECT_TRUE(r.all_signaled);
    EXPECT_FALSE(r.wait_failed);
    // The wait returned only after the ~400 ms stop, proving it was unbounded
    // and did not trip the 50 ms budget.
    EXPECT_GE(elapsed, 350);
}

// The join budget still protects against a worker that hangs AFTER stop: it must
// be reported as not-signaled (so the caller emits the join-timeout detail and
// the destructor detaches it) instead of blocking forever.
TEST(WorkerJoinTest, HungWorkerAfterStopReportedAsTimeout) {
    std::atomic<bool> stop{false};
    std::vector<FakeWorker> workers(2);
    std::vector<HANDLE> handles;
    workers[0].Start(&stop, /*drain=*/0ms);   // exits promptly on stop
    workers[1].Start(&stop, /*drain=*/800ms); // lingers well past the 100 ms budget
    handles.push_back(workers[0].NativeHandle());
    handles.push_back(workers[1].NativeHandle());

    stop.store(true); // request stop immediately

    const WorkerJoinResult r =
        WaitForWorkersThenJoin(handles, stop, /*join_budget_ms=*/100, /*stop_poll_interval_ms=*/10);

    EXPECT_FALSE(r.all_signaled);
    EXPECT_FALSE(r.wait_failed);
    ASSERT_EQ(r.signaled.size(), 2u);
    EXPECT_TRUE(r.signaled[0]);  // prompt worker joined within the budget
    EXPECT_FALSE(r.signaled[1]); // hung worker timed out

    // Let the lingering worker finish so the fixture tears down cleanly.
    for (auto& w : workers) {
        w.Join();
    }
}

// Edge case for the Phase-1 bWaitAll probe: a worker that exits on its own
// without stop ever being set must not hang the wait — the probe detects the
// exit and falls through to the (immediately satisfied) join.
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
