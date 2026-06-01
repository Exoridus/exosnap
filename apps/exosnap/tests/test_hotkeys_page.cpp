#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>

#include "pages/HotkeysPage.h"

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "hotkeys_page_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class HotkeysPageTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(HotkeysPageTest, ActiveControlsRemainAvailable) {
    HotkeysPage page;
    page.setBindings({QKeySequence(QStringLiteral("Alt+F9")), QKeySequence(QStringLiteral("Ctrl+Shift+F11")),
                      QKeySequence(), QKeySequence()});

    auto* set_start = page.findChild<QPushButton*>(QStringLiteral("hotkeySetBtn_0"));
    auto* unset_start = page.findChild<QPushButton*>(QStringLiteral("hotkeyUnsetBtn_0"));
    auto* set_pause = page.findChild<QPushButton*>(QStringLiteral("hotkeySetBtn_1"));
    auto* unset_pause = page.findChild<QPushButton*>(QStringLiteral("hotkeyUnsetBtn_1"));
    ASSERT_NE(set_start, nullptr);
    ASSERT_NE(unset_start, nullptr);
    ASSERT_NE(set_pause, nullptr);
    ASSERT_NE(unset_pause, nullptr);
    EXPECT_TRUE(set_start->isEnabled());
    EXPECT_TRUE(unset_start->isEnabled());
    EXPECT_TRUE(set_pause->isEnabled());
    EXPECT_TRUE(unset_pause->isEnabled());

    auto* status_start = page.findChild<QLabel*>(QStringLiteral("hotkeyStatus_0"));
    auto* status_pause = page.findChild<QLabel*>(QStringLiteral("hotkeyStatus_1"));
    ASSERT_NE(status_start, nullptr);
    ASSERT_NE(status_pause, nullptr);
    EXPECT_EQ(status_start->text(), QStringLiteral("Active"));
    EXPECT_EQ(status_pause->text(), QStringLiteral("Active"));
}

TEST_F(HotkeysPageTest, PlannedActionsAreUnavailableAndNotRebindable) {
    HotkeysPage page;
    page.setBindings({QKeySequence(), QKeySequence(), QKeySequence(QStringLiteral("Alt+F8")),
                      QKeySequence(QStringLiteral("Ctrl+Alt+M"))});

    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("hotkeySetBtn_2")), nullptr);
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("hotkeyUnsetBtn_2")), nullptr);
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("hotkeySetBtn_3")), nullptr);
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("hotkeyUnsetBtn_3")), nullptr);

    auto* status_split = page.findChild<QLabel*>(QStringLiteral("hotkeyStatus_2"));
    auto* status_mute = page.findChild<QLabel*>(QStringLiteral("hotkeyStatus_3"));
    ASSERT_NE(status_split, nullptr);
    ASSERT_NE(status_mute, nullptr);
    EXPECT_EQ(status_split->text(), QStringLiteral("Unavailable"));
    EXPECT_EQ(status_mute->text(), QStringLiteral("Unavailable"));

    auto* split_binding = page.findChild<QLabel*>(QStringLiteral("hotkeyBinding_2"));
    auto* mute_binding = page.findChild<QLabel*>(QStringLiteral("hotkeyBinding_3"));
    ASSERT_NE(split_binding, nullptr);
    ASSERT_NE(mute_binding, nullptr);
    EXPECT_TRUE(split_binding->text().contains(QStringLiteral("inactive")));
    EXPECT_TRUE(mute_binding->text().contains(QStringLiteral("inactive")));
}

} // namespace
} // namespace exosnap
