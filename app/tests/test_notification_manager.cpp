// NotificationManager unit tests
//
// The model under test: the hub is the record, the toast is a glance at it.
//   1. Every Enqueue() emits eventRecorded() exactly once — hub feed.
//   2. Timed toasts occupy a single slot: a new timed toast replaces the
//      current one, and never a standing one.
//   3. Standing toasts stack without limit and never auto-dismiss.
//   4. The timed toast, when present, is always the LAST visible element.
//   5. PresetSwitched is recorded but never shown.
//   6. SetToastsEnabled(false) suppresses toasts but not the record.
//
// These tests exercise pure queue / lifetime logic with no window.
// QCoreApplication is sufficient (no QApplication / widgets needed).

#include <gtest/gtest.h>

#include <QCoreApplication>

#include "notifications/NotificationEvent.h"
#include "notifications/NotificationManager.h"

namespace exosnap::notifications {
namespace {

// ── Helpers ──────────────────────────────────────────────────────────────────

QCoreApplication* EnsureApp() {
    if (auto* existing = QCoreApplication::instance())
        return existing;
    static int argc = 1;
    static char name[] = "notification_manager_tests";
    static char* argv[] = {name, nullptr};
    static QCoreApplication app(argc, argv);
    return &app;
}

NotificationEvent MakeEvent(NotificationType type, const QString& title = QStringLiteral("Title"),
                            const QString& body = QStringLiteral("Body")) {
    NotificationEvent e;
    e.type = type;
    e.title = title;
    e.body = body;
    return e;
}

// ── Test fixture ─────────────────────────────────────────────────────────────

class NotificationManagerTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApp();
    }

    NotificationManager mgr;
};

// ── Enqueue basics ───────────────────────────────────────────────────────────

TEST_F(NotificationManagerTest, Enqueue_SingleEvent_AppearsInVisible) {
    mgr.Enqueue(MakeEvent(NotificationType::Saved));
    EXPECT_EQ(mgr.VisibleEvents().size(), 1);
}

TEST_F(NotificationManagerTest, Enqueue_TitleAndBodyPreserved) {
    NotificationEvent e = MakeEvent(NotificationType::Saved, QStringLiteral("Hello"), QStringLiteral("World"));
    mgr.Enqueue(e);
    ASSERT_EQ(mgr.VisibleEvents().size(), 1);
    EXPECT_EQ(mgr.VisibleEvents()[0].title, QStringLiteral("Hello"));
    EXPECT_EQ(mgr.VisibleEvents()[0].body, QStringLiteral("World"));
    EXPECT_EQ(mgr.VisibleEvents()[0].type, NotificationType::Saved);
}

TEST_F(NotificationManagerTest, Enqueue_SequenceIsMonotonicallyIncreasing) {
    mgr.Enqueue(MakeEvent(NotificationType::LowStorage));
    mgr.Enqueue(MakeEvent(NotificationType::UnexpectedStop));
    const auto& vis = mgr.VisibleEvents();
    ASSERT_EQ(vis.size(), 2);
    EXPECT_LT(vis[0].sequence, vis[1].sequence);
}

// ── Timed vs. standing classification ────────────────────────────────────────
TEST(NotificationManagerTiming, AnEarlyWakeUpLeavesTheTimerArmed) {
    // The defect that made the "Recording saved" toast permanent. Qt gives a
    // coarse timer 5 % of slack in either direction -- half a second on a
    // ten-second toast -- so the handler routinely runs before anything has
    // expired. Rescheduling only when something was removed disarmed the one
    // thing that would ever remove it.
    EnsureApp();
    NotificationManager mgr;
    mgr.Enqueue(MakeEvent(NotificationType::Saved));
    ASSERT_EQ(mgr.VisibleEvents().size(), 1);
    ASSERT_TRUE(mgr.DismissTimerArmedForTest());

    mgr.FireDismissTimerForTest();

    EXPECT_EQ(mgr.VisibleEvents().size(), 1) << "nothing had expired yet";
    EXPECT_TRUE(mgr.DismissTimerArmedForTest()) << "the toast is now on screen with nothing to remove it";
}

