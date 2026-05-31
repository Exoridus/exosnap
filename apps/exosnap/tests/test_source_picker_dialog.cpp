#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <QPushButton>

#include "ui/dialogs/SourcePickerDialog.h"
#include "ui/widgets/CaptureTargetCard.h"

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        return existing;
    }

    static int argc = 1;
    static char app_name[] = "source_picker_dialog_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class SourcePickerDialogTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(SourcePickerDialogTest, Constructs_WithExpectedSectionsAndActions) {
    ui::dialogs::SourcePickerDialog dialog;

    EXPECT_NE(dialog.findChild<QPushButton*>(QStringLiteral("sourcePickerScreensButton")), nullptr);
    EXPECT_NE(dialog.findChild<QPushButton*>(QStringLiteral("sourcePickerWindowsButton")), nullptr);
    EXPECT_NE(dialog.findChild<QPushButton*>(QStringLiteral("sourcePickerRegionButton")), nullptr);
    EXPECT_NE(dialog.findChild<QPushButton*>(QStringLiteral("sourcePickerCancelButton")), nullptr);
    EXPECT_NE(dialog.findChild<QPushButton*>(QStringLiteral("sourcePickerUseButton")), nullptr);
}

TEST_F(SourcePickerDialogTest, SelectSource_UpdatesSelectionResult) {
    ui::dialogs::SourcePickerDialog dialog;

    ui::dialogs::SourcePickerDialog::SourceOption screen;
    screen.target_index = 1;
    screen.title = QStringLiteral("Display 1");
    screen.detail = QStringLiteral("Desktop - Display 1");
    screen.primary = true;
    std::vector<ui::dialogs::SourcePickerDialog::SourceOption> screens = {screen};

    ui::dialogs::SourcePickerDialog::SourceOption window;
    window.target_index = 9;
    window.title = QStringLiteral("Editor - README");
    window.detail = QStringLiteral("Window capture");
    window.primary = false;
    std::vector<ui::dialogs::SourcePickerDialog::SourceOption> windows = {window};

    dialog.setScreenOptions(screens);
    dialog.setWindowOptions(windows);
    dialog.setCurrentSelection(ui::dialogs::SourcePickerDialog::Section::Screens, 1);

    auto selection = dialog.selectionResult();
    EXPECT_TRUE(selection.valid);
    EXPECT_EQ(selection.section, ui::dialogs::SourcePickerDialog::Section::Screens);
    EXPECT_EQ(selection.target_index, 1);

    EXPECT_TRUE(dialog.selectSource(ui::dialogs::SourcePickerDialog::Section::Windows, 9));
    selection = dialog.selectionResult();
    EXPECT_TRUE(selection.valid);
    EXPECT_EQ(selection.section, ui::dialogs::SourcePickerDialog::Section::Windows);
    EXPECT_EQ(selection.target_index, 9);
}

TEST_F(SourcePickerDialogTest, PickRegionNow_AcceptsDialogWithRegionSelection) {
    ui::dialogs::SourcePickerDialog dialog;
    dialog.setRegionState(QStringLiteral("320, 180  —  1280 × 720"), true, true);
    dialog.setCurrentSelection(ui::dialogs::SourcePickerDialog::Section::Region, -1);

    auto* pick_now = dialog.findChild<QPushButton*>(QStringLiteral("sourcePickerPickRegionButton"));
    ASSERT_NE(pick_now, nullptr);
    pick_now->click();

    const auto selection = dialog.selectionResult();
    EXPECT_EQ(dialog.result(), QDialog::Accepted);
    EXPECT_TRUE(selection.valid);
    EXPECT_EQ(selection.section, ui::dialogs::SourcePickerDialog::Section::Region);
    EXPECT_TRUE(selection.pick_region_now);
}

TEST_F(SourcePickerDialogTest, TinyWindowSelection_IsMarkedInvalidAndCannotBeApplied) {
    ui::dialogs::SourcePickerDialog dialog;

    ui::dialogs::SourcePickerDialog::SourceOption window;
    window.target_index = 17;
    window.title = QStringLiteral("Brave — Tiny popup");
    window.detail = QStringLiteral("146 × 20 · brave.exe");
    window.status_badge = QStringLiteral("Too small");
    window.selectable = false;
    window.validation_summary = QStringLiteral(
        "Selected window is too small for the active encoder. Choose a larger window or use Display capture.");
    window.minimum_detail = QStringLiteral("Minimum 192×128");

    dialog.setWindowOptions({window});
    dialog.setCurrentSelection(ui::dialogs::SourcePickerDialog::Section::Windows, 17);

    const auto selection = dialog.selectionResult();
    EXPECT_FALSE(selection.valid);
    EXPECT_FALSE(dialog.selectSource(ui::dialogs::SourcePickerDialog::Section::Windows, 17));

    auto* use_button = dialog.findChild<QPushButton*>(QStringLiteral("sourcePickerUseButton"));
    ASSERT_NE(use_button, nullptr);
    EXPECT_FALSE(use_button->isEnabled());

    auto* summary = dialog.findChild<QLabel*>(QStringLiteral("sourcePickerSummary"));
    ASSERT_NE(summary, nullptr);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("too small"), Qt::CaseInsensitive));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("Minimum 192×128")));

    const auto cards = dialog.findChildren<ui::widgets::CaptureTargetCard*>();
    ASSERT_FALSE(cards.empty());
    EXPECT_EQ(cards.front()->statusText(), QStringLiteral("Too small"));
    EXPECT_EQ(cards.front()->property("captureCardTone").toString(), QStringLiteral("warning"));
}

} // namespace
} // namespace exosnap
