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

TEST_F(NotificationHubPanelTest, DeepLink_EmitsSignal) {
    ui::chrome::NotificationHubPanel panel;
    int deep_count = 0;
    QObject::connect(&panel, &ui::chrome::NotificationHubPanel::deepLinkRequested,
                     [&](const QString&) { ++deep_count; });
    panel.addAdvisory(QStringLiteral("link-test"), QStringLiteral("caution"), QStringLiteral("Deep link test"),
                      QStringLiteral("Body"), QString(), true, QStringLiteral("dl-id"), QStringLiteral("Open"), true);
    auto* advisory = panel.findChild<ui::widgets::AdvisoryItem*>();
    ASSERT_NE(advisory, nullptr);
    emit advisory->deepLinkRequested();
    EXPECT_EQ(deep_count, 1);
}

TEST_F(NotificationHubPanelTest, Width_Is380) {
    ui::chrome::NotificationHubPanel panel;
    EXPECT_EQ(panel.width(), 380);
}

TEST_F(NotificationHubPanelTest, HubTitle_IsNotifications) {
    ui::chrome::NotificationHubPanel panel;
    auto* title = panel.findChild<QLabel*>(QStringLiteral("hubTitle"));
    ASSERT_NE(title, nullptr);
    EXPECT_EQ(title->text(), QStringLiteral("Notifications"));
}

} // namespace
} // namespace exosnap