TEST(NotificationManagerTiming, NothingIsArmedOnceOnlyStandingToastsRemain) {
    // The other half: a timer left running with nothing to expire would wake the
    // application for no reason for as long as the condition holds.
    EnsureApp();
    NotificationManager mgr;
    mgr.Enqueue(MakeEvent(NotificationType::LowStorage));
    ASSERT_TRUE(NotificationManager::IsStanding(NotificationType::LowStorage));
    EXPECT_FALSE(mgr.DismissTimerArmedForTest());

    mgr.FireDismissTimerForTest();
    EXPECT_FALSE(mgr.DismissTimerArmedForTest());
}

// Standing is exactly DismissIntervalMs == 0, and it means one specific thing: a
// CONDITION that is true right now and that will CLEAR ITSELF when it stops being
// true. Everything else is an event that already happened, and events are timed.

TEST_F(NotificationManagerTest, Standing_Types_AreExactlyTheSelfClearingConditions) {
    // The drive is still full; the audio source is still gone; the capture is still
    // producing no frames. Each of the three is dismissed by the composition root
    // the moment its condition ends.
    EXPECT_TRUE(NotificationManager::IsStanding(NotificationType::LowStorage));
    EXPECT_TRUE(NotificationManager::IsStanding(NotificationType::AudioSourceDegraded));
    EXPECT_TRUE(NotificationManager::IsStanding(NotificationType::WindowCaptureStalled));

    // These two used to be standing and were the reason the rule needed stating.
    // Neither is a condition: a recording that stopped unexpectedly stays stopped,
    // and an unfinalized recording stays unfinalized, so nothing was ever going to
    // come along and clear them and they stood for the rest of the session. The hub
    // keeps both, and the recovery surface offers itself again at startup.
    EXPECT_FALSE(NotificationManager::IsStanding(NotificationType::UnexpectedStop));
    EXPECT_FALSE(NotificationManager::IsStanding(NotificationType::RecoveryAvailable));

    EXPECT_FALSE(NotificationManager::IsStanding(NotificationType::Saved));
    EXPECT_FALSE(NotificationManager::IsStanding(NotificationType::UpdateAvailable));
    EXPECT_FALSE(NotificationManager::IsStanding(NotificationType::FramesDropped));
    EXPECT_FALSE(NotificationManager::IsStanding(NotificationType::SettingsRepaired));
    EXPECT_FALSE(NotificationManager::IsStanding(NotificationType::OverlayOmitted));
    EXPECT_FALSE(NotificationManager::IsStanding(NotificationType::PresetSwitched));
    EXPECT_FALSE(NotificationManager::IsStanding(NotificationType::HotkeyConflict));
    EXPECT_FALSE(NotificationManager::IsStanding(NotificationType::SettingsSaveFailed));
}

TEST_F(NotificationManagerTest, TimedDwellsAreExactlyTwoValues) {
    // Asserted as a SET rather than per type: the point of the classification is
    // that there are two answers and a type picks one, so a bespoke third value is
    // the thing worth failing on -- not a particular type moving between them.
    for (const NotificationType type :
         {NotificationType::Saved, NotificationType::UnexpectedStop, NotificationType::RecoveryAvailable,
          NotificationType::UpdateAvailable, NotificationType::FramesDropped, NotificationType::SettingsRepaired,
          NotificationType::PresetSwitched, NotificationType::OverlayOmitted, NotificationType::HotkeyConflict,
          NotificationType::SettingsSaveFailed, NotificationType::CaptureActionFailed,
          NotificationType::RecoveryProtectionUnavailable, NotificationType::SettingsLoadFailed,
          NotificationType::LowStorage, NotificationType::AudioSourceDegraded, NotificationType::FrameCaptured,
          NotificationType::PresetTransferFailed, NotificationType::WindowCaptureStalled}) {
        const int dwell = NotificationManager::DismissIntervalMs(type);
        EXPECT_TRUE(dwell == 0 || dwell == NotificationManager::kDwellBrief ||
                    dwell == NotificationManager::kDwellAction)
            << "type " << static_cast<int>(type) << " carries a bespoke dwell of " << dwell;
    }
}

