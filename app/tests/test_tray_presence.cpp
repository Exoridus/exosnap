// TRAY-PRESENCE-R1 tests
// Tests are split into two fixture groups:
//   1. Pure-logic tests (TrayPresenceStateMapperTest) — no QApplication required.
//   2. Widget tests (TrayPresenceTest) — require a QApplication; exercise TrayPresence
//      construction, state transitions, tooltip text, and menu label updates.

#include <array>
#include <string>

#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QSystemTrayIcon>

#include "ui/tray/TrayPresence.h"

namespace exosnap::ui::tray {
namespace {

// ---------------------------------------------------------------------------
// Pure-logic tests: TrayIconStateFromStatusLabel
// ---------------------------------------------------------------------------

struct StateMapScenario {
    const char* input;
    TrayIconState expected;
};

class TrayPresenceStateMapperTest : public ::testing::TestWithParam<StateMapScenario> {};

TEST_P(TrayPresenceStateMapperTest, MapsLabel) {
    const StateMapScenario& s = GetParam();
    EXPECT_EQ(TrayIconStateFromStatusLabel(QString::fromLatin1(s.input)), s.expected) << "input=" << s.input;
}

INSTANTIATE_TEST_SUITE_P(
    StatusLabels, TrayPresenceStateMapperTest,
    ::testing::Values(
        StateMapScenario{"READY", TrayIconState::Idle}, StateMapScenario{"", TrayIconState::Idle},
        StateMapScenario{"BLOCKED", TrayIconState::Idle}, StateMapScenario{"ERROR", TrayIconState::Idle},
        StateMapScenario{"SAVED", TrayIconState::Idle}, StateMapScenario{"SAVING", TrayIconState::Idle},
        StateMapScenario{"CHECKING", TrayIconState::Idle}, StateMapScenario{"STOPPING", TrayIconState::Idle},
        StateMapScenario{"REC", TrayIconState::Recording}, StateMapScenario{"rec", TrayIconState::Recording},
        StateMapScenario{" REC ", TrayIconState::Recording}, StateMapScenario{"RECORDING", TrayIconState::Recording},
        StateMapScenario{"STARTING", TrayIconState::Recording}, StateMapScenario{"COUNTDOWN", TrayIconState::Recording},
        StateMapScenario{"PAUSED", TrayIconState::Paused}, StateMapScenario{"paused", TrayIconState::Paused},
        StateMapScenario{" PAUSED ", TrayIconState::Paused}));

// ---------------------------------------------------------------------------
// Widget tests: TrayPresence construction and state transitions
// ---------------------------------------------------------------------------

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

// Helper: access record-toggle and show/hide actions via direct accessors.
QAction* findRecordToggleAction(TrayPresence& tp) {
    return tp.recordToggleAction();
}

QAction* findShowHideAction(TrayPresence& tp) {
    return tp.showHideAction();
}

// ---- Construction ----

TEST_F(TrayPresenceTest, DefaultState_IsIdle) {
    TrayPresence tp;
    EXPECT_EQ(tp.currentState(), TrayIconState::Idle);
}

TEST_F(TrayPresenceTest, DefaultTooltip_ContainsAppNameAndReady) {
    TrayPresence tp;
    const QString tip = tp.currentTooltip();
    EXPECT_TRUE(tip.contains(QStringLiteral("ExoSnap"))) << tip.toStdString();
    EXPECT_TRUE(tip.contains(QStringLiteral("Ready"))) << tip.toStdString();
}

// ---- applyState: Idle ----

TEST_F(TrayPresenceTest, ApplyState_Idle_TooltipContainsReady) {
    TrayPresence tp;
    tp.applyState(TrayIconState::Idle, QStringLiteral("READY"));
    EXPECT_EQ(tp.currentState(), TrayIconState::Idle);
    EXPECT_TRUE(tp.currentTooltip().contains(QStringLiteral("Ready")));
}

TEST_F(TrayPresenceTest, ApplyState_Idle_ElapsedNotShown) {
    TrayPresence tp;
    tp.applyState(TrayIconState::Idle, QStringLiteral("READY"), QStringLiteral("00:05:00"));
    // Elapsed is suppressed for Idle state.
    EXPECT_FALSE(tp.currentTooltip().contains(QStringLiteral("00:05:00")));
}

// ---- applyState: Recording ----

TEST_F(TrayPresenceTest, ApplyState_Recording_TooltipContainsRecording) {
    TrayPresence tp;
    tp.applyState(TrayIconState::Recording, QStringLiteral("REC"));
    EXPECT_EQ(tp.currentState(), TrayIconState::Recording);
    EXPECT_TRUE(tp.currentTooltip().contains(QStringLiteral("Recording")));
}

TEST_F(TrayPresenceTest, ApplyState_Recording_TooltipContainsElapsed) {
    TrayPresence tp;
    tp.applyState(TrayIconState::Recording, QStringLiteral("REC"), QStringLiteral("01:23:45"));
    EXPECT_TRUE(tp.currentTooltip().contains(QStringLiteral("01:23:45")));
}

TEST_F(TrayPresenceTest, ApplyState_Recording_NoElapsed_TooltipOmitsParens) {
    TrayPresence tp;
    tp.applyState(TrayIconState::Recording, QStringLiteral("REC"));
    EXPECT_FALSE(tp.currentTooltip().contains(QStringLiteral("(")));
}

// ---- applyState: Paused ----

TEST_F(TrayPresenceTest, ApplyState_Paused_TooltipContainsPaused) {
    TrayPresence tp;
    tp.applyState(TrayIconState::Paused, QStringLiteral("PAUSED"), QStringLiteral("00:12:00"));
    EXPECT_EQ(tp.currentState(), TrayIconState::Paused);
    EXPECT_TRUE(tp.currentTooltip().contains(QStringLiteral("Paused")));
    EXPECT_TRUE(tp.currentTooltip().contains(QStringLiteral("00:12:00")));
}

// ---- updateElapsedText ----

TEST_F(TrayPresenceTest, UpdateElapsedText_UpdatesTooltip) {
    TrayPresence tp;
    tp.applyState(TrayIconState::Recording, QStringLiteral("REC"), QStringLiteral("00:01:00"));
    tp.updateElapsedText(QStringLiteral("00:02:30"));
    EXPECT_TRUE(tp.currentTooltip().contains(QStringLiteral("00:02:30")));
    EXPECT_FALSE(tp.currentTooltip().contains(QStringLiteral("00:01:00")));
}

// ---- Menu label transitions ----

TEST_F(TrayPresenceTest, MenuLabel_Idle_ShowsStartRecording) {
    TrayPresence tp;
    tp.applyState(TrayIconState::Idle, QStringLiteral("READY"));
    auto* action = findRecordToggleAction(tp);
    ASSERT_NE(action, nullptr);
    EXPECT_EQ(action->text(), QStringLiteral("Start recording"));
}

TEST_F(TrayPresenceTest, MenuLabel_Recording_ShowsStopRecording) {
    TrayPresence tp;
    tp.applyState(TrayIconState::Recording, QStringLiteral("REC"));
    auto* action = findRecordToggleAction(tp);
    ASSERT_NE(action, nullptr);
    EXPECT_EQ(action->text(), QStringLiteral("Stop recording"));
}

TEST_F(TrayPresenceTest, MenuLabel_Paused_ShowsStopRecording) {
    TrayPresence tp;
    tp.applyState(TrayIconState::Paused, QStringLiteral("PAUSED"));
    auto* action = findRecordToggleAction(tp);
    ASSERT_NE(action, nullptr);
    EXPECT_EQ(action->text(), QStringLiteral("Stop recording"));
}

// ---- Blocked / enabled state of the record toggle action ----

TEST_F(TrayPresenceTest, RecordToggle_Idle_NotBlocked_Enabled) {
    TrayPresence tp;
    tp.setRecordingBlocked(false);
    tp.applyState(TrayIconState::Idle, QStringLiteral("READY"));
    auto* action = findRecordToggleAction(tp);
    ASSERT_NE(action, nullptr);
    EXPECT_TRUE(action->isEnabled());
}

TEST_F(TrayPresenceTest, RecordToggle_Idle_Blocked_Disabled) {
    TrayPresence tp;
    tp.applyState(TrayIconState::Idle, QStringLiteral("BLOCKED"));
    tp.setRecordingBlocked(true);
    auto* action = findRecordToggleAction(tp);
    ASSERT_NE(action, nullptr);
    EXPECT_FALSE(action->isEnabled());
}

TEST_F(TrayPresenceTest, RecordToggle_Recording_AlwaysEnabled) {
    TrayPresence tp;
    tp.setRecordingBlocked(true);
    tp.applyState(TrayIconState::Recording, QStringLiteral("REC"));
    auto* action = findRecordToggleAction(tp);
    ASSERT_NE(action, nullptr);
    EXPECT_TRUE(action->isEnabled());
}

// ---- Show/hide window action label ----

TEST_F(TrayPresenceTest, ShowHideAction_WindowVisible_ShowsHide) {
    TrayPresence tp;
    tp.setWindowVisible(true);
    auto* action = findShowHideAction(tp);
    ASSERT_NE(action, nullptr);
    EXPECT_EQ(action->text(), QStringLiteral("Hide window"));
}

TEST_F(TrayPresenceTest, ShowHideAction_WindowHidden_ShowsShow) {
    TrayPresence tp;
    tp.setWindowVisible(false);
    auto* action = findShowHideAction(tp);
    ASSERT_NE(action, nullptr);
    EXPECT_EQ(action->text(), QStringLiteral("Show window"));
}

// ---- State round-trip: Idle → Recording → Paused → Idle ----

TEST_F(TrayPresenceTest, StateRoundTrip) {
    TrayPresence tp;

    tp.applyState(TrayIconState::Idle, QStringLiteral("READY"));
    EXPECT_EQ(tp.currentState(), TrayIconState::Idle);

    tp.applyState(TrayIconState::Recording, QStringLiteral("REC"), QStringLiteral("00:01:00"));
    EXPECT_EQ(tp.currentState(), TrayIconState::Recording);
    EXPECT_TRUE(tp.currentTooltip().contains(QStringLiteral("Recording")));

    tp.applyState(TrayIconState::Paused, QStringLiteral("PAUSED"), QStringLiteral("00:01:00"));
    EXPECT_EQ(tp.currentState(), TrayIconState::Paused);
    EXPECT_TRUE(tp.currentTooltip().contains(QStringLiteral("Paused")));

    tp.applyState(TrayIconState::Idle, QStringLiteral("READY"));
    EXPECT_EQ(tp.currentState(), TrayIconState::Idle);
    EXPECT_TRUE(tp.currentTooltip().contains(QStringLiteral("Ready")));
}

} // namespace
} // namespace exosnap::ui::tray
