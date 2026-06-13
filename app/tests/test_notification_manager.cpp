// NOTIFY-TOASTS-R1 — NotificationManager unit tests
//
// Covers:
//   1. Enqueue adds to the visible set (up to kMaxVisible).
//   2. FIFO order is preserved.
//   3. Max-concurrent cap: events beyond kMaxVisible go to the pending queue.
//   4. Auto-dismiss removes the right event; sticky types are never auto-dismissed.
//   5. Queue drains into visible slots as visible events are dismissed.
//   6. Manual Dismiss() removes the correct event.
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
    EXPECT_EQ(mgr.PendingCount(), 0);
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
    mgr.Enqueue(MakeEvent(NotificationType::Saved));
    mgr.Enqueue(MakeEvent(NotificationType::LowStorage));
    const auto& vis = mgr.VisibleEvents();
    ASSERT_EQ(vis.size(), 2);
    EXPECT_LT(vis[0].sequence, vis[1].sequence);
}

// ── FIFO order ────────────────────────────────────────────────────────────────

TEST_F(NotificationManagerTest, Enqueue_FifoOrder_WithinVisible) {
    mgr.Enqueue(MakeEvent(NotificationType::Saved, QStringLiteral("A"), QStringLiteral("a")));
    mgr.Enqueue(MakeEvent(NotificationType::LowStorage, QStringLiteral("B"), QStringLiteral("b")));
    mgr.Enqueue(MakeEvent(NotificationType::UnexpectedStop, QStringLiteral("C"), QStringLiteral("c")));
    ASSERT_EQ(mgr.VisibleEvents().size(), 3);
    EXPECT_EQ(mgr.VisibleEvents()[0].title, QStringLiteral("A"));
    EXPECT_EQ(mgr.VisibleEvents()[1].title, QStringLiteral("B"));
    EXPECT_EQ(mgr.VisibleEvents()[2].title, QStringLiteral("C"));
}

// ── Max concurrent cap ────────────────────────────────────────────────────────

TEST_F(NotificationManagerTest, Enqueue_BeyondMaxVisible_GoesToPending) {
    for (int i = 0; i < NotificationManager::kMaxVisible + 2; ++i) {
        mgr.Enqueue(MakeEvent(NotificationType::Saved, QStringLiteral("T%1").arg(i), {}));
    }
    EXPECT_EQ(mgr.VisibleEvents().size(), NotificationManager::kMaxVisible);
    EXPECT_EQ(mgr.PendingCount(), 2);
}

TEST_F(NotificationManagerTest, Enqueue_ExactlyMaxVisible_AllVisible) {
    for (int i = 0; i < NotificationManager::kMaxVisible; ++i) {
        mgr.Enqueue(MakeEvent(NotificationType::Saved, QStringLiteral("T%1").arg(i), {}));
    }
    EXPECT_EQ(mgr.VisibleEvents().size(), NotificationManager::kMaxVisible);
    EXPECT_EQ(mgr.PendingCount(), 0);
}

// ── Manual dismiss ────────────────────────────────────────────────────────────

TEST_F(NotificationManagerTest, Dismiss_ValidSequence_RemovesEvent) {
    mgr.Enqueue(MakeEvent(NotificationType::Saved, QStringLiteral("A"), {}));
    mgr.Enqueue(MakeEvent(NotificationType::LowStorage, QStringLiteral("B"), {}));
    ASSERT_EQ(mgr.VisibleEvents().size(), 2);
    const uint64_t seq = mgr.VisibleEvents()[0].sequence;

    mgr.Dismiss(seq);
    EXPECT_EQ(mgr.VisibleEvents().size(), 1);
    EXPECT_EQ(mgr.VisibleEvents()[0].title, QStringLiteral("B"));
}

TEST_F(NotificationManagerTest, Dismiss_InvalidSequence_IsNoOp) {
    mgr.Enqueue(MakeEvent(NotificationType::Saved));
    ASSERT_EQ(mgr.VisibleEvents().size(), 1);
    mgr.Dismiss(9999); // non-existent
    EXPECT_EQ(mgr.VisibleEvents().size(), 1);
}

