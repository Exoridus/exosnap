// TRAY-PRESENCE-R1 tests.
//
// The tray no longer derives anything: it renders the shell projection
// (models/ShellPresence.h), which the taskbar's thumbnail buttons render too.
// What is tested here is the rendering and the menu's own refusal to raise an
// action the projection does not allow -- the projection itself has its own
// tests, and re-asserting it here would be the second table this design exists
// to remove.

#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QSystemTrayIcon>

#include "models/RecordingPulse.h"
#include "models/ShellPresence.h"
#include "ui/tray/TrayPresence.h"
#include "viewmodels/RecordViewModel.h"

namespace exosnap::ui::tray {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char name[] = "tray_presence_tests";
    static char* argv[] = {name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class TrayPresenceTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

ShellPresenceState PresenceFor(UiRecordingState state, bool saved_dwell = false) {
    ShellPresenceInput input;
    input.state = state;
    input.can_start =
        state == UiRecordingState::Ready || state == UiRecordingState::Completed || state == UiRecordingState::Failed;
    input.can_stop = state == UiRecordingState::Recording || state == UiRecordingState::Paused;
    input.can_pause = state == UiRecordingState::Recording;
    input.can_resume = state == UiRecordingState::Paused;
    input.saved_dwell_active = saved_dwell;
    return ProjectShellPresence(input);
}

// ---- Construction ----

TEST_F(TrayPresenceTest, DefaultState_IsIdle) {
    TrayPresence tp;
    EXPECT_EQ(tp.currentIconState(), ShellIconState::Idle);
}

TEST_F(TrayPresenceTest, DefaultTooltip_ContainsAppNameAndReady) {
    TrayPresence tp;
    const QString tip = tp.currentTooltip();
    EXPECT_TRUE(tip.contains(QStringLiteral("ExoSnap"))) << tip.toStdString();
    EXPECT_TRUE(tip.contains(QStringLiteral("Ready"))) << tip.toStdString();
}

// ---- Icon state ----

TEST_F(TrayPresenceTest, ApplyState_FollowsTheProjectionsIconState) {
    TrayPresence tp;
    tp.applyState(PresenceFor(UiRecordingState::Recording));
    EXPECT_EQ(tp.currentIconState(), ShellIconState::Recording);
    tp.applyState(PresenceFor(UiRecordingState::Paused));
    EXPECT_EQ(tp.currentIconState(), ShellIconState::Paused);
    tp.applyState(PresenceFor(UiRecordingState::Completed, /*saved_dwell=*/true));
    EXPECT_EQ(tp.currentIconState(), ShellIconState::Saved);
    tp.applyState(PresenceFor(UiRecordingState::Ready));
    EXPECT_EQ(tp.currentIconState(), ShellIconState::Idle);
}

TEST_F(TrayPresenceTest, ThePulseFrameIsPushedInRatherThanTimed) {
    // The tray owns no timer: the taskbar badge and the in-app indicator read
    // the same phase, and three timers would drift.
    TrayPresence tp;
    tp.applyState(PresenceFor(UiRecordingState::Recording), QString(), 2);
    EXPECT_EQ(tp.currentPulseFrame(), 2);
    tp.applyState(PresenceFor(UiRecordingState::Recording), QString(), 3);
    EXPECT_EQ(tp.currentPulseFrame(), 3);
}

TEST_F(TrayPresenceTest, AnOutOfRangePulseFrameDoesNotBlankTheIcon) {
    TrayPresence tp;
    tp.applyState(PresenceFor(UiRecordingState::Recording), QString(), kRecordingPulseFrameCount + 5);
    EXPECT_EQ(tp.currentIconState(), ShellIconState::Recording);
}

// ---- Tooltip ----

TEST_F(TrayPresenceTest, Tooltip_Idle_MatchesSpec) {
    TrayPresence tp;
    tp.applyState(PresenceFor(UiRecordingState::Ready));
    EXPECT_EQ(tp.currentTooltip(), QStringLiteral("ExoSnap \xe2\x80\x94 Ready"));
}

TEST_F(TrayPresenceTest, Tooltip_Recording_NoElapsed_MatchesSpec) {
    TrayPresence tp;
    tp.applyState(PresenceFor(UiRecordingState::Recording));
    EXPECT_EQ(tp.currentTooltip(), QStringLiteral("ExoSnap \xe2\x80\x94 Recording"));
}

TEST_F(TrayPresenceTest, Tooltip_Recording_WithElapsed_MatchesSpec) {
    TrayPresence tp;
    tp.applyState(PresenceFor(UiRecordingState::Recording), QStringLiteral("04:17"));
    EXPECT_EQ(tp.currentTooltip(), QStringLiteral("ExoSnap \xe2\x80\x94 Recording 04:17"));
}

TEST_F(TrayPresenceTest, Tooltip_Paused_OmitsElapsed) {
    TrayPresence tp;
    tp.applyState(PresenceFor(UiRecordingState::Paused), QStringLiteral("00:12:00"));
    EXPECT_EQ(tp.currentTooltip(), QStringLiteral("ExoSnap \xe2\x80\x94 Paused"));
}

TEST_F(TrayPresenceTest, Tooltip_Saved_SaysSo) {
    TrayPresence tp;
    tp.applyState(PresenceFor(UiRecordingState::Completed, /*saved_dwell=*/true));
    EXPECT_EQ(tp.currentTooltip(), QStringLiteral("ExoSnap \xe2\x80\x94 Saved"));
}

TEST_F(TrayPresenceTest, Tooltip_Idle_ElapsedNotShown) {
    TrayPresence tp;
    tp.applyState(PresenceFor(UiRecordingState::Ready), QStringLiteral("00:05:00"));
    EXPECT_FALSE(tp.currentTooltip().contains(QStringLiteral("00:05:00")));
}

TEST_F(TrayPresenceTest, UpdateElapsedText_UpdatesTooltip) {
    TrayPresence tp;
    tp.applyState(PresenceFor(UiRecordingState::Recording), QStringLiteral("00:01:00"));
    tp.updateElapsedText(QStringLiteral("00:02:30"));
    EXPECT_TRUE(tp.currentTooltip().contains(QStringLiteral("00:02:30")));
    EXPECT_FALSE(tp.currentTooltip().contains(QStringLiteral("00:01:00")));
}

// ---- Menu, driven by the projection ----

TEST_F(TrayPresenceTest, Menu_Idle_OffersOnlyStart) {
    TrayPresence tp;
    tp.applyState(PresenceFor(UiRecordingState::Ready));
    EXPECT_TRUE(tp.recordAction()->isVisible());
    EXPECT_TRUE(tp.recordAction()->isEnabled());
    EXPECT_EQ(tp.recordAction()->text(), QStringLiteral("Start recording"));
    EXPECT_FALSE(tp.pauseResumeAction()->isVisible());
    EXPECT_FALSE(tp.stopAction()->isVisible());
}

TEST_F(TrayPresenceTest, Menu_Recording_OffersPauseAndStop) {
    TrayPresence tp;
    tp.applyState(PresenceFor(UiRecordingState::Recording));
    EXPECT_FALSE(tp.recordAction()->isVisible());
    EXPECT_TRUE(tp.pauseResumeAction()->isVisible());
    EXPECT_EQ(tp.pauseResumeAction()->text(), QStringLiteral("Pause recording"));
    EXPECT_TRUE(tp.stopAction()->isVisible());
}

TEST_F(TrayPresenceTest, Menu_Paused_TurnsTheSameEntryIntoResume) {
    TrayPresence tp;
    tp.applyState(PresenceFor(UiRecordingState::Paused));
    EXPECT_TRUE(tp.pauseResumeAction()->isVisible());
    EXPECT_EQ(tp.pauseResumeAction()->text(), QStringLiteral("Resume recording"));
    EXPECT_TRUE(tp.stopAction()->isVisible());
}

TEST_F(TrayPresenceTest, Menu_Finalizing_GreysStartRatherThanHidingIt) {
    TrayPresence tp;
    tp.applyState(PresenceFor(UiRecordingState::Saving));
    EXPECT_TRUE(tp.recordAction()->isVisible());
    EXPECT_FALSE(tp.recordAction()->isEnabled());
    EXPECT_FALSE(tp.pauseResumeAction()->isVisible());
    EXPECT_FALSE(tp.stopAction()->isVisible());
}

TEST_F(TrayPresenceTest, Menu_Blocked_GreysStart) {
    TrayPresence tp;
    tp.applyState(PresenceFor(UiRecordingState::Blocked));
    EXPECT_TRUE(tp.recordAction()->isVisible());
    EXPECT_FALSE(tp.recordAction()->isEnabled());
}

// ---- Action routing ----

TEST_F(TrayPresenceTest, TriggeringAnEntryRaisesTheProjectionsAction) {
    TrayPresence tp;
    QList<ShellAction> seen;
    QObject::connect(&tp, &TrayPresence::shellActionRequested, &tp,
                     [&seen](ShellAction action) { seen.append(action); });

    tp.applyState(PresenceFor(UiRecordingState::Ready));
    tp.recordAction()->trigger();
    tp.applyState(PresenceFor(UiRecordingState::Recording));
    tp.pauseResumeAction()->trigger();
    tp.stopAction()->trigger();
    tp.applyState(PresenceFor(UiRecordingState::Paused));
    tp.pauseResumeAction()->trigger();

    ASSERT_EQ(seen.size(), 4);
    EXPECT_EQ(seen[0], ShellAction::Start);
    EXPECT_EQ(seen[1], ShellAction::Pause);
    EXPECT_EQ(seen[2], ShellAction::Stop);
    EXPECT_EQ(seen[3], ShellAction::Resume);
}

TEST_F(TrayPresenceTest, ARefusedEntryCannotBypassTheProjection) {
    // A menu item can be reached by keyboard between a state change and the
    // repaint, and QAction::trigger() runs whatever is connected to it.
    TrayPresence tp;
    int raised = 0;
    QObject::connect(&tp, &TrayPresence::shellActionRequested, &tp, [&raised](ShellAction) { ++raised; });

    tp.applyState(PresenceFor(UiRecordingState::Saving));
    tp.recordAction()->trigger();
    tp.pauseResumeAction()->trigger();
    tp.stopAction()->trigger();
    EXPECT_EQ(raised, 0);
}

// ---- Show/hide window action ----

TEST_F(TrayPresenceTest, ShowHideAction_WindowVisible_ShowsHide) {
    TrayPresence tp;
    tp.setWindowVisible(true);
    EXPECT_EQ(tp.showHideAction()->text(), QStringLiteral("Hide window"));
}

TEST_F(TrayPresenceTest, ShowHideAction_WindowHidden_ShowsShow) {
    TrayPresence tp;
    tp.setWindowVisible(false);
    EXPECT_EQ(tp.showHideAction()->text(), QStringLiteral("Show window"));
}

// The label is only half the contract. Triggering the entry while the window is
// visible used to emit activateWindowRequested, so the menu offered to hide the
// window and then raised it -- there was no hide path at all.
TEST_F(TrayPresenceTest, ShowHideAction_WindowVisible_AsksToHideNotToShow) {
    TrayPresence tp;
    tp.setWindowVisible(true);
    int hides = 0;
    int activates = 0;
    QObject::connect(&tp, &TrayPresence::hideWindowRequested, &tp, [&hides]() { ++hides; });
    QObject::connect(&tp, &TrayPresence::activateWindowRequested, &tp, [&activates]() { ++activates; });

    tp.showHideAction()->trigger();

    EXPECT_EQ(hides, 1);
    EXPECT_EQ(activates, 0);
}

TEST_F(TrayPresenceTest, ShowHideAction_WindowHidden_AsksToShow) {
    TrayPresence tp;
    tp.setWindowVisible(false);
    int hides = 0;
    int activates = 0;
    QObject::connect(&tp, &TrayPresence::hideWindowRequested, &tp, [&hides]() { ++hides; });
    QObject::connect(&tp, &TrayPresence::activateWindowRequested, &tp, [&activates]() { ++activates; });

    tp.showHideAction()->trigger();

    EXPECT_EQ(activates, 1);
    EXPECT_EQ(hides, 0);
}

// ---- State round-trip ----

TEST_F(TrayPresenceTest, StateRoundTrip) {
    TrayPresence tp;

    tp.applyState(PresenceFor(UiRecordingState::Ready));
    EXPECT_EQ(tp.currentIconState(), ShellIconState::Idle);

    tp.applyState(PresenceFor(UiRecordingState::Recording), QStringLiteral("00:01:00"));
    EXPECT_EQ(tp.currentIconState(), ShellIconState::Recording);
    EXPECT_TRUE(tp.currentTooltip().contains(QStringLiteral("Recording")));

    tp.applyState(PresenceFor(UiRecordingState::Paused), QStringLiteral("00:01:00"));
    EXPECT_EQ(tp.currentIconState(), ShellIconState::Paused);
    EXPECT_TRUE(tp.currentTooltip().contains(QStringLiteral("Paused")));

    tp.applyState(PresenceFor(UiRecordingState::Completed, /*saved_dwell=*/true));
    EXPECT_EQ(tp.currentIconState(), ShellIconState::Saved);

    tp.applyState(PresenceFor(UiRecordingState::Ready));
    EXPECT_EQ(tp.currentIconState(), ShellIconState::Idle);
    EXPECT_TRUE(tp.currentTooltip().contains(QStringLiteral("Ready")));
}

// ---- Unread notification badge (NOTIFY-SKIN-R1) ----

TEST_F(TrayPresenceTest, UnreadCount_DefaultIsZero) {
    TrayPresence tp;
    EXPECT_EQ(tp.unreadCount(), 0);
}

TEST_F(TrayPresenceTest, IncrementUnreadCount_CountIncreases) {
    TrayPresence tp;
    tp.incrementUnreadCount();
    EXPECT_EQ(tp.unreadCount(), 1);
    tp.incrementUnreadCount();
    EXPECT_EQ(tp.unreadCount(), 2);
}

TEST_F(TrayPresenceTest, ClearUnreadCount_ResetsToZero) {
    TrayPresence tp;
    tp.incrementUnreadCount();
    tp.incrementUnreadCount();
    ASSERT_EQ(tp.unreadCount(), 2);
    tp.clearUnreadCount();
    EXPECT_EQ(tp.unreadCount(), 0);
}

TEST_F(TrayPresenceTest, ClearUnreadCount_WhenAlreadyZero_IsNoOp) {
    TrayPresence tp;
    ASSERT_EQ(tp.unreadCount(), 0);
    tp.clearUnreadCount();
    EXPECT_EQ(tp.unreadCount(), 0);
}

TEST_F(TrayPresenceTest, NotificationsAction_IsHiddenByDefault) {
    TrayPresence tp;
    EXPECT_FALSE(tp.notificationsAction()->isVisible());
}

TEST_F(TrayPresenceTest, NotificationsAction_VisibleAfterIncrement) {
    TrayPresence tp;
    tp.incrementUnreadCount();
    EXPECT_TRUE(tp.notificationsAction()->isVisible());
}

TEST_F(TrayPresenceTest, NotificationsAction_LabelContainsCount) {
    TrayPresence tp;
    for (int i = 0; i < 3; ++i)
        tp.incrementUnreadCount();
    EXPECT_TRUE(tp.notificationsAction()->text().contains(QStringLiteral("3")))
        << "Expected count in action text: " << tp.notificationsAction()->text().toStdString();
}

TEST_F(TrayPresenceTest, NotificationsAction_HiddenAfterClear) {
    TrayPresence tp;
    tp.incrementUnreadCount();
    ASSERT_TRUE(tp.notificationsAction()->isVisible());
    tp.clearUnreadCount();
    EXPECT_FALSE(tp.notificationsAction()->isVisible());
}

TEST_F(TrayPresenceTest, MultipleIncrements_CountCumulates) {
    TrayPresence tp;
    for (int i = 0; i < 5; ++i)
        tp.incrementUnreadCount();
    EXPECT_EQ(tp.unreadCount(), 5);
}

// ---- TRAY-CLOSE-TO-TRAY-R1: click semantics ----

TEST_F(TrayPresenceTest, ClickSemantics_SingleLeftClick_EmitsActivateWindow) {
    TrayPresence tp;
    bool activate_received = false;
    bool record_received = false;
    QObject::connect(&tp, &TrayPresence::activateWindowRequested, [&] { activate_received = true; });
    QObject::connect(&tp, &TrayPresence::recordToggleRequested, [&] { record_received = true; });

    QMetaObject::invokeMethod(&tp, "onTrayActivated", Qt::DirectConnection,
                              Q_ARG(QSystemTrayIcon::ActivationReason, QSystemTrayIcon::Trigger));

    EXPECT_TRUE(activate_received);
    EXPECT_FALSE(record_received);
}

TEST_F(TrayPresenceTest, ClickSemantics_DoubleClick_EmitsRecordToggle) {
    TrayPresence tp;
    bool activate_received = false;
    bool record_received = false;
    QObject::connect(&tp, &TrayPresence::activateWindowRequested, [&] { activate_received = true; });
    QObject::connect(&tp, &TrayPresence::recordToggleRequested, [&] { record_received = true; });

    QMetaObject::invokeMethod(&tp, "onTrayActivated", Qt::DirectConnection,
                              Q_ARG(QSystemTrayIcon::ActivationReason, QSystemTrayIcon::DoubleClick));

    EXPECT_FALSE(activate_received);
    EXPECT_TRUE(record_received);
}

} // namespace
} // namespace exosnap::ui::tray
