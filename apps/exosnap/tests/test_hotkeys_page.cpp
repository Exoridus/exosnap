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

    // Active index 4 (SplitRecording) has rebind controls — it is now active.
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("hotkeySetBtn_4")), nullptr);
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("hotkeyUnsetBtn_4")), nullptr);

    // No rebind / unset controls for planned actions (indices 5-10).
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("hotkeySetBtn_5")), nullptr);
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("hotkeyUnsetBtn_5")), nullptr);
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("hotkeySetBtn_6")), nullptr);
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("hotkeyUnsetBtn_6")), nullptr);

    // No keycap chips fabricated for planned actions, only an honest "Not in this build" tag.
    EXPECT_EQ(page.findChild<QWidget*>(QStringLiteral("hotkeyBinding_5")), nullptr);
    EXPECT_EQ(page.findChild<QWidget*>(QStringLiteral("hotkeyBinding_6")), nullptr);

    // Active index 4 now has a binding chip and no planned tag.
    EXPECT_NE(page.findChild<QWidget*>(QStringLiteral("hotkeyBinding_4")), nullptr);
    EXPECT_EQ(page.findChild<QLabel*>(QStringLiteral("hotkeyPlannedTag_4")), nullptr);

    auto* tag_mute = page.findChild<QLabel*>(QStringLiteral("hotkeyPlannedTag_5"));
    auto* tag_source = page.findChild<QLabel*>(QStringLiteral("hotkeyPlannedTag_6"));
    ASSERT_NE(tag_mute, nullptr);
    ASSERT_NE(tag_source, nullptr);
    EXPECT_EQ(tag_mute->text(), QStringLiteral("Not in this build"));
    EXPECT_EQ(tag_source->text(), QStringLiteral("Not in this build"));
}

TEST_F(HotkeysPageTest, DesignTargetPlannedActions_ArePresent) {
    HotkeysPage page;
    // The planned section must include all six design-target actions (indices 5–10,
    // shifted by one now that Split recording moved to the Active section).
    const QStringList expected = {
        QStringLiteral("hotkeyPlannedTag_5"), QStringLiteral("hotkeyPlannedTag_6"),
        QStringLiteral("hotkeyPlannedTag_7"), QStringLiteral("hotkeyPlannedTag_8"),
        QStringLiteral("hotkeyPlannedTag_9"), QStringLiteral("hotkeyPlannedTag_10"),
    };
    for (const auto& name : expected) {
        auto* tag = page.findChild<QLabel*>(name);
        ASSERT_NE(tag, nullptr) << name.toStdString() << " not found";
        EXPECT_EQ(tag->text(), QStringLiteral("Not in this build"));
    }
}

TEST_F(HotkeysPageTest, DesignTargetPlannedActions_ActionLabelsPresent) {
    HotkeysPage page;
    // "Capture frame" is now an active action (index 2) — its label changed from
    // "Capture frame (screenshot)" to "Capture frame" and it moved to Active section.
    const QStringList expected_labels = {
        QStringLiteral("Change source"),       QStringLiteral("Toggle microphone"), QStringLiteral("Toggle webcam"),
        QStringLiteral("Toggle system audio"), QStringLiteral("Open diagnostics"),
        QStringLiteral("Capture frame"), // active; was "Capture frame (screenshot)" in planned
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
    // Indices 5–10 are design-target planned rows and must not expose Set/Unset buttons.
    for (int i = 5; i <= 10; ++i) {
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
    // Index 3 is AddMarker and index 4 is Split recording — both active, both have reset buttons.
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("hotkeyResetRowBtn_3")), nullptr);
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("hotkeyResetRowBtn_4")), nullptr);
    // Planned rows have no reset button (index 5+).
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("hotkeyResetRowBtn_5")), nullptr);
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
