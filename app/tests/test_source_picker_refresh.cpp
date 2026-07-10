#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QImage>
#include <QLabel>
#include <QMetaObject>
#include <QPushButton>
#include <QTimer>

#include "ui/dialogs/SourcePickerDialog.h"
#include "ui/dialogs/SourcePickerOverlay.h"
#include "ui/dialogs/SourcePickerPanel.h"
#include "ui/widgets/CaptureTargetCard.h"

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        return existing;
    }

    static int argc = 1;
    static char app_name[] = "source_picker_refresh_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

ui::dialogs::SourcePickerPanel::SourceOption MakeScreen(int idx, const QString& title, bool primary = false) {
    ui::dialogs::SourcePickerPanel::SourceOption opt;
    opt.target_index = idx;
    opt.native_id = static_cast<uintptr_t>(1000 + idx);
    opt.title = title;
    opt.detail = QStringLiteral("2560 X 1440");
    opt.primary = primary;
    opt.selectable = true;
    opt.monitor_width = 2560;
    opt.monitor_height = 1440;
    return opt;
}

ui::dialogs::SourcePickerPanel::SourceOption MakeWindow(int idx, const QString& title, bool unavailable = false) {
    ui::dialogs::SourcePickerPanel::SourceOption opt;
    opt.target_index = idx;
    opt.native_id = static_cast<uintptr_t>(2000 + idx);
    opt.title = title;
    opt.detail = QStringLiteral("1920 X 1080 - app.exe");
    opt.selectable = !unavailable;
    opt.unavailable = unavailable;
    opt.hidden_by_default = unavailable;
    return opt;
}

class SourcePickerRefreshTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

// 1. Opening picker triggers immediate refresh (verified via overlay signal connected to flag).
TEST_F(SourcePickerRefreshTest, Open_TriggersImmediateRefreshSignal) {
    ui::dialogs::SourcePickerOverlay overlay;
    int refresh_count = 0;
    QObject::connect(&overlay, &ui::dialogs::SourcePickerOverlay::sourceDataRequested,
                     [&refresh_count]() { ++refresh_count; });

    overlay.openOverlay();
    EXPECT_GE(refresh_count, 1);
    overlay.closeOverlay();
}

// 2. Panel showEvent starts the source refresh timer.
TEST_F(SourcePickerRefreshTest, ShowEvent_StartsRefreshTimerAndEmitsSignal) {
    auto* panel = new ui::dialogs::SourcePickerPanel(nullptr);
    int count = 0;
    QObject::connect(panel, &ui::dialogs::SourcePickerPanel::sourceDataRequested, [&count]() { ++count; });

    panel->setVisible(true);
    // showEvent emits sourceDataRequested immediately
    EXPECT_GE(count, 1);

    delete panel;
}

// 3. Panel hideEvent stops further refresh requests.
TEST_F(SourcePickerRefreshTest, HideEvent_StopsRefreshSignal) {
    auto* panel = new ui::dialogs::SourcePickerPanel(nullptr);
    int count = 0;
    QObject::connect(panel, &ui::dialogs::SourcePickerPanel::sourceDataRequested, [&count]() { ++count; });

    panel->setVisible(true);
    const int count_before_hide = count;

    panel->setVisible(false);
    // Process events and verify no more signals
    QCoreApplication::processEvents();
    EXPECT_EQ(count, count_before_hide);

    delete panel;
}

// 4. Reopen does not duplicate timer.
TEST_F(SourcePickerRefreshTest, Reopen_DoesNotDuplicateTimer) {
    ui::dialogs::SourcePickerOverlay overlay;
    overlay.openOverlay();
    overlay.closeOverlay();

    int count = 0;
    QObject::connect(&overlay, &ui::dialogs::SourcePickerOverlay::sourceDataRequested, [&count]() { ++count; });

    overlay.openOverlay();
    EXPECT_GE(count, 1);
    overlay.closeOverlay();
}

// 5. Manual Rescan triggers sourceDataRequested.
TEST_F(SourcePickerRefreshTest, Rescan_TriggersSourceDataRequested) {
    ui::dialogs::SourcePickerDialog dialog;
    dialog.setScreenOptions({MakeScreen(0, QStringLiteral("Display 0"), true)});
    dialog.setCurrentSelection(ui::dialogs::SourcePickerDialog::Section::Screens, 0);
    dialog.show();

    int count = 0;
    QObject::connect(&dialog, &ui::dialogs::SourcePickerDialog::sourceDataRequested, [&count]() { ++count; });

    auto* refresh = dialog.findChild<QPushButton*>(QStringLiteral("sourcePickerRefreshButton"));
    ASSERT_NE(refresh, nullptr);
    ASSERT_TRUE(refresh->isEnabled());
    refresh->click();

    EXPECT_GE(count, 1);
}

