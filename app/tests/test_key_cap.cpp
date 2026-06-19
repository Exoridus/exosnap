#include "ui/widgets/KeyCap.h"
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
    static char app_name[] = "key_cap_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class KeyCapTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(KeyCapTest, Constructs_WithKey) {
    ui::widgets::KeyCap cap(QStringLiteral("Ctrl"));
    EXPECT_EQ(cap.key(), QStringLiteral("Ctrl"));
    EXPECT_EQ(cap.text(), QStringLiteral("Ctrl"));
}

TEST_F(KeyCapTest, Constructs_Default_EmptyKey) {
    ui::widgets::KeyCap cap;
    EXPECT_TRUE(cap.key().isEmpty());
}

TEST_F(KeyCapTest, SetKey_UpdatesTextAndKey) {
    ui::widgets::KeyCap cap;
    cap.setKey(QStringLiteral("Alt"));
    EXPECT_EQ(cap.key(), QStringLiteral("Alt"));
    EXPECT_EQ(cap.text(), QStringLiteral("Alt"));
}

TEST_F(KeyCapTest, WidgetRole_Property) {
    ui::widgets::KeyCap cap(QStringLiteral("F9"));
    EXPECT_EQ(cap.property("widgetRole").toString(), QStringLiteral("keyCap"));
}

TEST_F(KeyCapTest, IsQLabel) {
    ui::widgets::KeyCap cap(QStringLiteral("Shift"));
    EXPECT_NE(qobject_cast<QLabel*>(&cap), nullptr);
}

TEST_F(KeyCapTest, AlignmentIsCenter) {
    ui::widgets::KeyCap cap(QStringLiteral("Esc"));
    EXPECT_TRUE(cap.alignment().testFlag(Qt::AlignHCenter));
}

TEST_F(KeyCapTest, SetKey_OverwritesPrevious) {
    ui::widgets::KeyCap cap(QStringLiteral("A"));
    cap.setKey(QStringLiteral("B"));
    EXPECT_EQ(cap.key(), QStringLiteral("B"));
}

TEST_F(KeyCapTest, EmptyKey_SetAndGet) {
    ui::widgets::KeyCap cap(QStringLiteral("X"));
    cap.setKey(QStringLiteral(""));
    EXPECT_TRUE(cap.key().isEmpty());
}

} // namespace
} // namespace exosnap
