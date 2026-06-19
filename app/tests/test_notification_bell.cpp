#include "ui/widgets/NotificationBell.h"
#include <QApplication>
#include <QCoreApplication>
#include <QToolButton>
#include <gtest/gtest.h>

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "notification_bell_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class NotificationBellTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(NotificationBellTest, Constructs_DefaultZeroCount) {
    ui::widgets::NotificationBell bell;
    EXPECT_EQ(bell.unreadCount(), 0);
}

TEST_F(NotificationBellTest, SetUnreadCount_IsRetrievable) {
    ui::widgets::NotificationBell bell;
    bell.setUnreadCount(5);
    EXPECT_EQ(bell.unreadCount(), 5);
}

TEST_F(NotificationBellTest, SetUnreadCount_Zero_ResetsCount) {
    ui::widgets::NotificationBell bell;
    bell.setUnreadCount(3);
    bell.setUnreadCount(0);
    EXPECT_EQ(bell.unreadCount(), 0);
}

TEST_F(NotificationBellTest, IsQToolButton) {
    ui::widgets::NotificationBell bell;
    EXPECT_NE(qobject_cast<QToolButton*>(&bell), nullptr);
}

TEST_F(NotificationBellTest, FixedSize_34x34) {
    ui::widgets::NotificationBell bell;
    EXPECT_EQ(bell.width(), 34);
    EXPECT_EQ(bell.height(), 34);
}

TEST_F(NotificationBellTest, NoFocusPolicy) {
    ui::widgets::NotificationBell bell;
    EXPECT_EQ(bell.focusPolicy(), Qt::NoFocus);
}

TEST_F(NotificationBellTest, Clicked_Signal_Emitted) {
    // Manual signal recorder — avoids Qt6::Test / QSignalSpy dependency
    ui::widgets::NotificationBell bell;
    int click_count = 0;
    QObject::connect(&bell, &ui::widgets::NotificationBell::clicked, [&]() { ++click_count; });
    bell.click();
    EXPECT_EQ(click_count, 1);
}

TEST_F(NotificationBellTest, LargeCount_DoesNotCrash) {
    ui::widgets::NotificationBell bell;
    bell.setUnreadCount(999);
    EXPECT_EQ(bell.unreadCount(), 999);
}

} // namespace
} // namespace exosnap