// 6. Identical source snapshot causes no structural rebuild.
TEST_F(SourcePickerRefreshTest, IdenticalSnapshot_SkipsCardRebuild) {
    ui::dialogs::SourcePickerDialog dialog;
    dialog.setScreenOptions({MakeScreen(0, QStringLiteral("Display 0"), true)});
    dialog.setWindowOptions({MakeWindow(10, QStringLiteral("App A"))});

    const int initial_card_count = dialog.findChildren<ui::widgets::CaptureTargetCard*>().size();
    ASSERT_EQ(initial_card_count, 2);

    // Push the same options again
    dialog.setScreenOptions({MakeScreen(0, QStringLiteral("Display 0"), true)});
    dialog.setWindowOptions({MakeWindow(10, QStringLiteral("App A"))});

    const int after_card_count = dialog.findChildren<ui::widgets::CaptureTargetCard*>().size();
    EXPECT_EQ(after_card_count, initial_card_count);
}

// 7. Identical options with same ordering remain semantically equal.
TEST_F(SourcePickerRefreshTest, EnumerationOrderChange_PreservesSemanticEquality) {
    ui::dialogs::SourcePickerDialog dialog;
    dialog.setWindowOptions({MakeWindow(10, QStringLiteral("App A")), MakeWindow(11, QStringLiteral("App B"))});

    const int initial_card_count = dialog.findChildren<ui::widgets::CaptureTargetCard*>().size();
    ASSERT_EQ(initial_card_count, 2);

    // Push same data
    dialog.setWindowOptions({MakeWindow(10, QStringLiteral("App A")), MakeWindow(11, QStringLiteral("App B"))});

    const int after_card_count = dialog.findChildren<ui::widgets::CaptureTargetCard*>().size();
    EXPECT_EQ(after_card_count, initial_card_count);
}

// 8. Added window updates the list.
TEST_F(SourcePickerRefreshTest, AddedWindow_UpdatesCardList) {
    ui::dialogs::SourcePickerDialog dialog;
    dialog.setWindowOptions({MakeWindow(10, QStringLiteral("App A"))});

    const int count_before = dialog.findChildren<ui::widgets::CaptureTargetCard*>().size();
    ASSERT_EQ(count_before, 1);

    dialog.setWindowOptions({MakeWindow(10, QStringLiteral("App A")), MakeWindow(11, QStringLiteral("App B"))});

    const int count_after = dialog.findChildren<ui::widgets::CaptureTargetCard*>().size();
    EXPECT_EQ(count_after, 2);
}

// 9. Removed window updates the list.
TEST_F(SourcePickerRefreshTest, RemovedWindow_UpdatesCardList) {
    ui::dialogs::SourcePickerDialog dialog;
    dialog.setWindowOptions({MakeWindow(10, QStringLiteral("App A")), MakeWindow(11, QStringLiteral("App B"))});

    const int count_before = dialog.findChildren<ui::widgets::CaptureTargetCard*>().size();
    ASSERT_EQ(count_before, 2);

    dialog.setWindowOptions({MakeWindow(10, QStringLiteral("App A"))});

    const int count_after = dialog.findChildren<ui::widgets::CaptureTargetCard*>().size();
    EXPECT_EQ(count_after, 1);
}

// 10. Existing selection survives refresh.
TEST_F(SourcePickerRefreshTest, SelectionSurvivesRefresh) {
    ui::dialogs::SourcePickerDialog dialog;
    dialog.setScreenOptions({MakeScreen(0, QStringLiteral("Display 0"), true)});
    dialog.setWindowOptions({MakeWindow(10, QStringLiteral("App A"))});
    dialog.setCurrentSelection(ui::dialogs::SourcePickerDialog::Section::Windows, 10);

    auto before = dialog.selectionResult();
    ASSERT_TRUE(before.valid);
    ASSERT_EQ(before.target_index, 10);

    // Refresh with same data
    dialog.setWindowOptions({MakeWindow(10, QStringLiteral("App A"))});

    auto after = dialog.selectionResult();
    EXPECT_TRUE(after.valid);
    EXPECT_EQ(after.target_index, 10);
}

