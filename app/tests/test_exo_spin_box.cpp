#include "ui/widgets/ExoSpinBox.h"
#include <QApplication>
#include <QCoreApplication>
#include <QSpinBox>
#include <gtest/gtest.h>

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "exo_spin_box_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class ExoSpinBoxTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(ExoSpinBoxTest, Constructs_DefaultState) {
    ui::widgets::ExoSpinBox sb;
    EXPECT_EQ(sb.value(), 0);
}

TEST_F(ExoSpinBoxTest, SetValue_IsRetrievable) {
    ui::widgets::ExoSpinBox sb;
    sb.setRange(0, 100);
    sb.setValue(42);
    EXPECT_EQ(sb.value(), 42);
}

TEST_F(ExoSpinBoxTest, SetRange_ClampsValue) {
    ui::widgets::ExoSpinBox sb;
    sb.setRange(10, 50);
    sb.setValue(100);
    EXPECT_EQ(sb.value(), 50);
}

TEST_F(ExoSpinBoxTest, SetSuffix_IsRetrievable) {
    ui::widgets::ExoSpinBox sb;
    sb.setSuffix(QStringLiteral(" fps"));
    EXPECT_EQ(sb.suffix(), QStringLiteral(" fps"));
}

TEST_F(ExoSpinBoxTest, WidgetRole_Property) {
    ui::widgets::ExoSpinBox sb;
    EXPECT_EQ(sb.property("widgetRole").toString(), QStringLiteral("exoSpinBox"));
}

TEST_F(ExoSpinBoxTest, IsQSpinBox) {
    ui::widgets::ExoSpinBox sb;
    EXPECT_NE(qobject_cast<QSpinBox*>(&sb), nullptr);
}

TEST_F(ExoSpinBoxTest, ValueChanged_Signal) {
    // Manual signal recorder — avoids Qt6::Test / QSignalSpy dependency
    ui::widgets::ExoSpinBox sb;
    sb.setRange(0, 100);
    int change_count = 0;
    QObject::connect(&sb, &QSpinBox::valueChanged, [&](int) { ++change_count; });
    sb.setValue(7);
    EXPECT_EQ(change_count, 1);
}

TEST_F(ExoSpinBoxTest, MinimumSize) {
    ui::widgets::ExoSpinBox sb;
    EXPECT_GE(sb.minimumWidth(), 100);
    EXPECT_GE(sb.minimumHeight(), 32);
}

} // namespace
} // namespace exosnap
