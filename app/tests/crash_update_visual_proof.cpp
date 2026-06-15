// crash_update_visual_proof.cpp — CRASH-UPDATE-VISUAL-PROOF-R1
//
// Offscreen visual proof for the 0.4.0 crash + update UI surfaces. Mirrors the
// overlay_visual_proof.cpp pattern: builds a QApplication, applies the *real*
// ExoSnap theme (QSS + bundled fonts + palette) so the renders reflect shipping
// styling, constructs the real widgets with realistic sample data, then writes
// PNGs via QWidget rendering for human visual verification.
//
// Produces 6 PNGs under .workspace/screenshots/0.4.0/:
//   01-crash-next-launch.png — CrashReportPanel, honest next-launch state
//                              (no exception/module/thread/stack), not recording.
//   02-crash-recording.png   — same panel with recording_was_active=true
//                              (green "recording secured" banner visible).
//   03-crash-details.png     — next-launch model + populated scrubbed report
//                              fields, with the details section expanded.
//   04-update-available.png  — UpdateSettingsPanel, Available state with What's new.
//   05-update-uptodate.png   — UpdateSettingsPanel, UpToDate state.
//   06-update-error.png      — UpdateSettingsPanel, Error state with caution banner.
//
// All renders are composited onto a neutral mid-gray backdrop (#606060) at a
// devicePixelRatio of 2 so text is crisp when judged at full resolution.
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
#include <QPushButton>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "ui/dialogs/CrashReportPanel.h"
#include "ui/dialogs/UpdateSettingsPanel.h"
#include "ui/theme/ExoSnapTheme.h"

namespace exosnap {
namespace {

using ui::dialogs::CrashReportModel;
using ui::dialogs::CrashReportPanel;
using ui::dialogs::UpdateSettingsPanel;
using ui::dialogs::UpdateUiModel;
using ui::dialogs::UpdateUiState;

// Supersampling factor for crisp text in the saved PNGs.
constexpr qreal kDpr = 2.0;

// ── QApplication fixture ─────────────────────────────────────────────────────

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "crash_update_visual_proof";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class CrashUpdateVisualProofTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        QApplication* app = EnsureApplication();

        // Apply the *real* shipping theme so the renders reflect production
        // styling (QSS from :/theme/exosnap_dark.qss + bundled IBM Plex Mono +
        // Fusion palette). Without this the widgets render unstyled.
        ui::theme::ApplyExoSnapTheme(*app);

        output_dir_ = resolveOutputDir();
        QDir().mkpath(output_dir_);

        std::printf("[crash-update-proof] Output directory: %s\n", output_dir_.toUtf8().constData());
        std::fflush(stdout);
    }

    static QString resolveOutputDir() {
        QDir d(QCoreApplication::applicationDirPath());
        for (int i = 0; i < 12; ++i) {
            if (d.exists(QStringLiteral(".git"))) {
                return d.absolutePath() + QStringLiteral("/.workspace/screenshots/0.4.0");
            }
            if (!d.cdUp())
                break;
        }
        return QCoreApplication::applicationDirPath() + QStringLiteral("/0.4.0");
    }

    // Flush the event loop and, crucially, any pending DeferredDelete events so
    // widgets removed during a panel rebuild() (which uses deleteLater()) are
    // actually destroyed before we render — otherwise stale body widgets ghost
    // under the new layout.
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

    // Render a widget at devicePixelRatio kDpr, composite onto a neutral mid-gray
    // (#606060) backdrop, and save as PNG. Rendering (vs. grab()) lets us drive
    // the device pixel ratio for crisp text.
    static bool renderAndSave(QWidget& widget, const QString& filename) {
        settleLayout(widget);
        const QSize wsize = widget.sizeHint().expandedTo(widget.size()).expandedTo(QSize(1, 1));
        widget.resize(wsize);
        // Show off-screen so a real layout/polish pass runs: persistent widgets that
        // are re-parented + re-shown across a rebuild (e.g. the channel selector
        // buttons) only acquire geometry after an actual show — render() on a
        // never-shown tree leaves them collapsed. Off-screen avoids any flicker.
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

        // Render the widget (and its children) into a DPR-aware pixmap.
        QPixmap shot(wsize * kDpr);
        shot.setDevicePixelRatio(kDpr);
        shot.fill(Qt::transparent);
        widget.render(&shot, QPoint(), QRegion(), QWidget::DrawWindowBackground | QWidget::DrawChildren);

        QPainter painter(&canvas);
        painter.drawPixmap(QPoint(kMargin, kMargin), shot);
        painter.end();

        const QString full_path = output_dir_ + QStringLiteral("/") + filename;
        const bool ok = canvas.save(full_path, "PNG");
        std::printf(ok ? "[crash-update-proof] Saved: %s\n" : "[crash-update-proof] FAILED to save: %s\n",
                    full_path.toUtf8().constData());
        std::fflush(stdout);
        return ok;
    }

