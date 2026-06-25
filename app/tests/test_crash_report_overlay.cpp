#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QFrame>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QWidget>

#include "ui/dialogs/CrashReportOverlay.h"
#include "ui/dialogs/CrashReportPanel.h"
#include "ui/widgets/ExoCheckBox.h"

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "crash_report_overlay_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

ui::dialogs::CrashReportModel SampleModel(bool recording = false) {
    ui::dialogs::CrashReportModel model;
    model.recording_was_active = recording;
    model.exception = QStringLiteral("0xC0000005 \xc2\xb7 ACCESS_VIOLATION");
    model.module = QStringLiteral("exosnap.dll +0x3f1a2");
    model.thread = QStringLiteral("\"encoder\" (#7)");
    model.stack = {QStringLiteral("exo::EncoderNVENC::submitFrame()"), QStringLiteral("exo::Pipeline::onFrameReady()"),
                   QStringLiteral("exo::CaptureLoop::tick()")};
    model.version = QStringLiteral("1.0.4 \xc2\xb7 build a5d55f1");
    model.os = QStringLiteral("Windows 11 \xc2\xb7 26100.1742");
    model.gpu = QStringLiteral("NVIDIA RTX 4070 \xc2\xb7 driver 552.44");
    model.encoder = QStringLiteral("NVENC AV1 \xe2\x86\x92 MKV");
    model.crash_dir = QStringLiteral("crash-dir");
    model.dmp_path = QStringLiteral("crash-dir/report.dmp");
    return model;
}

// Returns the union of every label text inside a widget tree (for copy assertions).
bool ContainsLabel(const QWidget& root, const QString& needle) {
    for (auto* label : root.findChildren<QLabel*>())
        if (label->text().contains(needle))
            return true;
    return false;
}

class CrashReportTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(CrashReportTest, PanelRendersInWindowNotAsNativeDialog) {
    ui::dialogs::CrashReportPanel panel(SampleModel());
    // The crash surface must be a plain QWidget, never a separate native QDialog.
    EXPECT_EQ(qobject_cast<QDialog*>(&panel), nullptr);
    EXPECT_EQ(panel.objectName(), QStringLiteral("crashReportCard"));
}

TEST_F(CrashReportTest, OverlayRendersInWindowNotAsNativeDialog) {
    ui::dialogs::CrashReportOverlay overlay(SampleModel());
    EXPECT_EQ(qobject_cast<QDialog*>(&overlay), nullptr);
    // The card lives inside the overlay.
    EXPECT_NE(overlay.findChild<QWidget*>(QStringLiteral("crashReportCard")), nullptr);
}

TEST_F(CrashReportTest, AutoSendOptInDefaultsUnchecked) {
    ui::dialogs::CrashReportPanel panel(SampleModel());
    auto* check = panel.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("crashAutoSendCheck"));
    ASSERT_NE(check, nullptr);
    EXPECT_FALSE(check->isChecked());
    EXPECT_FALSE(panel.autoSendChecked());
}

TEST_F(CrashReportTest, DetailsToggleShowsAndHidesScrubbedReport) {
    ui::dialogs::CrashReportPanel panel(SampleModel());
    auto* toggle = panel.findChild<QPushButton*>(QStringLiteral("crashDetailsToggle"));
    auto* report = panel.findChild<QFrame*>(QStringLiteral("crashScrubbedReport"));
    ASSERT_NE(toggle, nullptr);
    ASSERT_NE(report, nullptr);

    // Collapsed by default.
    EXPECT_FALSE(report->isVisible());
    EXPECT_EQ(toggle->text(), QStringLiteral("Show report details"));

    toggle->click();
    EXPECT_TRUE(report->isVisibleTo(&panel));
    EXPECT_EQ(toggle->text(), QStringLiteral("Hide report details"));

    toggle->click();
    EXPECT_FALSE(report->isVisibleTo(&panel));
    EXPECT_EQ(toggle->text(), QStringLiteral("Show report details"));
}

TEST_F(CrashReportTest, SendButtonEmitsSendReportRequested) {
    ui::dialogs::CrashReportPanel panel(SampleModel());
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("crashSendButton"));
    ASSERT_NE(send, nullptr);

    int count = 0;
    QObject::connect(&panel, &ui::dialogs::CrashReportPanel::sendReportRequested, &panel, [&count]() { ++count; });
    send->click();
    EXPECT_EQ(count, 1);
}

TEST_F(CrashReportTest, RestartButtonEmitsRestartRequested) {
    ui::dialogs::CrashReportPanel panel(SampleModel());
    auto* restart = panel.findChild<QPushButton*>(QStringLiteral("crashRestartButton"));
    ASSERT_NE(restart, nullptr);

    int count = 0;
    QObject::connect(&panel, &ui::dialogs::CrashReportPanel::restartRequested, &panel, [&count]() { ++count; });
    restart->click();
    EXPECT_EQ(count, 1);
}

TEST_F(CrashReportTest, AutoSendToggleEmitsToggledTrue) {
    ui::dialogs::CrashReportPanel panel(SampleModel());
    auto* check = panel.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("crashAutoSendCheck"));
    ASSERT_NE(check, nullptr);

    int count = 0;
    bool last = false;
    QObject::connect(&panel, &ui::dialogs::CrashReportPanel::autoSendToggled, &panel, [&](bool checked) {
        ++count;
        last = checked;
    });
    check->setChecked(true);
    EXPECT_EQ(count, 1);
    EXPECT_TRUE(last);
    EXPECT_TRUE(panel.autoSendChecked());
}

