// ADR 0033 / Wave D: the PRODUCTION present session, compiled exactly as it ships.
//
// This file exists because of a specific hole. PresentMonEtwSession used to be split by
// `#ifdef EXOSNAP_HAS_PRESENTMON`, and this test target compiled the no-op side -- so
// the attribution boundaries, the accumulator resets, the held process handle and the
// meaning of "available" were all asserted against an implementation that did nothing,
// while the shipping implementation was asserted against by nobody. It is the same
// shape as the original defect (the sources were compiled ONLY by a test target and the
// product shipped the no-op), and finding it twice is why the ETW boundary became a
// seam instead of an ifdef.
//
// What is faked here is exactly what a test cannot reproduce: opening a real-time ETW
// session needs elevation, and the consume call blocks in the kernel. Everything above
// that -- classification, intervals, accumulators, attribution, liveness -- is the
// production code, running here unmodified.
//
// Two things this file deliberately does NOT do:
//   * open the real trace. An elevated run would call StopNamedSession on
//     "ExoSnapPresentMon" and tear the session out from under a running ExoSnap.
//   * sleep to synchronise. Every wait is a bounded poll of an actual condition.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <gtest/gtest.h>

#include "diagnostics/ElevationProvider.h"
#include "diagnostics/PresentMonEtwSession.h"
#include "diagnostics/PresentMonProvider.h"
#include "diagnostics/PresentTraceBackend.h"

namespace {

using exosnap::diagnostics::IElevationProvider;
using exosnap::diagnostics::IPresentTraceBackend;
using exosnap::diagnostics::MakePresentTraceBackend;
using exosnap::diagnostics::PresentMode;
using exosnap::diagnostics::PresentMonEtwSession;
using exosnap::diagnostics::PresentMonProvider;
using exosnap::diagnostics::PresentSample;
using exosnap::diagnostics::TracePresentEvent;

// PresentMon's PresentMode enum values, named here so the intent of a test is legible
// without the vendored header. Only the classification boundary matters: everything
// that is not a flip/fullscreen variant classifies as Composed.
constexpr int kModeComposedFlip = 4;            // PresentMode::Composed_Flip -> Composed
constexpr int kModeHardwareIndependentFlip = 3; // PresentMode::Hardware_Independent_Flip -> IndependentFlip

// A bounded poll of an actual condition -- not a synchronisation sleep. The predicate
// is the measurement; the ceiling only bounds a hang.
bool WaitUntil(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

// The ETW boundary, and nothing above it. Consume() blocks exactly like ProcessTrace
// does, because a backend that returned immediately would make every lifecycle
// assertion below meaningless.
class FakeTraceBackend final : public IPresentTraceBackend {
  public:
    bool Open() override {
        open_calls.fetch_add(1);
        return open_result;
    }

    void Consume() override {
        consume_entered.store(true);
        std::unique_lock lk(mutex_);
        closed_cv_.wait(lk, [this] { return closed_; });
        consume_returned.store(true);
    }

    void Close() override {
        {
            std::lock_guard lk(mutex_);
            closed_ = true;
        }
        closed_cv_.notify_all();
    }

    int64_t TimestampFrequency() const override {
        return frequency;
    }

    std::vector<TracePresentEvent> Drain() override {
        std::lock_guard lk(mutex_);
        std::vector<TracePresentEvent> out;
        out.swap(queued_);
        drain_calls.fetch_add(1);
        return out;
    }

    // --- test controls -----------------------------------------------------
    void Publish(const TracePresentEvent& event) {
        std::lock_guard lk(mutex_);
        queued_.push_back(event);
    }

    // The trace dying under us: the consume call returns although Stop() never asked.
    void EndTraceWithoutBeingAsked() {
        Close();
    }

    bool open_result = true;
    int64_t frequency = 10'000'000; // 100 ns ticks, so 1 ms == 10'000
    std::atomic<int> open_calls{0};
    std::atomic<int> drain_calls{0};
    std::atomic<bool> consume_entered{false};
    std::atomic<bool> consume_returned{false};

  private:
    mutable std::mutex mutex_;
    std::condition_variable closed_cv_;
    bool closed_ = false;
    std::vector<TracePresentEvent> queued_;
};

std::function<std::shared_ptr<IPresentTraceBackend>()> FactoryFor(const std::shared_ptr<FakeTraceBackend>& backend) {
    return [backend] { return std::static_pointer_cast<IPresentTraceBackend>(backend); };
}

TracePresentEvent Present(unsigned long pid, uint64_t qpc, int mode_code, bool discarded = false) {
    TracePresentEvent event;
    event.process_id = pid;
    event.present_qpc = qpc;
    event.present_mode_code = mode_code;
    event.sync_interval = 1;
    event.tearing_flag = false;
    event.discarded = discarded;
    return event;
}

class StubElevationProvider final : public IElevationProvider {
  public:
    explicit StubElevationProvider(bool elevated) : elevated_(elevated) {
    }
    [[nodiscard]] bool IsElevated() const override {
        return elevated_;
    }

  private:
    bool elevated_;
};

// A process this test owns and can kill on demand. `cmd /c pause` blocks on stdin,
// which is closed here, so it exits on its own even if a test aborts before killing it.
class ChildProcess {
  public:
    ChildProcess() {
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        std::wstring command = L"cmd.exe /c pause";
        if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                             &startup, &info) != 0) {
            handle_ = info.hProcess;
            ::CloseHandle(info.hThread);
            pid_ = info.dwProcessId;
        }
    }
    ~ChildProcess() {
        Kill();
        if (handle_ != nullptr)
            ::CloseHandle(handle_);
    }
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    void Kill() {
        if (handle_ == nullptr)
            return;
        ::TerminateProcess(handle_, 1);
        ::WaitForSingleObject(handle_, 5000);
    }
    [[nodiscard]] bool Started() const {
        return handle_ != nullptr;
    }
    [[nodiscard]] unsigned long Pid() const {
        return pid_;
    }

