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

#include <QByteArray>
#include <QMetaMethod>
#include <QMetaObject>

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

// A confirmed guard has to produce BOTH signals. `closeApproved` lets the window
// go; `closeDecided("allow")` is the one the application hangs the explicit quit
// off. With only the first, a confirmed close destroyed the window and left the
// process running behind a live tray icon, because Qt's quit-on-last-window
// counts the five capture overlays as primary windows and they are hidden, never
// closed. Asserted for every guard kind, since each one approves from the same
// place and any of them could have been the one that forgot.
TEST(ShellAdapterCloseDecisionTest, ConfirmingAnyGuardReportsTheAllowDecisionThatEndsTheProcess) {
    struct Case {
        const char* name;
        bool recording;
        bool exporting;
        bool remuxing;
    };
    // A FRESH adapter per case, not the fixture's: confirming a guard latches a
    // waiver, and a reused adapter would answer the next case with the previous
    // one's waiver instead of raising its prompt.
    for (const Case& c : {Case{"recording", true, false, false}, Case{"exporting", false, true, false},
                          Case{"remuxing", false, false, true}}) {
        SCOPED_TRACE(c.name);
        CloseGuardState state;
        state.recording = c.recording;
        state.exporting = c.exporting;
        state.remuxing = c.remuxing;
        ShellAdapter adapter;
        adapter.setStateProvider([&state]() { return state; });

        QStringList decisions;
        QObject::connect(&adapter, &ShellAdapter::closeDecided, &adapter,
                         [&decisions](const QString& kind, bool, bool, bool) { decisions << kind; });
        int approved = 0;
        QObject::connect(&adapter, &ShellAdapter::closeApproved, &adapter, [&approved]() { ++approved; });

        EXPECT_FALSE(adapter.requestClose());
        adapter.confirmCloseGuard();

        EXPECT_EQ(approved, 1);
        // The prompt was reported first, the approval second: one close attempt,
        // two decisions, and the last one is what ends the process.
        ASSERT_EQ(decisions.size(), 2);
        EXPECT_EQ(decisions.back(), QStringLiteral("allow"));
    }
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
// Close means close
// ---------------------------------------------------------------------------
// There is no preference that turns a close into something else. The only inputs
// are the four in-flight conditions, and the only outcomes are the ones
// CloseGuardPolicy names. Close-to-tray used to sit between them: with it on, a
// close during a recording silently meant "hide", and the user never learned that
// the thing they asked to close was still running.
//
// Minimize-to-tray replaces it -- a different gesture, decided in
// models/WindowPresencePolicy and never on this path.

TEST_F(ShellAdapterTest, NothingInFlightClosesForReal) {
    QStringList kinds;
    QObject::connect(&adapter_, &ShellAdapter::closeDecided, &adapter_,
                     [&kinds](const QString& kind, bool, bool, bool) { kinds.append(kind); });

    EXPECT_TRUE(adapter_.requestClose());
    EXPECT_FALSE(adapter_.closeGuardActive());
    ASSERT_EQ(kinds.size(), 1);
    EXPECT_EQ(kinds.at(0), QStringLiteral("allow"));
}

TEST_F(ShellAdapterTest, EveryCloseAttemptReportsExactlyOneDecision) {
    // The application quits on an "allow", so a close that reported nothing would
    // strand the process with no window.
    QStringList kinds;
    QObject::connect(&adapter_, &ShellAdapter::closeDecided, &adapter_,
                     [&kinds](const QString& kind, bool, bool, bool) { kinds.append(kind); });

    EXPECT_TRUE(adapter_.requestClose());
    ASSERT_EQ(kinds.size(), 1);
    EXPECT_EQ(kinds.at(0), QStringLiteral("allow"));

    kinds.clear();
    state_.recording = true;
    EXPECT_FALSE(adapter_.requestClose());
    ASSERT_EQ(kinds.size(), 1);
    EXPECT_EQ(kinds.at(0), QStringLiteral("confirmRecording"));

    kinds.clear();
    state_.recording = false;
    state_.exporting = true;
    EXPECT_FALSE(adapter_.requestClose());
    ASSERT_EQ(kinds.size(), 1);
    EXPECT_EQ(kinds.at(0), QStringLiteral("confirmExport"));

    kinds.clear();
    state_.exporting = false;
    state_.remuxing = true;
    EXPECT_FALSE(adapter_.requestClose());
    ASSERT_EQ(kinds.size(), 1);
    EXPECT_EQ(kinds.at(0), QStringLiteral("confirmRemux"));

    kinds.clear();
    state_.remuxing = false;
    state_.finalizing = true;
    EXPECT_FALSE(adapter_.requestClose());
    ASSERT_EQ(kinds.size(), 1);
    EXPECT_EQ(kinds.at(0), QStringLiteral("blockSilently"));
}

TEST_F(ShellAdapterTest, ARecordingIsAlwaysAskedAboutAndConfirmingClosesForReal) {
    state_.recording = true;
    SignalCounter approved(&adapter_, &ShellAdapter::closeApproved);

    EXPECT_FALSE(adapter_.requestClose());
    ASSERT_TRUE(adapter_.closeGuardActive());
    EXPECT_EQ(adapter_.closeGuardProceedLabel(), QStringLiteral("Stop recording and close"));

    state_.recording = false;
    adapter_.confirmCloseGuard();
    EXPECT_EQ(approved.count(), 1);
}

TEST_F(ShellAdapterTest, APromptLeftOverFromAnAbandonedAttemptDoesNotSurviveTheNextClose) {
    // A guard was raised, the user walked away, and the recording then ended on
    // its own. The next close must start from a clean slate rather than
    // reappearing a dialog for work that is no longer running.
    state_.recording = true;
    EXPECT_FALSE(adapter_.requestClose());
    ASSERT_TRUE(adapter_.closeGuardActive());

    state_.recording = false;
    EXPECT_TRUE(adapter_.requestClose());
    EXPECT_FALSE(adapter_.closeGuardActive());
}

// ── Navigation model ────────────────────────────────────────────────────────
//
// These indices are not private bookkeeping: AppShell's StackLayout, the
// elevated-relaunch landing page (ADR 0033), every notification action that
// navigates, and the --visual-page harness flag all address pages by this
// number. A silent renumbering points all four somewhere else.

TEST(ShellNavigation, PageIndicesAreTheShippedNavigationOrder) {
    EXPECT_EQ(static_cast<int>(ShellAdapter::RecordPage), 0);
    EXPECT_EQ(static_cast<int>(ShellAdapter::SettingsPage), 1);
    EXPECT_EQ(static_cast<int>(ShellAdapter::DiagnosticsPage), 2);
    EXPECT_EQ(static_cast<int>(ShellAdapter::LogsPage), 3);
    EXPECT_EQ(static_cast<int>(ShellAdapter::AboutPage), 4);
}

TEST(ShellNavigation, EveryDestinationIsDirectlyAddressable) {
    // All five are peers in the band — there is no secondary tier and no page
    // that has to be reached through a menu. A sixth destination added here
    // must therefore be a deliberate decision about the band's width budget at
    // the 860 px minimum window, not an accident of appending an enumerator.
    EXPECT_EQ(static_cast<int>(ShellAdapter::AboutPage) + 1, 5);
}

TEST(ShellNavigation, TheNavigationSignalCarriesThePageEnumRatherThanAnInt) {
    // QCR-716. The enum's own header comment says it exists "so a navigation
    // request never has to spell a bare integer", and the one navigation signal
    // took an int anyway. With the enum in the signature, QML addresses the
    // destinations as ShellAdapter.SettingsPage instead of a 1 that would keep
    // compiling — and mean something else — after a reorder.
    const QMetaObject& meta = ShellAdapter::staticMetaObject;
    int found = -1;
    for (int i = meta.methodOffset(); i < meta.methodCount(); ++i) {
        const QMetaMethod method = meta.method(i);
        if (method.methodType() == QMetaMethod::Signal &&
            method.name() == QByteArrayLiteral("navigateToPageRequested")) {
            found = i;
            break;
        }
    }
    ASSERT_GE(found, 0) << "the navigation signal must exist";

    const QMetaMethod method = meta.method(found);
    ASSERT_EQ(method.parameterCount(), 1);
    EXPECT_EQ(QByteArray(method.parameterTypeName(0)), QByteArrayLiteral("Page"));
}

} // namespace
