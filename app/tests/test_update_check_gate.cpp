// QCR-202. An update check runs on a worker while the GUI thread stays live, so
// the user can change the channel or ask for another check before the answer
// comes back. The engine's UpdateCheckResult carries neither a channel nor a
// request identity, so an answer cannot say which question it belongs to; the
// service used to read its *current* channel and state at completion time and
// publish whatever it found. These cases pin the resolver that fixes that: the
// answer is judged against the immutable context of the operation that asked.

#include <gtest/gtest.h>

#include "services/UpdateCheckGate.h"

namespace exosnap {
namespace {

using exosnap::update::SemVer;
using exosnap::update::UpdateChannel;
using exosnap::update::UpdateCheckResult;
using exosnap::update::UpdateState;

UpdateCheckOperation Operation(std::uint64_t id, UpdateChannel channel, bool verify_reinstall = false) {
    UpdateCheckOperation op;
    op.id = id;
    op.channel = channel;
    op.verify_reinstall = verify_reinstall;
    return op;
}

UpdateState CheckingOn(UpdateChannel channel) {
    UpdateState state;
    state.channel = channel;
    state.checking = true;
    return state;
}

UpdateCheckResult Available(std::uint32_t major, std::uint32_t minor, std::uint32_t patch) {
    UpdateCheckResult result;
    result.update_available = true;
    result.available_version = SemVer{major, minor, patch};
    return result;
}

UpdateCheckResult Failed(const char* message) {
    UpdateCheckResult result;
    result.check_failed = true;
    result.error_message = message;
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// The ordinary case, unchanged in behaviour
// ---------------------------------------------------------------------------

TEST(UpdateCheckGateTest, SingleCheckAdoptsItsOwnResult) {
    const auto op = Operation(1, UpdateChannel::Stable);
    const auto out = ResolveUpdateCheckCompletion(CheckingOn(UpdateChannel::Stable), op, /*active=*/1,
                                                  UpdateChannel::Stable, Available(1, 2, 0));

    EXPECT_EQ(out.verdict, UpdateCompletionVerdict::Adopt);
    EXPECT_TRUE(out.publish);
    EXPECT_TRUE(out.adopt_notes);
    EXPECT_FALSE(out.state.checking);
    EXPECT_EQ(out.state.channel, UpdateChannel::Stable);
    EXPECT_TRUE(out.state.update_available);
    ASSERT_TRUE(out.state.available_version.has_value());
    EXPECT_EQ(out.state.available_version->minor, 2);
}

TEST(UpdateCheckGateTest, AFailedCheckIsAdoptedToo) {
    // A failure is still this operation's answer: it must clear `checking` and
    // land its message, or the card sits on "Checking for updates…" forever.
    const auto op = Operation(7, UpdateChannel::Preview);
    const auto out = ResolveUpdateCheckCompletion(CheckingOn(UpdateChannel::Preview), op, /*active=*/7,
                                                  UpdateChannel::Preview, Failed("no network"));

    EXPECT_EQ(out.verdict, UpdateCompletionVerdict::Adopt);
    EXPECT_FALSE(out.state.checking);
    EXPECT_FALSE(out.state.update_available);
    EXPECT_EQ(out.state.last_error, "no network");
}

TEST(UpdateCheckGateTest, ASuccessfulCheckClearsAPreviousError) {
    UpdateState prior = CheckingOn(UpdateChannel::Stable);
    prior.last_error = "no network";

    const auto out = ResolveUpdateCheckCompletion(prior, Operation(2, UpdateChannel::Stable), /*active=*/2,
                                                  UpdateChannel::Stable, Available(1, 3, 0));

    EXPECT_EQ(out.verdict, UpdateCompletionVerdict::Adopt);
    EXPECT_TRUE(out.state.last_error.empty()) << "last_error describes the most recent check, not an older one";
}

// ---------------------------------------------------------------------------
// Channel change mid-check — the misattribution this gate exists for
// ---------------------------------------------------------------------------

TEST(UpdateCheckGateTest, AStableAnswerIsNotPublishedUnderPreview) {
    // Timeline: a Stable check starts, the user switches to Preview, the Stable
    // request returns. Before the gate, the completion read the service's now
    // mutable channel and published Stable's availability as Preview's.
    const auto op = Operation(1, UpdateChannel::Stable);
    UpdateState prior = CheckingOn(UpdateChannel::Preview); // SetChannel already ran
    const auto out = ResolveUpdateCheckCompletion(prior, op, /*active=*/1, UpdateChannel::Preview, Available(9, 9, 9));

    EXPECT_EQ(out.verdict, UpdateCompletionVerdict::ChannelChanged);
    EXPECT_TRUE(out.publish) << "this operation is still the active one, so it must report itself finished";
    EXPECT_FALSE(out.state.checking);
    EXPECT_EQ(out.state.channel, UpdateChannel::Preview);
    EXPECT_FALSE(out.state.update_available);
    EXPECT_FALSE(out.state.available_version.has_value());
    EXPECT_FALSE(out.adopt_notes) << "release notes for a feed the user left must not reach the What's-new payload";
}

TEST(UpdateCheckGateTest, APreviewAnswerIsNotPublishedUnderStable) {
    const auto op = Operation(4, UpdateChannel::Preview);
    const auto out = ResolveUpdateCheckCompletion(CheckingOn(UpdateChannel::Stable), op, /*active=*/4,
                                                  UpdateChannel::Stable, Available(2, 0, 0));

    EXPECT_EQ(out.verdict, UpdateCompletionVerdict::ChannelChanged);
    EXPECT_EQ(out.state.channel, UpdateChannel::Stable);
    EXPECT_FALSE(out.state.update_available);
}

TEST(UpdateCheckGateTest, AFailureFromAChannelTheUserLeftIsAlsoDiscarded) {
    // Symmetric on purpose: a Preview outage must not paint the Stable card with
    // an error either.
    UpdateState prior = CheckingOn(UpdateChannel::Stable);
    const auto out = ResolveUpdateCheckCompletion(prior, Operation(5, UpdateChannel::Preview), /*active=*/5,
                                                  UpdateChannel::Stable, Failed("preview feed unreachable"));

    EXPECT_EQ(out.verdict, UpdateCompletionVerdict::ChannelChanged);
    EXPECT_FALSE(out.state.checking);
    EXPECT_TRUE(out.state.last_error.empty());
}

// ---------------------------------------------------------------------------
// Stale completion — an older answer arriving after a newer request
// ---------------------------------------------------------------------------

TEST(UpdateCheckGateTest, AnOlderCompletionDoesNotOverwriteANewerOperation) {
    // Operation 1 returns after operation 2 has already started. The old
    // behaviour cleared `checking` here, so the UI claimed the newer check was
    // over and then flipped again when it actually finished.
    UpdateState prior = CheckingOn(UpdateChannel::Stable);
    prior.update_available = true;
    prior.available_version = SemVer{1, 0, 0};

    const auto out = ResolveUpdateCheckCompletion(prior, Operation(1, UpdateChannel::Stable), /*active=*/2,
                                                  UpdateChannel::Stable, Available(3, 0, 0));

    EXPECT_EQ(out.verdict, UpdateCompletionVerdict::SupersededByNewerCheck);
    EXPECT_FALSE(out.publish);
    EXPECT_FALSE(out.adopt_notes);
    EXPECT_TRUE(out.state.checking) << "the newer operation still owns the checking flag";
    ASSERT_TRUE(out.state.available_version.has_value());
    EXPECT_EQ(out.state.available_version->major, 1) << "the newer operation's state must survive untouched";
}

TEST(UpdateCheckGateTest, AnOlderCompletionIsDiscardedEvenWithNoOperationInFlight) {
    // active == 0: the operation was already accounted for (shutdown, or its
    // slot was released). Nothing may be published on its behalf a second time.
    const auto out =
        ResolveUpdateCheckCompletion(CheckingOn(UpdateChannel::Stable), Operation(1, UpdateChannel::Stable),
                                     /*active=*/0, UpdateChannel::Stable, Available(3, 0, 0));

    EXPECT_EQ(out.verdict, UpdateCompletionVerdict::SupersededByNewerCheck);
    EXPECT_FALSE(out.publish);
}

TEST(UpdateCheckGateTest, StalenessOutranksTheChannelCheck) {
    // Both conditions at once. Being stale is the stronger one: the newer
    // operation owns `checking`, so the stale answer must not clear it even
    // though its own channel is also out of date.
    const auto out =
        ResolveUpdateCheckCompletion(CheckingOn(UpdateChannel::Preview), Operation(1, UpdateChannel::Stable),
                                     /*active=*/2, UpdateChannel::Preview, Available(3, 0, 0));

    EXPECT_EQ(out.verdict, UpdateCompletionVerdict::SupersededByNewerCheck);
    EXPECT_TRUE(out.state.checking);
}

// ---------------------------------------------------------------------------
// Selecting a channel invalidates the other channel's answer
// ---------------------------------------------------------------------------

TEST(UpdateCheckGateTest, SelectingAChannelDropsThePreviousChannelsAvailability) {
    UpdateState prior;
    prior.channel = UpdateChannel::Stable;
    prior.update_available = true;
    prior.available_version = SemVer{1, 2, 0};
    prior.last_error = "stale";

    const auto next = ApplyUpdateChannelSelection(prior, UpdateChannel::Preview);

    EXPECT_EQ(next.channel, UpdateChannel::Preview);
    EXPECT_FALSE(next.update_available);
    EXPECT_FALSE(next.available_version.has_value());
    EXPECT_TRUE(next.last_error.empty());
}

TEST(UpdateCheckGateTest, SelectingAChannelLeavesAnInFlightCheckMarkedAsChecking) {
    UpdateState prior = CheckingOn(UpdateChannel::Stable);
    const auto next = ApplyUpdateChannelSelection(prior, UpdateChannel::Preview);
    EXPECT_TRUE(next.checking) << "the running check still has to report itself finished";
}

TEST(UpdateCheckGateTest, ReselectingTheSameChannelChangesNothing) {
    UpdateState prior;
    prior.channel = UpdateChannel::Preview;
    prior.update_available = true;
    prior.available_version = SemVer{1, 2, 0};

    const auto next = ApplyUpdateChannelSelection(prior, UpdateChannel::Preview);

    EXPECT_TRUE(next.update_available) << "a no-op selection must not discard a valid answer";
    ASSERT_TRUE(next.available_version.has_value());
    EXPECT_EQ(next.available_version->minor, 2);
}

// ---------------------------------------------------------------------------
// The verification-reinstall mode travels with the operation (ADR 0055)
// ---------------------------------------------------------------------------

TEST(UpdateCheckGateTest, TheOperationCarriesItsOwnVerifyReinstallFlag) {
    // The context exists so that a mode toggled mid-check cannot retroactively
    // change what the running check was allowed to offer. The resolver never
    // reads the live flag; this pins that the snapshot is part of the context.
    const auto op = Operation(1, UpdateChannel::Stable, /*verify_reinstall=*/true);
    EXPECT_TRUE(op.verify_reinstall);

    UpdateCheckResult result = Available(0, 9, 0);
    result.verification_reinstall = true;
    const auto out =
        ResolveUpdateCheckCompletion(CheckingOn(UpdateChannel::Stable), op, 1, UpdateChannel::Stable, result);
    EXPECT_EQ(out.verdict, UpdateCompletionVerdict::Adopt);
    EXPECT_TRUE(out.state.update_available);
}

} // namespace exosnap
