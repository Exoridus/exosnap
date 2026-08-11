// Close-guard policy and its asynchronous routing.
//
// The Widgets shell could express this as a chain of `QMessageBox::exec()`
// calls and read each answer on the next line. The Quick shell cannot, so the
// interesting property is no longer "does the right dialog appear" but "does
// the chain still hold when every answer arrives one event loop later" —
// especially the export-then-recording fall-through, which the Widgets version
// got for free by not returning early.

#include "ShellAdapter.h"
#include "models/CloseGuardPolicy.h"

#include <gtest/gtest.h>

using exosnap::CloseGuardKind;
using exosnap::CloseGuardState;
using exosnap::EvaluateCloseGuard;
using exosnap::quick::ShellAdapter;

namespace {

TEST(CloseGuardPolicy, NothingInFlightAllowsClose) {
    EXPECT_EQ(EvaluateCloseGuard(CloseGuardState{}).kind, CloseGuardKind::Allow);
}

TEST(CloseGuardPolicy, FinalizingBlocksWithoutAPrompt) {
    CloseGuardState state;
    state.finalizing = true;
    const auto prompt = EvaluateCloseGuard(state);
    EXPECT_EQ(prompt.kind, CloseGuardKind::BlockSilently);
    // Aborting a container finalize corrupts the file, so there is deliberately
    // no proceeding option to label.
    EXPECT_TRUE(prompt.proceed_label.isEmpty());
}

TEST(CloseGuardPolicy, FinalizeOutranksEveryOtherGuard) {
    CloseGuardState state;
    state.finalizing = true;
    state.remuxing = true;
    state.exporting = true;
    state.recording = true;
    EXPECT_EQ(EvaluateCloseGuard(state).kind, CloseGuardKind::BlockSilently);
}

TEST(CloseGuardPolicy, RemuxOutranksExportAndRecording) {
    CloseGuardState state;
    state.remuxing = true;
    state.exporting = true;
    state.recording = true;
    EXPECT_EQ(EvaluateCloseGuard(state).kind, CloseGuardKind::ConfirmRemux);
}

TEST(CloseGuardPolicy, ExportOutranksRecording) {
    CloseGuardState state;
    state.exporting = true;
    state.recording = true;
    EXPECT_EQ(EvaluateCloseGuard(state).kind, CloseGuardKind::ConfirmExport);
}

TEST(CloseGuardPolicy, EveryPromptDefaultsToKeepingTheWindowOpen) {
    for (CloseGuardState state :
         {CloseGuardState{false, true, false, false}, CloseGuardState{false, false, true, false},
          CloseGuardState{false, false, false, true}}) {
        EXPECT_TRUE(EvaluateCloseGuard(state).default_is_cancel);
    }
}

// --- ShellAdapter routing -------------------------------------------------

// Counts emissions of one signal. Hand-rolled rather than QSignalSpy so this
// binary keeps the repo's existing "no Qt6::Test dependency in gtest targets"
// convention (see app/tests/test_audio_device_notifier.cpp).
class SignalCounter {
  public:
    template <typename Sender, typename Signal> SignalCounter(Sender* sender, Signal signal) {
        QObject::connect(sender, signal, sender, [this]() { ++count_; });
    }
    [[nodiscard]] int count() const noexcept {
        return count_;
    }

  private:
    int count_ = 0;
};

class ShellAdapterTest : public ::testing::Test {
  protected:
    void SetUp() override {
        adapter_.setStateProvider([this]() { return state_; });
    }

