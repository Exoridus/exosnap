#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QWidget>

#include "pages/HotkeysPage.h"
#include "ui/widgets/KeycapChip.h"

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
    page.setBindings({QKeySequence(QStringLiteral("Alt+F9")), QKeySequence(QStringLiteral("Ctrl+Shift+F11"))});

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
}

TEST_F(HotkeysPageTest, ActiveRowsRenderRealBindingsAsKeycaps) {
    HotkeysPage page;
    page.setBindings({QKeySequence(QStringLiteral("Alt+F9")), QKeySequence(QStringLiteral("Alt+F10"))});

    auto* start_chips = page.findChild<QWidget*>(QStringLiteral("hotkeyBinding_0"));
    ASSERT_NE(start_chips, nullptr);
    const auto keycaps = start_chips->findChildren<ui::widgets::KeycapChip*>();
    ASSERT_EQ(keycaps.size(), 2);
    EXPECT_FALSE(keycaps.at(0)->isMuted());
    EXPECT_FALSE(keycaps.at(1)->isMuted());
    QStringList labels;
    for (auto* chip : keycaps)
        labels << chip->text();
    EXPECT_TRUE(labels.contains(QStringLiteral("Alt")));
    EXPECT_TRUE(labels.contains(QStringLiteral("F9")));
}

TEST_F(HotkeysPageTest, UnsetActiveRowShowsMutedKeycap) {
    HotkeysPage page;
    page.setBindings({QKeySequence(QStringLiteral("Alt+F9")), QKeySequence()});

    auto* pause_chips = page.findChild<QWidget*>(QStringLiteral("hotkeyBinding_1"));
    ASSERT_NE(pause_chips, nullptr);
    const auto keycaps = pause_chips->findChildren<ui::widgets::KeycapChip*>();
    ASSERT_EQ(keycaps.size(), 1);
    EXPECT_TRUE(keycaps.at(0)->isMuted());
}

TEST_F(HotkeysPageTest, PlannedActionsAreUnavailableAndNotRebindable) {
    HotkeysPage page;

    // No rebind / unset controls for planned actions (indices 2-9).
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("hotkeySetBtn_2")), nullptr);
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("hotkeyUnsetBtn_2")), nullptr);
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("hotkeySetBtn_3")), nullptr);
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("hotkeyUnsetBtn_3")), nullptr);

    // No keycap chips fabricated for planned actions, only an honest "Not in this build" tag.
    EXPECT_EQ(page.findChild<QWidget*>(QStringLiteral("hotkeyBinding_2")), nullptr);
    EXPECT_EQ(page.findChild<QWidget*>(QStringLiteral("hotkeyBinding_3")), nullptr);

    auto* tag_split = page.findChild<QLabel*>(QStringLiteral("hotkeyPlannedTag_2"));
    auto* tag_mute = page.findChild<QLabel*>(QStringLiteral("hotkeyPlannedTag_3"));
    ASSERT_NE(tag_split, nullptr);
    ASSERT_NE(tag_mute, nullptr);
    EXPECT_EQ(tag_split->text(), QStringLiteral("Not in this build"));
    EXPECT_EQ(tag_mute->text(), QStringLiteral("Not in this build"));
}

TEST_F(HotkeysPageTest, DesignTargetPlannedActions_ArePresent) {
    HotkeysPage page;
    // The planned section must include all six design-target actions (indices 4–9).
    const QStringList expected = {
        QStringLiteral("hotkeyPlannedTag_4"), QStringLiteral("hotkeyPlannedTag_5"),
        QStringLiteral("hotkeyPlannedTag_6"), QStringLiteral("hotkeyPlannedTag_7"),
        QStringLiteral("hotkeyPlannedTag_8"), QStringLiteral("hotkeyPlannedTag_9"),
    };
    for (const auto& name : expected) {
        auto* tag = page.findChild<QLabel*>(name);
        ASSERT_NE(tag, nullptr) << name.toStdString() << " not found";
        EXPECT_EQ(tag->text(), QStringLiteral("Not in this build"));
    }
}

