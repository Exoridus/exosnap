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
// Timed = reports something that already finished. Standing = a condition that
// still holds. Standing is exactly DismissIntervalMs == 0.

TEST_F(NotificationManagerTest, Standing_Types_AreExactlyTheConditionReports) {
    EXPECT_TRUE(NotificationManager::IsStanding(NotificationType::LowStorage));
    EXPECT_TRUE(NotificationManager::IsStanding(NotificationType::UnexpectedStop));
    EXPECT_TRUE(NotificationManager::IsStanding(NotificationType::RecoveryAvailable));

    EXPECT_FALSE(NotificationManager::IsStanding(NotificationType::Saved));
    EXPECT_FALSE(NotificationManager::IsStanding(NotificationType::UpdateAvailable));
    EXPECT_FALSE(NotificationManager::IsStanding(NotificationType::FramesDropped));
    EXPECT_FALSE(NotificationManager::IsStanding(NotificationType::SettingsRepaired));
    EXPECT_FALSE(NotificationManager::IsStanding(NotificationType::OverlayOmitted));
    EXPECT_FALSE(NotificationManager::IsStanding(NotificationType::PresetSwitched));
    EXPECT_FALSE(NotificationManager::IsStanding(NotificationType::HotkeyConflict));
    EXPECT_FALSE(NotificationManager::IsStanding(NotificationType::SettingsSaveFailed));
}

TEST_F(NotificationManagerTest, DismissInterval_Saved_IsExactly5000ms) {
    EXPECT_EQ(NotificationManager::kDismissMs_Saved, 5000);
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
    mgr.Enqueue(MakeEvent(NotificationType::RecoveryAvailable, QStringLiteral("standing-2")));
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

} // namespace
} // namespace exosnap::notifications