TEST_F(NotificationManagerTest, ADwellIsChosenByWhetherThereIsAnythingToDo) {
    // Saved is the case that drove the split: it is the only toast carrying two
    // actions and a filename, it appears the instant a recording ends, and it used
    // to have the SHORTEST dwell in the system.
    EXPECT_EQ(NotificationManager::kDismissMs_Saved, NotificationManager::kDwellAction);
    EXPECT_EQ(NotificationManager::kDismissMs_SettingsSaveFailed, NotificationManager::kDwellAction);
    // Nothing to do about either: the repair already happened, the overlay was
    // already omitted. A glance is the whole interaction.
    EXPECT_EQ(NotificationManager::kDismissMs_SettingsRepaired, NotificationManager::kDwellBrief);
    EXPECT_EQ(NotificationManager::kDismissMs_OverlayOmitted, NotificationManager::kDwellBrief);
    // Past 10 s a toast reads as standing and teaches the reflex to dismiss unread.
    EXPECT_LE(NotificationManager::kDwellAction, 10000);
    EXPECT_LT(NotificationManager::kDwellBrief, NotificationManager::kDwellAction);
}

// ── One timed slot ───────────────────────────────────────────────────────────

TEST_F(NotificationManagerTest, TimedToast_ReplacesThePreviousTimedToast) {
    mgr.Enqueue(MakeEvent(NotificationType::Saved, QStringLiteral("first")));
    mgr.Enqueue(MakeEvent(NotificationType::UpdateAvailable, QStringLiteral("second")));
    ASSERT_EQ(mgr.VisibleEvents().size(), 1);
    EXPECT_EQ(mgr.VisibleEvents()[0].title, QStringLiteral("second"));
}

TEST_F(NotificationManagerTest, TimedToast_NeverReplacesAStandingToast) {
    mgr.Enqueue(MakeEvent(NotificationType::LowStorage, QStringLiteral("standing")));
    mgr.Enqueue(MakeEvent(NotificationType::Saved, QStringLiteral("timed")));
    ASSERT_EQ(mgr.VisibleEvents().size(), 2);
    EXPECT_EQ(mgr.VisibleEvents()[0].title, QStringLiteral("standing"));
    EXPECT_EQ(mgr.VisibleEvents()[1].title, QStringLiteral("timed"));
}

TEST_F(NotificationManagerTest, TimedToast_IsAlwaysTheLastVisibleElement) {
    mgr.Enqueue(MakeEvent(NotificationType::Saved, QStringLiteral("timed")));
    mgr.Enqueue(MakeEvent(NotificationType::LowStorage, QStringLiteral("standing-1")));
    mgr.Enqueue(MakeEvent(NotificationType::AudioSourceDegraded, QStringLiteral("standing-2")));
    ASSERT_EQ(mgr.VisibleEvents().size(), 3);
    EXPECT_EQ(mgr.VisibleEvents()[0].title, QStringLiteral("standing-1"));
    EXPECT_EQ(mgr.VisibleEvents()[1].title, QStringLiteral("standing-2"));
    EXPECT_EQ(mgr.VisibleEvents()[2].title, QStringLiteral("timed"));
}

// ── Standing toasts stack without limit ──────────────────────────────────────

TEST_F(NotificationManagerTest, StandingToasts_StackWithoutLimit) {
    for (int i = 0; i < 5; ++i)
        mgr.Enqueue(MakeEvent(NotificationType::LowStorage, QStringLiteral("S%1").arg(i)));
    EXPECT_EQ(mgr.VisibleEvents().size(), 5);
}

// ── Manual dismiss ────────────────────────────────────────────────────────────

TEST_F(NotificationManagerTest, Dismiss_ValidSequence_RemovesEvent) {
    mgr.Enqueue(MakeEvent(NotificationType::LowStorage, QStringLiteral("A")));
    mgr.Enqueue(MakeEvent(NotificationType::UnexpectedStop, QStringLiteral("B")));
    ASSERT_EQ(mgr.VisibleEvents().size(), 2);
    const uint64_t seq = mgr.VisibleEvents()[0].sequence;

    mgr.Dismiss(seq);
    ASSERT_EQ(mgr.VisibleEvents().size(), 1);
    EXPECT_EQ(mgr.VisibleEvents()[0].title, QStringLiteral("B"));
}

TEST_F(NotificationManagerTest, Dismiss_InvalidSequence_IsNoOp) {
    mgr.Enqueue(MakeEvent(NotificationType::Saved));
    ASSERT_EQ(mgr.VisibleEvents().size(), 1);
    mgr.Dismiss(9999); // non-existent
    EXPECT_EQ(mgr.VisibleEvents().size(), 1);
}

