#include "ui/chrome/NotificationHubPanel.h"
#include "ui/widgets/AdvisoryItem.h"

#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <gtest/gtest.h>

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "hub_panel_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class NotificationHubPanelTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(NotificationHubPanelTest, Constructs_WithoutCrash) {
    ui::chrome::NotificationHubPanel panel;
    EXPECT_NE(&panel, nullptr);
}

// Helper: find the empty-state container and return whether it is not hidden.
// isVisible() returns false for children of hidden top-level windows, so we
// use !isHidden() on the container (hubEmptyState) — which tracks the explicit
// show/hide call made by refreshEmptyState().
static const QWidget* findEmptyState(const ui::chrome::NotificationHubPanel& panel) {
    return panel.findChild<QWidget*>(QStringLiteral("hubEmptyState"));
}

TEST_F(NotificationHubPanelTest, EmptyState_VisibleByDefault) {
    ui::chrome::NotificationHubPanel panel;
    const auto* empty = findEmptyState(panel);
    ASSERT_NE(empty, nullptr);
    EXPECT_FALSE(empty->isHidden());
}

TEST_F(NotificationHubPanelTest, AddAdvisory_HidesEmptyState) {
    ui::chrome::NotificationHubPanel panel;
    panel.addAdvisory(QStringLiteral("a1"), QStringLiteral("info"), QStringLiteral("Test"), QStringLiteral("Body"),
                      QStringLiteral("now"), true, QStringLiteral("act"), QStringLiteral("Action"), false);
    const auto* empty = findEmptyState(panel);
    ASSERT_NE(empty, nullptr);
    EXPECT_TRUE(empty->isHidden());
}

TEST_F(NotificationHubPanelTest, ClearAdvisories_RestoresEmptyState) {
    ui::chrome::NotificationHubPanel panel;
    panel.addAdvisory(QStringLiteral("a1"), QStringLiteral("info"), QStringLiteral("Test"), QStringLiteral("Body"),
                      QString(), true, QString(), QString(), false);
    panel.clearAdvisories();
    const auto* empty = findEmptyState(panel);
    ASSERT_NE(empty, nullptr);
    EXPECT_FALSE(empty->isHidden());
}

TEST_F(NotificationHubPanelTest, SetDemoAdvisories_PopulatesItems) {
    ui::chrome::NotificationHubPanel panel;
    panel.setDemoAdvisories(true);
    const auto* empty = findEmptyState(panel);
    ASSERT_NE(empty, nullptr);
    EXPECT_TRUE(empty->isHidden());
}

TEST_F(NotificationHubPanelTest, SetDemoAdvisories_False_ClearsItems) {
    ui::chrome::NotificationHubPanel panel;
    panel.setDemoAdvisories(true);
    panel.setDemoAdvisories(false);
    const auto* empty = findEmptyState(panel);
    ASSERT_NE(empty, nullptr);
    EXPECT_FALSE(empty->isHidden());
}

TEST_F(NotificationHubPanelTest, DeepLink_EmitsTheActionTarget) {
    ui::chrome::NotificationHubPanel panel;
    int deep_count = 0;
    QString received;
    QObject::connect(&panel, &ui::chrome::NotificationHubPanel::deepLinkRequested, [&](const QString& target) {
        ++deep_count;
        received = target;
    });
    panel.addAdvisory(QStringLiteral("link-test"), QStringLiteral("caution"), QStringLiteral("Deep link test"),
                      QStringLiteral("Body"), QString(), true, QStringLiteral("dl-id"), QStringLiteral("Open"), true);
    auto* advisory = panel.findChild<ui::widgets::AdvisoryItem*>();
    ASSERT_NE(advisory, nullptr);
    // The action click carries the ACTION id — the navigation target — not the
    // advisory id under which the entry is stored.
    emit advisory->actionTriggered(QStringLiteral("dl-id"));
    EXPECT_EQ(deep_count, 1);
    EXPECT_EQ(received, QStringLiteral("dl-id"));
}

TEST_F(NotificationHubPanelTest, Width_VisiblePanelIs380PlusShadowMargin) {
    ui::chrome::NotificationHubPanel panel;
    // Canon visible width is 380px; the outer popup adds a 4px painted drop-shadow
    // margin on each side (VG-1), so the widget itself measures 388px.
    EXPECT_EQ(panel.width(), 388);
}