TEST_F(NotificationManagerTest, Dismiss_DrainsPendingQueue) {
    // Fill visible up to cap + 1 pending.
    for (int i = 0; i <= NotificationManager::kMaxVisible; ++i) {
        mgr.Enqueue(MakeEvent(NotificationType::Saved, QStringLiteral("T%1").arg(i), {}));
    }
    ASSERT_EQ(mgr.PendingCount(), 1);

    // Dismiss one visible — pending should drain into the slot.
    const uint64_t seq = mgr.VisibleEvents()[0].sequence;
    mgr.Dismiss(seq);

    EXPECT_EQ(mgr.VisibleEvents().size(), NotificationManager::kMaxVisible);
    EXPECT_EQ(mgr.PendingCount(), 0);
}

// ── Per-type dwell behavior (NOTIFY-SKIN-R1: exact spec from Mappe ToastTypeTable) ──
//
// success / "Recording saved"  → Auto-dismiss 5 s (glanceable; file already written)
// caution / "Storage running low" → Sticky (demands a decision before space runs out)
// error   / "Unexpected stop"  → Sticky (failure; never vanish before seen)
// info    / "Recovery available" → Sticky (pending choice on relaunch)

TEST_F(NotificationManagerTest, DismissInterval_Saved_IsExactly5000ms) {
    // spec: "Auto · 5 s"
    EXPECT_EQ(NotificationManager::kDismissMs_Saved, 5000);
}

TEST_F(NotificationManagerTest, DismissInterval_LowStorage_IsZero_Sticky) {
    // spec: "Sticky — demands a decision before space runs out"
    EXPECT_EQ(NotificationManager::kDismissMs_LowStorage, 0);
}

TEST_F(NotificationManagerTest, DismissInterval_UnexpectedStop_IsZero_Sticky) {
    // spec: "Sticky — a failure; never vanish before it's seen"
    EXPECT_EQ(NotificationManager::kDismissMs_UnexpectedStop, 0);
}

TEST_F(NotificationManagerTest, DismissInterval_RecoveryAvailable_IsZero_Sticky) {
    // spec: "Sticky — a pending choice on relaunch"
    EXPECT_EQ(NotificationManager::kDismissMs_RecoveryAvailable, 0);
}

TEST_F(NotificationManagerTest, OnlySaved_IsAutoDissmissType) {
    // Exactly one type should auto-dismiss; the rest are sticky.
    EXPECT_GT(NotificationManager::kDismissMs_Saved, 0);
    EXPECT_EQ(NotificationManager::kDismissMs_LowStorage, 0);
    EXPECT_EQ(NotificationManager::kDismissMs_UnexpectedStop, 0);
    EXPECT_EQ(NotificationManager::kDismissMs_RecoveryAvailable, 0);
}

// ── Signal emission ───────────────────────────────────────────────────────────
// Avoid Qt6::Test / QSignalSpy dependency — use a manual counter connected via
// Qt::DirectConnection so the count is updated synchronously.

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

// ── Visible set emptied when all dismissed ────────────────────────────────────

TEST_F(NotificationManagerTest, DismissAll_VisibleSetIsEmpty) {
    mgr.Enqueue(MakeEvent(NotificationType::Saved));
    mgr.Enqueue(MakeEvent(NotificationType::LowStorage));

    for (int i = mgr.VisibleEvents().size() - 1; i >= 0; --i) {
        mgr.Dismiss(mgr.VisibleEvents()[i].sequence);
    }
    EXPECT_TRUE(mgr.VisibleEvents().isEmpty());
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

// ── actionableEventShown signal (NOTIFY-SKIN-R1: tray badge) ─────────────────

TEST_F(NotificationManagerTest, Enqueue_ActionableEvent_EmitsActionableEventShown) {
    // An event with a non-None action must emit actionableEventShown.
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
    // An event with action == None must NOT emit actionableEventShown.
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

} // namespace
} // namespace exosnap::notifications