TEST_F(NotificationManagerTest, DismissAll_VisibleSetIsEmpty) {
    mgr.Enqueue(MakeEvent(NotificationType::Saved));
    mgr.Enqueue(MakeEvent(NotificationType::LowStorage));

    for (int i = mgr.VisibleEvents().size() - 1; i >= 0; --i) {
        mgr.Dismiss(mgr.VisibleEvents()[i].sequence);
    }
    EXPECT_TRUE(mgr.VisibleEvents().isEmpty());
}

// ── The hub feed: eventRecorded ──────────────────────────────────────────────

TEST_F(NotificationManagerTest, Enqueue_EmitsEventRecordedExactlyOnce) {
    int count = 0;
    NotificationEvent recorded;
    QObject::connect(
        &mgr, &NotificationManager::eventRecorded, &mgr,
        [&](const NotificationEvent& e) {
            ++count;
            recorded = e;
        },
        Qt::DirectConnection);

    mgr.Enqueue(MakeEvent(NotificationType::Saved, QStringLiteral("hello")));
    EXPECT_EQ(count, 1);
    EXPECT_EQ(recorded.title, QStringLiteral("hello"));
    EXPECT_GT(recorded.sequence, 0u) << "the sequence must be assigned before the record is announced";
}

TEST_F(NotificationManagerTest, ReplacedTimedToast_StillLeftARecord) {
    int count = 0;
    QObject::connect(
        &mgr, &NotificationManager::eventRecorded, &mgr, [&count](const NotificationEvent&) { ++count; },
        Qt::DirectConnection);

    mgr.Enqueue(MakeEvent(NotificationType::Saved));
    mgr.Enqueue(MakeEvent(NotificationType::FramesDropped)); // replaces the toast…
    EXPECT_EQ(count, 2) << "…but both events belong in the record";
}

// ── PresetSwitched: recorded, never shown ────────────────────────────────────

TEST_F(NotificationManagerTest, PresetSwitched_IsRecordedButNeverShown) {
    int recorded = 0;
    QObject::connect(
        &mgr, &NotificationManager::eventRecorded, &mgr, [&recorded](const NotificationEvent&) { ++recorded; },
        Qt::DirectConnection);

    NotificationEvent e = MakeEvent(NotificationType::PresetSwitched, QStringLiteral("Switched"));
    e.action = NotificationAction::UndoPresetSwitch;
    mgr.Enqueue(e);

    EXPECT_EQ(recorded, 1);
    EXPECT_TRUE(mgr.VisibleEvents().isEmpty());
}

// ── Toasts disabled: the record survives ─────────────────────────────────────

TEST_F(NotificationManagerTest, ToastsDisabled_SuppressesToastsButNotTheRecord) {
    mgr.SetToastsEnabled(false);

    int recorded = 0;
    QObject::connect(
        &mgr, &NotificationManager::eventRecorded, &mgr, [&recorded](const NotificationEvent&) { ++recorded; },
        Qt::DirectConnection);

    mgr.Enqueue(MakeEvent(NotificationType::Saved));
    mgr.Enqueue(MakeEvent(NotificationType::LowStorage));

    EXPECT_EQ(recorded, 2);
    EXPECT_TRUE(mgr.VisibleEvents().isEmpty());
}

TEST_F(NotificationManagerTest, DisablingToasts_ClearsTheVisibleSet) {
    mgr.Enqueue(MakeEvent(NotificationType::LowStorage));
    ASSERT_EQ(mgr.VisibleEvents().size(), 1);

    mgr.SetToastsEnabled(false);
    EXPECT_TRUE(mgr.VisibleEvents().isEmpty());
}

// ── Signal emission ───────────────────────────────────────────────────────────

TEST_F(NotificationManagerTest, Enqueue_EmitsVisibleSetChanged) {
    int signal_count = 0;
    QObject::connect(
        &mgr, &NotificationManager::visibleSetChanged, &mgr, [&signal_count]() { ++signal_count; },
        Qt::DirectConnection);
    mgr.Enqueue(MakeEvent(NotificationType::Saved));
    EXPECT_EQ(signal_count, 1);
}

TEST_F(NotificationManagerTest, Dismiss_EmitsVisibleSetChanged) {
    mgr.Enqueue(MakeEvent(NotificationType::Saved));
    const uint64_t seq = mgr.VisibleEvents()[0].sequence;

    int signal_count = 0;
    QObject::connect(
        &mgr, &NotificationManager::visibleSetChanged, &mgr, [&signal_count]() { ++signal_count; },
        Qt::DirectConnection);
    mgr.Dismiss(seq);
    EXPECT_GE(signal_count, 1);
}