TEST_F(NotificationHubPanelTest, HubTitle_IsNotifications) {
    ui::chrome::NotificationHubPanel panel;
    auto* title = panel.findChild<QLabel*>(QStringLiteral("hubTitle"));
    ASSERT_NE(title, nullptr);
    EXPECT_EQ(title->text(), QStringLiteral("Notifications"));
}

// PS-PHASE-E: removeAdvisoryById + unreadCount tests

TEST_F(NotificationHubPanelTest, UnreadCount_ZeroByDefault) {
    ui::chrome::NotificationHubPanel panel;
    EXPECT_EQ(panel.unreadCount(), 0);
}

TEST_F(NotificationHubPanelTest, UnreadCount_IncreasesOnUnreadAdvisory) {
    ui::chrome::NotificationHubPanel panel;
    panel.addAdvisory(QStringLiteral("a1"), QStringLiteral("info"), QStringLiteral("Test"), QStringLiteral("Body"),
                      QStringLiteral("now"), /*unread=*/true, QString(), QString(), false);
    EXPECT_EQ(panel.unreadCount(), 1);

    panel.addAdvisory(QStringLiteral("a2"), QStringLiteral("info"), QStringLiteral("Test2"), QStringLiteral("Body2"),
                      QStringLiteral("now"), /*unread=*/false, QString(), QString(), false);
    // Only the first was unread
    EXPECT_EQ(panel.unreadCount(), 1);
}

TEST_F(NotificationHubPanelTest, RemoveAdvisoryById_NoOpForUnknownId) {
    ui::chrome::NotificationHubPanel panel;
    panel.addAdvisory(QStringLiteral("a1"), QStringLiteral("info"), QStringLiteral("Test"), QStringLiteral("Body"),
                      QString(), true, QString(), QString(), false);
    // Should not crash or change count
    panel.removeAdvisoryById(QStringLiteral("nonexistent"));
    const auto* empty = findEmptyState(panel);
    ASSERT_NE(empty, nullptr);
    EXPECT_TRUE(empty->isHidden()); // still has 1 item
}

TEST_F(NotificationHubPanelTest, RemoveAdvisoryById_RemovesItem) {
    ui::chrome::NotificationHubPanel panel;
    panel.addAdvisory(QStringLiteral("a1"), QStringLiteral("info"), QStringLiteral("Test"), QStringLiteral("Body"),
                      QString(), true, QString(), QString(), false);
    panel.addAdvisory(QStringLiteral("a2"), QStringLiteral("caution"), QStringLiteral("Test2"), QStringLiteral("Body2"),
                      QString(), false, QString(), QString(), false);

    panel.removeAdvisoryById(QStringLiteral("a1"));

    // One item remains — empty state still hidden.
    const auto* empty = findEmptyState(panel);
    ASSERT_NE(empty, nullptr);
    EXPECT_TRUE(empty->isHidden());
    EXPECT_EQ(panel.unreadCount(), 0); // the unread one was removed
}

TEST_F(NotificationHubPanelTest, RemoveAdvisoryById_LastItem_ShowsEmpty) {
    ui::chrome::NotificationHubPanel panel;
    panel.addAdvisory(QStringLiteral("only"), QStringLiteral("info"), QStringLiteral("Only item"),
                      QStringLiteral("Body"), QString(), true, QString(), QString(), false);
    EXPECT_EQ(panel.unreadCount(), 1);

    panel.removeAdvisoryById(QStringLiteral("only"));

    const auto* empty = findEmptyState(panel);
    ASSERT_NE(empty, nullptr);
    EXPECT_FALSE(empty->isHidden()); // should show empty state again
    EXPECT_EQ(panel.unreadCount(), 0);
}

