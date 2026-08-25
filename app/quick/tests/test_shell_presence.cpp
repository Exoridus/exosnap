#include "models/RecordingPulse.h"
#include "models/ShellPresence.h"
#include "models/TaskbarProgressLease.h"
#include "viewmodels/RecordViewModel.h"

#include <gtest/gtest.h>

#include <set>

using exosnap::kRecordingPulseFrameCount;
using exosnap::kShellButtonIdPauseResume;
using exosnap::kShellButtonIdRecord;
using exosnap::kShellButtonIdStop;
using exosnap::NextRecordingPulseFrame;
using exosnap::ProjectShellPresence;
using exosnap::RecordingPulseIntensity;
using exosnap::ResolveShellCommand;
using exosnap::ShellAction;
using exosnap::ShellButton;
using exosnap::ShellButtonFor;
using exosnap::ShellButtonFromCommandId;
using exosnap::ShellIconState;
using exosnap::ShellPhase;
using exosnap::ShellPresenceInput;
using exosnap::ShellPresenceState;
using exosnap::TaskbarProgressLedger;
using exosnap::TaskbarProgressOwner;
using exosnap::TaskbarProgressState;
using exosnap::UiRecordingState;

namespace {

// The projection's inputs as the RecordViewModel would compute them for a state.
// Assembled here rather than by calling the view model so what is under test is
// the projection, not the model's own predicates.
ShellPresenceInput InputFor(UiRecordingState state) {
    ShellPresenceInput input;
    input.state = state;
    input.can_start =
        state == UiRecordingState::Ready || state == UiRecordingState::Completed || state == UiRecordingState::Failed;
    input.can_stop = state == UiRecordingState::Recording || state == UiRecordingState::Paused;
    input.can_pause = state == UiRecordingState::Recording;
    input.can_resume = state == UiRecordingState::Paused;
    return input;
}

ShellPresenceState StateFor(UiRecordingState state) {
    return ProjectShellPresence(InputFor(state));
}

} // namespace

// -- Projection: phase ------------------------------------------------------

TEST(ShellPresenceProjection, ReadyIsIdle) {
    const ShellPresenceState state = StateFor(UiRecordingState::Ready);
    EXPECT_EQ(state.phase, ShellPhase::Idle);
    EXPECT_EQ(state.icon_state, ShellIconState::Idle);
    EXPECT_TRUE(state.can_start);
    EXPECT_FALSE(state.recording);
    EXPECT_FALSE(state.paused);
    EXPECT_FALSE(state.busy);
    EXPECT_FALSE(state.saved);
}

TEST(ShellPresenceProjection, RecordingIsRecording) {
    const ShellPresenceState state = StateFor(UiRecordingState::Recording);
    EXPECT_EQ(state.phase, ShellPhase::Recording);
    EXPECT_EQ(state.icon_state, ShellIconState::Recording);
    EXPECT_TRUE(state.recording);
    EXPECT_TRUE(state.can_pause);
    EXPECT_TRUE(state.can_stop);
    EXPECT_FALSE(state.can_resume);
    EXPECT_FALSE(state.busy);
}

TEST(ShellPresenceProjection, PausedIsPaused) {
    const ShellPresenceState state = StateFor(UiRecordingState::Paused);
    EXPECT_EQ(state.phase, ShellPhase::Paused);
    EXPECT_EQ(state.icon_state, ShellIconState::Paused);
    EXPECT_TRUE(state.paused);
    EXPECT_TRUE(state.can_resume);
    EXPECT_TRUE(state.can_stop);
    EXPECT_FALSE(state.can_pause);
}

TEST(ShellPresenceProjection, ArmedFromRecoveryReadsAsPaused) {
    // The transport already treats it as Paused; the shell must not invent a
    // fourth badge for a state the user was told is paused.
    EXPECT_EQ(StateFor(UiRecordingState::ArmedFromRecovery).phase, ShellPhase::Paused);
    EXPECT_EQ(StateFor(UiRecordingState::ArmedFromRecovery).icon_state, ShellIconState::Paused);
}

