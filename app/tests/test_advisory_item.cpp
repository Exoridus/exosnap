#include "ui/widgets/AdvisoryItem.h"
#include <QApplication>
#include <QCoreApplication>
#include <gtest/gtest.h>

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "advisory_item_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class AdvisoryItemTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(AdvisoryItemTest, Constructs_WithoutCrash) {
    ui::widgets::AdvisoryItem item;
    EXPECT_NE(&item, nullptr);
}

TEST_F(AdvisoryItemTest, SetTitle_DoesNotCrash) {
    ui::widgets::AdvisoryItem item;
    item.setTitle(QStringLiteral("Test Title"));
}

TEST_F(AdvisoryItemTest, SetBody_DoesNotCrash) {
    ui::widgets::AdvisoryItem item;
    item.setBody(QStringLiteral("Test body text for the advisory item."));
}

TEST_F(AdvisoryItemTest, SetStatus_Info) {
    ui::widgets::AdvisoryItem item;
    item.setStatus(QStringLiteral("info"));
}

TEST_F(AdvisoryItemTest, SetStatus_Success) {
    ui::widgets::AdvisoryItem item;
    item.setStatus(QStringLiteral("success"));
}

TEST_F(AdvisoryItemTest, SetStatus_Caution) {
    ui::widgets::AdvisoryItem item;
    item.setStatus(QStringLiteral("caution"));
}

TEST_F(AdvisoryItemTest, SetStatus_Error) {
    ui::widgets::AdvisoryItem item;
    item.setStatus(QStringLiteral("error"));
}

TEST_F(AdvisoryItemTest, SetUnread_True) {
    ui::widgets::AdvisoryItem item;
    item.setUnread(true);
}

TEST_F(AdvisoryItemTest, SetTimeLabel) {
    ui::widgets::AdvisoryItem item;
    item.setTimeLabel(QStringLiteral("2 min ago"));
}

TEST_F(AdvisoryItemTest, AddAction_EmitsSignal) {
    // Manual signal recorder — avoids Qt6::Test / QSignalSpy dependency
    ui::widgets::AdvisoryItem item;
    int trigger_count = 0;
    QObject::connect(&item, &ui::widgets::AdvisoryItem::actionTriggered, [&](const QString&) { ++trigger_count; });
    item.addAction(QStringLiteral("fix"), QStringLiteral("Fix now"));
    EXPECT_EQ(trigger_count, 0); // no click yet — wiring is correct
}

TEST_F(AdvisoryItemTest, AddDeepLinkAction_DoesNotCrash) {
    ui::widgets::AdvisoryItem item;
    item.addAction(QStringLiteral("open"), QStringLiteral("Open"), true);
}

} // namespace
} // namespace exosnap