// ── Action and payload preserved ─────────────────────────────────────────────

TEST_F(NotificationManagerTest, Enqueue_ActionAndPayloadPreserved) {
    NotificationEvent e;
    e.type = NotificationType::Saved;
    e.title = QStringLiteral("Saved");
    e.body = QStringLiteral("file.mkv");
    e.action = NotificationAction::OpenFolder;
    e.action_payload = QStringLiteral("C:/Videos");
    mgr.Enqueue(e);

    ASSERT_EQ(mgr.VisibleEvents().size(), 1);
    EXPECT_EQ(mgr.VisibleEvents()[0].action, NotificationAction::OpenFolder);
    EXPECT_EQ(mgr.VisibleEvents()[0].action_payload, QStringLiteral("C:/Videos"));
}

// ── actionableEventShown signal (tray badge) ─────────────────────────────────

TEST_F(NotificationManagerTest, Enqueue_ActionableEvent_EmitsActionableEventShown) {
    int count = 0;
    QObject::connect(
        &mgr, &NotificationManager::actionableEventShown, &mgr, [&count]() { ++count; }, Qt::DirectConnection);

    NotificationEvent e;
    e.type = NotificationType::Saved;
    e.action = NotificationAction::OpenFolder;
    mgr.Enqueue(e);
    EXPECT_EQ(count, 1);
}

TEST_F(NotificationManagerTest, Enqueue_NoActionEvent_DoesNotEmitActionableEventShown) {
    int count = 0;
    QObject::connect(
        &mgr, &NotificationManager::actionableEventShown, &mgr, [&count]() { ++count; }, Qt::DirectConnection);

    NotificationEvent e;
    e.type = NotificationType::LowStorage;
    e.action = NotificationAction::None;
    mgr.Enqueue(e);
    EXPECT_EQ(count, 0);
}

// ── hasAction helper ──────────────────────────────────────────────────────────

TEST_F(NotificationManagerTest, HasAction_NoneAction_ReturnsFalse) {
    NotificationEvent e;
    e.action = NotificationAction::None;
    e.secondary_action = NotificationAction::None;
    EXPECT_FALSE(e.hasAction());
}

TEST_F(NotificationManagerTest, HasAction_PrimaryAction_ReturnsTrue) {
    NotificationEvent e;
    e.action = NotificationAction::OpenFolder;
    EXPECT_TRUE(e.hasAction());
}

TEST_F(NotificationManagerTest, HasAction_SecondaryAction_ReturnsTrue) {
    NotificationEvent e;
    e.action = NotificationAction::None;
    e.secondary_action = NotificationAction::Discard;
    EXPECT_TRUE(e.hasAction());
}

// ---------------------------------------------------------------------------
// "Webcam not recorded" reports a condition of the running session that the user
// can do nothing about mid-recording. It states the fact and leaves on its own.
// ---------------------------------------------------------------------------
TEST(NotificationManagerOverlayOmitted, AutoDismisses) {
    EXPECT_GT(NotificationManager::DismissIntervalMs(NotificationType::OverlayOmitted), 0)
        << "the recording is unaffected; the notice must not demand a decision";
}

TEST(NotificationManagerOverlayOmitted, IsNotStandingLikeAFailure) {
    EXPECT_NE(NotificationManager::DismissIntervalMs(NotificationType::OverlayOmitted),
              NotificationManager::DismissIntervalMs(NotificationType::UnexpectedStop));
}

// ── Enqueue() returns the assigned sequence ───────────────────────────────────
// A caller (MainWindow) that raises a standing toast programmatically needs the
// sequence back immediately so it can Dismiss() the SAME toast later when the
// condition it reports clears on its own — not via a user click on the ✕.

TEST_F(NotificationManagerTest, Enqueue_ReturnsTheAssignedSequence) {
    const uint64_t seq = mgr.Enqueue(MakeEvent(NotificationType::Saved));
    ASSERT_EQ(mgr.VisibleEvents().size(), 1);
    EXPECT_EQ(seq, mgr.VisibleEvents()[0].sequence);
}