TEST(ShellPresenceProjection, PreparingIsBusyAndAlreadyReadsAsRecording) {
    const ShellPresenceState state = StateFor(UiRecordingState::Preparing);
    EXPECT_EQ(state.phase, ShellPhase::Preparing);
    // The capture is committed from the user's point of view, which is what the
    // tray has always shown.
    EXPECT_EQ(state.icon_state, ShellIconState::Recording);
    EXPECT_TRUE(state.busy);
    EXPECT_FALSE(state.can_start);
}

TEST(ShellPresenceProjection, CountdownIsPreparing) {
    const ShellPresenceState state = StateFor(UiRecordingState::Countdown);
    EXPECT_EQ(state.phase, ShellPhase::Preparing);
    EXPECT_EQ(state.icon_state, ShellIconState::Recording);
    EXPECT_TRUE(state.busy);
}

TEST(ShellPresenceProjection, StoppingAndSavingAreFinalizing) {
    for (const UiRecordingState state : {UiRecordingState::Stopping, UiRecordingState::Saving}) {
        const ShellPresenceState projected = StateFor(state);
        EXPECT_EQ(projected.phase, ShellPhase::Finalizing);
        EXPECT_TRUE(projected.busy);
        EXPECT_FALSE(projected.can_stop);
        EXPECT_FALSE(projected.can_start);
    }
}

TEST(ShellPresenceProjection, BlockedAndLoadingCannotStart) {
    for (const UiRecordingState state :
         {UiRecordingState::Blocked, UiRecordingState::LoadingCapabilities, UiRecordingState::RegionSelecting}) {
        const ShellPresenceState projected = StateFor(state);
        EXPECT_EQ(projected.phase, ShellPhase::Blocked);
        EXPECT_EQ(projected.icon_state, ShellIconState::Idle);
        EXPECT_FALSE(projected.can_start);
        EXPECT_FALSE(projected.busy);
    }
}

TEST(ShellPresenceProjection, CompletedWithoutADwellIsPlainIdle) {
    const ShellPresenceState state = StateFor(UiRecordingState::Completed);
    EXPECT_EQ(state.phase, ShellPhase::Idle);
    EXPECT_EQ(state.icon_state, ShellIconState::Idle);
    EXPECT_TRUE(state.can_start);
}

TEST(ShellPresenceProjection, CompletedWithADwellIsSaved) {
    ShellPresenceInput input = InputFor(UiRecordingState::Completed);
    input.saved_dwell_active = true;
    const ShellPresenceState state = ProjectShellPresence(input);
    EXPECT_EQ(state.phase, ShellPhase::Saved);
    EXPECT_EQ(state.icon_state, ShellIconState::Saved);
    EXPECT_TRUE(state.saved);
    // Saved is a badge, not a lock: the next recording starts from it.
    EXPECT_TRUE(state.can_start);
}

TEST(ShellPresenceProjection, ADwellCannotPaintALiveRecordingGreen) {
    // The structural half of the stale-timeout guard: even handed a dwell flag
    // that outlived its recording, the projection refuses it in a live state.
    ShellPresenceInput input = InputFor(UiRecordingState::Recording);
    input.saved_dwell_active = true;
    const ShellPresenceState state = ProjectShellPresence(input);
    EXPECT_EQ(state.phase, ShellPhase::Recording);
    EXPECT_EQ(state.icon_state, ShellIconState::Recording);
    EXPECT_FALSE(state.saved);
}

TEST(ShellPresenceProjection, AFailedRecordingIsNotSaved) {
    ShellPresenceInput input = InputFor(UiRecordingState::Failed);
    input.saved_dwell_active = true;
    const ShellPresenceState state = ProjectShellPresence(input);
    EXPECT_EQ(state.phase, ShellPhase::Idle);
    EXPECT_FALSE(state.saved);
}

TEST(ShellPresenceProjection, ABlockedResultSurfaceKeepsTheIdlePhaseWithoutTheAffordance) {
    // A result panel that still owns the session reports Completed with
    // can_start false. The phase must stay Idle -- there is nothing running --
    // while the affordance goes away.
    ShellPresenceInput input = InputFor(UiRecordingState::Completed);
    input.can_start = false;
    const ShellPresenceState state = ProjectShellPresence(input);
    EXPECT_EQ(state.phase, ShellPhase::Idle);
    EXPECT_FALSE(state.can_start);
}

