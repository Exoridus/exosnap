#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QWidget>

#include "ui/widgets/ExportPanel.h"

namespace exosnap::ui::widgets {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "export_panel_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

// The status groups are shown/hidden as a unit; their children are never
// explicitly hidden, so visibility is asked of the group (isVisibleTo, which
// answers for an unmapped test widget too).
QWidget* Running(const ExportPanel& panel) {
    return panel.findChild<QWidget*>(QStringLiteral("exportPanelRunning"));
}
QWidget* Result(const ExportPanel& panel) {
    return panel.findChild<QWidget*>(QStringLiteral("exportPanelResult"));
}

class ExportPanelTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

// ---- The panel is embedded, not an overlay ----

// The whole point of the rework: the export settings sit in the rail, so the
// panel must be an ordinary child widget that a layout can place — not a
// self-positioning overlay that paints a backdrop over its parent.
TEST_F(ExportPanelTest, IsAPlainEmbeddedChildWidget) {
    QWidget host;
    host.resize(280, 700);
    ExportPanel panel(&host);

    EXPECT_EQ(panel.objectName(), QStringLiteral("exportPanel"));
    EXPECT_EQ(panel.parentWidget(), &host);
    // An overlay sized itself to its parent's full rect; this must not.
    EXPECT_NE(panel.geometry(), host.rect());
}

TEST_F(ExportPanelTest, StartsInOptionsWithNoStatusShowing) {
    QWidget host;
    ExportPanel panel(&host);

    EXPECT_EQ(panel.state(), ExportPanel::State::Options);
    EXPECT_FALSE(Running(panel)->isVisibleTo(&panel));
    EXPECT_FALSE(Result(panel)->isVisibleTo(&panel));
}

// ---- Output rows ----

TEST_F(ExportPanelTest, ContainerCombo_OffersBothStreamCopyContainers) {
    QWidget host;
    ExportPanel panel(&host);

    auto* combo = panel.findChild<QComboBox*>(QStringLiteral("outputContainerCombo"));
    ASSERT_NE(combo, nullptr);
    ASSERT_EQ(combo->count(), 2);
    EXPECT_EQ(combo->itemData(0).toString(), QStringLiteral("mkv"));
    EXPECT_EQ(combo->itemData(1).toString(), QStringLiteral("mp4"));
}

TEST_F(ExportPanelTest, SaveModeCombo_OffersNewFileAndOverwrite) {
    QWidget host;
    ExportPanel panel(&host);

    auto* combo = panel.findChild<QComboBox*>(QStringLiteral("outputSaveModeCombo"));
    ASSERT_NE(combo, nullptr);
    ASSERT_EQ(combo->count(), 2);
    EXPECT_EQ(combo->itemData(0).toString(), QStringLiteral("new"));
    EXPECT_EQ(combo->itemData(1).toString(), QStringLiteral("overwrite"));
}

TEST_F(ExportPanelTest, ContainerKeyAndSaveModeKey_ReflectTheSelection) {
    QWidget host;
    ExportPanel panel(&host);

    EXPECT_EQ(panel.containerKey(), QStringLiteral("mkv"));
    EXPECT_EQ(panel.saveModeKey(), QStringLiteral("new"));

    panel.findChild<QComboBox*>(QStringLiteral("outputContainerCombo"))->setCurrentIndex(1);
    panel.findChild<QComboBox*>(QStringLiteral("outputSaveModeCombo"))->setCurrentIndex(1);

    EXPECT_EQ(panel.containerKey(), QStringLiteral("mp4"));
    EXPECT_EQ(panel.saveModeKey(), QStringLiteral("overwrite"));
}

// The destination is fully determined by the save mode (ADR 0022: no folder
// picker), so the line has to say what the current mode actually does.
TEST_F(ExportPanelTest, DestinationLine_FollowsTheSaveMode) {
    QWidget host;
    ExportPanel panel(&host);

    auto* dest = panel.findChild<QLabel*>(QStringLiteral("editExportDestFolder"));
    ASSERT_NE(dest, nullptr);
    EXPECT_TRUE(dest->text().contains(QStringLiteral("_edit"))) << dest->text().toStdString();

    panel.findChild<QComboBox*>(QStringLiteral("outputSaveModeCombo"))->setCurrentIndex(1);
    EXPECT_TRUE(dest->text().contains(QStringLiteral("Replaces the original"))) << dest->text().toStdString();
}

TEST_F(ExportPanelTest, DestinationLine_NamesTheSelectedContainerExtension) {
    QWidget host;
    ExportPanel panel(&host);

    auto* dest = panel.findChild<QLabel*>(QStringLiteral("editExportDestFolder"));
    ASSERT_NE(dest, nullptr);
    EXPECT_TRUE(dest->text().contains(QStringLiteral(".mkv"))) << dest->text().toStdString();

    panel.findChild<QComboBox*>(QStringLiteral("outputContainerCombo"))->setCurrentIndex(1);
    EXPECT_TRUE(dest->text().contains(QStringLiteral(".mp4"))) << dest->text().toStdString();
}

// ---- The four states ----

TEST_F(ExportPanelTest, ShowRunning_ShowsProgressAndCancelAndResetsTheBar) {
    QWidget host;
    ExportPanel panel(&host);
    host.show();

    panel.setProgress(80);
    panel.showRunning();

    EXPECT_EQ(panel.state(), ExportPanel::State::Running);
    auto* progress = panel.findChild<QProgressBar*>(QStringLiteral("exportProgressBar"));
    ASSERT_NE(progress, nullptr);
    EXPECT_EQ(progress->value(), 0);
    EXPECT_TRUE(Running(panel)->isVisibleTo(&panel));
    EXPECT_TRUE(panel.findChild<QPushButton*>(QStringLiteral("exportCancelBtn"))->isVisibleTo(&panel));
    EXPECT_FALSE(Result(panel)->isVisibleTo(&panel));
}

// A run in flight must not have its container or target changed under it, but
// the rows stay visible so the panel does not swap content out mid-export.
TEST_F(ExportPanelTest, OutputRowsStayVisibleButDisabledWhileRunning) {
    QWidget host;
    ExportPanel panel(&host);
    host.show();

    auto* options = panel.findChild<QWidget*>(QStringLiteral("exportPanelOptions"));
    ASSERT_NE(options, nullptr);
    EXPECT_TRUE(options->isEnabled());

    panel.showRunning();
    EXPECT_FALSE(options->isHidden());
    EXPECT_FALSE(options->isEnabled());

    panel.showDone(QStringLiteral("C:\\out\\clip_edit.mkv"));
    EXPECT_TRUE(options->isEnabled());
}

TEST_F(ExportPanelTest, SetProgress_ClampsToTheValidRange) {
    QWidget host;
    ExportPanel panel(&host);
    auto* progress = panel.findChild<QProgressBar*>(QStringLiteral("exportProgressBar"));
    ASSERT_NE(progress, nullptr);

    panel.setProgress(42);
    EXPECT_EQ(progress->value(), 42);
    panel.setProgress(-10);
    EXPECT_EQ(progress->value(), 0);
    panel.setProgress(150);
    EXPECT_EQ(progress->value(), 100);
}

TEST_F(ExportPanelTest, ShowDone_NamesTheOutputFileAndOffersTheFileActions) {
    QWidget host;
    ExportPanel panel(&host);
    host.show();

    panel.showDone(QStringLiteral("C:\\Videos\\clip_edit.mkv"));

    EXPECT_EQ(panel.state(), ExportPanel::State::Done);
    auto* title = panel.findChild<QLabel*>(QStringLiteral("exportResultTitle"));
    ASSERT_NE(title, nullptr);
    EXPECT_EQ(title->text(), QStringLiteral("Export complete"));
    EXPECT_EQ(panel.findChild<QLabel*>(QStringLiteral("exportResultDetail"))->text(), QStringLiteral("clip_edit.mkv"));

    EXPECT_TRUE(panel.findChild<QPushButton*>(QStringLiteral("exportOpenFolderBtn"))->isVisibleTo(&panel));
    EXPECT_TRUE(panel.findChild<QPushButton*>(QStringLiteral("exportRevealBtn"))->isVisibleTo(&panel));
    EXPECT_FALSE(panel.findChild<QPushButton*>(QStringLiteral("exportRetryBtn"))->isVisibleTo(&panel));
    EXPECT_FALSE(Running(panel)->isVisibleTo(&panel));
}

TEST_F(ExportPanelTest, ShowFailed_CarriesTheRealErrorAndOffersRetry) {
    QWidget host;
    ExportPanel panel(&host);
    host.show();

    panel.showFailed(QStringLiteral("Could not open the edit master."));

    EXPECT_EQ(panel.state(), ExportPanel::State::Failed);
    EXPECT_EQ(panel.findChild<QLabel*>(QStringLiteral("exportResultTitle"))->text(), QStringLiteral("Export failed"));
    EXPECT_EQ(panel.findChild<QLabel*>(QStringLiteral("exportResultDetail"))->text(),
              QStringLiteral("Could not open the edit master."));

    EXPECT_TRUE(panel.findChild<QPushButton*>(QStringLiteral("exportRetryBtn"))->isVisibleTo(&panel));
    EXPECT_FALSE(panel.findChild<QPushButton*>(QStringLiteral("exportOpenFolderBtn"))->isVisibleTo(&panel));
    EXPECT_FALSE(panel.findChild<QPushButton*>(QStringLiteral("exportRevealBtn"))->isVisibleTo(&panel));
}

TEST_F(ExportPanelTest, ShowFailed_WithNoMessageStillSaysSomething) {
    QWidget host;
    ExportPanel panel(&host);
    panel.showFailed(QString());
    EXPECT_EQ(panel.findChild<QLabel*>(QStringLiteral("exportResultDetail"))->text(), QStringLiteral("Unknown error"));
}

TEST_F(ExportPanelTest, Reset_ReturnsToOptionsAndClearsTheStatusArea) {
    QWidget host;
    ExportPanel panel(&host);
    host.show();

    panel.showFailed(QStringLiteral("boom"));
    ASSERT_EQ(panel.state(), ExportPanel::State::Failed);

    panel.reset();
    EXPECT_EQ(panel.state(), ExportPanel::State::Options);
    EXPECT_FALSE(Result(panel)->isVisibleTo(&panel));
    EXPECT_FALSE(Running(panel)->isVisibleTo(&panel));
    EXPECT_EQ(panel.findChild<QProgressBar*>(QStringLiteral("exportProgressBar"))->value(), 0);
}

// ---- The panel only ever emits; the page owns every transition ----

TEST_F(ExportPanelTest, CancelButton_EmitsCancelRequested) {
    QWidget host;
    ExportPanel panel(&host);
    panel.showRunning();

    int count = 0;
    QObject::connect(&panel, &ExportPanel::cancelRequested, [&]() { ++count; });
    panel.findChild<QPushButton*>(QStringLiteral("exportCancelBtn"))->click();

    EXPECT_EQ(count, 1);
    EXPECT_EQ(panel.state(), ExportPanel::State::Running) << "the page decides what a cancel does";
}

TEST_F(ExportPanelTest, RetryButton_EmitsRetryRequested) {
    QWidget host;
    ExportPanel panel(&host);
    panel.showFailed(QStringLiteral("boom"));

    int count = 0;
    QObject::connect(&panel, &ExportPanel::retryRequested, [&]() { ++count; });
    panel.findChild<QPushButton*>(QStringLiteral("exportRetryBtn"))->click();

    EXPECT_EQ(count, 1);
    EXPECT_EQ(panel.state(), ExportPanel::State::Failed);
}

TEST_F(ExportPanelTest, FileActionButtons_EmitTheirRequests) {
    QWidget host;
    ExportPanel panel(&host);
    panel.showDone(QStringLiteral("C:\\Videos\\clip_edit.mkv"));

    int folder = 0;
    int reveal = 0;
    QObject::connect(&panel, &ExportPanel::openFolderRequested, [&]() { ++folder; });
    QObject::connect(&panel, &ExportPanel::revealFileRequested, [&]() { ++reveal; });

    panel.findChild<QPushButton*>(QStringLiteral("exportOpenFolderBtn"))->click();
    panel.findChild<QPushButton*>(QStringLiteral("exportRevealBtn"))->click();

    EXPECT_EQ(folder, 1);
    EXPECT_EQ(reveal, 1);
}

// The panel carries no Export button of its own — the surface's action bar is
// the single trigger, and a second equal-weight one beside it would be a coin
// flip for the user.
TEST_F(ExportPanelTest, CarriesNoExportButtonOfItsOwn) {
    QWidget host;
    ExportPanel panel(&host);
    for (auto* b : panel.findChildren<QPushButton*>()) {
        EXPECT_NE(b->text(), QStringLiteral("Export"));
        EXPECT_NE(b->text(), QString::fromUtf8("Export\xe2\x80\xa6"));
    }
}

// ---- It has to fit the narrow rail ----

// The rail is 240 px at the 860 px minimum window, and its scrollbar takes 15 px
// of that. A combo that demands the width of its widest item would push the
// column past what is left and clip it (the rail scrolls vertically only).
TEST_F(ExportPanelTest, FitsTheNarrowestRailWidthWithoutOverflowing) {
    QWidget host;
    host.resize(240, 700);
    ExportPanel panel(&host);
    panel.resize(225, panel.sizeHint().height());
    host.show();
    for (int i = 0; i < 8; ++i)
        QCoreApplication::processEvents();

    EXPECT_LE(panel.minimumSizeHint().width(), 225) << "panel demands " << panel.minimumSizeHint().width() << " px";
    for (auto* combo : panel.findChildren<QComboBox*>())
        EXPECT_LE(combo->minimumSizeHint().width(), 225) << combo->objectName().toStdString();
}

} // namespace
} // namespace exosnap::ui::widgets