TEST_F(NotificationManagerTest, Enqueue_ReturnedSequence_DismissesTheRightEvent) {
    const uint64_t first = mgr.Enqueue(MakeEvent(NotificationType::LowStorage));
    const uint64_t second = mgr.Enqueue(MakeEvent(NotificationType::RecoveryAvailable));
    ASSERT_EQ(mgr.VisibleEvents().size(), 2);

    mgr.Dismiss(first);
    ASSERT_EQ(mgr.VisibleEvents().size(), 1);
    EXPECT_EQ(mgr.VisibleEvents()[0].sequence, second);
}

TEST_F(NotificationManagerTest, Enqueue_ReturnsSequenceEvenWhenToastsDisabled) {
    // The hub still records every event (and needs its identity) even though no
    // toast becomes visible — mirrors the "recorded but not shown" contract.
    mgr.SetToastsEnabled(false);
    const uint64_t seq = mgr.Enqueue(MakeEvent(NotificationType::AudioSourceDegraded));
    EXPECT_GT(seq, 0u);
    EXPECT_TRUE(mgr.VisibleEvents().isEmpty());
}

// ---------------------------------------------------------------------------
// AudioSourceDegraded (ADR 0046 follow-up): a live device-loss condition, exactly
// like LowStorage/UnexpectedStop/RecoveryAvailable — standing, never auto-dismisses.
// ---------------------------------------------------------------------------

TEST(NotificationManagerAudioSourceDegraded, IsStanding) {
    EXPECT_EQ(NotificationManager::DismissIntervalMs(NotificationType::AudioSourceDegraded), 0);
    EXPECT_TRUE(NotificationManager::IsStanding(NotificationType::AudioSourceDegraded));
}

// ── MakeAudioSourceDegradedEvent (pure resolver) ──────────────────────────────

TEST(MakeAudioSourceDegradedEventTest, SingleSource_UsesSingularCalmWording) {
    const NotificationEvent e = MakeAudioSourceDegradedEvent(1);
    EXPECT_EQ(e.type, NotificationType::AudioSourceDegraded);
    EXPECT_EQ(e.title, QStringLiteral("Audio source went silent"));
    EXPECT_TRUE(e.body.contains(QStringLiteral("An audio source")));
    EXPECT_TRUE(e.body.contains(QStringLiteral("Recording continues")));
    EXPECT_EQ(e.action, NotificationAction::OpenDiagnostics);
}

TEST(MakeAudioSourceDegradedEventTest, MultipleSources_UsesPluralWordingWithCount) {
    const NotificationEvent e = MakeAudioSourceDegradedEvent(3);
    EXPECT_EQ(e.title, QStringLiteral("Audio sources went silent"));
    EXPECT_TRUE(e.body.contains(QStringLiteral("3 audio sources")));
    EXPECT_EQ(e.action, NotificationAction::OpenDiagnostics);
}

TEST(MakeAudioSourceDegradedEventTest, NeverAlarmist_NoExclamationOrErrorWording) {
    // CLAUDE.md: Diagnostics/notifications stay calm, not alarmist. The recording
    // is unaffected by a degraded source — the wording must not read as a failure.
    for (uint32_t count : {1u, 2u, 5u}) {
        const NotificationEvent e = MakeAudioSourceDegradedEvent(count);
        EXPECT_FALSE(e.title.contains(QLatin1Char('!')));
        EXPECT_FALSE(e.body.contains(QLatin1Char('!')));
        EXPECT_FALSE(e.body.contains(QStringLiteral("fail"), Qt::CaseInsensitive));
        EXPECT_FALSE(e.body.contains(QStringLiteral("error"), Qt::CaseInsensitive));
    }
}

// ── MakeWindowCaptureStalledEvent (pure resolver, QCR-804) ───────────────────
// The wording carries the whole truthfulness contract of the feature, so it is
// pinned here rather than left to the composition root.

TEST(MakeWindowCaptureStalledEventTest, StatesTheMeasurementAndNotACause) {
    const NotificationEvent e = MakeWindowCaptureStalledEvent(10.4, /*exclusive_fullscreen_hint=*/false);
    EXPECT_EQ(e.type, NotificationType::WindowCaptureStalled);
    EXPECT_TRUE(e.title.contains(QStringLiteral("appears to have stalled")));
    EXPECT_TRUE(e.body.contains(QStringLiteral("10 seconds")));
    EXPECT_TRUE(e.body.contains(QStringLiteral("may be frozen")));
    EXPECT_EQ(e.action, NotificationAction::OpenDiagnostics);
    // Without corroboration, exclusive fullscreen must not be named at all.
    EXPECT_FALSE(e.body.contains(QStringLiteral("fullscreen"), Qt::CaseInsensitive));
}