TEST(ShellPresenceProjection, EqualityComparesTheWholeProjection) {
    const ShellPresenceState recording = StateFor(UiRecordingState::Recording);
    EXPECT_TRUE(recording == StateFor(UiRecordingState::Recording));
    EXPECT_FALSE(recording != StateFor(UiRecordingState::Recording));
    EXPECT_TRUE(recording != StateFor(UiRecordingState::Paused));
}

// -- Thumbnail buttons ------------------------------------------------------

TEST(ShellButtons, IdleOffersOnlyRecord) {
    const ShellPresenceState state = StateFor(UiRecordingState::Ready);
    const auto record = ShellButtonFor(ShellButton::Record, state);
    EXPECT_TRUE(record.visible);
    EXPECT_TRUE(record.enabled);
    EXPECT_EQ(record.action, ShellAction::Start);
    EXPECT_FALSE(ShellButtonFor(ShellButton::PauseResume, state).visible);
    EXPECT_FALSE(ShellButtonFor(ShellButton::Stop, state).visible);
}

TEST(ShellButtons, RecordingOffersPauseAndStop) {
    const ShellPresenceState state = StateFor(UiRecordingState::Recording);
    EXPECT_FALSE(ShellButtonFor(ShellButton::Record, state).visible);
    const auto pause = ShellButtonFor(ShellButton::PauseResume, state);
    EXPECT_TRUE(pause.visible);
    EXPECT_TRUE(pause.enabled);
    EXPECT_EQ(pause.action, ShellAction::Pause);
    const auto stop = ShellButtonFor(ShellButton::Stop, state);
    EXPECT_TRUE(stop.visible);
    EXPECT_TRUE(stop.enabled);
    EXPECT_EQ(stop.action, ShellAction::Stop);
}

TEST(ShellButtons, PausedTurnsTheSameSlotIntoResume) {
    const ShellPresenceState state = StateFor(UiRecordingState::Paused);
    const auto resume = ShellButtonFor(ShellButton::PauseResume, state);
    EXPECT_TRUE(resume.visible);
    EXPECT_TRUE(resume.enabled);
    EXPECT_EQ(resume.action, ShellAction::Resume);
    EXPECT_TRUE(ShellButtonFor(ShellButton::Stop, state).visible);
    EXPECT_FALSE(ShellButtonFor(ShellButton::Record, state).visible);
}

TEST(ShellButtons, TransitionalStatesShowARecordButtonThatIsGreyedRatherThanGone) {
    for (const UiRecordingState state : {UiRecordingState::Preparing, UiRecordingState::Countdown,
                                         UiRecordingState::Stopping, UiRecordingState::Saving}) {
        const ShellPresenceState projected = StateFor(state);
        const auto record = ShellButtonFor(ShellButton::Record, projected);
        EXPECT_TRUE(record.visible) << static_cast<int>(state);
        EXPECT_FALSE(record.enabled) << static_cast<int>(state);
        EXPECT_FALSE(ShellButtonFor(ShellButton::PauseResume, projected).visible);
        EXPECT_FALSE(ShellButtonFor(ShellButton::Stop, projected).visible);
    }
}

TEST(ShellButtons, ABlockedResultSurfaceGreysRecordRatherThanHidingIt) {
    ShellPresenceInput input = InputFor(UiRecordingState::Completed);
    input.can_start = false;
    const ShellPresenceState state = ProjectShellPresence(input);
    const auto record = ShellButtonFor(ShellButton::Record, state);
    EXPECT_TRUE(record.visible);
    EXPECT_FALSE(record.enabled);
    // The button keeps its meaning while it is greyed -- that is what its icon
    // and tooltip are drawn from -- and the dispatch refuses it anyway.
    EXPECT_EQ(record.action, ShellAction::Start);
    EXPECT_EQ(ResolveShellCommand(kShellButtonIdRecord, state), ShellAction::None);
}

// -- Command dispatch -------------------------------------------------------

TEST(ShellCommandDispatch, TheThreeIdsAreDistinct) {
    const std::set<int> ids{kShellButtonIdRecord, kShellButtonIdPauseResume, kShellButtonIdStop};
    EXPECT_EQ(ids.size(), 3u);
}

