#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QWidget>

#include "ui/dialogs/SourcePickerOverlay.h"
#include "ui/dialogs/SourcePickerPanel.h"
#include "ui/widgets/CaptureTargetCard.h"

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "source_picker_overlay_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class SourcePickerOverlayTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(SourcePickerOverlayTest, RendersInWindowNotAsNativeDialog) {
    ui::dialogs::SourcePickerOverlay overlay;

    // Must be a plain QWidget overlay — never a separate native QDialog.
    EXPECT_EQ(qobject_cast<QDialog*>(&overlay), nullptr);
}

TEST_F(SourcePickerOverlayTest, ContainsEmbeddedPickerDialog) {
    ui::dialogs::SourcePickerOverlay overlay;

    // The embedded picker dialog must be a child of the overlay, not a
    // top-level window.
    auto* dialog = overlay.findChild<ui::dialogs::SourcePickerPanel*>(QStringLiteral("sourcePickerDialog"));
    ASSERT_NE(dialog, nullptr);
    EXPECT_EQ(dialog->parent(), &overlay);
}

TEST_F(SourcePickerOverlayTest, ContainsDisplaysWindowsRegionTabs) {
    ui::dialogs::SourcePickerOverlay overlay;

    EXPECT_NE(overlay.findChild<QPushButton*>(QStringLiteral("sourcePickerScreensButton")), nullptr);
    EXPECT_NE(overlay.findChild<QPushButton*>(QStringLiteral("sourcePickerWindowsButton")), nullptr);
    EXPECT_NE(overlay.findChild<QPushButton*>(QStringLiteral("sourcePickerRegionButton")), nullptr);
}

TEST_F(SourcePickerOverlayTest, ContainsCancelAndUseSelectedButtons) {
    ui::dialogs::SourcePickerOverlay overlay;

    EXPECT_NE(overlay.findChild<QPushButton*>(QStringLiteral("sourcePickerCancelButton")), nullptr);
    EXPECT_NE(overlay.findChild<QPushButton*>(QStringLiteral("sourcePickerUseButton")), nullptr);
}

TEST_F(SourcePickerOverlayTest, IsInitiallyHidden) {
    ui::dialogs::SourcePickerOverlay overlay;
    EXPECT_TRUE(overlay.isHidden());
    EXPECT_FALSE(overlay.isOpen());
}

TEST_F(SourcePickerOverlayTest, OpenOverlay_MakesOverlayVisible) {
    QWidget host;
    host.resize(1280, 820);
    ui::dialogs::SourcePickerOverlay overlay(&host);

    overlay.openOverlay();
    EXPECT_TRUE(overlay.isOpen());
    overlay.closeOverlay();
}

TEST_F(SourcePickerOverlayTest, CloseOverlay_HidesOverlayAndEmitsClosedSignal) {
    QWidget host;
    host.resize(1280, 820);
    ui::dialogs::SourcePickerOverlay overlay(&host);
    overlay.openOverlay();

    bool closed_fired = false;
    QObject::connect(&overlay, &ui::dialogs::SourcePickerOverlay::closed, [&]() { closed_fired = true; });

    overlay.closeOverlay();
    EXPECT_TRUE(overlay.isHidden());
    EXPECT_TRUE(closed_fired);
}

TEST_F(SourcePickerOverlayTest, CancelButton_ClosesOverlayAndEmitsClosed) {
    QWidget host;
    host.resize(1280, 820);
    ui::dialogs::SourcePickerOverlay overlay(&host);
    overlay.openOverlay();

    bool closed_fired = false;
    QObject::connect(&overlay, &ui::dialogs::SourcePickerOverlay::closed, [&]() { closed_fired = true; });

    auto* cancel = overlay.findChild<QPushButton*>(QStringLiteral("sourcePickerCancelButton"));
    ASSERT_NE(cancel, nullptr);
    cancel->click();

    EXPECT_TRUE(overlay.isHidden());
    EXPECT_TRUE(closed_fired);
}

TEST_F(SourcePickerOverlayTest, UseSelectedSource_EmitsSourceSelectedThenClosed) {
    QWidget host;
    host.resize(1280, 820);
    ui::dialogs::SourcePickerOverlay overlay(&host);

    ui::dialogs::SourcePickerPanel::SourceOption screen;
    screen.target_index = 1;
    screen.title = QStringLiteral("Display 1");
    screen.detail = QStringLiteral("2560 × 1440");
    screen.primary = true;
    overlay.setScreenOptions({screen});
    overlay.setCurrentSection(ui::dialogs::SourcePickerPanel::Section::Screens, 1);
    overlay.openOverlay();

    bool selected_fired = false;
    bool closed_fired = false;
    ui::dialogs::SourcePickerPanel::SelectionResult captured_result;

    QObject::connect(&overlay, &ui::dialogs::SourcePickerOverlay::sourceSelected,
                     [&](ui::dialogs::SourcePickerPanel::SelectionResult r) {
                         selected_fired = true;
                         captured_result = r;
                     });
    QObject::connect(&overlay, &ui::dialogs::SourcePickerOverlay::closed, [&]() { closed_fired = true; });

    auto* use = overlay.findChild<QPushButton*>(QStringLiteral("sourcePickerUseButton"));
    ASSERT_NE(use, nullptr);
    ASSERT_TRUE(use->isEnabled());
    use->click();

    EXPECT_TRUE(selected_fired);
    EXPECT_TRUE(closed_fired);
    EXPECT_TRUE(captured_result.valid);
    EXPECT_EQ(captured_result.section, ui::dialogs::SourcePickerPanel::Section::Screens);
    EXPECT_EQ(captured_result.target_index, 1);
    EXPECT_TRUE(overlay.isHidden());
}