TEST(MakeWindowCaptureStalledEventTest, SaysTheRecordingIsStillRunning) {
    // Not a failure report: the file keeps growing and Stop/Pause still work.
    for (const bool hint : {false, true}) {
        const NotificationEvent e = MakeWindowCaptureStalledEvent(12.0, hint);
        EXPECT_TRUE(e.body.contains(QStringLiteral("recording is still running")));
        EXPECT_FALSE(e.body.contains(QStringLiteral("fail"), Qt::CaseInsensitive));
        EXPECT_FALSE(e.title.contains(QLatin1Char('!')));
        EXPECT_FALSE(e.body.contains(QLatin1Char('!')));
    }
}

TEST(MakeWindowCaptureStalledEventTest, FullscreenHintIsConditionalNeverAClaim) {
    const NotificationEvent e = MakeWindowCaptureStalledEvent(10.0, /*exclusive_fullscreen_hint=*/true);
    EXPECT_TRUE(e.body.contains(QStringLiteral("If the application switched to exclusive fullscreen")));
    // "detected" would assert a cause the pre-flight ladder alone cannot prove.
    EXPECT_FALSE(e.body.contains(QStringLiteral("detected"), Qt::CaseInsensitive));
}

TEST(NotificationTypeDwellTest, WindowCaptureStalled_IsStanding) {
    // It reports a condition that still holds; the composition root clears it on
    // recovery and at session end.
    EXPECT_EQ(NotificationManager::DismissIntervalMs(NotificationType::WindowCaptureStalled), 0);
    EXPECT_TRUE(NotificationManager::IsStanding(NotificationType::WindowCaptureStalled));
}

// ── Replace-in-place lifecycle (MainWindow's Dismiss-then-Enqueue pattern) ────
// MainWindow does not call a manager "update" API — it owns the tracked sequence
// and replaces the standing toast by dismissing the old one and enqueueing the
// new body. This exercises that exact sequence at the manager level.

TEST_F(NotificationManagerTest, AudioSourceDegraded_ReplaceInPlace_StaysAtOneVisibleEntry) {
    const uint64_t first = mgr.Enqueue(MakeAudioSourceDegradedEvent(1));
    ASSERT_EQ(mgr.VisibleEvents().size(), 1);

    // The degraded count changed 1 -> 2: dismiss the old one, enqueue the new body.
    mgr.Dismiss(first);
    const uint64_t second = mgr.Enqueue(MakeAudioSourceDegradedEvent(2));

    ASSERT_EQ(mgr.VisibleEvents().size(), 1);
    EXPECT_NE(first, second);
    EXPECT_EQ(mgr.VisibleEvents()[0].sequence, second);
    EXPECT_EQ(mgr.VisibleEvents()[0].title, QStringLiteral("Audio sources went silent"));
}

TEST_F(NotificationManagerTest, AudioSourceDegraded_StacksAboveATimedToast) {
    // Standing toasts insert before a trailing timed toast (never displace it).
    mgr.Enqueue(MakeEvent(NotificationType::Saved)); // timed
    mgr.Enqueue(MakeAudioSourceDegradedEvent(1));    // standing

    const auto& vis = mgr.VisibleEvents();
    ASSERT_EQ(vis.size(), 2);
    EXPECT_EQ(vis.last().type, NotificationType::Saved) << "the timed toast stays anchor-nearest";
    EXPECT_EQ(vis.first().type, NotificationType::AudioSourceDegraded);
}

// ── AdvisoryStatusForType ────────────────────────────────────────────────────
// The status drives the title-bar bell's dot colour, so every type is pinned
// here: a wrong rung is not a cosmetic icon slip, it is the bar reporting the
// wrong urgency. Types that used to fall through the old `default` to "info"
// are called out individually below.

TEST(AdvisoryStatusForTypeTest, Saved_IsSuccess) {
    EXPECT_EQ(AdvisoryStatusForType(NotificationType::Saved), QStringLiteral("success"));
}

