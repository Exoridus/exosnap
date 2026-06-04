#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <QList>
#include <QPushButton>
#include <QString>

#include "ui/widgets/AudioSourceToggle.h"
#include "ui/widgets/TransportDock.h"

namespace exosnap {
namespace {

using ui::widgets::AudioSourceToggle;
using ui::widgets::TransportDock;

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "transport_dock_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class TransportDockTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }

    static QPushButton* Button(const TransportDock& dock, const char* object_name) {
        return dock.findChild<QPushButton*>(QString::fromLatin1(object_name));
    }

    static AudioSourceToggle* Toggle(const TransportDock& dock, const QString& source_key) {
        for (auto* toggle : dock.findChildren<AudioSourceToggle*>())
            if (toggle->sourceKey() == source_key)
                return toggle;
        return nullptr;
    }

    // isVisibleTo(&dock) reflects effective visibility (incl. a hidden parent zone)
    // without requiring the dock to be shown on screen.
    static bool ButtonVisible(const TransportDock& dock, const char* object_name) {
        auto* button = Button(dock, object_name);
        return button && button->isVisibleTo(&dock);
    }
};

TEST_F(TransportDockTest, ExposesDockSeams) {
    TransportDock dock;
    EXPECT_EQ(dock.objectName(), QStringLiteral("recordTransportDock"));
    EXPECT_NE(dock.findChild<QLabel*>(QStringLiteral("recordDockTimer")), nullptr);
    EXPECT_NE(Button(dock, "recordDockRecord"), nullptr);
    EXPECT_NE(Button(dock, "recordDockStop"), nullptr);
}

TEST_F(TransportDockTest, ReadyState_ShowsRecordAction) {
    TransportDock dock;
    dock.setState(TransportDock::State::Ready);

    EXPECT_TRUE(ButtonVisible(dock, "recordDockRecord"));
    EXPECT_FALSE(ButtonVisible(dock, "recordDockStop"));
    EXPECT_FALSE(ButtonVisible(dock, "recordDockPause"));
    EXPECT_FALSE(ButtonVisible(dock, "recordDockResume"));
    EXPECT_FALSE(ButtonVisible(dock, "recordDockRecordAgain"));
}

TEST_F(TransportDockTest, RecordingState_ShowsPauseAndStop) {
    TransportDock dock;
    dock.setState(TransportDock::State::Recording);

    EXPECT_TRUE(ButtonVisible(dock, "recordDockPause"));
    EXPECT_TRUE(ButtonVisible(dock, "recordDockStop"));
    EXPECT_FALSE(ButtonVisible(dock, "recordDockRecord"));
    EXPECT_FALSE(ButtonVisible(dock, "recordDockResume"));
}

TEST_F(TransportDockTest, PausedState_ShowsResumeAndStop) {
    TransportDock dock;
    dock.setState(TransportDock::State::Paused);

    EXPECT_TRUE(ButtonVisible(dock, "recordDockResume"));
    EXPECT_TRUE(ButtonVisible(dock, "recordDockStop"));
    EXPECT_FALSE(ButtonVisible(dock, "recordDockPause"));
    EXPECT_FALSE(ButtonVisible(dock, "recordDockRecord"));
}

TEST_F(TransportDockTest, CompletedState_ShowsRecordAgainAndResultInfo) {
    TransportDock dock;
    dock.setState(TransportDock::State::Completed);
    dock.setCompletedInfo(QStringLiteral("clip.mkv"), QStringLiteral("128 MB"), true);

    EXPECT_TRUE(ButtonVisible(dock, "recordDockRecordAgain"));
    EXPECT_TRUE(ButtonVisible(dock, "recordDockFilename"));
    EXPECT_TRUE(ButtonVisible(dock, "recordDockOpenFolder"));
    EXPECT_FALSE(ButtonVisible(dock, "recordDockRecord"));
    EXPECT_FALSE(ButtonVisible(dock, "recordDockStop"));
    // Source toggles give way to result info in the completed state.
    auto* system_toggle = Toggle(dock, QStringLiteral("system"));
    ASSERT_NE(system_toggle, nullptr);
    EXPECT_FALSE(system_toggle->isVisibleTo(&dock));
}

TEST_F(TransportDockTest, NoKioskStartLabel) {
    TransportDock dock;
    // The hybrid dock uses "Record", never the legacy kiosk "Start Recording".
    for (const QPushButton* button : dock.findChildren<QPushButton*>())
        EXPECT_NE(button->text(), QStringLiteral("Start Recording"));
}

TEST_F(TransportDockTest, PrimaryEnabledGatesRecord) {
    TransportDock dock;
    dock.setState(TransportDock::State::Ready);
    dock.setPrimaryEnabled(false);
    EXPECT_FALSE(Button(dock, "recordDockRecord")->isEnabled());
    dock.setPrimaryEnabled(true);
    EXPECT_TRUE(Button(dock, "recordDockRecord")->isEnabled());
}

TEST_F(TransportDockTest, InteractiveToggleEmitsSourceKey) {
    TransportDock dock;
    dock.setState(TransportDock::State::Ready);
    dock.setToggleState(QStringLiteral("mic"), false, true);

    QString emitted;
    QObject::connect(&dock, &TransportDock::sourceToggleClicked, &dock,
                     [&emitted](const QString& key) { emitted = key; });

    auto* mic = Toggle(dock, QStringLiteral("mic"));
    ASSERT_NE(mic, nullptr);
    EXPECT_TRUE(mic->isInteractive());
    mic->click();
    EXPECT_EQ(emitted, QStringLiteral("mic"));
}

TEST_F(TransportDockTest, NonInteractiveToggleDoesNotEmit) {
    TransportDock dock;
    // Webcam is a read-only status pill in this slice.
    dock.setToggleState(QStringLiteral("webcam"), true, false);

    bool fired = false;
    QObject::connect(&dock, &TransportDock::sourceToggleClicked, &dock, [&fired](const QString&) { fired = true; });

    auto* webcam = Toggle(dock, QStringLiteral("webcam"));
    ASSERT_NE(webcam, nullptr);
    EXPECT_FALSE(webcam->isInteractive());
    webcam->click(); // disabled button: no clicked() signal
    EXPECT_FALSE(fired);
}

TEST_F(TransportDockTest, RecordButtonEmitsRecordClicked) {
    TransportDock dock;
    dock.setState(TransportDock::State::Ready);
    dock.setPrimaryEnabled(true);

    bool fired = false;
    QObject::connect(&dock, &TransportDock::recordClicked, &dock, [&fired]() { fired = true; });
    Button(dock, "recordDockRecord")->click();
    EXPECT_TRUE(fired);
}

TEST_F(TransportDockTest, TimerTextAndRoleApply) {
    TransportDock dock;
    dock.setTimerText(QStringLiteral("00:01:23"));
    dock.setTimerRole(QStringLiteral("recording"));
    auto* timer = dock.findChild<QLabel*>(QStringLiteral("recordDockTimer"));
    ASSERT_NE(timer, nullptr);
    EXPECT_EQ(timer->text(), QStringLiteral("00:01:23"));
    EXPECT_EQ(timer->property("timerState").toString(), QStringLiteral("recording"));
}

// ── Audio meter API ──────────────────────────────────────────────────────────

TEST_F(TransportDockTest, SetMeterLevel_SystemToggleReceivesLevel) {
    TransportDock dock;
    dock.setMeterLevel(QStringLiteral("system"), 0.75f);
    auto* tog = Toggle(dock, QStringLiteral("system"));
    ASSERT_NE(tog, nullptr);
    EXPECT_FLOAT_EQ(tog->meterLevel(), 0.75f);
    EXPECT_TRUE(tog->isMeterActive());
}

TEST_F(TransportDockTest, SetMeterLevel_MicToggleReceivesLevel) {
    TransportDock dock;
    dock.setMeterLevel(QStringLiteral("mic"), 0.4f);
    auto* tog = Toggle(dock, QStringLiteral("mic"));
    ASSERT_NE(tog, nullptr);
    EXPECT_FLOAT_EQ(tog->meterLevel(), 0.4f);
    EXPECT_TRUE(tog->isMeterActive());
}

TEST_F(TransportDockTest, SetMeterLevel_AppToggleReceivesLevel) {
    TransportDock dock;
    dock.setMeterLevel(QStringLiteral("app"), 0.2f);
    auto* tog = Toggle(dock, QStringLiteral("app"));
    ASSERT_NE(tog, nullptr);
    EXPECT_FLOAT_EQ(tog->meterLevel(), 0.2f);
    EXPECT_TRUE(tog->isMeterActive());
}

TEST_F(TransportDockTest, SetMeterLevel_ZeroDeactivates) {
    TransportDock dock;
    dock.setMeterLevel(QStringLiteral("mic"), 0.6f);
    dock.setMeterLevel(QStringLiteral("mic"), 0.0f);
    auto* tog = Toggle(dock, QStringLiteral("mic"));
    ASSERT_NE(tog, nullptr);
    EXPECT_FLOAT_EQ(tog->meterLevel(), 0.0f);
    EXPECT_FALSE(tog->isMeterActive());
}

TEST_F(TransportDockTest, SetMeterLevel_WebcamKeyIsIgnored) {
    TransportDock dock;
    // Should not crash; webcam has no audio meter
    dock.setMeterLevel(QStringLiteral("webcam"), 0.9f);
    auto* webcam = Toggle(dock, QStringLiteral("webcam"));
    ASSERT_NE(webcam, nullptr);
    EXPECT_FLOAT_EQ(webcam->meterLevel(), 0.0f);
    EXPECT_FALSE(webcam->isMeterActive());
}

TEST_F(TransportDockTest, SetMeterLevel_UnknownKeyIsIgnored) {
    TransportDock dock;
    // Should not crash or affect any toggle
    dock.setMeterLevel(QStringLiteral("nonexistent"), 1.0f);
    for (auto* tog : dock.findChildren<AudioSourceToggle*>())
        EXPECT_FLOAT_EQ(tog->meterLevel(), 0.0f);
}

// ── Timer contract (HYBRID-FIDELITY-R1 Part B) ───────────────────────────────

TEST_F(TransportDockTest, Timer_DefaultIsZeroNotPlaceholder) {
    // The dock must show 00:00:00 at construction, not the legacy --:--:-- placeholder.
    TransportDock dock;
    auto* timer = dock.findChild<QLabel*>(QStringLiteral("recordDockTimer"));
    ASSERT_NE(timer, nullptr);
    EXPECT_EQ(timer->text(), QStringLiteral("00:00:00"));
    EXPECT_NE(timer->text(), QStringLiteral("--:--:--"));
}

TEST_F(TransportDockTest, Timer_Recording_NeverShowsPlaceholder) {
    // The view layer must set a live clock (00:00:NN), never the --:--:-- sentinel,
    // when the dock is in the Recording state.
    TransportDock dock;
    dock.setState(TransportDock::State::Recording);
    dock.setTimerText(QStringLiteral("00:00:00")); // immediate start — 0 s elapsed
    dock.setTimerRole(QStringLiteral("recording"));

    auto* timer = dock.findChild<QLabel*>(QStringLiteral("recordDockTimer"));
    ASSERT_NE(timer, nullptr);
    EXPECT_NE(timer->text(), QStringLiteral("--:--:--"));
    EXPECT_EQ(timer->text(), QStringLiteral("00:00:00"));
}

TEST_F(TransportDockTest, Timer_AdvancesWhenSetToLaterValue) {
    TransportDock dock;
    dock.setState(TransportDock::State::Recording);
    dock.setTimerText(QStringLiteral("00:00:05")); // 5 s elapsed

    auto* timer = dock.findChild<QLabel*>(QStringLiteral("recordDockTimer"));
    ASSERT_NE(timer, nullptr);
    EXPECT_EQ(timer->text(), QStringLiteral("00:00:05"));
    EXPECT_NE(timer->text(), QStringLiteral("--:--:--"));
}

TEST_F(TransportDockTest, Timer_PausedFreezes) {
    // The dock retains the last-set text when paused (view must not send --:--:--).
    TransportDock dock;
    dock.setState(TransportDock::State::Recording);
    dock.setTimerText(QStringLiteral("00:01:30"));
    dock.setState(TransportDock::State::Paused);
    dock.setTimerRole(QStringLiteral("paused"));

    auto* timer = dock.findChild<QLabel*>(QStringLiteral("recordDockTimer"));
    ASSERT_NE(timer, nullptr);
    EXPECT_EQ(timer->text(), QStringLiteral("00:01:30")); // frozen at pause time
}

TEST_F(TransportDockTest, Timer_CompletedRetainsDuration) {
    TransportDock dock;
    dock.setState(TransportDock::State::Recording);
    dock.setTimerText(QStringLiteral("00:03:42"));
    dock.setState(TransportDock::State::Completed);
    dock.setTimerRole(QStringLiteral("done"));

    auto* timer = dock.findChild<QLabel*>(QStringLiteral("recordDockTimer"));
    ASSERT_NE(timer, nullptr);
    EXPECT_EQ(timer->text(), QStringLiteral("00:03:42"));
}

TEST_F(TransportDockTest, Timer_ReadyResetsToZero) {
    TransportDock dock;
    dock.setState(TransportDock::State::Recording);
    dock.setTimerText(QStringLiteral("00:00:10"));
    dock.setState(TransportDock::State::Ready);
    dock.setTimerText(QStringLiteral("00:00:00")); // view resets on Ready
    dock.setTimerRole(QStringLiteral("idle"));

    auto* timer = dock.findChild<QLabel*>(QStringLiteral("recordDockTimer"));
    ASSERT_NE(timer, nullptr);
    EXPECT_EQ(timer->text(), QStringLiteral("00:00:00"));
}

} // namespace
} // namespace exosnap