TEST_F(HotkeysPageTest, DesignTargetPlannedActions_ActionLabelsPresent) {
    HotkeysPage page;
    const QStringList expected_labels = {
        QStringLiteral("Change source"),    QStringLiteral("Toggle microphone"),
        QStringLiteral("Toggle webcam"),    QStringLiteral("Toggle system audio"),
        QStringLiteral("Open diagnostics"), QStringLiteral("Capture frame (screenshot)"),
    };
    for (const auto& label : expected_labels) {
        bool found = false;
        for (auto* lbl : page.findChildren<QLabel*>()) {
            if (lbl->text() == label) {
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found) << label.toStdString() << " label not found on page";
    }
}

TEST_F(HotkeysPageTest, DesignTargetPlannedActions_HaveNoRebindControls) {
    HotkeysPage page;
    // Indices 4–9 are design-target planned rows and must not expose Set/Unset buttons.
    for (int i = 4; i <= 9; ++i) {
        EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("hotkeySetBtn_%1").arg(i)), nullptr)
            << "Set button found for planned index " << i;
        EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("hotkeyUnsetBtn_%1").arg(i)), nullptr)
            << "Unset button found for planned index " << i;
        EXPECT_EQ(page.findChild<QWidget*>(QStringLiteral("hotkeyBinding_%1").arg(i)), nullptr)
            << "Binding chip widget found for planned index " << i;
    }
}

TEST_F(HotkeysPageTest, ActiveRowsUnaffectedByPlannedExpansion) {
    HotkeysPage page;
    page.setBindings({QKeySequence(QStringLiteral("Alt+F9")), QKeySequence()});
    // Active row 0 (Start/Stop) still has Set/Unset.
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("hotkeySetBtn_0")), nullptr);
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("hotkeyUnsetBtn_0")), nullptr);
    // Active row 1 (Pause/Resume) still has Set/Unset.
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("hotkeySetBtn_1")), nullptr);
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("hotkeyUnsetBtn_1")), nullptr);
}

TEST_F(HotkeysPageTest, ResetAllToDefaultsRestoresActiveBindings) {
    HotkeysPage page;
    auto* reset = page.findChild<QPushButton*>(QStringLiteral("hotkeyResetBtn"));
    ASSERT_NE(reset, nullptr);

    // Move both active rows away from defaults.
    page.setBindings({QKeySequence(QStringLiteral("Ctrl+F1")), QKeySequence(QStringLiteral("Ctrl+F2"))});

    int emit_count = 0;
    QObject::connect(&page, &HotkeysPage::bindingChanged, &page, [&emit_count](int, QKeySequence) { ++emit_count; });
    reset->click();

    // Both active rows differed from defaults, so both emit on reset.
    ASSERT_EQ(emit_count, 2);
    auto* start_chips = page.findChild<QWidget*>(QStringLiteral("hotkeyBinding_0"));
    ASSERT_NE(start_chips, nullptr);
    QStringList labels;
    for (auto* chip : start_chips->findChildren<ui::widgets::KeycapChip*>())
        labels << chip->text();
    EXPECT_TRUE(labels.contains(QStringLiteral("F9")));
}

TEST_F(HotkeysPageTest, PerRowResetButtonPresent) {
    HotkeysPage page;
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("hotkeyResetRowBtn_0")), nullptr);
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("hotkeyResetRowBtn_1")), nullptr);
    // Planned rows have no reset button.
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("hotkeyResetRowBtn_2")), nullptr);
}

TEST_F(HotkeysPageTest, EditingLockedDisablesActiveControls) {
    HotkeysPage page;
    page.setBindings({QKeySequence(QStringLiteral("Alt+F9")), QKeySequence()});
    page.setEditingLocked(true);

    auto* set_btn = page.findChild<QPushButton*>(QStringLiteral("hotkeySetBtn_0"));
    auto* unset_btn = page.findChild<QPushButton*>(QStringLiteral("hotkeyUnsetBtn_0"));
    auto* reset_all = page.findChild<QPushButton*>(QStringLiteral("hotkeyResetBtn"));
    ASSERT_NE(set_btn, nullptr);
    ASSERT_NE(unset_btn, nullptr);
    ASSERT_NE(reset_all, nullptr);
    EXPECT_FALSE(set_btn->isEnabled());
    EXPECT_FALSE(unset_btn->isEnabled());
    EXPECT_FALSE(reset_all->isEnabled());
}

TEST_F(HotkeysPageTest, EditingUnlockedReenablesActiveControls) {
    HotkeysPage page;
    page.setBindings({QKeySequence(QStringLiteral("Alt+F9")), QKeySequence()});
    page.setEditingLocked(true);
    page.setEditingLocked(false);

    auto* set_btn = page.findChild<QPushButton*>(QStringLiteral("hotkeySetBtn_0"));
    ASSERT_NE(set_btn, nullptr);
    EXPECT_TRUE(set_btn->isEnabled());
}

TEST_F(HotkeysPageTest, ErrorLabelHiddenByDefault) {
    HotkeysPage page;
    auto* error = page.findChild<QLabel*>(QStringLiteral("hotkeyError_0"));
    ASSERT_NE(error, nullptr);
    EXPECT_FALSE(error->isVisible());
}

} // namespace
} // namespace exosnap