TEST_F(NotificationHubPanelTest, AddAdvisory_DuplicateId_Replaces) {
    ui::chrome::NotificationHubPanel panel;
    panel.addAdvisory(QStringLiteral("dup"), QStringLiteral("info"), QStringLiteral("Original"), QStringLiteral("Body"),
                      QString(), true, QString(), QString(), false);
    EXPECT_EQ(panel.unreadCount(), 1);

    // Adding same id again should replace the original.
    panel.addAdvisory(QStringLiteral("dup"), QStringLiteral("caution"), QStringLiteral("Replaced"),
                      QStringLiteral("Body2"), QString(), false, QString(), QString(), false);
    // The replaced entry is not unread; original was removed.
    EXPECT_EQ(panel.unreadCount(), 0);

    // Empty state should still be hidden (one item exists).
    const auto* empty = findEmptyState(panel);
    ASSERT_NE(empty, nullptr);
    EXPECT_TRUE(empty->isHidden());
}

TEST_F(NotificationHubPanelTest, ClearAdvisories_ResetsUnreadCount) {
    ui::chrome::NotificationHubPanel panel;
    panel.addAdvisory(QStringLiteral("a1"), QStringLiteral("info"), QStringLiteral("Test"), QStringLiteral("Body"),
                      QString(), true, QString(), QString(), false);
    panel.addAdvisory(QStringLiteral("a2"), QStringLiteral("info"), QStringLiteral("Test2"), QStringLiteral("Body2"),
                      QString(), true, QString(), QString(), false);
    EXPECT_EQ(panel.unreadCount(), 2);

    panel.clearAdvisories();
    EXPECT_EQ(panel.unreadCount(), 0);
}

// --- AdvisoryItem entry padding / alignment (Slice B) ---------------------------
// The unread dot used to sit before the title and consume title-row space only on
// unread rows, shoving the title right against its own body and against read rows.
// These tests pin the trailing-dot layout: every title left edge lines up with the
// body beneath it, regardless of the unread state.

namespace {
ui::widgets::AdvisoryItem* buildLaidOutItem(const QString& status, bool unread) {
    auto* item = new ui::widgets::AdvisoryItem();
    item->setStatus(status);
    item->setTitle(QStringLiteral("A concise advisory title"));
    item->setBody(QStringLiteral("Body copy that describes what happened in a sentence."));
    item->setUnread(unread);
    item->resize(360, 120);
    item->show();
    QCoreApplication::processEvents();
    return item;
}

int leftEdgeIn(const ui::widgets::AdvisoryItem* item, const QString& childName) {
    const auto* child = item->findChild<QLabel*>(childName);
    return child ? child->mapTo(item, QPoint(0, 0)).x() : -1;
}
} // namespace

TEST_F(NotificationHubPanelTest, AdvisoryItem_Unread_TitleAlignsWithBody) {
    auto* item = buildLaidOutItem(QStringLiteral("info"), /*unread=*/true);
    const int title_x = leftEdgeIn(item, QStringLiteral("advisoryTitle"));
    const int body_x = leftEdgeIn(item, QStringLiteral("advisoryBody"));
    ASSERT_GE(title_x, 0);
    ASSERT_GE(body_x, 0);
    // Trailing dot: the unread indicator no longer indents the title.
    EXPECT_EQ(title_x, body_x);
    delete item;
}

TEST_F(NotificationHubPanelTest, AdvisoryItem_Read_TitleAlignsWithBody) {
    auto* item = buildLaidOutItem(QStringLiteral("success"), /*unread=*/false);
    const int title_x = leftEdgeIn(item, QStringLiteral("advisoryTitle"));
    const int body_x = leftEdgeIn(item, QStringLiteral("advisoryBody"));
    ASSERT_GE(title_x, 0);
    ASSERT_GE(body_x, 0);
    EXPECT_EQ(title_x, body_x);
    delete item;
}

TEST_F(NotificationHubPanelTest, AdvisoryItem_ReadAndUnread_TitlesShareLeftEdge) {
    auto* read_item = buildLaidOutItem(QStringLiteral("success"), /*unread=*/false);
    auto* unread_item = buildLaidOutItem(QStringLiteral("error"), /*unread=*/true);
    const int read_title_x = leftEdgeIn(read_item, QStringLiteral("advisoryTitle"));
    const int unread_title_x = leftEdgeIn(unread_item, QStringLiteral("advisoryTitle"));
    ASSERT_GE(read_title_x, 0);
    ASSERT_GE(unread_title_x, 0);
    // A read row and an unread row present the same title indentation.
    EXPECT_EQ(read_title_x, unread_title_x);
    delete read_item;
    delete unread_item;
}

} // namespace
} // namespace exosnap
