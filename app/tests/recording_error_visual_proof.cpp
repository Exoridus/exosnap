// recording_error_visual_proof.cpp — RECORDING-ERROR-MODAL-R1
//
// Offscreen visual proof for the non-fatal recording-error dialog. Mirrors
// crash_update_visual_proof.cpp: builds a QApplication, applies the *real*
// ExoSnap theme (QSS + bundled fonts + palette) so the renders reflect shipping
// styling, constructs the real RecordingErrorPanel with realistic sample data,
// then writes PNGs via QWidget rendering for human visual verification.
//
// Produces PNGs under .workspace/screenshots/0.7.0/:
//   01-recording-error-start.png  — "could not start" failure, self-build
//                                    (no Send action — can_send_report=false).
//   02-recording-error-official.png — same failure in an official build
//                                    (Send report action + privacy note visible).
//   03-recording-error-runtime.png — mid-session interruption ("stopped
//                                    unexpectedly", partial-file summary).
//
// The test always passes as long as the PNGs are non-empty; its real product is
// the PNG files for human visual verification.

#include <gtest/gtest.h>

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QEventLoop>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QSize>
#include <QString>
#include <QWidget>

#include "ui/dialogs/RecordingErrorPanel.h"
#include "ui/theme/ExoSnapTheme.h"

namespace exosnap {
namespace {

using ui::dialogs::RecordingErrorModel;
using ui::dialogs::RecordingErrorPanel;

constexpr qreal kDpr = 2.0;

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "recording_error_visual_proof";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class RecordingErrorVisualProofTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        QApplication* app = EnsureApplication();
        // Apply the *real* shipping theme so renders reflect production styling.
        ui::theme::ApplyExoSnapTheme(*app);

        output_dir_ = resolveOutputDir();
        QDir().mkpath(output_dir_);
        std::printf("[recording-error-proof] Output directory: %s\n", output_dir_.toUtf8().constData());
        std::fflush(stdout);
    }

    static QString resolveOutputDir() {
        QDir d(QCoreApplication::applicationDirPath());
        for (int i = 0; i < 12; ++i) {
            if (d.exists(QStringLiteral(".git")))
                return d.absolutePath() + QStringLiteral("/.workspace/screenshots/0.7.0");
            if (!d.cdUp())
                break;
        }
        return QCoreApplication::applicationDirPath() + QStringLiteral("/0.7.0");
    }

    static void settleLayout(QWidget& widget) {
        for (int i = 0; i < 3; ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            widget.ensurePolished();
            widget.updateGeometry();
            widget.adjustSize();
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents);
    }

    static bool renderAndSave(QWidget& widget, const QString& filename) {
        settleLayout(widget);
        const QSize wsize = widget.sizeHint().expandedTo(widget.size()).expandedTo(QSize(1, 1));
        widget.resize(wsize);
        widget.move(-20000, -20000);
        widget.show();
        settleLayout(widget);
        widget.resize(wsize);
        settleLayout(widget);

        constexpr int kMargin = 20;
        const QSize canvas_logical(wsize.width() + kMargin * 2, wsize.height() + kMargin * 2);

        QImage canvas(canvas_logical * kDpr, QImage::Format_ARGB32_Premultiplied);
        canvas.setDevicePixelRatio(kDpr);
        canvas.fill(QColor(0x60, 0x60, 0x60));

        QPixmap shot(wsize * kDpr);
        shot.setDevicePixelRatio(kDpr);
        shot.fill(Qt::transparent);
        widget.render(&shot, QPoint(), QRegion(), QWidget::DrawWindowBackground | QWidget::DrawChildren);

        QPainter painter(&canvas);
        painter.drawPixmap(QPoint(kMargin, kMargin), shot);
        painter.end();

        const QString full_path = output_dir_ + QStringLiteral("/") + filename;
        const bool ok = canvas.save(full_path, "PNG");
        std::printf(ok ? "[recording-error-proof] Saved: %s\n" : "[recording-error-proof] FAILED to save: %s\n",
                    full_path.toUtf8().constData());
        std::fflush(stdout);
        return ok;
    }

    // Start-failure model (validation rejected the config before any frame).
    static RecordingErrorModel startModel() {
        RecordingErrorModel m;
        m.title = QStringLiteral("Recording could not start");
        m.summary = QStringLiteral("ExoSnap couldn't start this recording. The details below may help identify why.");
        m.phase = QStringLiteral("Validate");
        m.code = QStringLiteral("0x80004001");
        m.detail = QStringLiteral("Container::Matroska requires VideoCodec::Av1Nvenc, VideoCodec::H264Nvenc, "
                                  "or VideoCodec::HevcNvenc");
        m.container = QStringLiteral("MKV");
        m.video_codec = QStringLiteral("HEVC");
        m.audio_codec = QStringLiteral("Opus");
        m.can_send_report = false;
        return m;
    }

    static QString output_dir_;
};

QString RecordingErrorVisualProofTest::output_dir_;

TEST_F(RecordingErrorVisualProofTest, StartFailure_SelfBuild) {
    RecordingErrorPanel panel(startModel(), nullptr);
    EXPECT_TRUE(renderAndSave(panel, QStringLiteral("01-recording-error-start.png")));
}

TEST_F(RecordingErrorVisualProofTest, StartFailure_OfficialBuild) {
    RecordingErrorModel m = startModel();
    m.can_send_report = true; // official build with DSN: Send action + privacy note
    RecordingErrorPanel panel(m, nullptr);
    EXPECT_TRUE(renderAndSave(panel, QStringLiteral("02-recording-error-official.png")));
}

TEST_F(RecordingErrorVisualProofTest, RuntimeFailure_PartialFile) {
    RecordingErrorModel m;
    m.title = QStringLiteral("Recording stopped unexpectedly");
    m.summary = QStringLiteral("The recording was interrupted before it finished. A partial file may have been "
                               "saved to your output folder.");
    m.phase = QStringLiteral("Mux");
    m.code = QStringLiteral("0x80004005");
    m.detail = QStringLiteral("Failed to build hvcC from HEVC VPS/SPS/PPS for Matroska");
    m.container = QStringLiteral("MKV");
    m.video_codec = QStringLiteral("HEVC");
    m.audio_codec = QStringLiteral("Opus");
    m.can_send_report = true;
    RecordingErrorPanel panel(m, nullptr);
    EXPECT_TRUE(renderAndSave(panel, QStringLiteral("03-recording-error-runtime.png")));
}

TEST_F(RecordingErrorVisualProofTest, OutputDirectoryExists) {
    const QDir dir(output_dir_);
    EXPECT_TRUE(dir.exists()) << "Output directory does not exist: " << output_dir_.toStdString();
}

} // namespace
} // namespace exosnap
