#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QWidget>

#include "ui/widgets/HotkeysSettingsPanel.h"
#include "ui/widgets/KeycapChip.h"

namespace exosnap {
namespace {

using ui::widgets::HotkeysSettingsPanel;

// updateSlot() rebuilds a row's state-slot by deleteLater()-ing the old chip and
// adding a new one. Without a running event loop the old chip lingers in the
// widget tree, so flush pending DeferredDelete events before inspecting a slot.
void FlushDeferredDeletes() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "hotkeys_settings_panel_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

// PS-PHASE-C: coverage migrated here from the removed standalone HotkeysPage.
// The five active (rebindable) hotkeys now live as an embedded card inside
// Settings; this is the shipped surface, so the behavior is verified on the
// panel widget directly (no live GlobalHotkeyService — applyVisualState and the
// no-service commit path avoid Win32 registration).
class HotkeysSettingsPanelTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(HotkeysSettingsPanelTest, ActiveRowsExposeRebindControls) {
    HotkeysSettingsPanel panel;
    for (int i = 0; i < 5; ++i) {
        EXPECT_NE(panel.findChild<QPushButton*>(QStringLiteral("settingsHkSetBtn_%1").arg(i)), nullptr)
            << "Set button missing for active row " << i;
        EXPECT_NE(panel.findChild<QWidget*>(QStringLiteral("settingsHkBinding_%1").arg(i)), nullptr)
            << "Binding slot missing for active row " << i;
    }
    // Exactly five active rows — no sixth.
    EXPECT_EQ(panel.findChild<QPushButton*>(QStringLiteral("settingsHkSetBtn_5")), nullptr);
}

TEST_F(HotkeysSettingsPanelTest, DefaultBindingsRenderAsKeycapChip) {
    HotkeysSettingsPanel panel;
    auto* slot = panel.findChild<QWidget*>(QStringLiteral("settingsHkBinding_0"));
    ASSERT_NE(slot, nullptr);
    const auto chips = slot->findChildren<ui::widgets::KeycapChip*>();
    ASSERT_EQ(chips.size(), 1); // one chip holds the whole chord (e.g. "Alt + F9")
    EXPECT_FALSE(chips.at(0)->text().isEmpty());
}

TEST_F(HotkeysSettingsPanelTest, CustomBindingUpdatesChordChip) {
    HotkeysSettingsPanel panel;
    panel.applyVisualState(QStringLiteral("Ctrl+Shift+R"), QStringLiteral("Alt+F10"), -1, -1, QString(), false);
    FlushDeferredDeletes();

    auto* slot = panel.findChild<QWidget*>(QStringLiteral("settingsHkBinding_0"));
    ASSERT_NE(slot, nullptr);
    const auto chips = slot->findChildren<ui::widgets::KeycapChip*>();
    ASSERT_EQ(chips.size(), 1);
    // ChordText joins tokens with " + " — assert the modifiers/key survived.
    const QString chord = chips.at(0)->text();
    EXPECT_TRUE(chord.contains(QStringLiteral("Ctrl")));
    EXPECT_TRUE(chord.contains(QStringLiteral("Shift")));
    EXPECT_TRUE(chord.contains(QStringLiteral("R")));
}

TEST_F(HotkeysSettingsPanelTest, ClearingABindingShowsNotSetChip) {
    HotkeysSettingsPanel panel;
    // Clicking the quiet × commits an empty binding via the no-service path.
    auto* unset = panel.findChild<QPushButton*>(QStringLiteral("settingsHkUnsetBtn_0"));
    ASSERT_NE(unset, nullptr);
    unset->click();
    FlushDeferredDeletes();

    auto* chip = panel.findChild<QLabel*>(QStringLiteral("settingsHkSlotChip_0"));
    ASSERT_NE(chip, nullptr);
    EXPECT_EQ(chip->property("hotkeySlot").toString(), QStringLiteral("unset"));
    EXPECT_EQ(chip->text(), QStringLiteral("Not set"));
}

TEST_F(HotkeysSettingsPanelTest, CaptureModeShowsPressKeysChipAndCancel) {
    HotkeysSettingsPanel panel;
    panel.applyVisualState(QString(), QString(), /*capture_row=*/1, -1, QString(), false);
    FlushDeferredDeletes();

    auto* chip = panel.findChild<QLabel*>(QStringLiteral("settingsHkSlotChip_1"));
    ASSERT_NE(chip, nullptr);
    EXPECT_EQ(chip->property("hotkeySlot").toString(), QStringLiteral("capturing"));

    // While capturing, the full-width Cancel replaces Set/×.
    auto* cancel = panel.findChild<QPushButton*>(QStringLiteral("settingsHkCancelBtn_1"));
    ASSERT_NE(cancel, nullptr);
    EXPECT_TRUE(cancel->isVisibleTo(&panel));
}

TEST_F(HotkeysSettingsPanelTest, ConflictShowsAmberChipWithMessageTooltip) {
    HotkeysSettingsPanel panel;
    const QString message = QStringLiteral("Alt+F9 is already assigned to Start / Stop recording.");
    panel.applyVisualState(QString(), QString(), -1, /*conflict_row=*/1, message, false);
    FlushDeferredDeletes();

    auto* chip = panel.findChild<QLabel*>(QStringLiteral("settingsHkSlotChip_1"));
    ASSERT_NE(chip, nullptr);
    EXPECT_EQ(chip->property("hotkeySlot").toString(), QStringLiteral("conflict"));
    EXPECT_EQ(chip->toolTip(), message);
}

TEST_F(HotkeysSettingsPanelTest, EditingLockedDisablesControls) {
    HotkeysSettingsPanel panel;
    panel.setEditingLocked(true);

    auto* set_btn = panel.findChild<QPushButton*>(QStringLiteral("settingsHkSetBtn_0"));
    auto* reset_all = panel.resetAllButton();
    ASSERT_NE(set_btn, nullptr);
    ASSERT_NE(reset_all, nullptr);
    EXPECT_FALSE(set_btn->isEnabled());
    EXPECT_FALSE(reset_all->isEnabled());
}

TEST_F(HotkeysSettingsPanelTest, EditingUnlockedReenablesControls) {
    HotkeysSettingsPanel panel;
    panel.setEditingLocked(true);
    panel.setEditingLocked(false);

    auto* set_btn = panel.findChild<QPushButton*>(QStringLiteral("settingsHkSetBtn_0"));
    ASSERT_NE(set_btn, nullptr);
    EXPECT_TRUE(set_btn->isEnabled());
    EXPECT_TRUE(panel.resetAllButton()->isEnabled());
}

TEST_F(HotkeysSettingsPanelTest, NoConflictChipByDefault) {
    HotkeysSettingsPanel panel;
    for (int i = 0; i < 5; ++i) {
        auto* chip = panel.findChild<QLabel*>(QStringLiteral("settingsHkSlotChip_%1").arg(i));
        if (chip)
            EXPECT_NE(chip->property("hotkeySlot").toString(), QStringLiteral("conflict"));
    }
}

TEST_F(HotkeysSettingsPanelTest, ResetAllButtonPresentAndEnabled) {
    HotkeysSettingsPanel panel;
    auto* reset_all = panel.resetAllButton();
    ASSERT_NE(reset_all, nullptr);
    EXPECT_EQ(reset_all->objectName(), QStringLiteral("settingsHkResetAllBtn"));
    EXPECT_TRUE(reset_all->isEnabled());
}

} // namespace
} // namespace exosnap
