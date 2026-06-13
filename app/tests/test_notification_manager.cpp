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

// ── Sticky types do not auto-dismiss ─────────────────────────────────────────

TEST_F(NotificationManagerTest, DismissInterval_UnexpectedStop_IsZero) {
    EXPECT_EQ(NotificationManager::kDismissMs_UnexpectedStop, 0);
}

TEST_F(NotificationManagerTest, DismissInterval_RecoveryAvailable_IsZero) {
    EXPECT_EQ(NotificationManager::kDismissMs_RecoveryAvailable, 0);
}

TEST_F(NotificationManagerTest, DismissInterval_Saved_IsPositive) {
    EXPECT_GT(NotificationManager::kDismissMs_Saved, 0);
}

TEST_F(NotificationManagerTest, DismissInterval_LowStorage_IsPositive) {
    EXPECT_GT(NotificationManager::kDismissMs_LowStorage, 0);
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

} // namespace
} // namespace exosnap::notifications