TEST(AdvisoryStatusForTypeTest, FailuresAreErrors) {
    EXPECT_EQ(AdvisoryStatusForType(NotificationType::UnexpectedStop), QStringLiteral("error"));
    // Crosses the hard-stop threshold and ends the recording — not a hint.
    EXPECT_EQ(AdvisoryStatusForType(NotificationType::LowStorage), QStringLiteral("error"));
    // Previously fell through to "info": the write failed and the change may be lost.
    EXPECT_EQ(AdvisoryStatusForType(NotificationType::SettingsSaveFailed), QStringLiteral("error"));
    // Previously fell through to "info": the requested action did not happen.
    EXPECT_EQ(AdvisoryStatusForType(NotificationType::CaptureActionFailed), QStringLiteral("error"));
}

TEST(AdvisoryStatusForTypeTest, DegradedButWorkingIsCaution) {
    EXPECT_EQ(AdvisoryStatusForType(NotificationType::FramesDropped), QStringLiteral("caution"));
    EXPECT_EQ(AdvisoryStatusForType(NotificationType::OverlayOmitted), QStringLiteral("caution"));
    EXPECT_EQ(AdvisoryStatusForType(NotificationType::AudioSourceDegraded), QStringLiteral("caution"));
    // Previously fell through to "info": a bound hotkey is dead.
    EXPECT_EQ(AdvisoryStatusForType(NotificationType::HotkeyConflict), QStringLiteral("caution"));
    // Previously fell through to "info": the preset store needed repairing.
    EXPECT_EQ(AdvisoryStatusForType(NotificationType::SettingsRepaired), QStringLiteral("caution"));
}

TEST(AdvisoryStatusForTypeTest, WindowCaptureStalled_IsCautionNotError) {
    // QCR-804: the recording is still running and still being written. Coral
    // would claim it failed, which it did not.
    EXPECT_EQ(AdvisoryStatusForType(NotificationType::WindowCaptureStalled), QStringLiteral("caution"));
}

TEST(AdvisoryStatusForTypeTest, RecoveryAvailable_IsCautionNotError) {
    // As "error" the bell would go coral within seconds of every launch that
    // finds a recoverable session. Recovery offers to rescue work; it does not
    // report a live failure.
    EXPECT_EQ(AdvisoryStatusForType(NotificationType::RecoveryAvailable), QStringLiteral("caution"));
}

TEST(AdvisoryStatusForTypeTest, NeutralTypesAreInfo) {
    EXPECT_EQ(AdvisoryStatusForType(NotificationType::UpdateAvailable), QStringLiteral("info"));
    EXPECT_EQ(AdvisoryStatusForType(NotificationType::PresetSwitched), QStringLiteral("info"));
}

TEST(AdvisoryStatusForTypeTest, EveryTypeResolvesToAKnownStatus) {
    // Guards the resolver against a new NotificationType arriving with no
    // considered severity. The switch has no default, so a missing case is a
    // compiler warning first — this is the runtime backstop.
    constexpr NotificationType kAll[] = {
        NotificationType::LowStorage,          NotificationType::Saved,
        NotificationType::UnexpectedStop,      NotificationType::RecoveryAvailable,
        NotificationType::UpdateAvailable,     NotificationType::FramesDropped,
        NotificationType::SettingsRepaired,    NotificationType::PresetSwitched,
        NotificationType::OverlayOmitted,      NotificationType::HotkeyConflict,
        NotificationType::SettingsSaveFailed,  NotificationType::AudioSourceDegraded,
        NotificationType::CaptureActionFailed, NotificationType::RecoveryProtectionUnavailable,
        NotificationType::SettingsLoadFailed,  NotificationType::WindowCaptureStalled,
        NotificationType::FrameCaptured,       NotificationType::PresetTransferFailed,
    };
    // The count is a reminder, not a proof: it compares this list against itself,
    // so a type added here and nowhere else still passes. FrameCaptured was
    // missing from it for exactly that reason.
    ASSERT_EQ(std::size(kAll), 18u) << "a NotificationType was added without a severity decision";

    for (const NotificationType type : kAll) {
        const QString status = AdvisoryStatusForType(type);
        EXPECT_TRUE(status == QStringLiteral("success") || status == QStringLiteral("info") ||
                    status == QStringLiteral("caution") || status == QStringLiteral("error"))
            << "unexpected status '" << status.toStdString() << "' for type " << static_cast<int>(type);
    }
}

} // namespace
} // namespace exosnap::notifications