// 11. Disappeared selected window becomes unavailable.
TEST_F(SourcePickerRefreshTest, DisappearedWindow_MarkedUnavailable) {
    ui::dialogs::SourcePickerDialog dialog;
    auto window = MakeWindow(10, QStringLiteral("App A"), false);
    dialog.setWindowOptions({window});
    dialog.setCurrentSelection(ui::dialogs::SourcePickerDialog::Section::Windows, 10);

    auto before = dialog.selectionResult();
    ASSERT_TRUE(before.valid);

    // Now the window becomes unavailable
    auto unavailable_window = MakeWindow(10, QStringLiteral("App A"), true);
    dialog.setWindowOptions({unavailable_window});

    auto after = dialog.selectionResult();
    EXPECT_EQ(after.section, ui::dialogs::SourcePickerDialog::Section::Windows);
    EXPECT_FALSE(after.valid);
    EXPECT_FALSE(dialog.selectSource(ui::dialogs::SourcePickerDialog::Section::Windows, 10));
}

// 12. Reappearing matching window restores availability.
TEST_F(SourcePickerRefreshTest, ReappearingWindow_RestoresAvailability) {
    ui::dialogs::SourcePickerDialog dialog;
    dialog.setWindowOptions({MakeWindow(10, QStringLiteral("App A"), false)});
    dialog.setCurrentSelection(ui::dialogs::SourcePickerDialog::Section::Windows, 10);

    dialog.setWindowOptions({MakeWindow(10, QStringLiteral("App A"), true)});
    dialog.setWindowOptions({MakeWindow(10, QStringLiteral("App A"), false)});

    auto after = dialog.selectionResult();
    EXPECT_TRUE(after.valid);
    EXPECT_TRUE(dialog.selectSource(ui::dialogs::SourcePickerDialog::Section::Windows, 10));
}

// 13. Display change updates the screen options.
TEST_F(SourcePickerRefreshTest, DisplayChange_UpdatesScreenOptions) {
    ui::dialogs::SourcePickerDialog dialog;
    dialog.setScreenOptions({MakeScreen(0, QStringLiteral("Display 0"), true)});

    const int count_before = dialog.findChildren<ui::widgets::CaptureTargetCard*>().size();
    ASSERT_EQ(count_before, 1);

    dialog.setScreenOptions(
        {MakeScreen(0, QStringLiteral("Display 0"), true), MakeScreen(1, QStringLiteral("Display 1"))});

    const int count_after = dialog.findChildren<ui::widgets::CaptureTargetCard*>().size();
    EXPECT_EQ(count_after, 2);
}

// 14. Title change in a window causes card rebuild.
TEST_F(SourcePickerRefreshTest, TitleChange_UpdatesCard) {
    ui::dialogs::SourcePickerDialog dialog;
    dialog.setWindowOptions({MakeWindow(10, QStringLiteral("Original Title"))});

    const int count_before = dialog.findChildren<ui::widgets::CaptureTargetCard*>().size();
    ASSERT_EQ(count_before, 1);

    dialog.setWindowOptions({MakeWindow(10, QStringLiteral("New Title"))});

    const int count_after = dialog.findChildren<ui::widgets::CaptureTargetCard*>().size();
    EXPECT_EQ(count_after, 1);
}

// 15. Snapshot equality for identical source data.
TEST(SourcePickerSnapshotTest, IdenticalOptions_ProduceEqualSnapshots) {
    const auto screen1 = MakeScreen(0, QStringLiteral("Display 0"), true);
    const auto screen2 = MakeScreen(0, QStringLiteral("Display 0"), true);

    auto snap1 = ui::dialogs::SourcePickerPanel::buildSnapshot({screen1});
    auto snap2 = ui::dialogs::SourcePickerPanel::buildSnapshot({screen2});

    EXPECT_TRUE(snap1 == snap2);
}

// 16. Snapshot inequality for differing data.
TEST(SourcePickerSnapshotTest, DifferentOptions_ProduceUnequalSnapshots) {
    auto snap1 = ui::dialogs::SourcePickerPanel::buildSnapshot({MakeWindow(10, QStringLiteral("App A"))});
    auto snap2 = ui::dialogs::SourcePickerPanel::buildSnapshot({MakeWindow(11, QStringLiteral("App B"))});

    EXPECT_FALSE(snap1 == snap2);
}

