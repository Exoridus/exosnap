#include "ui/widgets/AdvisoryItem.h"
#include <QApplication>
#include <QCoreApplication>
#include <QPushButton>
#include <algorithm>
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

// Regression test: addAction() used to insert each button just before the
// actions row's stretch, which left the stretch trailing and left-aligned the
// buttons instead of hugging the row's right edge (the app-wide convention for
// trailing actions). The stretch is seeded FIRST (leading), so buttons appended
// after it must land on the right.
TEST_F(AdvisoryItemTest, AddAction_ButtonIsRightAligned) {
    ui::widgets::AdvisoryItem item;
    item.setTitle(QStringLiteral("Title"));
    item.addAction(QStringLiteral("fix"), QStringLiteral("Fix now"));
    item.resize(400, item.sizeHint().height());
    item.show();
    QCoreApplication::processEvents();

    auto* btn = item.findChild<QPushButton*>();
    ASSERT_NE(btn, nullptr);
    auto* row = qobject_cast<QWidget*>(btn->parent());
    ASSERT_NE(row, nullptr);

    // The button's right edge must sit at (or very near) the actions row's
    // right edge -- a left-aligned button would instead leave most of the row
    // empty to its right.
    const int gap_to_right_edge = row->width() - (btn->x() + btn->width());
    EXPECT_LE(gap_to_right_edge, 2) << "Action button must hug the row's right edge (button right="
                                    << (btn->x() + btn->width()) << ", row width=" << row->width() << ")";
    item.hide();
}

// With two actions, both must stay flush against the right edge as a group --
// the rightmost button's right edge trailing the row, not floating mid-row.
TEST_F(AdvisoryItemTest, AddAction_TwoButtons_BothRightAligned) {
    ui::widgets::AdvisoryItem item;
    item.setTitle(QStringLiteral("Title"));
    item.addAction(QStringLiteral("dismiss"), QStringLiteral("Dismiss"));
    item.addAction(QStringLiteral("fix"), QStringLiteral("Fix now"));
    item.resize(400, item.sizeHint().height());
    item.show();
    QCoreApplication::processEvents();

    const auto buttons = item.findChildren<QPushButton*>();
    ASSERT_EQ(buttons.size(), 2);
    auto* row = qobject_cast<QWidget*>(buttons.first()->parent());
    ASSERT_NE(row, nullptr);

    int rightmost_edge = 0;
    for (auto* btn : buttons)
        rightmost_edge = std::max(rightmost_edge, btn->x() + btn->width());

    const int gap_to_right_edge = row->width() - rightmost_edge;
    EXPECT_LE(gap_to_right_edge, 2) << "The rightmost action button must hug the row's right edge";
    item.hide();
}

} // namespace
} // namespace exosnap