  private:
    HANDLE handle_ = nullptr;
    unsigned long pid_ = 0;
};

// ---------------------------------------------------------------------------
// 1. Start / consuming
// ---------------------------------------------------------------------------

TEST(PresentSession, StartOpensTheTraceOnceAndTheWorkerBeginsConsuming) {
    auto backend = std::make_shared<FakeTraceBackend>();
    PresentMonEtwSession session(FactoryFor(backend));

    EXPECT_FALSE(session.IsOpen()) << "nothing has been started";
    ASSERT_TRUE(session.Start());
    EXPECT_EQ(backend->open_calls.load(), 1);

    // IsOpen() may legitimately still be false at this instant: the trace is open but
    // the consumer thread has not necessarily been scheduled. `consuming` is published
    // from inside the worker rather than by Start(), so the gap is truthful. The half of
    // that contract which IS decisive -- open_ true while no consumer is running must
    // still read as closed -- is proved by
    // ATraceThatEndsWithoutBeingAskedStopsBeingReportedAsCurrent below.
    ASSERT_TRUE(WaitUntil([&] { return backend->consume_entered.load(); }));
    EXPECT_TRUE(WaitUntil([&] { return session.IsOpen(); }));

    // A second Start() is a no-op, not a second trace. The provider calls it on every
    // SetOptIn(true), so this runs whenever a user toggles the setting twice.
    EXPECT_TRUE(session.Start());
    EXPECT_EQ(backend->open_calls.load(), 1) << "an already-open session must not open a second trace";

    session.Stop();
}

TEST(PresentSession, ATraceThatRefusesToOpenIsNotAnOpenSession) {
    auto backend = std::make_shared<FakeTraceBackend>();
    backend->open_result = false; // ERROR_ACCESS_DENIED is the ordinary unelevated answer
    PresentMonEtwSession session(FactoryFor(backend));

    EXPECT_FALSE(session.Start());
    EXPECT_FALSE(session.IsOpen());
    EXPECT_FALSE(session.Latest().available);
    EXPECT_FALSE(backend->consume_entered.load()) << "no consumer may run without a trace";
}

