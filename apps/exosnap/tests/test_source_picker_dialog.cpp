#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>

#include "ui/dialogs/SourcePickerDialog.h"
#include "ui/dialogs/SourcePickerWindowRules.h"
#include "ui/widgets/CaptureTargetCard.h"
#include "ui/widgets/RegionPresetCard.h"

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
    EXPECT_NE(dialog.findChild<QPushButton*>(QStringLiteral("sourcePickerRefreshButton")), nullptr);
    EXPECT_NE(dialog.findChild<QPushButton*>(QStringLiteral("sourcePickerCancelButton")), nullptr);
    EXPECT_NE(dialog.findChild<QPushButton*>(QStringLiteral("sourcePickerUseButton")), nullptr);
}

TEST_F(SourcePickerDialogTest, ScreensTab_UsesHybridDisplaysLabel) {
    ui::dialogs::SourcePickerDialog dialog;

    auto* screens = dialog.findChild<QPushButton*>(QStringLiteral("sourcePickerScreensButton"));
    auto* windows = dialog.findChild<QPushButton*>(QStringLiteral("sourcePickerWindowsButton"));
    auto* region = dialog.findChild<QPushButton*>(QStringLiteral("sourcePickerRegionButton"));
    ASSERT_NE(screens, nullptr);
    ASSERT_NE(windows, nullptr);
    ASSERT_NE(region, nullptr);

    EXPECT_EQ(screens->text(), QStringLiteral("Displays"));
    EXPECT_EQ(windows->text(), QStringLiteral("Windows"));
    EXPECT_EQ(region->text(), QStringLiteral("Region"));
}

TEST_F(SourcePickerDialogTest, DoesNotExposeSearchFieldAtCurrentListSize) {
    ui::dialogs::SourcePickerDialog dialog;
    EXPECT_TRUE(dialog.findChildren<QLineEdit*>().isEmpty());
}

TEST_F(SourcePickerDialogTest, RegionTab_ExposesPresetCardsWithDrawCustomOption) {
    ui::dialogs::SourcePickerDialog dialog;

    const auto preset_cards = dialog.findChildren<ui::widgets::RegionPresetCard*>();
    EXPECT_EQ(preset_cards.size(), 6);

    auto* draw_card = dialog.findChild<ui::widgets::RegionPresetCard*>(QStringLiteral("sourcePickerRegionDrawCard"));
    ASSERT_NE(draw_card, nullptr);
    EXPECT_TRUE(draw_card->isDrawVariant());
    EXPECT_FALSE(draw_card->isPlanned());

    int planned_count = 0;
    int draw_count = 0;
    for (auto* card : preset_cards) {
        if (card->isDrawVariant()) {
            ++draw_count;
        }
        if (card->isPlanned()) {
            ++planned_count;
        }
    }
    EXPECT_EQ(draw_count, 1);
    EXPECT_EQ(planned_count, 5);
}

TEST_F(SourcePickerDialogTest, RegionSelection_IsValidAndSelectsDrawCustomCard) {
    ui::dialogs::SourcePickerDialog dialog;
    dialog.setRegionState(QStringLiteral("320, 180  —  1280 × 720"), true, true);

    auto* region = dialog.findChild<QPushButton*>(QStringLiteral("sourcePickerRegionButton"));
    ASSERT_NE(region, nullptr);
    region->click();

    auto* draw_card = dialog.findChild<ui::widgets::RegionPresetCard*>(QStringLiteral("sourcePickerRegionDrawCard"));
    ASSERT_NE(draw_card, nullptr);
    EXPECT_TRUE(draw_card->isSelected());

    auto* use_button = dialog.findChild<QPushButton*>(QStringLiteral("sourcePickerUseButton"));
    ASSERT_NE(use_button, nullptr);
    EXPECT_TRUE(use_button->isEnabled());

    const auto selection = dialog.selectionResult();
    EXPECT_TRUE(selection.valid);
    EXPECT_EQ(selection.section, ui::dialogs::SourcePickerDialog::Section::Region);
}

