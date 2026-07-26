#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QWidget>

#include "ui/dialogs/RecordingErrorOverlay.h"
#include "ui/dialogs/RecordingErrorPanel.h"

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "recording_error_overlay_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

ui::dialogs::RecordingErrorModel SampleModel(bool can_send = false) {
    ui::dialogs::RecordingErrorModel model;
    model.title = QStringLiteral("Recording could not start");
    model.summary = QStringLiteral("ExoSnap couldn't start this recording.");
    model.phase = QStringLiteral("Validate");
    model.code = QStringLiteral("0x80004001");
    model.detail = QStringLiteral("Container::Matroska requires VideoCodec::Av1");
    model.container = QStringLiteral("MKV");
    model.video_codec = QStringLiteral("HEVC");
    model.audio_codec = QStringLiteral("Opus");
    model.can_send_report = can_send;
    return model;
}

// Returns true if any QLabel in the tree contains the needle (for copy checks).
bool ContainsLabel(const QWidget& root, const QString& needle) {
    for (auto* label : root.findChildren<QLabel*>())
        if (label->text().contains(needle))
            return true;
    return false;
}

class RecordingErrorTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(RecordingErrorTest, PanelRendersInWindowNotAsNativeDialog) {
    ui::dialogs::RecordingErrorPanel panel(SampleModel());
    // The error surface must be a plain QWidget, never a separate native QDialog.
    EXPECT_EQ(qobject_cast<QDialog*>(&panel), nullptr);
    EXPECT_EQ(panel.objectName(), QStringLiteral("recordingErrorCard"));
}

TEST_F(RecordingErrorTest, OverlayEmbedsCardInWindow) {
    ui::dialogs::RecordingErrorOverlay overlay(SampleModel());
    EXPECT_EQ(qobject_cast<QDialog*>(&overlay), nullptr);
    EXPECT_NE(overlay.findChild<QWidget*>(QStringLiteral("recordingErrorCard")), nullptr);
}

TEST_F(RecordingErrorTest, RendersTitlePhaseCodeDetailAndFormat) {
    ui::dialogs::RecordingErrorPanel panel(SampleModel());
    EXPECT_TRUE(ContainsLabel(panel, QStringLiteral("Recording could not start")));
    EXPECT_TRUE(ContainsLabel(panel, QStringLiteral("Validate")));
    EXPECT_TRUE(ContainsLabel(panel, QStringLiteral("0x80004001")));
    // The DETAIL line humanizes raw engine enum tokens to canonical labels:
    // "Container::Matroska requires VideoCodec::Av1" -> "MKV requires AV1".
    EXPECT_TRUE(ContainsLabel(panel, QStringLiteral("MKV requires AV1")));
    EXPECT_FALSE(ContainsLabel(panel, QStringLiteral("VideoCodec::")));
    // Codec triple collapses into one FORMAT line.
    EXPECT_TRUE(ContainsLabel(panel, QStringLiteral("MKV \xc2\xb7 HEVC \xc2\xb7 Opus")));
}

TEST_F(RecordingErrorTest, SendButtonHiddenWhenCannotSend) {
    ui::dialogs::RecordingErrorPanel panel(SampleModel(/*can_send=*/false));
    EXPECT_FALSE(panel.canSendReport());
    EXPECT_EQ(panel.findChild<QPushButton*>(QStringLiteral("recordingErrorSendButton")), nullptr);
    // The privacy note only accompanies the send action.
    EXPECT_FALSE(ContainsLabel(panel, QStringLiteral("never file paths")));
}

TEST_F(RecordingErrorTest, SendButtonPresentWhenCanSend) {
    ui::dialogs::RecordingErrorPanel panel(SampleModel(/*can_send=*/true));
    EXPECT_TRUE(panel.canSendReport());
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("recordingErrorSendButton"));
    ASSERT_NE(send, nullptr);
    EXPECT_TRUE(ContainsLabel(panel, QStringLiteral("never file paths")));

    int count = 0;
    QObject::connect(&panel, &ui::dialogs::RecordingErrorPanel::sendReportRequested, &panel, [&count]() { ++count; });
    send->click();
    EXPECT_EQ(count, 1);
}

TEST_F(RecordingErrorTest, LogsButtonEmitsOpenLogsRequested) {
    ui::dialogs::RecordingErrorPanel panel(SampleModel());
    auto* logs = panel.findChild<QPushButton*>(QStringLiteral("recordingErrorLogsButton"));
    ASSERT_NE(logs, nullptr);

    int count = 0;
    QObject::connect(&panel, &ui::dialogs::RecordingErrorPanel::openLogsRequested, &panel, [&count]() { ++count; });
    logs->click();
    EXPECT_EQ(count, 1);
}

