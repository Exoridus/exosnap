#include "ui/widgets/PlaceholderRow.h"
#include <QApplication>
#include <QCoreApplication>
#include <gtest/gtest.h>

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "placeholder_row_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class PlaceholderRowTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(PlaceholderRowTest, Constructs_WithoutCrash) {
    ui::widgets::PlaceholderRow row;
    EXPECT_NE(&row, nullptr);
}

TEST_F(PlaceholderRowTest, SetLabel_DoesNotCrash) {
    ui::widgets::PlaceholderRow row;
    row.setLabel(QStringLiteral("Noise Suppression"));
}

TEST_F(PlaceholderRowTest, SetVersionTag_DoesNotCrash) {
    ui::widgets::PlaceholderRow row;
    row.setVersionTag(QStringLiteral("0.7"));
}

TEST_F(PlaceholderRowTest, SetBoth_DoesNotCrash) {
    ui::widgets::PlaceholderRow row;
    row.setLabel(QStringLiteral("Scene Transitions"));
    row.setVersionTag(QStringLiteral("0.8"));
}

TEST_F(PlaceholderRowTest, WidgetRole_Property) {
    ui::widgets::PlaceholderRow row;
    EXPECT_EQ(row.property("widgetRole").toString(), QStringLiteral("placeholderRow"));
}

TEST_F(PlaceholderRowTest, WidgetIsEnabled_ByDefault) {
    // PlaceholderRow uses styling only (not setEnabled(false)) so it stays enabled
    ui::widgets::PlaceholderRow row;
    EXPECT_TRUE(row.isEnabled());
}

TEST_F(PlaceholderRowTest, UpdateLabel_MultipleCallsOk) {
    ui::widgets::PlaceholderRow row;
    row.setLabel(QStringLiteral("First"));
    row.setLabel(QStringLiteral("Second"));
    // No crash = pass
}

TEST_F(PlaceholderRowTest, UpdateVersionTag_MultipleCallsOk) {
    ui::widgets::PlaceholderRow row;
    row.setVersionTag(QStringLiteral("0.6"));
    row.setVersionTag(QStringLiteral("0.9"));
}

} // namespace
} // namespace exosnap