TEST_F(SourcePickerDialogTest, RefreshButton_DisabledForRegionAndEnabledForVisualSections) {
    ui::dialogs::SourcePickerDialog dialog;

    auto* refresh = dialog.findChild<QPushButton*>(QStringLiteral("sourcePickerRefreshButton"));
    auto* screens = dialog.findChild<QPushButton*>(QStringLiteral("sourcePickerScreensButton"));
    auto* region = dialog.findChild<QPushButton*>(QStringLiteral("sourcePickerRegionButton"));
    ASSERT_NE(refresh, nullptr);
    ASSERT_NE(screens, nullptr);
    ASSERT_NE(region, nullptr);

    region->click();
    EXPECT_FALSE(refresh->isEnabled());

    screens->click();
    EXPECT_TRUE(refresh->isEnabled());
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

TEST_F(SourcePickerDialogTest, NormalWindowCard_DoesNotRequireWindowBadge) {
    ui::dialogs::SourcePickerDialog dialog;

    ui::dialogs::SourcePickerDialog::SourceOption window;
    window.target_index = 23;
    window.title = QStringLiteral("Editor - README");
    window.detail = QStringLiteral("1280 × 720 · code.exe");

    dialog.setWindowOptions({window});
    dialog.setCurrentSelection(ui::dialogs::SourcePickerDialog::Section::Windows, 23);

    const auto cards = dialog.findChildren<ui::widgets::CaptureTargetCard*>();
    ASSERT_EQ(cards.size(), 1);
    EXPECT_TRUE(cards.front()->statusText().isEmpty());
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

TEST_F(SourcePickerDialogTest, UnavailableWindow_IsMarkedInvalidAndCannotBeApplied) {
    ui::dialogs::SourcePickerDialog dialog;

    ui::dialogs::SourcePickerDialog::SourceOption window;
    window.target_index = -5;
    window.title = QStringLiteral("Minimized App");
    window.detail = QStringLiteral("640 × 480");
    window.status_badge = QStringLiteral("Minimized");
    window.selectable = false;
    window.unavailable = true;
    window.hidden_by_default = true;
    window.help_text = QStringLiteral("Restore the window to capture it.");

    ui::dialogs::SourcePickerDialog::SourceOption valid_window;
    valid_window.target_index = 7;
    valid_window.title = QStringLiteral("Editor - README");
    valid_window.detail = QStringLiteral("1920 × 1080");

    dialog.setWindowOptions({valid_window, window});
    dialog.setCurrentSelection(ui::dialogs::SourcePickerDialog::Section::Windows, 7);

    const auto selection = dialog.selectionResult();
    EXPECT_TRUE(selection.valid);
    EXPECT_FALSE(dialog.selectSource(ui::dialogs::SourcePickerDialog::Section::Windows, -5));

    auto* use_button = dialog.findChild<QPushButton*>(QStringLiteral("sourcePickerUseButton"));
    ASSERT_NE(use_button, nullptr);
    EXPECT_TRUE(use_button->isEnabled());

    auto* toggle = dialog.findChild<QPushButton*>(QStringLiteral("sourcePickerShowUnavailableButton"));
    ASSERT_NE(toggle, nullptr);
    EXPECT_FALSE(toggle->isHidden());
    EXPECT_EQ(toggle->text(), QStringLiteral("Show unavailable (1)"));

    const auto cards = dialog.findChildren<ui::widgets::CaptureTargetCard*>();
    ASSERT_EQ(cards.size(), 1);
    EXPECT_FALSE(cards.front()->isUnavailable());

    toggle->click();
    EXPECT_EQ(toggle->text(), QStringLiteral("Hide unavailable (1)"));
    const auto expanded_cards = dialog.findChildren<ui::widgets::CaptureTargetCard*>();
    ASSERT_EQ(expanded_cards.size(), 2);
    EXPECT_TRUE(expanded_cards.back()->isUnavailable());
    EXPECT_EQ(expanded_cards.back()->statusText(), QStringLiteral("Minimized"));
}

TEST_F(SourcePickerDialogTest, CaptureTargetCard_ThumbnailSupport) {
    ui::widgets::CaptureTargetCard card;

    EXPECT_FALSE(card.hasThumbnail());
    card.setThumbnailPlaceholder();
    EXPECT_FALSE(card.hasThumbnail());

    QImage img(40, 30, QImage::Format_ARGB32);
    img.fill(Qt::blue);
    card.setThumbnail(QPixmap::fromImage(img));
    EXPECT_TRUE(card.hasThumbnail());

    card.setThumbnail(QPixmap{});
    EXPECT_FALSE(card.hasThumbnail());
}

TEST_F(SourcePickerDialogTest, CaptureTargetCard_UnavailableState) {
    ui::widgets::CaptureTargetCard card;

    EXPECT_FALSE(card.isUnavailable());
    EXPECT_TRUE(card.isEnabled());

    card.setUnavailable(true);
    EXPECT_TRUE(card.isUnavailable());
    EXPECT_FALSE(card.isEnabled());

    card.setUnavailable(false);
    EXPECT_FALSE(card.isUnavailable());
    EXPECT_TRUE(card.isEnabled());
}

TEST_F(SourcePickerDialogTest, CaptureTargetCard_HelpText) {
    ui::widgets::CaptureTargetCard card;

    card.setHelpText(QStringLiteral("Restore the window to capture it."));
    card.setUnavailable(true);
    EXPECT_TRUE(card.isUnavailable());

    card.setHelpText({});
    card.setUnavailable(false);
    EXPECT_FALSE(card.isUnavailable());
}

TEST_F(SourcePickerDialogTest, CaptureTargetCard_ThumbnailStateTransitions) {
    ui::widgets::CaptureTargetCard card;

    card.setThumbnailLoadingText(QStringLiteral("Loading preview..."));
    EXPECT_FALSE(card.hasThumbnail());

    card.setThumbnailFailureText(QStringLiteral("Preview unavailable"));
    EXPECT_FALSE(card.hasThumbnail());

    card.setThumbnailUnavailableText(QStringLiteral("Minimized"));
    EXPECT_FALSE(card.hasThumbnail());
}

TEST_F(SourcePickerDialogTest, CardsReceiveThumbnailPlaceholderByDefault) {
    ui::dialogs::SourcePickerDialog dialog;

    ui::dialogs::SourcePickerDialog::SourceOption screen;
    screen.target_index = 0;
    screen.title = QStringLiteral("Display 1");
    dialog.setScreenOptions({screen});

    const auto cards = dialog.findChildren<ui::widgets::CaptureTargetCard*>();
    ASSERT_EQ(cards.size(), 1);
    EXPECT_FALSE(cards.front()->hasThumbnail());
}

TEST(SourcePickerWindowRulesTest, FiltersKnownSystemHelperWindows) {
    const ui::dialogs::SourcePickerWindowIdentity identity{QStringLiteral("Windows Input Experience"),
                                                           QStringLiteral("TextInputHost.exe"),
                                                           QStringLiteral("Windows.UI.Core.CoreWindow")};
    EXPECT_TRUE(ui::dialogs::ShouldExcludeByIdentity(identity));
}

TEST(SourcePickerWindowRulesTest, FiltersProgramManagerShellWindow) {
    const ui::dialogs::SourcePickerWindowIdentity identity{QStringLiteral("Program Manager"),
                                                           QStringLiteral("explorer.exe"), QStringLiteral("Progman")};
    EXPECT_TRUE(ui::dialogs::ShouldExcludeByIdentity(identity));
}

TEST(SourcePickerWindowRulesTest, FiltersOverlayWindows) {
    const ui::dialogs::SourcePickerWindowIdentity identity{
        QStringLiteral("NVIDIA Overlay"), QStringLiteral("NVIDIA Share.exe"), QStringLiteral("CEF-OSC-WIDGET")};
    EXPECT_TRUE(ui::dialogs::ShouldExcludeByIdentity(identity));
}

TEST(SourcePickerWindowRulesTest, FiltersDeveloperToolsWindows) {
    const ui::dialogs::SourcePickerWindowIdentity identity{
        QStringLiteral("claude.ai — DevTools"), QStringLiteral("brave.exe"), QStringLiteral("Chrome_WidgetWin_1")};
    EXPECT_TRUE(ui::dialogs::ShouldExcludeByIdentity(identity));
}

TEST(SourcePickerWindowRulesTest, FiltersPhantomWinStubTitle) {
    const ui::dialogs::SourcePickerWindowIdentity identity{QStringLiteral("[Win...]"),
                                                           QStringLiteral("applicationframehost.exe"),
                                                           QStringLiteral("ApplicationFrameWindow")};
    EXPECT_TRUE(ui::dialogs::ShouldExcludeByIdentity(identity));
}

TEST(SourcePickerWindowRulesTest, KeepsNormalUserFacingWindow) {
    const ui::dialogs::SourcePickerWindowIdentity identity{QStringLiteral("README.md - Visual Studio Code"),
                                                           QStringLiteral("Code.exe"),
                                                           QStringLiteral("Chrome_WidgetWin_1")};
    EXPECT_FALSE(ui::dialogs::ShouldExcludeByIdentity(identity));
}

} // namespace
} // namespace exosnap