TEST_F(RecordingErrorTest, CloseButtonEmitsDismissRequested) {
    ui::dialogs::RecordingErrorPanel panel(SampleModel());
    auto* close = panel.findChild<QPushButton*>(QStringLiteral("recordingErrorCloseButton"));
    ASSERT_NE(close, nullptr);

    int count = 0;
    QObject::connect(&panel, &ui::dialogs::RecordingErrorPanel::dismissRequested, &panel, [&count]() { ++count; });
    close->click();
    EXPECT_EQ(count, 1);
}

TEST_F(RecordingErrorTest, OverlayForwardsSendReportSignal) {
    ui::dialogs::RecordingErrorOverlay overlay(SampleModel(/*can_send=*/true));
    auto* send = overlay.findChild<QPushButton*>(QStringLiteral("recordingErrorSendButton"));
    ASSERT_NE(send, nullptr);

    int count = 0;
    QObject::connect(&overlay, &ui::dialogs::RecordingErrorOverlay::sendReportRequested, &overlay,
                     [&count]() { ++count; });
    send->click();
    EXPECT_EQ(count, 1);
}

TEST_F(RecordingErrorTest, OverlayOpenThenCloseTogglesOpenState) {
    QWidget host;
    auto* overlay = new ui::dialogs::RecordingErrorOverlay(SampleModel(), &host);

    EXPECT_FALSE(overlay->isOpen());
    overlay->openOverlay();
    EXPECT_TRUE(overlay->isOpen());
    overlay->closeOverlay();
    EXPECT_FALSE(overlay->isOpen());
}

TEST_F(RecordingErrorTest, OverlayCloseButtonDismissesAndEmitsClosed) {
    QWidget host;
    auto* overlay = new ui::dialogs::RecordingErrorOverlay(SampleModel(), &host);
    overlay->openOverlay();
    ASSERT_TRUE(overlay->isOpen());

    int closed_count = 0;
    QObject::connect(overlay, &ui::dialogs::RecordingErrorOverlay::closed, overlay,
                     [&closed_count]() { ++closed_count; });

    auto* close = overlay->findChild<QPushButton*>(QStringLiteral("recordingErrorCloseButton"));
    ASSERT_NE(close, nullptr);
    close->click();

    EXPECT_FALSE(overlay->isOpen());
    EXPECT_EQ(closed_count, 1);
}

// ---------------------------------------------------------------------------
// The detail box must not clip its rows.
//
// A word-wrapped label reports a single-line minimum height. Embedded in the
// overlay, the detail frame collapsed to that minimum and the PHASE / FORMAT /
// DETAIL rows overlapped each other -- unreadable at exactly the moment the user
// needs to read why a recording failed. The standalone panel rendered fine, so
// the regression only shows through the real embedding.
// ---------------------------------------------------------------------------
TEST(RecordingErrorOverlayLayout, DetailBoxIsTallEnoughForItsRows) {
    EnsureApplication();

    ui::dialogs::RecordingErrorModel model = SampleModel(/*can_send=*/true);
    model.phase = QStringLiteral("Prepare");
    model.code = QString(); // the live failure carries no HRESULT text
    model.detail = QStringLiteral("Window target PID unavailable; the selected window may have been closed.");
    model.container = QStringLiteral("MKV");
    model.video_codec = QStringLiteral("AV1");
    model.audio_codec = QStringLiteral("Opus");

    QWidget host;
    host.resize(1310, 663);
    auto* overlay = new ui::dialogs::RecordingErrorOverlay(model, &host);
    overlay->setGeometry(host.rect());
    overlay->show();
    host.show();
    QCoreApplication::processEvents();

    auto* detail = host.findChild<QFrame*>(QStringLiteral("recordingErrorDetail"));
    ASSERT_NE(detail, nullptr);

    // Every row must be given at least the height it needs at its actual width.
    int needed = 0;
    int rows = 0;
    for (QObject* child : detail->children()) {
        auto* row = qobject_cast<QWidget*>(child);
        if (row == nullptr)
            continue;
        ++rows;
        const int want = row->hasHeightForWidth() ? row->heightForWidth(row->width()) : row->sizeHint().height();
        EXPECT_GE(row->height(), want) << "detail row " << rows << " is clipped";
        needed += want;
    }
    ASSERT_GE(rows, 3) << "expected PHASE, FORMAT and DETAIL rows";
    EXPECT_GE(detail->height(), needed) << "the detail box is shorter than the rows it contains";
}

} // namespace
} // namespace exosnap
