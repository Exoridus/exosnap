#include <gtest/gtest.h>

#include <atomic>
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
    std::atomic<bool> release{false};
    std::thread worker([&] {
        gate.Invoke([&] {
            entered.set_value();
            while (!release.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        });
    });
    entered.get_future().wait();

    // The callback is blocked (a stalled disk, say). Close() must give up within
    // its bound instead of holding the Record() thread hostage, and say so.
    const auto before = std::chrono::steady_clock::now();
    EXPECT_FALSE(gate.Close(std::chrono::milliseconds(50)));
    EXPECT_LT(std::chrono::steady_clock::now() - before, std::chrono::seconds(2));
    EXPECT_FALSE(gate.IsOpen());

    release.store(true);
    worker.join();

    // The late worker is done; anything it fires now is dropped.
    int calls = 0;
    gate.Invoke([&] { ++calls; });
    EXPECT_EQ(calls, 0);
}

TEST(SessionCallbackGate, ConcurrentInvocationsDoNotSerialize) {
    SessionCallbackGate gate;
    std::promise<void> first_entered;
    std::atomic<bool> release{false};
    std::thread first([&] {
        gate.Invoke([&] {
            first_entered.set_value();
            while (!release.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        });
    });
    first_entered.get_future().wait();

    // A second worker's callback (a preview frame while the mux is finalizing a
    // segment) must not wait for the first to finish.
    std::atomic<bool> second_ran{false};
    std::thread second([&] { gate.Invoke([&] { second_ran.store(true); }); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!second_ran.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_TRUE(second_ran.load());

    release.store(true);
    first.join();
    second.join();
}