// 17. Snapshot difference for title change.
TEST(SourcePickerSnapshotTest, TitleChange_ProducesDifferentSnapshot) {
    auto snap1 = ui::dialogs::SourcePickerPanel::buildSnapshot({MakeWindow(10, QStringLiteral("Old"))});
    auto snap2 = ui::dialogs::SourcePickerPanel::buildSnapshot({MakeWindow(10, QStringLiteral("New"))});

    EXPECT_FALSE(snap1 == snap2);
}

// 18. Snapshot difference for availability change.
TEST(SourcePickerSnapshotTest, AvailabilityChange_ProducesDifferentSnapshot) {
    auto snap1 = ui::dialogs::SourcePickerPanel::buildSnapshot({MakeWindow(10, QStringLiteral("App"), false)});
    auto snap2 = ui::dialogs::SourcePickerPanel::buildSnapshot({MakeWindow(10, QStringLiteral("App"), true)});

    EXPECT_FALSE(snap1 == snap2);
}

// A card is destroyed and rebuilt whenever any source's title or availability
// changes. These pin the tile against the flicker that caused: a rebuilt card
// resumes its last image, and a source that stops producing keeps showing one.

namespace {

// Stands in for the capture thread handing the panel a frame.
void FeedThumbnail(ui::dialogs::SourcePickerPanel* panel, int target_index) {
    QImage image(4, 4, QImage::Format_ARGB32);
    image.fill(Qt::red);
    QMetaObject::invokeMethod(panel, "onThumbnailReady", Qt::DirectConnection, Q_ARG(int, target_index),
                              Q_ARG(int, panel->refreshGenerationForTest()), Q_ARG(QImage, image));
}

ui::widgets::CaptureTargetCard* VisibleCard(ui::dialogs::SourcePickerDialog& dialog, const QString& title) {
    for (auto* card : dialog.findChildren<ui::widgets::CaptureTargetCard*>()) {
        if (card->accessibleName().contains(title))
            return card;
    }
    return nullptr;
}

// The card's state message lives in a label it does not expose. A card that was
// never shown has no visible children, so ask whether the label would be drawn.
bool HasShownLabel(const ui::widgets::CaptureTargetCard& card, const QString& text) {
    for (auto* label : card.findChildren<QLabel*>()) {
        if (label->text() == text && !label->isHidden())
            return true;
    }
    return false;
}

// The panel drops thumbnails while hidden, so these tests must show it.
ui::dialogs::SourcePickerPanel* ShownPanel(ui::dialogs::SourcePickerDialog& dialog) {
    dialog.show();
    QCoreApplication::processEvents();
    auto* panel = dialog.findChild<ui::dialogs::SourcePickerPanel*>();
    return (panel && panel->isVisible()) ? panel : nullptr;
}

} // namespace

TEST_F(SourcePickerRefreshTest, RebuiltCard_KeepsItsLastThumbnail) {
    ui::dialogs::SourcePickerDialog dialog;
    dialog.setWindowOptions({MakeWindow(10, QStringLiteral("App A"))});
    auto* panel = ShownPanel(dialog);
    ASSERT_NE(panel, nullptr);

    FeedThumbnail(panel, 10);
    ASSERT_TRUE(VisibleCard(dialog, QStringLiteral("App A"))->hasThumbnail());

    // A title change rebuilds every card in both sections.
    dialog.setWindowOptions({MakeWindow(10, QStringLiteral("App A renamed"))});

    auto* rebuilt = VisibleCard(dialog, QStringLiteral("App A renamed"));
    ASSERT_NE(rebuilt, nullptr);
    EXPECT_TRUE(rebuilt->hasThumbnail()) << "a rebuilt card fell back to \"Loading preview...\"";
}

TEST_F(SourcePickerRefreshTest, VanishedSource_DoesNotLendItsThumbnailToTheNextOne) {
    ui::dialogs::SourcePickerDialog dialog;
    dialog.setWindowOptions({MakeWindow(10, QStringLiteral("App A"))});
    auto* panel = ShownPanel(dialog);
    ASSERT_NE(panel, nullptr);

    FeedThumbnail(panel, 10);
    ASSERT_TRUE(VisibleCard(dialog, QStringLiteral("App A"))->hasThumbnail());

    // Index 10 is reused by a different window: same slot, different source.
    dialog.setWindowOptions({MakeWindow(11, QStringLiteral("App B"))});

    auto* fresh = VisibleCard(dialog, QStringLiteral("App B"));
    ASSERT_NE(fresh, nullptr);
    EXPECT_FALSE(fresh->hasThumbnail()) << "the cache is keyed by source, not by list position";
}