TEST(ShellCommandDispatch, KnownIdsMapToTheirButton) {
    ShellButton button = ShellButton::Stop;
    ASSERT_TRUE(ShellButtonFromCommandId(kShellButtonIdRecord, button));
    EXPECT_EQ(button, ShellButton::Record);
    ASSERT_TRUE(ShellButtonFromCommandId(kShellButtonIdPauseResume, button));
    EXPECT_EQ(button, ShellButton::PauseResume);
    ASSERT_TRUE(ShellButtonFromCommandId(kShellButtonIdStop, button));
    EXPECT_EQ(button, ShellButton::Stop);
}

TEST(ShellCommandDispatch, AnUnknownIdIsNotOurs) {
    ShellButton button = ShellButton::Record;
    EXPECT_FALSE(ShellButtonFromCommandId(0, button));
    EXPECT_FALSE(ShellButtonFromCommandId(kShellButtonIdStop + 1, button));
}

TEST(ShellCommandDispatch, RecordIdStartsFromIdle) {
    EXPECT_EQ(ResolveShellCommand(kShellButtonIdRecord, StateFor(UiRecordingState::Ready)), ShellAction::Start);
}

TEST(ShellCommandDispatch, TheSharedIdPausesWhileRecordingAndResumesWhilePaused) {
    EXPECT_EQ(ResolveShellCommand(kShellButtonIdPauseResume, StateFor(UiRecordingState::Recording)),
              ShellAction::Pause);
    EXPECT_EQ(ResolveShellCommand(kShellButtonIdPauseResume, StateFor(UiRecordingState::Paused)), ShellAction::Resume);
}

TEST(ShellCommandDispatch, StopIdStopsWhileRecording) {
    EXPECT_EQ(ResolveShellCommand(kShellButtonIdStop, StateFor(UiRecordingState::Recording)), ShellAction::Stop);
}

TEST(ShellCommandDispatch, AnUnknownIdAsksForNoProductAction) {
    EXPECT_EQ(ResolveShellCommand(0x1234, StateFor(UiRecordingState::Recording)), ShellAction::None);
}

TEST(ShellCommandDispatch, AClickCannotBypassTheStatesOwnPolicy) {
    // A thumbnail strip Explorer has not repainted yet still carries the ids of
    // the previous state. Every one of them has to resolve to nothing.
    const ShellPresenceState finalizing = StateFor(UiRecordingState::Saving);
    EXPECT_EQ(ResolveShellCommand(kShellButtonIdRecord, finalizing), ShellAction::None);
    EXPECT_EQ(ResolveShellCommand(kShellButtonIdPauseResume, finalizing), ShellAction::None);
    EXPECT_EQ(ResolveShellCommand(kShellButtonIdStop, finalizing), ShellAction::None);

    const ShellPresenceState idle = StateFor(UiRecordingState::Ready);
    EXPECT_EQ(ResolveShellCommand(kShellButtonIdPauseResume, idle), ShellAction::None);
    EXPECT_EQ(ResolveShellCommand(kShellButtonIdStop, idle), ShellAction::None);
}

// -- Pulse ------------------------------------------------------------------

TEST(RecordingPulseMath, PhaseWrapsDeterministically) {
    int frame = 0;
    for (int i = 0; i < kRecordingPulseFrameCount; ++i)
        frame = NextRecordingPulseFrame(frame);
    EXPECT_EQ(frame, 0);
}

TEST(RecordingPulseMath, EveryFrameIsVisitedOncePerPeriod) {
    std::set<int> seen;
    int frame = 0;
    for (int i = 0; i < kRecordingPulseFrameCount; ++i) {
        seen.insert(frame);
        frame = NextRecordingPulseFrame(frame);
    }
    EXPECT_EQ(static_cast<int>(seen.size()), kRecordingPulseFrameCount);
}

