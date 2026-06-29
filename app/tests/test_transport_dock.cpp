#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QFrame>
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

TEST_F(TransportDockTest, CompletedState_ReturnsToReadyLayout) {
    // v10: Completed is no longer a distinct dock state. The dock stays in the
    // Ready layout so the user can immediately start a new recording.
    // The result is surfaced by the NotificationManager toast, not by the dock.
    TransportDock dock;
    dock.setState(TransportDock::State::Completed);

    // Ready layout: Record split pill visible, no stop/pause/resume.
    EXPECT_TRUE(ButtonVisible(dock, "recordDockRecord"));
    EXPECT_FALSE(ButtonVisible(dock, "recordDockStop"));
    EXPECT_FALSE(ButtonVisible(dock, "recordDockPause"));
    // Source toggles always visible in v10 (no result row to swap in).
    auto* system_toggle = Toggle(dock, QStringLiteral("system"));
    ASSERT_NE(system_toggle, nullptr);
    EXPECT_TRUE(system_toggle->isVisibleTo(&dock));
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

// ── Split Record + countdown menu (v10-overlays R2) ─────────────────────────
// The standalone CountdownSelect dropdown is replaced by a split pill button:
// left face = "Record" (starts immediately); right chevron = opens a QMenu
// with "Start in 3/5/10 seconds". Tests verify the new widget model; the
// underlying intentions (available delays, lock during active session) are
// preserved.

TEST_F(TransportDockTest, Countdown_ChevronPresentInReadyState) {
    // In Ready state the split container and its chevron half are both visible
    // and enabled.  countdownSeconds() defaults to 0 (no delay pre-selected).
    TransportDock dock;
    dock.setState(TransportDock::State::Ready);

    auto* container = dock.findChild<QFrame*>(QStringLiteral("recordSplitContainer"));
    auto* chevron = dock.findChild<QPushButton*>(QStringLiteral("recordDockChevron"));
    ASSERT_NE(container, nullptr);
    ASSERT_NE(chevron, nullptr);
    EXPECT_TRUE(container->isVisibleTo(&dock));
    EXPECT_TRUE(chevron->isEnabled());
    EXPECT_EQ(dock.countdownSeconds(), 0);
}

TEST_F(TransportDockTest, Countdown_CanSelectSupportedDelays) {
    // setCountdownSeconds / countdownSeconds provide the preset round-trip for
    // 3 s, 5 s, and 10 s — all three values the chevron menu exposes.
    TransportDock dock;
    dock.setState(TransportDock::State::Ready);

    dock.setCountdownSeconds(3);
    EXPECT_EQ(dock.countdownSeconds(), 3);

    dock.setCountdownSeconds(5);
    EXPECT_EQ(dock.countdownSeconds(), 5);

    dock.setCountdownSeconds(10);
    EXPECT_EQ(dock.countdownSeconds(), 10);

    // Reset to no-delay round-trips correctly.
    dock.setCountdownSeconds(0);
    EXPECT_EQ(dock.countdownSeconds(), 0);
}

TEST_F(TransportDockTest, Countdown_StateShowsCancelAndLocksChevron) {
    // When the dock is in Countdown state the record face shows "Cancel" with
    // stop styling, and the chevron is disabled so the delay cannot be changed
    // while a countdown is running.
    TransportDock dock;
    dock.setCountdownSeconds(3);
    dock.setState(TransportDock::State::Countdown);

    auto* container = dock.findChild<QFrame*>(QStringLiteral("recordSplitContainer"));
    auto* chevron = dock.findChild<QPushButton*>(QStringLiteral("recordDockChevron"));
    auto* record = dock.findChild<QPushButton*>(QStringLiteral("recordDockRecord"));
    ASSERT_NE(container, nullptr);
    ASSERT_NE(chevron, nullptr);
    ASSERT_NE(record, nullptr);

    EXPECT_TRUE(container->isVisibleTo(&dock));
    EXPECT_FALSE(chevron->isEnabled());
    EXPECT_EQ(dock.countdownSeconds(), 3);
    EXPECT_EQ(record->text(), QStringLiteral("Cancel"));
    EXPECT_EQ(record->property("dockAction").toString(), QStringLiteral("stop"));
}

TEST_F(TransportDockTest, Countdown_ContainerHiddenDuringRecordingAndPaused) {
    // The split Record button (container) is hidden while a recording is active
    // or paused — the Pause/Stop and Resume/Stop controls take over instead.
    TransportDock dock;

    dock.setState(TransportDock::State::Recording);
    auto* container = dock.findChild<QFrame*>(QStringLiteral("recordSplitContainer"));
    ASSERT_NE(container, nullptr);
    EXPECT_FALSE(container->isVisibleTo(&dock));

    dock.setState(TransportDock::State::Paused);
    EXPECT_FALSE(container->isVisibleTo(&dock));
}

// ── Dock icon keys and tooltips (HYBRID-RECORD-FIDELITY-R2 Part E) ────────────

TEST_F(TransportDockTest, DockToggle_SystemAudioTooltip) {
    TransportDock dock;
    auto* toggle = Toggle(dock, QStringLiteral("system"));
    ASSERT_NE(toggle, nullptr);
    EXPECT_EQ(toggle->toolTip(), QStringLiteral("System audio"));
    EXPECT_EQ(toggle->property("sourceKey").toString(), QStringLiteral("system"));
}

TEST_F(TransportDockTest, DockToggle_MicrophoneTooltip) {
    TransportDock dock;
    auto* toggle = Toggle(dock, QStringLiteral("mic"));
    ASSERT_NE(toggle, nullptr);
    EXPECT_EQ(toggle->toolTip(), QStringLiteral("Microphone"));
    EXPECT_EQ(toggle->property("sourceKey").toString(), QStringLiteral("mic"));
}

TEST_F(TransportDockTest, DockToggle_WebcamTooltip) {
    // The webcam toggle must read as "Webcam" — the icon key drives the camera SVG path.
    TransportDock dock;
    auto* toggle = Toggle(dock, QStringLiteral("webcam"));
    ASSERT_NE(toggle, nullptr);
    EXPECT_EQ(toggle->toolTip(), QStringLiteral("Webcam"));
    EXPECT_EQ(toggle->property("sourceKey").toString(), QStringLiteral("webcam"));
}

TEST_F(TransportDockTest, DockToggle_AppAudioTooltip) {
    // The app toggle tooltip reads "App audio" (v10 user spec); the icon key still drives the window SVG path.
    TransportDock dock;
    auto* toggle = Toggle(dock, QStringLiteral("app"));
    ASSERT_NE(toggle, nullptr);
    EXPECT_EQ(toggle->toolTip(), QStringLiteral("App audio"));
    EXPECT_EQ(toggle->property("sourceKey").toString(), QStringLiteral("app"));
}

TEST_F(TransportDockTest, DockToggle_FourSourcesPresent) {
    TransportDock dock;
    EXPECT_NE(Toggle(dock, QStringLiteral("system")), nullptr);
    EXPECT_NE(Toggle(dock, QStringLiteral("mic")), nullptr);
    EXPECT_NE(Toggle(dock, QStringLiteral("webcam")), nullptr);
    EXPECT_NE(Toggle(dock, QStringLiteral("app")), nullptr);
}

} // namespace
} // namespace exosnap