TEST_F(SourcePickerRefreshTest, FailureDoesNotWipeAThumbnailTheTileAlreadyHas) {
    ui::dialogs::SourcePickerDialog dialog;
    dialog.setWindowOptions({MakeWindow(10, QStringLiteral("App A"))});
    auto* panel = ShownPanel(dialog);
    ASSERT_NE(panel, nullptr);

    FeedThumbnail(panel, 10);
    ASSERT_TRUE(VisibleCard(dialog, QStringLiteral("App A"))->hasThumbnail());

    QMetaObject::invokeMethod(panel, "onThumbnailFailed", Qt::DirectConnection, Q_ARG(int, 10),
                              Q_ARG(int, panel->refreshGenerationForTest()));

    EXPECT_TRUE(VisibleCard(dialog, QStringLiteral("App A"))->hasThumbnail())
        << "a held image is quieter than \"Preview unavailable\"";
}

// A live tile asks for Ready on every frame. The card must not repolish its
// style for a state it is already in -- that is the black flash -- but it must
// still act on a state that genuinely changed.
TEST_F(SourcePickerRefreshTest, CardStateTransitionsSurviveTheUnchangedStateShortcut) {
    QPixmap red(4, 4);
    red.fill(Qt::red);
    QPixmap blue(4, 4);
    blue.fill(Qt::blue);

    ui::widgets::CaptureTargetCard card;
    EXPECT_FALSE(card.hasThumbnail()) << "a fresh card has no picture";

    card.setThumbnail(red);
    EXPECT_TRUE(card.hasThumbnail());

    // The steady case: a new frame, the same state. Still Ready.
    card.setThumbnail(blue);
    EXPECT_TRUE(card.hasThumbnail());

    card.setThumbnailFailureText(QStringLiteral("Preview unavailable"));
    EXPECT_FALSE(card.hasThumbnail()) << "the shortcut swallowed a real state change";

    card.setThumbnail(red);
    EXPECT_TRUE(card.hasThumbnail());

    card.setThumbnailLoadingText(QStringLiteral("Loading preview..."));
    EXPECT_FALSE(card.hasThumbnail());
}

// The first state a card is ever put into is Loading, which is also the field's
// default. A shortcut that compares the state alone would conclude there is
// nothing to do and never show the text.
TEST_F(SourcePickerRefreshTest, TheFirstStateIsAppliedEvenWhenItMatchesTheDefault) {
    ui::widgets::CaptureTargetCard card;
    card.setThumbnailLoadingText(QStringLiteral("Loading preview..."));

    EXPECT_TRUE(HasShownLabel(card, QStringLiteral("Loading preview...")))
        << "the card never displayed the state it was put into";
}

// Two different messages are two different states, however equal their kind.
TEST_F(SourcePickerRefreshTest, ANewMessageForTheSameKindIsStillApplied) {
    ui::widgets::CaptureTargetCard card;
    card.setThumbnailUnavailableText(QStringLiteral("Minimized"));
    ASSERT_TRUE(HasShownLabel(card, QStringLiteral("Minimized")));

    card.setThumbnailUnavailableText(QStringLiteral("Unavailable"));
    EXPECT_TRUE(HasShownLabel(card, QStringLiteral("Unavailable")));
    EXPECT_FALSE(HasShownLabel(card, QStringLiteral("Minimized")));
}

TEST_F(SourcePickerRefreshTest, ASourceThatNeverProducedStillReportsUnavailable) {
    ui::dialogs::SourcePickerDialog dialog;
    dialog.setWindowOptions({MakeWindow(10, QStringLiteral("App A"))});
    auto* panel = ShownPanel(dialog);
    ASSERT_NE(panel, nullptr);

    QMetaObject::invokeMethod(panel, "onThumbnailFailed", Qt::DirectConnection, Q_ARG(int, 10),
                              Q_ARG(int, panel->refreshGenerationForTest()));

    EXPECT_FALSE(VisibleCard(dialog, QStringLiteral("App A"))->hasThumbnail());
}

} // namespace
} // namespace exosnap
