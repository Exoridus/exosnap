#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <thread>

#include "session_callback_gate.h"

using exosnap::engine::SessionCallbackGate;

// A gate stands in for one Record() call: the callbacks a worker fires reach the
// caller only while that call is running. An abandoned worker returning late
// finds it closed.

TEST(SessionCallbackGate, OpenGateRunsTheCallback) {
    SessionCallbackGate gate;
    int calls = 0;
    gate.Invoke([&] { ++calls; });
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(gate.IsOpen());
}

TEST(SessionCallbackGate, ClosedGateDropsTheCallback) {
    SessionCallbackGate gate;
    EXPECT_TRUE(gate.Close());
    int calls = 0;
    gate.Invoke([&] { ++calls; });
    EXPECT_EQ(calls, 0);
    EXPECT_FALSE(gate.IsOpen());
}

TEST(SessionCallbackGate, CloseWithNothingInFlightDrainsImmediately) {
    SessionCallbackGate gate;
    gate.Invoke([] {});
    EXPECT_TRUE(gate.Close(std::chrono::milliseconds(0)));
}

TEST(SessionCallbackGate, CloseReportsAnInvocationStillRunning) {
    SessionCallbackGate gate;
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future();
    std::thread worker([&] {
        gate.Invoke([&] {
            entered.set_value();
            released.wait();
        });
    });
    entered.get_future().wait();

    // The callback is blocked (a stalled disk, say). Close() must give up within
    // its bound instead of holding the Record() thread hostage, and say so.
    const auto before = std::chrono::steady_clock::now();
    EXPECT_FALSE(gate.Close(std::chrono::milliseconds(50)));
    EXPECT_LT(std::chrono::steady_clock::now() - before, std::chrono::seconds(2));
    EXPECT_FALSE(gate.IsOpen());

    release.set_value();
    worker.join();

    // The late worker is done; anything it fires now is dropped.
    int calls = 0;
    gate.Invoke([&] { ++calls; });
    EXPECT_EQ(calls, 0);
}

TEST(SessionCallbackGate, ConcurrentInvocationsDoNotSerialize) {
    SessionCallbackGate gate;
    std::promise<void> first_entered;
    std::promise<void> release;
    auto released = release.get_future();
    std::thread first([&] {
        gate.Invoke([&] {
            first_entered.set_value();
            released.wait();
        });
    });
    first_entered.get_future().wait();

    // A second worker's callback (a preview frame while the mux is finalizing a
    // segment) must not wait for the first to finish.
    std::promise<void> second_ran;
    auto second_completed = second_ran.get_future();
    std::thread second([&] { gate.Invoke([&] { second_ran.set_value(); }); });
    EXPECT_EQ(second_completed.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    release.set_value();
    first.join();
    second.join();
}
