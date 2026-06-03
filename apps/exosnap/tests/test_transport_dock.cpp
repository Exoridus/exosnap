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

} // namespace
} // namespace exosnap