    // Base model: the honest next-launch state (no exception/module/thread/stack).
    static CrashReportModel baseModel() {
        CrashReportModel m;
        m.recording_was_active = false;
        m.version = QStringLiteral("0.4.0 \xc2\xb7 build a5d55f1");
        m.os = QStringLiteral("Windows 11 \xc2\xb7 26100.1742");
        m.gpu = QStringLiteral("NVIDIA RTX 4070");
        m.encoder = QStringLiteral("NVENC AV1 \xe2\x86\x92 MKV");
        m.crash_dir = QStringLiteral("C:/Users/test/AppData/Local/ExoSnap/crash");
        m.dmp_path = QStringLiteral("C:/Users/test/AppData/Local/ExoSnap/crash/report.dmp");
        return m;
    }

    static UpdateUiModel availableModel() {
        UpdateUiModel m;
        m.current_version = QStringLiteral("0.4.0");
        m.available_version = QStringLiteral("0.5.0");
        m.last_checked = QStringLiteral("just now");
        m.whats_new = {QStringLiteral("Faster AV1 NVENC frame submission"),
                       QStringLiteral("Fix: rare crash on encoder restart"),
                       QStringLiteral("Per-monitor HDR capture (preview)")};
        m.release_url = QStringLiteral("https://github.com/Exoridus/exosnap/releases");
        m.release_notes_url = QStringLiteral("https://github.com/Exoridus/exosnap/releases/tag/v0.5.0");
        m.channel = QStringLiteral("Stable");
        return m;
    }

    static QString output_dir_;
};

QString CrashUpdateVisualProofTest::output_dir_;

// ── Crash panel proofs ───────────────────────────────────────────────────────

TEST_F(CrashUpdateVisualProofTest, Crash_NextLaunch) {
    CrashReportPanel panel(baseModel(), nullptr);
    const bool saved = renderAndSave(panel, QStringLiteral("01-crash-next-launch.png"));
    EXPECT_TRUE(saved);
}

TEST_F(CrashUpdateVisualProofTest, Crash_RecordingSecured) {
    CrashReportModel m = baseModel();
    m.recording_was_active = true;
    CrashReportPanel panel(m, nullptr);
    const bool saved = renderAndSave(panel, QStringLiteral("02-crash-recording.png"));
    EXPECT_TRUE(saved);
}

TEST_F(CrashUpdateVisualProofTest, Crash_DetailsExpanded) {
    // Next-launch chrome but with the scrubbed report populated so the expanded
    // section shows real content.
    CrashReportModel m = baseModel();
    m.exception = QStringLiteral("0xC0000005 \xc2\xb7 ACCESS_VIOLATION");
    m.module = QStringLiteral("exosnap.dll +0x3f1a2");
    m.thread = QStringLiteral("\"encoder\" (#7)");
    m.stack = {QStringLiteral("exo::EncoderNVENC::submitFrame()"), QStringLiteral("exo::Pipeline::onFrameReady()"),
               QStringLiteral("exo::CaptureLoop::tick()")};

    CrashReportPanel panel(m, nullptr);

    // Expand the scrubbed report via the details toggle button.
    auto* toggle = panel.findChild<QPushButton*>(QStringLiteral("crashDetailsToggle"));
    ASSERT_NE(toggle, nullptr) << "crashDetailsToggle not found";
    toggle->click();
    QCoreApplication::processEvents();

    const bool saved = renderAndSave(panel, QStringLiteral("03-crash-details.png"));
    EXPECT_TRUE(saved);
}

// ── Update panel proofs ──────────────────────────────────────────────────────

TEST_F(CrashUpdateVisualProofTest, Update_Available) {
    UpdateSettingsPanel panel;
    panel.setModel(availableModel());
    panel.setState(UpdateUiState::Available);
    QCoreApplication::processEvents();

    const bool saved = renderAndSave(panel, QStringLiteral("04-update-available.png"));
    EXPECT_TRUE(saved);
}

TEST_F(CrashUpdateVisualProofTest, Update_UpToDate) {
    UpdateUiModel m;
    m.current_version = QStringLiteral("0.4.0");
    m.last_checked = QStringLiteral("just now");
    m.channel = QStringLiteral("Stable");

    UpdateSettingsPanel panel;
    panel.setModel(m);
    panel.setState(UpdateUiState::UpToDate);
    QCoreApplication::processEvents();

    const bool saved = renderAndSave(panel, QStringLiteral("05-update-uptodate.png"));
    EXPECT_TRUE(saved);
}

TEST_F(CrashUpdateVisualProofTest, Update_Error) {
    UpdateUiModel m;
    m.current_version = QStringLiteral("0.4.0");
    m.last_checked = QStringLiteral("just now");
    m.channel = QStringLiteral("Stable");
    m.error_message = QStringLiteral("Couldn't reach the update server. Check your connection and try again.");

    UpdateSettingsPanel panel;
    panel.setModel(m);
    panel.setState(UpdateUiState::Error);
    QCoreApplication::processEvents();

    const bool saved = renderAndSave(panel, QStringLiteral("06-update-error.png"));
    EXPECT_TRUE(saved);
}

// ── Output directory confirmation ────────────────────────────────────────────

TEST_F(CrashUpdateVisualProofTest, OutputDirectoryExists) {
    const QDir dir(output_dir_);
    EXPECT_TRUE(dir.exists()) << "Output directory does not exist: " << output_dir_.toStdString();
    std::printf("[crash-update-proof] Proof PNGs written to:\n  %s\n",
                QDir::toNativeSeparators(output_dir_).toUtf8().constData());
    std::fflush(stdout);
}

} // namespace
} // namespace exosnap