TEST(PresentSession, ABuildWithoutATraceBackendDegradesInsteadOfBranching) {
    // A null backend is the same answer as a refused trace, on purpose: the no-op twin
    // that used to live behind the ifdef is gone, so there is only one way to be
    // unavailable and it is exercised here.
    PresentMonEtwSession session([] { return std::shared_ptr<IPresentTraceBackend>{}; });
    EXPECT_FALSE(session.Start());
    EXPECT_FALSE(session.IsOpen());
    EXPECT_FALSE(session.Latest().available);
}

TEST(PresentSession, TheRealBackendIsConstructibleInThisBuild) {
    // The structural half of the fix. PresentMonTraceBackend.cpp used to compile only
    // inside app/quick, so nothing linked it outside the shipping binary and a break
    // there was invisible to every test. This asserts that it compiles, links and
    // constructs here -- and deliberately never calls Open(), which would seize a real
    // system-wide ETW session and stop a running ExoSnap's.
    const auto backend = MakePresentTraceBackend();
#ifdef EXOSNAP_HAS_PRESENTMON
    EXPECT_NE(backend, nullptr) << "the vendored consumer is part of this build";
#else
    EXPECT_EQ(backend, nullptr) << "without the vendored consumer there is no backend to make";
#endif
}

// ---------------------------------------------------------------------------
// 2. Stop
// ---------------------------------------------------------------------------

TEST(PresentSession, StopEndsConsumptionAndClearsWhatTheTraceMeasured) {
    auto backend = std::make_shared<FakeTraceBackend>();
    PresentMonEtwSession session(FactoryFor(backend));
    ASSERT_TRUE(session.Start());
    ASSERT_TRUE(WaitUntil([&] { return session.IsOpen(); }));

    backend->Publish(Present(0, 1000, kModeComposedFlip));
    ASSERT_TRUE(session.Latest().available);
    ASSERT_GT(session.AccumulatorForTest().present_count, 0u);

    session.Stop();

    EXPECT_TRUE(WaitUntil([&] { return backend->consume_returned.load(); }));
    EXPECT_FALSE(session.IsOpen());
    // The trace is over, so the last present describes something that has ended. Left
    // in place it would become the first reading of the next trace.
    EXPECT_FALSE(session.Latest().available);
    EXPECT_EQ(session.AccumulatorForTest().present_count, 0u);
}

TEST(PresentSession, StopIsIdempotentAndDoesNotDoubleClose) {
    auto backend = std::make_shared<FakeTraceBackend>();
    PresentMonEtwSession session(FactoryFor(backend));
    ASSERT_TRUE(session.Start());
    ASSERT_TRUE(WaitUntil([&] { return session.IsOpen(); }));

    session.Stop();
    session.Stop(); // must not hang, join twice, or close a released backend
    session.Stop();
    EXPECT_FALSE(session.IsOpen());
}

TEST(PresentSession, ARestartedTraceDoesNotInheritThePreviousEpochOrTotals) {
    auto first = std::make_shared<FakeTraceBackend>();
    auto second = std::make_shared<FakeTraceBackend>();
    std::atomic<int> generation{0};
    PresentMonEtwSession session([&]() -> std::shared_ptr<IPresentTraceBackend> {
        return generation.fetch_add(1) == 0 ? std::static_pointer_cast<IPresentTraceBackend>(first)
                                            : std::static_pointer_cast<IPresentTraceBackend>(second);
    });

    ASSERT_TRUE(session.Start());
    ASSERT_TRUE(WaitUntil([&] { return session.IsOpen(); }));
    first->Publish(Present(0, 1'000'000, kModeComposedFlip));
    first->Publish(Present(0, 1'100'000, kModeComposedFlip));
    ASSERT_GT(session.Latest().present_interval_ms, 0.0);
    session.Stop();

    ASSERT_TRUE(session.Start());
    ASSERT_TRUE(WaitUntil([&] { return session.IsOpen(); }));
    // A QPC far below the previous trace's epoch. Carrying last_present_qpc_ across
    // would produce either a negative gap or a nonsensical one; the first present of a
    // trace has no predecessor, so it has no interval.
    second->Publish(Present(0, 500, kModeComposedFlip));
    const PresentSample sample = session.Latest();
    EXPECT_TRUE(sample.available);
    EXPECT_DOUBLE_EQ(sample.present_interval_ms, 0.0);
    EXPECT_EQ(sample.present_count, 1u) << "the new trace starts its own accounting";
    session.Stop();
}

