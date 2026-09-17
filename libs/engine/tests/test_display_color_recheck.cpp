// When a session re-reads the colour facts of the display it captures, and how
// that answer changes once the OS is reporting the change itself. Pure, no COM.

#include <exosnap/engine/display_color_recheck.h>

#include <gtest/gtest.h>

using exosnap::engine::DisplayColorRecheckGate;
using exosnap::engine::kDisplayColorNotifiedBackstop;
using exosnap::engine::kDisplayColorPolledBackstop;

namespace {

using Clock = std::chrono::steady_clock;

constexpr Clock::time_point kT0{};

Clock::time_point At(std::chrono::milliseconds ms) {
    return kT0 + ms;
}

} // namespace

// Starting the gate is not itself a reason to re-read: the caller has just read
// the facts to open the session with, and reading them again in the first tick
// would only cost two queries.
TEST(DisplayColorRecheckGate, NothingIsOwedImmediatelyAfterStarting) {
    DisplayColorRecheckGate gate;
    gate.Start(kT0, /*notified=*/false);

    EXPECT_FALSE(gate.TakeDue(kT0, /*signalled=*/false));
    EXPECT_FALSE(gate.TakeDue(At(kDisplayColorPolledBackstop - std::chrono::milliseconds{1}), false));
}

TEST(DisplayColorRecheckGate, WithoutANotificationTheBackstopIsTheMechanism) {
    DisplayColorRecheckGate gate;
    gate.Start(kT0, /*notified=*/false);

    EXPECT_EQ(gate.Backstop(), kDisplayColorPolledBackstop);
    EXPECT_TRUE(gate.TakeDue(At(kDisplayColorPolledBackstop), false));
    // And it re-arms rather than staying due.
    EXPECT_FALSE(gate.TakeDue(At(kDisplayColorPolledBackstop), false));
    EXPECT_TRUE(gate.TakeDue(At(2 * kDisplayColorPolledBackstop), false));
}

// The whole reason the notification is worth having: a change is acted on when
// Windows reports it, not at the next interval.
TEST(DisplayColorRecheckGate, ASignalIsActedOnImmediately) {
    DisplayColorRecheckGate gate;
    gate.Start(kT0, /*notified=*/true);

    EXPECT_TRUE(gate.TakeDue(At(std::chrono::milliseconds{5}), /*signalled=*/true));
}

// A signal is one read, not a licence to read on every later tick.
TEST(DisplayColorRecheckGate, ASignalIsConsumedRatherThanLatched) {
    DisplayColorRecheckGate gate;
    gate.Start(kT0, /*notified=*/true);

    ASSERT_TRUE(gate.TakeDue(At(std::chrono::milliseconds{5}), true));
    EXPECT_FALSE(gate.TakeDue(At(std::chrono::milliseconds{6}), false));
    EXPECT_FALSE(gate.TakeDue(At(std::chrono::milliseconds{500}), false));
}

// The backstop does not disappear when a notification exists. It covers what the
// notification structurally cannot -- it is scoped to one monitor, and a capture
// that reopens on a new handle is unwatched until the subscription follows.
TEST(DisplayColorRecheckGate, ANotifiedSessionStillReadsOnItsBackstop) {
    DisplayColorRecheckGate gate;
    gate.Start(kT0, /*notified=*/true);

    EXPECT_EQ(gate.Backstop(), kDisplayColorNotifiedBackstop);
    EXPECT_FALSE(gate.TakeDue(At(kDisplayColorNotifiedBackstop - std::chrono::milliseconds{1}), false));
    EXPECT_TRUE(gate.TakeDue(At(kDisplayColorNotifiedBackstop), false));
}

// The notified backstop must stay longer than the polled one, or subscribing
// would buy latency and cost queries.
TEST(DisplayColorRecheckGate, TheNotifiedBackstopIsTheLongerOfTheTwo) {
    EXPECT_GT(kDisplayColorNotifiedBackstop, kDisplayColorPolledBackstop);
}

// The subscription is established on another thread, so a session starts polled
// and may become notified a moment later. Taking the longer cadence must not
// leave a re-read that was already owed unowed.
TEST(DisplayColorRecheckGate, BecomingNotifiedDoesNotCancelAnOwedRead) {
    DisplayColorRecheckGate gate;
    gate.Start(kT0, /*notified=*/false);

    const auto owed_at = At(kDisplayColorPolledBackstop + std::chrono::milliseconds{10});
    gate.SetNotified(owed_at, true);
    EXPECT_EQ(gate.Backstop(), kDisplayColorNotifiedBackstop);
    EXPECT_TRUE(gate.TakeDue(owed_at, false));
    // Exactly one read, not a permanently due gate.
    EXPECT_FALSE(gate.TakeDue(owed_at + std::chrono::milliseconds{1}, false));
}

// And the other direction: a subscription lost mid-session (a monitor that went
// away) must put the short cadence back, because nothing else is watching now.
TEST(DisplayColorRecheckGate, LosingTheNotificationRestoresTheShortCadence) {
    DisplayColorRecheckGate gate;
    gate.Start(kT0, /*notified=*/true);
    ASSERT_FALSE(gate.TakeDue(At(kDisplayColorPolledBackstop), false));

    gate.SetNotified(At(kDisplayColorPolledBackstop), false);
    EXPECT_EQ(gate.Backstop(), kDisplayColorPolledBackstop);
    EXPECT_TRUE(gate.TakeDue(At(kDisplayColorPolledBackstop), false));
}

// A transition that changes nothing must not move the phase, or a hub calling
// this on every tick would keep pushing its own backstop out of reach.
TEST(DisplayColorRecheckGate, RepeatingTheSameMechanismChangesNothing) {
    DisplayColorRecheckGate gate;
    gate.Start(kT0, /*notified=*/false);
    for (int ms = 0; ms < kDisplayColorPolledBackstop.count(); ms += 100) {
        gate.SetNotified(At(std::chrono::milliseconds{ms}), false);
    }
    EXPECT_TRUE(gate.TakeDue(At(kDisplayColorPolledBackstop), false));
}

TEST(DisplayColorRecheckGate, ReportsWhichMechanismIsInForce) {
    DisplayColorRecheckGate gate;
    gate.Start(kT0, /*notified=*/false);
    EXPECT_FALSE(gate.Notified());
    gate.SetNotified(kT0, true);
    EXPECT_TRUE(gate.Notified());
}