TEST_F(SourcePickerOverlayTest, ClickingScreenCard_SelectsThenUseButtonCommits) {
    QWidget host;
    host.resize(1280, 820);
    ui::dialogs::SourcePickerOverlay overlay(&host);

    ui::dialogs::SourcePickerPanel::SourceOption screen;
    screen.target_index = 3;
    screen.title = QStringLiteral("Display 3");
    screen.detail = QStringLiteral("1920 × 1080");
    screen.primary = true;
    overlay.setScreenOptions({screen});
    overlay.setCurrentSection(ui::dialogs::SourcePickerPanel::Section::Screens, 3);
    overlay.openOverlay();

    bool selected_fired = false;
    bool closed_fired = false;
    ui::dialogs::SourcePickerPanel::SelectionResult captured_result;
    QObject::connect(&overlay, &ui::dialogs::SourcePickerOverlay::sourceSelected,
                     [&](ui::dialogs::SourcePickerPanel::SelectionResult r) {
                         selected_fired = true;
                         captured_result = r;
                     });
    QObject::connect(&overlay, &ui::dialogs::SourcePickerOverlay::closed, [&]() { closed_fired = true; });

    // Click selects the card — it does NOT commit or close (restored click-to-select).
    const auto cards = overlay.findChildren<ui::widgets::CaptureTargetCard*>();
    ASSERT_FALSE(cards.isEmpty());
    emit cards.first()->clicked();
    EXPECT_FALSE(selected_fired) << "clicking a card selects, it does not commit";
    EXPECT_FALSE(closed_fired);

    // The Use button confirms the selection and closes.
    auto* use = overlay.findChild<QPushButton*>(QStringLiteral("sourcePickerUseButton"));
    ASSERT_NE(use, nullptr);
    emit use->clicked();

    EXPECT_TRUE(selected_fired);
    EXPECT_TRUE(closed_fired);
    EXPECT_TRUE(captured_result.valid);
    EXPECT_EQ(captured_result.section, ui::dialogs::SourcePickerPanel::Section::Screens);
    EXPECT_EQ(captured_result.target_index, 3);
    EXPECT_TRUE(overlay.isHidden());
}

TEST_F(SourcePickerOverlayTest, UseButtonVisibleForAllSections) {
    QWidget host;
    host.resize(1280, 820);
    ui::dialogs::SourcePickerOverlay overlay(&host);
    overlay.openOverlay();

    auto* use = overlay.findChild<QPushButton*>(QStringLiteral("sourcePickerUseButton"));
    ASSERT_NE(use, nullptr);

    // The Use button is the single confirm affordance for every section now
    // (click selects, the Use button commits) — visible for Screens and Region alike.
    overlay.setCurrentSection(ui::dialogs::SourcePickerPanel::Section::Screens, -1);
    EXPECT_FALSE(use->isHidden());

    overlay.setCurrentSection(ui::dialogs::SourcePickerPanel::Section::Region, -1);
    EXPECT_FALSE(use->isHidden());
}

TEST_F(SourcePickerOverlayTest, SetScreenOptions_ForwardedToEmbeddedDialog) {
    ui::dialogs::SourcePickerOverlay overlay;

    ui::dialogs::SourcePickerPanel::SourceOption screen;
    screen.target_index = 5;
    screen.title = QStringLiteral("Display 5");
    overlay.setScreenOptions({screen});

    // Verify via setCurrentSection + selectionResult on the embedded dialog.
    overlay.setCurrentSection(ui::dialogs::SourcePickerPanel::Section::Screens, 5);

    auto* dialog = overlay.findChild<ui::dialogs::SourcePickerPanel*>(QStringLiteral("sourcePickerDialog"));
    ASSERT_NE(dialog, nullptr);
    const auto result = dialog->selectionResult();
    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.target_index, 5);
}

TEST_F(SourcePickerOverlayTest, CloseOverlay_IdempotentWhenAlreadyClosed) {
    ui::dialogs::SourcePickerOverlay overlay;

    int closed_count = 0;
    QObject::connect(&overlay, &ui::dialogs::SourcePickerOverlay::closed, [&]() { ++closed_count; });

    overlay.closeOverlay(); // already hidden
    overlay.closeOverlay(); // still hidden — must not fire again
    EXPECT_EQ(closed_count, 0);
}

TEST_F(SourcePickerOverlayTest, EmbeddedDialog_HasOverlayAsParent) {
    ui::dialogs::SourcePickerOverlay overlay;

    auto* dialog = overlay.findChild<ui::dialogs::SourcePickerPanel*>(QStringLiteral("sourcePickerDialog"));
    ASSERT_NE(dialog, nullptr);
    // The dialog must be a child widget inside the overlay, not an independent
    // top-level widget. A non-null parent confirms it is embedded.
    EXPECT_NE(dialog->parentWidget(), nullptr);
    EXPECT_FALSE(dialog->parentWidget()->parentWidget() == nullptr && dialog->parentWidget() != &overlay);
}

} // namespace
} // namespace exosnap