    CloseGuardState state_;
    ShellAdapter adapter_;
};

TEST_F(ShellAdapterTest, IdleCloseIsApprovedImmediately) {
    EXPECT_TRUE(adapter_.requestClose());
    EXPECT_FALSE(adapter_.closeGuardActive());
}

TEST_F(ShellAdapterTest, MissingProviderFailsOpen) {
    ShellAdapter unwired;
    // A window nobody can close is worse than one that closes without a guard.
    EXPECT_TRUE(unwired.requestClose());
}

TEST_F(ShellAdapterTest, FinalizingRefusesWithoutShowingAPrompt) {
    state_.finalizing = true;
    EXPECT_FALSE(adapter_.requestClose());
    EXPECT_FALSE(adapter_.closeGuardActive());
}

TEST_F(ShellAdapterTest, RecordingPromptsAndCancelKeepsWindowOpen) {
    state_.recording = true;
    SignalCounter approved(&adapter_, &ShellAdapter::closeApproved);

    EXPECT_FALSE(adapter_.requestClose());
    EXPECT_TRUE(adapter_.closeGuardActive());
    EXPECT_EQ(adapter_.closeGuardProceedLabel(), QStringLiteral("Stop recording and close"));

    adapter_.cancelCloseGuard();
    EXPECT_FALSE(adapter_.closeGuardActive());
    EXPECT_EQ(approved.count(), 0);
}

TEST_F(ShellAdapterTest, ConfirmingRecordingStopsItAndApprovesTheClose) {
    state_.recording = true;
    SignalCounter stop(&adapter_, &ShellAdapter::stopRecordingRequested);
    SignalCounter approved(&adapter_, &ShellAdapter::closeApproved);

    EXPECT_FALSE(adapter_.requestClose());
    adapter_.confirmCloseGuard();

    EXPECT_EQ(stop.count(), 1);
    // The stop is asynchronous, so the state provider still reports a running
    // recording here. Approval must not wait for it.
    EXPECT_EQ(approved.count(), 1);
    EXPECT_FALSE(adapter_.closeGuardActive());
}

TEST_F(ShellAdapterTest, ExportConfirmFallsThroughToTheRecordingGuard) {
    state_.exporting = true;
    state_.recording = true;
    SignalCounter cancel_export(&adapter_, &ShellAdapter::cancelExportRequested);
    SignalCounter approved(&adapter_, &ShellAdapter::closeApproved);

    EXPECT_FALSE(adapter_.requestClose());
    EXPECT_EQ(adapter_.closeGuardProceedLabel(), QStringLiteral("Cancel export and close"));

    adapter_.confirmCloseGuard();
    EXPECT_EQ(cancel_export.count(), 1);
    // This is the case the Widgets chain handled by NOT returning early.
    EXPECT_TRUE(adapter_.closeGuardActive());
    EXPECT_EQ(adapter_.closeGuardProceedLabel(), QStringLiteral("Stop recording and close"));
    EXPECT_EQ(approved.count(), 0);

    adapter_.confirmCloseGuard();
    EXPECT_EQ(approved.count(), 1);
}

TEST_F(ShellAdapterTest, CancellingClearsWaiversSoTheNextAttemptAsksAgain) {
    state_.exporting = true;
    state_.recording = true;

    EXPECT_FALSE(adapter_.requestClose());
    adapter_.confirmCloseGuard(); // waives the export, now asking about recording
    adapter_.cancelCloseGuard();  // user backs out entirely

    // A second attempt must re-ask about the export: the user never agreed to
    // close, so the earlier waiver carries no consent forward.
    EXPECT_FALSE(adapter_.requestClose());
    EXPECT_EQ(adapter_.closeGuardProceedLabel(), QStringLiteral("Cancel export and close"));
}

TEST_F(ShellAdapterTest, ConfirmWithoutAnActivePromptIsANoOp) {
    SignalCounter approved(&adapter_, &ShellAdapter::closeApproved);
    adapter_.confirmCloseGuard();
    EXPECT_EQ(approved.count(), 0);
}

TEST_F(ShellAdapterTest, FinalizeStartedWhileThePromptWasUpKeepsTheWindowOpen) {
    state_.remuxing = true;
    SignalCounter approved(&adapter_, &ShellAdapter::closeApproved);
    EXPECT_FALSE(adapter_.requestClose());

    // The remux completed into a finalize while the dialog was on screen.
    state_.remuxing = false;
    state_.finalizing = true;
    adapter_.confirmCloseGuard();

    EXPECT_EQ(approved.count(), 0);
    EXPECT_FALSE(adapter_.closeGuardActive());
}

// ---------------------------------------------------------------------------
// Close-to-tray
// ---------------------------------------------------------------------------
// The tray branch sits AHEAD of the guards, which is the whole point: hiding
// tears nothing down, so warning about a running recording before a hide would
// contradict the preference the user switched on. The guards must still be
// reachable — a tray "Quit" is a real quit.

TEST_F(ShellAdapterTest, HideToTrayShortCircuitsTheCloseWithoutApprovingIt) {
    SignalCounter hide(&adapter_, &ShellAdapter::hideToTrayRequested);
    SignalCounter approved(&adapter_, &ShellAdapter::closeApproved);
    adapter_.setHideToTrayProvider([]() { return true; });

    EXPECT_FALSE(adapter_.requestClose());
    EXPECT_EQ(hide.count(), 1);
    EXPECT_EQ(approved.count(), 0);
    EXPECT_FALSE(adapter_.closeGuardActive());
}

TEST_F(ShellAdapterTest, HideToTrayNeverAsksAboutARunningRecording) {
    state_.recording = true;
    SignalCounter hide(&adapter_, &ShellAdapter::hideToTrayRequested);
    SignalCounter stop(&adapter_, &ShellAdapter::stopRecordingRequested);
    adapter_.setHideToTrayProvider([]() { return true; });

    EXPECT_FALSE(adapter_.requestClose());
    EXPECT_EQ(hide.count(), 1);
    EXPECT_FALSE(adapter_.closeGuardActive());
    // The recording is meant to keep running behind the hidden window.
    EXPECT_EQ(stop.count(), 0);
}

TEST_F(ShellAdapterTest, HideToTrayEvenSkipsTheFinalizeBlock) {
    // Nothing is being torn down, so the reason the finalize guard exists — a
    // half-written container — cannot arise.
    state_.finalizing = true;
    SignalCounter hide(&adapter_, &ShellAdapter::hideToTrayRequested);
    adapter_.setHideToTrayProvider([]() { return true; });

    EXPECT_FALSE(adapter_.requestClose());
    EXPECT_EQ(hide.count(), 1);
}

TEST_F(ShellAdapterTest, ProviderDecliningLeavesTheGuardsInCharge) {
    state_.recording = true;
    SignalCounter hide(&adapter_, &ShellAdapter::hideToTrayRequested);
    adapter_.setHideToTrayProvider([]() { return false; });

    EXPECT_FALSE(adapter_.requestClose());
    EXPECT_EQ(hide.count(), 0);
    EXPECT_TRUE(adapter_.closeGuardActive());
    EXPECT_EQ(adapter_.closeGuardProceedLabel(), QStringLiteral("Stop recording and close"));
}

TEST_F(ShellAdapterTest, ForceQuitAfterAHideStillReachesTheGuards) {
    // Exactly the tray sequence: close once (hides), then "Quit" from the tray
    // menu, which flips the provider for that one attempt.
    state_.recording = true;
    bool force_quit = false;
    adapter_.setHideToTrayProvider([&force_quit]() { return !force_quit; });

    EXPECT_FALSE(adapter_.requestClose());
    EXPECT_FALSE(adapter_.closeGuardActive());

    force_quit = true;
    EXPECT_FALSE(adapter_.requestClose());
    EXPECT_TRUE(adapter_.closeGuardActive());
}

TEST_F(ShellAdapterTest, HideDiscardsAPromptLeftOverFromAnAbandonedAttempt) {
    // A guard was raised, the user walked away, and the next close resolved to a
    // hide. Leaving the dialog standing would reappear over a window that is no
    // longer closing, and its stale waivers would carry into the next attempt.
    state_.recording = true;
    EXPECT_FALSE(adapter_.requestClose());
    ASSERT_TRUE(adapter_.closeGuardActive());

    adapter_.setHideToTrayProvider([]() { return true; });
    EXPECT_FALSE(adapter_.requestClose());
    EXPECT_FALSE(adapter_.closeGuardActive());
}

TEST_F(ShellAdapterTest, NoProviderMeansNoTrayBranchAtAll) {
    // The platform reported no system tray, so nothing set a provider. Closing
    // must behave exactly as it did before close-to-tray existed.
    SignalCounter hide(&adapter_, &ShellAdapter::hideToTrayRequested);
    EXPECT_TRUE(adapter_.requestClose());
    EXPECT_EQ(hide.count(), 0);
}

} // namespace