TEST(RecordingPulseMath, AnOutOfRangeFrameComesBackInsideTheArray) {
    // The frame indexes an icon array, so a bad value has to be corrected here
    // rather than reaching a shell call.
    for (const int bogus : {-1, -7, kRecordingPulseFrameCount, kRecordingPulseFrameCount * 3}) {
        const int next = NextRecordingPulseFrame(bogus);
        EXPECT_GE(next, 0) << bogus;
        EXPECT_LT(next, kRecordingPulseFrameCount) << bogus;
    }
    EXPECT_DOUBLE_EQ(RecordingPulseIntensity(-1), RecordingPulseIntensity(0));
    EXPECT_DOUBLE_EQ(RecordingPulseIntensity(kRecordingPulseFrameCount), RecordingPulseIntensity(0));
}

TEST(RecordingPulseMath, IntensityIsATriangleFromTroughToPeakAndBack) {
    EXPECT_DOUBLE_EQ(RecordingPulseIntensity(0), 0.0);
    EXPECT_DOUBLE_EQ(RecordingPulseIntensity(1), 0.5);
    EXPECT_DOUBLE_EQ(RecordingPulseIntensity(2), 1.0);
    EXPECT_DOUBLE_EQ(RecordingPulseIntensity(3), 0.5);
}

TEST(TaskbarProgressLedgerTest, StartsUnheldAndSilent) {
    TaskbarProgressLedger ledger;
    EXPECT_FALSE(ledger.held());
    EXPECT_EQ(ledger.owner(), TaskbarProgressOwner::None);
    EXPECT_EQ(ledger.state(), TaskbarProgressState::NoProgress);
}

TEST(TaskbarProgressLedgerTest, AcquireTakesTheBarIndeterminate) {
    TaskbarProgressLedger ledger;
    const auto lease = ledger.acquire(TaskbarProgressOwner::RecordingSave);
    ASSERT_TRUE(lease.valid());
    EXPECT_TRUE(ledger.held());
    EXPECT_EQ(ledger.owner(), TaskbarProgressOwner::RecordingSave);
    // A producer that has not reported a fraction yet is running, not idle.
    EXPECT_EQ(ledger.state(), TaskbarProgressState::Indeterminate);
}

TEST(TaskbarProgressLedgerTest, UpdatePublishesAFraction) {
    TaskbarProgressLedger ledger;
    const auto lease = ledger.acquire(TaskbarProgressOwner::EditExport);
    EXPECT_TRUE(ledger.update(lease, 0.5));
    EXPECT_EQ(ledger.state(), TaskbarProgressState::Normal);
    EXPECT_DOUBLE_EQ(ledger.fraction(), 0.5);
}

TEST(TaskbarProgressLedgerTest, UpdateIsGatedToAWholePercent) {
    TaskbarProgressLedger ledger;
    const auto lease = ledger.acquire(TaskbarProgressOwner::EditExport);
    EXPECT_TRUE(ledger.update(lease, 0.5));
    EXPECT_FALSE(ledger.update(lease, 0.5004));
    EXPECT_TRUE(ledger.update(lease, 0.51));
}

TEST(TaskbarProgressLedgerTest, AFractionIsClampedIntoRange) {
    TaskbarProgressLedger ledger;
    const auto lease = ledger.acquire(TaskbarProgressOwner::EditExport);
    ledger.update(lease, 7.0);
    EXPECT_DOUBLE_EQ(ledger.fraction(), 1.0);
    ledger.update(lease, -3.0);
    EXPECT_DOUBLE_EQ(ledger.fraction(), 0.0);
}

TEST(TaskbarProgressLedgerTest, FinishReleasesAndClearsTheBar) {
    TaskbarProgressLedger ledger;
    const auto lease = ledger.acquire(TaskbarProgressOwner::RecordingSave);
    ledger.update(lease, 0.4);
    EXPECT_TRUE(ledger.finish(lease));
    EXPECT_FALSE(ledger.held());
    EXPECT_EQ(ledger.state(), TaskbarProgressState::NoProgress);
}

TEST(TaskbarProgressLedgerTest, FailureReleasesButLeavesTheErrorOnScreen) {
    TaskbarProgressLedger ledger;
    const auto lease = ledger.acquire(TaskbarProgressOwner::RecoveryFinish);
    EXPECT_TRUE(ledger.fail(lease));
    EXPECT_FALSE(ledger.held());
    EXPECT_EQ(ledger.state(), TaskbarProgressState::Error);
}