// ---------------------------------------------------------------------------
// 3. + 4. Attribution boundaries
// ---------------------------------------------------------------------------

TEST(PresentSession, SettingATargetIsAnAttributionBoundary) {
    auto backend = std::make_shared<FakeTraceBackend>();
    PresentMonEtwSession session(FactoryFor(backend));
    ASSERT_TRUE(session.Start());
    ASSERT_TRUE(WaitUntil([&] { return session.IsOpen(); }));

    backend->Publish(Present(0, 1'000'000, kModeComposedFlip));
    backend->Publish(Present(0, 1'200'000, kModeHardwareIndependentFlip, /*discarded=*/true));
    ASSERT_EQ(session.Latest().present_count, 2u);
    ASSERT_EQ(session.AccumulatorForTest().discarded_count, 1u);
    ASSERT_EQ(session.AccumulatorForTest().mode_flip_count, 1u);

    session.SetTargetProcessId(4242);

    EXPECT_EQ(session.TargetProcessIdForTest(), 4242u);
    EXPECT_EQ(session.AccumulatorForTest().present_count, 0u);
    EXPECT_EQ(session.AccumulatorForTest().discarded_count, 0u);
    EXPECT_EQ(session.AccumulatorForTest().mode_flip_count, 0u);
    EXPECT_EQ(session.AccumulatorForTest().last_mode, PresentMode::Unknown)
        << "the flip counter must not compare across the boundary either";
    EXPECT_FALSE(session.Latest().available) << "the previous window's sample is not the new window's current";
    session.Stop();
}

TEST(PresentSession, TheBoundaryIsUnconditionalSoAStopToPidZeroStillResets) {
    // This is the case a pid-equality guard would skip and the recording-stop edge
    // depends on: a Monitor or Region recording targets pid 0 exactly like the idle
    // desktop, so "the pid did not move" is true precisely when the totals must go.
    auto backend = std::make_shared<FakeTraceBackend>();
    PresentMonEtwSession session(FactoryFor(backend));
    ASSERT_TRUE(session.Start());
    ASSERT_TRUE(WaitUntil([&] { return session.IsOpen(); }));

    session.SetTargetProcessId(0); // recording start, Monitor target
    backend->Publish(Present(0, 1'000'000, kModeComposedFlip));
    backend->Publish(Present(0, 1'100'000, kModeComposedFlip));
    // The drain is reader-side, so the totals only exist once somebody asks.
    ASSERT_TRUE(session.Latest().available);
    ASSERT_EQ(session.AccumulatorForTest().present_count, 2u);

    session.SetTargetProcessId(0); // recording stop, back to the idle desktop

    EXPECT_EQ(session.AccumulatorForTest().present_count, 0u)
        << "a finished recording's totals must not describe the idle desktop";
    EXPECT_FALSE(session.Latest().available);
    session.Stop();
}

TEST(PresentSession, PresentsFromAnotherProcessNeverEnterTheAttributionWindow) {
    auto backend = std::make_shared<FakeTraceBackend>();
    PresentMonEtwSession session(FactoryFor(backend));
    ASSERT_TRUE(session.Start());
    ASSERT_TRUE(WaitUntil([&] { return session.IsOpen(); }));

    // A pid that certainly exists and is certainly not the publisher below.
    session.SetTargetProcessId(::GetCurrentProcessId());
    backend->Publish(Present(::GetCurrentProcessId() + 1, 1'000'000, kModeComposedFlip));
    EXPECT_FALSE(session.Latest().available) << "a present from a different process is not this window's";
    EXPECT_EQ(session.AccumulatorForTest().present_count, 0u);

    backend->Publish(Present(::GetCurrentProcessId(), 1'100'000, kModeComposedFlip));
    EXPECT_TRUE(session.Latest().available);
    EXPECT_EQ(session.AccumulatorForTest().present_count, 1u);
    session.Stop();
}