TEST_F(CrashReportTest, ChromeCloseEmitsDontSendRequested) {
    ui::dialogs::CrashReportPanel panel(SampleModel());
    auto* close = panel.findChild<QPushButton*>(QStringLiteral("crashChromeCloseButton"));
    ASSERT_NE(close, nullptr);

    int count = 0;
    QObject::connect(&panel, &ui::dialogs::CrashReportPanel::dontSendRequested, &panel, [&count]() { ++count; });
    close->click();
    EXPECT_EQ(count, 1);
}

TEST_F(CrashReportTest, RecordingBannerPresentOnlyWhenRecordingWasActive) {
    ui::dialogs::CrashReportPanel without(SampleModel(false));
    EXPECT_EQ(without.findChild<QFrame*>(QStringLiteral("crashRecordingBanner")), nullptr);

    ui::dialogs::CrashReportPanel with(SampleModel(true));
    auto* banner = with.findChild<QFrame*>(QStringLiteral("crashRecordingBanner"));
    ASSERT_NE(banner, nullptr);
    EXPECT_TRUE(ContainsLabel(with, QStringLiteral("recording was secured")));
}

TEST_F(CrashReportTest, TransparencyColumnsHeadingsPresent) {
    ui::dialogs::CrashReportPanel panel(SampleModel());
    EXPECT_TRUE(ContainsLabel(panel, QStringLiteral("What gets sent")));
    EXPECT_TRUE(ContainsLabel(panel, QStringLiteral("Never sent")));
    // Spot-check one sent and one never-sent item from the FINAL copy.
    EXPECT_TRUE(ContainsLabel(panel, QStringLiteral("Stack trace + exception code")));
    EXPECT_TRUE(ContainsLabel(panel, QStringLiteral("File paths & filenames")));
}

TEST_F(CrashReportTest, ScrubbedReportRendersModelFields) {
    ui::dialogs::CrashReportPanel panel(SampleModel());
    auto* toggle = panel.findChild<QPushButton*>(QStringLiteral("crashDetailsToggle"));
    ASSERT_NE(toggle, nullptr);
    toggle->click();

    EXPECT_TRUE(ContainsLabel(panel, QStringLiteral("0xC0000005")));
    EXPECT_TRUE(ContainsLabel(panel, QStringLiteral("exosnap.dll +0x3f1a2")));
    EXPECT_TRUE(ContainsLabel(panel, QStringLiteral("submitFrame")));
}

TEST_F(CrashReportTest, OverflowMenuHasFallbackActions) {
    ui::dialogs::CrashReportPanel panel(SampleModel());
    auto* overflow = panel.findChild<QPushButton*>(QStringLiteral("crashOverflowButton"));
    ASSERT_NE(overflow, nullptr);
    ASSERT_NE(overflow->menu(), nullptr);

    QStringList action_texts;
    for (auto* action : overflow->menu()->actions())
        if (!action->isSeparator())
            action_texts << action->text();
    EXPECT_TRUE(action_texts.contains(QStringLiteral("Report on GitHub")));
    EXPECT_TRUE(action_texts.contains(QStringLiteral("Open crash folder")));
    EXPECT_TRUE(action_texts.contains(QStringLiteral("Don't send & close")));
}

TEST_F(CrashReportTest, OverlayForwardsSendReportSignal) {
    ui::dialogs::CrashReportOverlay overlay(SampleModel());
    auto* send = overlay.findChild<QPushButton*>(QStringLiteral("crashSendButton"));
    ASSERT_NE(send, nullptr);

    int count = 0;
    QObject::connect(&overlay, &ui::dialogs::CrashReportOverlay::sendReportRequested, &overlay,
                     [&count]() { ++count; });
    send->click();
    EXPECT_EQ(count, 1);
}

TEST_F(CrashReportTest, OverlayOpenThenCloseTogglesOpenState) {
    QWidget host;
    auto* overlay = new ui::dialogs::CrashReportOverlay(SampleModel(), &host);

    EXPECT_FALSE(overlay->isOpen());
    overlay->openOverlay();
    EXPECT_TRUE(overlay->isOpen());
    overlay->closeOverlay();
    EXPECT_FALSE(overlay->isOpen());
}

TEST_F(CrashReportTest, OverlayDeclineDismissesAndEmitsClosedAndDontSend) {
    QWidget host;
    auto* overlay = new ui::dialogs::CrashReportOverlay(SampleModel(), &host);
    overlay->openOverlay();
    ASSERT_TRUE(overlay->isOpen());

    int closed_count = 0;
    int decline_count = 0;
    QObject::connect(overlay, &ui::dialogs::CrashReportOverlay::closed, overlay, [&closed_count]() { ++closed_count; });
    QObject::connect(overlay, &ui::dialogs::CrashReportOverlay::dontSendRequested, overlay,
                     [&decline_count]() { ++decline_count; });

    auto* close = overlay->findChild<QPushButton*>(QStringLiteral("crashChromeCloseButton"));
    ASSERT_NE(close, nullptr);
    close->click();

    EXPECT_FALSE(overlay->isOpen());
    EXPECT_EQ(closed_count, 1);
    EXPECT_EQ(decline_count, 1);
}

} // namespace
} // namespace exosnap