TEST(TaskbarProgressLedgerTest, CancelAndTeardownBothRelease) {
    TaskbarProgressLedger ledger;
    auto lease = ledger.acquire(TaskbarProgressOwner::EditExport);
    EXPECT_TRUE(ledger.cancel(lease));
    EXPECT_FALSE(ledger.held());
    EXPECT_EQ(ledger.state(), TaskbarProgressState::NoProgress);

    lease = ledger.acquire(TaskbarProgressOwner::EditExport);
    EXPECT_TRUE(ledger.release(lease));
    EXPECT_FALSE(ledger.held());
}

TEST(TaskbarProgressLedgerTest, ASecondProducerIsRefusedRatherThanInterleaved) {
    TaskbarProgressLedger ledger;
    const auto first = ledger.acquire(TaskbarProgressOwner::RecordingSave);
    ledger.update(first, 0.6);

    const auto second = ledger.acquire(TaskbarProgressOwner::EditExport);
    EXPECT_FALSE(second.valid());
    EXPECT_EQ(ledger.owner(), TaskbarProgressOwner::RecordingSave);

    // And a refused producer publishing anyway moves nothing.
    EXPECT_FALSE(ledger.update(second, 0.1));
    EXPECT_DOUBLE_EQ(ledger.fraction(), 0.6);
    EXPECT_FALSE(ledger.fail(second));
    EXPECT_EQ(ledger.state(), TaskbarProgressState::Normal);
    EXPECT_FALSE(ledger.finish(second));
    EXPECT_TRUE(ledger.held());
}

TEST(TaskbarProgressLedgerTest, AStalePreviousOwnerCannotUpdateTheNextOne) {
    TaskbarProgressLedger ledger;
    const auto stale = ledger.acquire(TaskbarProgressOwner::RecordingSave);
    ledger.finish(stale);

    const auto current = ledger.acquire(TaskbarProgressOwner::EditExport);
    ledger.update(current, 0.2);

    EXPECT_FALSE(ledger.update(stale, 0.9));
    EXPECT_DOUBLE_EQ(ledger.fraction(), 0.2);
    EXPECT_EQ(ledger.owner(), TaskbarProgressOwner::EditExport);
    EXPECT_FALSE(ledger.finish(stale));
    EXPECT_TRUE(ledger.held());
}

TEST(TaskbarProgressLedgerTest, TheSameOwnerKindTwiceStillNeedsTheCurrentGeneration) {
    // The case a pointer-shaped ownership check cannot see: two remuxes in one
    // session are the same producer, and the first one's late callback must
    // still not move the second one's bar.
    TaskbarProgressLedger ledger;
    const auto first = ledger.acquire(TaskbarProgressOwner::RecordingSave);
    ledger.finish(first);
    const auto second = ledger.acquire(TaskbarProgressOwner::RecordingSave);
    ledger.update(second, 0.3);

    EXPECT_FALSE(ledger.update(first, 0.95));
    EXPECT_DOUBLE_EQ(ledger.fraction(), 0.3);
    EXPECT_TRUE(first != second);
}

TEST(TaskbarProgressLedgerTest, IndeterminateIsPublishableMidOperation) {
    TaskbarProgressLedger ledger;
    const auto lease = ledger.acquire(TaskbarProgressOwner::RecoveryFinish);
    ledger.update(lease, 0.3);
    EXPECT_TRUE(ledger.setIndeterminate(lease));
    EXPECT_EQ(ledger.state(), TaskbarProgressState::Indeterminate);
    EXPECT_FALSE(ledger.setIndeterminate(lease));
}

TEST(TaskbarProgressLedgerTest, AcquireAfterAFailureClearsTheError) {
    TaskbarProgressLedger ledger;
    const auto failed = ledger.acquire(TaskbarProgressOwner::RecordingSave);
    ledger.fail(failed);
    ASSERT_EQ(ledger.state(), TaskbarProgressState::Error);

    const auto next = ledger.acquire(TaskbarProgressOwner::EditExport);
    ASSERT_TRUE(next.valid());
    EXPECT_EQ(ledger.state(), TaskbarProgressState::Indeterminate);
}