// ---------------------------------------------------------------------------
// 5. + 6. Process identity and unexpected termination
// ---------------------------------------------------------------------------

TEST(PresentSession, AnAttributedProcessThatExitsEndsTheWindowRatherThanFreezingIt) {
    ChildProcess child;
    ASSERT_TRUE(child.Started()) << "the test needs a process it owns";

    auto backend = std::make_shared<FakeTraceBackend>();
    PresentMonEtwSession session(FactoryFor(backend));
    ASSERT_TRUE(session.Start());
    ASSERT_TRUE(WaitUntil([&] { return session.IsOpen(); }));

    session.SetTargetProcessId(child.Pid());
    backend->Publish(Present(child.Pid(), 1'000'000, kModeHardwareIndependentFlip));
    ASSERT_TRUE(session.Latest().available) << "a live target reports normally";

    child.Kill();

    // No further present will ever match the filter, so without this the last frame the
    // process rendered would sit on the Diagnostics page as the current present mode
    // for the rest of the run.
    EXPECT_TRUE(WaitUntil([&] { return !session.Latest().available; }));
    // The TRACE is still fine -- only the attribution ended. Collapsing the two would
    // report a healthy ETW session as broken.
    EXPECT_TRUE(session.IsOpen());
    session.Stop();
}

TEST(PresentSession, LivenessIsBoundToTheHandleTakenAtTheBoundaryNotToTheNumber) {
    // The PID-reuse contract, stated so that it is observable. An implementation that
    // answered liveness by re-opening the pid each sample would, for an exited process,
    // get nullptr from OpenProcess -- which this code treats as UNKNOWN and therefore
    // as alive. So "reports dead" is only reachable through a handle acquired while the
    // process was running, and that is what makes the assertion below a proof rather
    // than a restatement.
    ChildProcess child;
    ASSERT_TRUE(child.Started());
    const unsigned long pid = child.Pid();

    auto backend = std::make_shared<FakeTraceBackend>();
    PresentMonEtwSession session(FactoryFor(backend));
    ASSERT_TRUE(session.Start());
    ASSERT_TRUE(WaitUntil([&] { return session.IsOpen(); }));

    session.SetTargetProcessId(pid); // handle taken HERE, while the process is alive
    backend->Publish(Present(pid, 1'000'000, kModeComposedFlip));
    ASSERT_TRUE(session.Latest().available);

    child.Kill();
    EXPECT_TRUE(WaitUntil([&] { return !session.Latest().available; }));

    // And a target that was never openable is UNKNOWN, not dead: the System process
    // cannot be opened even from an elevated session, and blanking diagnostics for a
    // program that is plainly running would be the same lie pointed the other way.
    session.SetTargetProcessId(4); // System
    backend->Publish(Present(4, 2'000'000, kModeComposedFlip));
    EXPECT_TRUE(session.Latest().available) << "unknown liveness must not read as dead";
    session.Stop();
}

// ---------------------------------------------------------------------------
// 7. No present observed yet
// ---------------------------------------------------------------------------

TEST(PresentSession, AnOpenTraceWithNoPresentYetIsOpenButHasNothingToReport) {
    auto backend = std::make_shared<FakeTraceBackend>();
    PresentMonEtwSession session(FactoryFor(backend));
    ASSERT_TRUE(session.Start());
    ASSERT_TRUE(WaitUntil([&] { return session.IsOpen(); }));

    const PresentSample sample = session.Latest();
    EXPECT_TRUE(session.IsOpen()) << "the trace is open; that is a different fact from having a measurement";
    EXPECT_FALSE(sample.available);
    EXPECT_EQ(sample.mode, PresentMode::Unknown);
    EXPECT_EQ(sample.present_count, 0u);
    EXPECT_GT(backend->drain_calls.load(), 0) << "it asked, and the honest answer was nothing yet";
    session.Stop();
}

TEST(PresentSession, ATraceThatEndsWithoutBeingAskedStopsBeingReportedAsCurrent) {
    auto backend = std::make_shared<FakeTraceBackend>();
    PresentMonEtwSession session(FactoryFor(backend));
    ASSERT_TRUE(session.Start());
    ASSERT_TRUE(WaitUntil([&] { return session.IsOpen(); }));

    backend->Publish(Present(0, 1'000'000, kModeHardwareIndependentFlip));
    ASSERT_TRUE(session.Latest().available);

    // Another process stopped the named session, a driver reset tore it down, or ETW
    // hit a buffer error. Nobody called Stop().
    backend->EndTraceWithoutBeingAsked();

    EXPECT_TRUE(WaitUntil([&] { return !session.IsOpen(); }))
        << "a dead trace must not keep reporting the last present it decoded";
    EXPECT_FALSE(session.Latest().available);
    session.Stop(); // still has to tidy up
}

// ---------------------------------------------------------------------------
// 8. Availability truth table, through the provider
// ---------------------------------------------------------------------------

TEST(PresentProviderAvailability, TruthTable) {
    const StubElevationProvider elevated(true);
    const StubElevationProvider not_elevated(false);

    {
        // opt-in off: the gate never opens, so nothing is even attempted.
        auto backend = std::make_shared<FakeTraceBackend>();
        PresentMonProvider provider(elevated, /*opt_in=*/false, FactoryFor(backend));
        EXPECT_FALSE(provider.GateOpen());
        EXPECT_FALSE(provider.IsAvailable());
        EXPECT_FALSE(provider.Sample().available);
        EXPECT_EQ(backend->open_calls.load(), 0) << "no trace may be opened before the gate is open";
    }
    {
        // opt-in on, not elevated: the gate is closed for a different reason, and the
        // distinction is what lets a client say WHY there is no measurement.
        auto backend = std::make_shared<FakeTraceBackend>();
        PresentMonProvider provider(not_elevated, /*opt_in=*/true, FactoryFor(backend));
        EXPECT_FALSE(provider.GateOpen());
        EXPECT_FALSE(provider.IsAvailable());
        EXPECT_EQ(backend->open_calls.load(), 0);
    }
    {
        // elevated + opt-in, but the trace refuses: available stays false, truthfully.
        auto backend = std::make_shared<FakeTraceBackend>();
        backend->open_result = false;
        PresentMonProvider provider(elevated, /*opt_in=*/true, FactoryFor(backend));
        EXPECT_TRUE(provider.GateOpen());
        EXPECT_FALSE(provider.IsAvailable());
        EXPECT_EQ(backend->open_calls.load(), 1);
    }
    {
        // elevated + opt-in + open trace + no present yet: AVAILABLE as a session,
        // with a sample that reports nothing. These are two different questions and
        // the provider answers both.
        auto backend = std::make_shared<FakeTraceBackend>();
        PresentMonProvider provider(elevated, /*opt_in=*/true, FactoryFor(backend));
        ASSERT_TRUE(WaitUntil([&] { return provider.IsAvailable(); }));
        EXPECT_FALSE(provider.Sample().available) << "no present has been decoded yet";

        // ... and once one has been.
        backend->Publish(Present(0, 1'000'000, kModeHardwareIndependentFlip));
        const PresentSample sample = provider.Sample();
        EXPECT_TRUE(sample.available);
        EXPECT_EQ(sample.mode, PresentMode::IndependentFlip);
        EXPECT_EQ(sample.present_count, 1u);
    }
}

TEST(PresentProviderAvailability, TurningTheOptInOffClosesTheTrace) {
    const StubElevationProvider elevated(true);
    auto backend = std::make_shared<FakeTraceBackend>();
    PresentMonProvider provider(elevated, /*opt_in=*/true, FactoryFor(backend));
    ASSERT_TRUE(WaitUntil([&] { return provider.IsAvailable(); }));

    provider.SetOptIn(false);

    EXPECT_FALSE(provider.IsAvailable());
    EXPECT_TRUE(WaitUntil([&] { return backend->consume_returned.load(); }))
        << "the consumer must not outlive the opt-in that started it";
    EXPECT_FALSE(provider.Sample().available);
}

} // namespace
