// overlay_visual_proof.cpp — OVERLAY-SKIN-AND-PROOF-R1 + COUNTDOWN-OVERLAY-R1
//
// Offscreen visual proof for capture-excluded overlay windows.
// These windows use SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) and cannot
// be screenshotted by normal means. QWidget::grab() renders regardless of
// capture exclusion, so it is the correct verification path.
//
// Produces 7 PNGs under .workspace/screenshots/overlay-proof/:
//   toast-success.png      — NotificationToastWindow, Saved type
//   toast-caution.png      — NotificationToastWindow, LowStorage type
//   toast-error.png        — NotificationToastWindow, UnexpectedStop type
//   toast-info.png         — NotificationToastWindow, RecoveryAvailable type
//   status-pill.png        — RecordingOverlayWindow, REC state with timecode
//   diagnostics-pill.png   — DiagnosticsOverlayWindow, sample metrics + muted glyph
//   countdown-overlay.png  — CountdownOverlayWindow, digit "3", ~mid-depletion (3/5)
//
// All renders are composited onto a neutral mid-gray backdrop (#606060) so the
// glassy dark pills read clearly against a non-black background.
//
// The test always passes as long as the PNGs are non-empty; its real product is
// the PNG files for human visual verification.

#include <gtest/gtest.h>

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QSize>
#include <QString>
#include <QWidget>

#include "notifications/NotificationEvent.h"
#include "notifications/NotificationManager.h"
#include "ui/overlay/CountdownOverlayWindow.h"
#include "ui/overlay/DiagnosticsOverlayWindow.h"
#include "ui/overlay/NotificationToastWindow.h"
#include "ui/overlay/RecordingOverlayWindow.h"

namespace exosnap {
namespace {

using notifications::NotificationAction;
using notifications::NotificationEvent;
using notifications::NotificationManager;
using notifications::NotificationType;
using ui::overlay::CountdownOverlayWindow;
using ui::overlay::DiagnosticsOverlayWindow;
using ui::overlay::NotificationToastWindow;
using ui::overlay::RecordingOverlayWindow;

// ── QApplication fixture ─────────────────────────────────────────────────────

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "overlay_visual_proof";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class OverlayVisualProofTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();

        // Determine the output directory: <repo>/.workspace/screenshots/overlay-proof/
        // We anchor via the applicationDirPath which, in the build tree, is the
        // binary output directory. Walk up to the repo root by looking for .git.
        output_dir_ = resolveOutputDir();
        QDir().mkpath(output_dir_);

        std::printf("[overlay-proof] Output directory: %s\n", output_dir_.toUtf8().constData());
        std::fflush(stdout);
    }

    static QString resolveOutputDir() {
        // Try to find the repo root by walking up from applicationDirPath.
        QDir d(QCoreApplication::applicationDirPath());
        for (int i = 0; i < 12; ++i) {
            if (d.exists(QStringLiteral(".git"))) {
                return d.absolutePath() + QStringLiteral("/.workspace/screenshots/overlay-proof");
            }
            if (!d.cdUp())
                break;
        }
        // Fallback: use a subdirectory next to the binary.
        return QCoreApplication::applicationDirPath() + QStringLiteral("/overlay-proof");
    }

    // Composite a widget onto a neutral mid-gray (#606060) backdrop and save as PNG.
    // The widget is grabbed offscreen — no window handle or capture exclusion needed.
    static bool grabAndSave(QWidget& widget, const QString& filename) {
        widget.adjustSize();
        const QSize wsize = widget.sizeHint().expandedTo(QSize(1, 1));
        widget.resize(wsize);

        // Backdrop: 20px margin on all sides, mid-gray background
        constexpr int kMargin = 20;
        const QSize canvas_size(wsize.width() + kMargin * 2, wsize.height() + kMargin * 2);
        QImage canvas(canvas_size, QImage::Format_ARGB32_Premultiplied);
        canvas.fill(QColor(0x60, 0x60, 0x60));

        // Grab the widget (WDA_EXCLUDEFROMCAPTURE has no effect on grab())
        const QPixmap grab = widget.grab(QRect(QPoint(0, 0), wsize));

        // Composite onto canvas
        QPainter painter(&canvas);
        painter.drawPixmap(kMargin, kMargin, grab);
        painter.end();

        const QString full_path = output_dir_ + QStringLiteral("/") + filename;
        const bool ok = canvas.save(full_path, "PNG");
        if (ok) {
            std::printf("[overlay-proof] Saved: %s\n", full_path.toUtf8().constData());
        } else {
            std::printf("[overlay-proof] FAILED to save: %s\n", full_path.toUtf8().constData());
        }
        std::fflush(stdout);
        return ok;
    }

    static QString output_dir_;
};

QString OverlayVisualProofTest::output_dir_;

// ── Toast proofs ─────────────────────────────────────────────────────────────

TEST_F(OverlayVisualProofTest, Toast_Success) {
    NotificationManager mgr;
    NotificationEvent e;
    e.type = NotificationType::Saved;
    e.title = QStringLiteral("Recording saved");
    e.body = QStringLiteral("exosnap_2026-06-11_02.mkv \xC2\xB7 179 MB");
    e.action = NotificationAction::OpenFolder;
    mgr.Enqueue(e);

    NotificationToastWindow toast(&mgr, nullptr);
    // Force a size so grab() can render it even without a real window handle.
    toast.resize(toast.sizeHint());
    toast.show();
    QCoreApplication::processEvents();

    const QPixmap grab = toast.grab(QRect(QPoint(0, 0), toast.size()));
    ASSERT_FALSE(grab.isNull()) << "grab returned null pixmap";
    ASSERT_GT(grab.width(), 0);
    ASSERT_GT(grab.height(), 0);

    const bool saved = grabAndSave(toast, QStringLiteral("toast-success.png"));
    EXPECT_TRUE(saved) << "Failed to save toast-success.png";
    toast.hide();
}

TEST_F(OverlayVisualProofTest, Toast_Caution) {
    NotificationManager mgr;
    NotificationEvent e;
    e.type = NotificationType::LowStorage;
    e.title = QStringLiteral("Storage running low");
    e.body = QStringLiteral("1.2 GB free on C:. Recording may stop soon.");
    e.action = NotificationAction::ChangeFolder;
    e.secondary_action = NotificationAction::None;
    mgr.Enqueue(e);

    NotificationToastWindow toast(&mgr, nullptr);
    toast.resize(toast.sizeHint());
    toast.show();
    QCoreApplication::processEvents();

    const bool saved = grabAndSave(toast, QStringLiteral("toast-caution.png"));
    EXPECT_TRUE(saved) << "Failed to save toast-caution.png";
    toast.hide();
}

TEST_F(OverlayVisualProofTest, Toast_Error) {
    NotificationManager mgr;
    NotificationEvent e;
    e.type = NotificationType::UnexpectedStop;
    e.title = QStringLiteral("Recording stopped unexpectedly");
    e.body = QStringLiteral("Disk write failed at 04:12. A partial file was recovered.");
    e.action = NotificationAction::ShowFile;
    mgr.Enqueue(e);

    NotificationToastWindow toast(&mgr, nullptr);
    toast.resize(toast.sizeHint());
    toast.show();
    QCoreApplication::processEvents();

    const bool saved = grabAndSave(toast, QStringLiteral("toast-error.png"));
    EXPECT_TRUE(saved) << "Failed to save toast-error.png";
    toast.hide();
}

TEST_F(OverlayVisualProofTest, Toast_Info) {
    NotificationManager mgr;
    NotificationEvent e;
    e.type = NotificationType::RecoveryAvailable;
    e.title = QStringLiteral("Recover last session?");
    e.body = QStringLiteral("A recording from 14:02 wasn\xE2\x80\x99t finalized.");
    e.action = NotificationAction::OpenRecovery;
    e.secondary_action = NotificationAction::Discard;
    mgr.Enqueue(e);

    NotificationToastWindow toast(&mgr, nullptr);
    toast.resize(toast.sizeHint());
    toast.show();
    QCoreApplication::processEvents();

    const bool saved = grabAndSave(toast, QStringLiteral("toast-info.png"));
    EXPECT_TRUE(saved) << "Failed to save toast-info.png";
    toast.hide();
}

// ── Status pill proof ─────────────────────────────────────────────────────────

TEST_F(OverlayVisualProofTest, StatusPill_RecordingState) {
    RecordingOverlayWindow pill;
    // Bypass WDA_EXCLUDEFROMCAPTURE exclusion guard for grab:
    // We call updateElapsed directly (no show) and grab the painted output.
    pill.updateElapsed(QStringLiteral("00:04:17"));
    pill.resize(pill.sizeHint());

    // Force a paint by making it visible (grab() needs the widget rendered).
    // show() may be blocked by exclusion failure in a test process; we show
    // without activating and rely on grab() which bypasses that guard.
    pill.setWindowFlags(pill.windowFlags() | Qt::WindowTransparentForInput);
    pill.setAttribute(Qt::WA_DontShowOnScreen, false);
    pill.show();
    QCoreApplication::processEvents();

    const bool saved = grabAndSave(pill, QStringLiteral("status-pill.png"));
    EXPECT_TRUE(saved) << "Failed to save status-pill.png";
    pill.hide();
}

// ── Diagnostics pill proof ───────────────────────────────────────────────────

TEST_F(OverlayVisualProofTest, DiagnosticsPill_WithMetrics) {
    DiagnosticsOverlayWindow pill;
    // Spec sample from Mappe OverlayReadouts: fps 60.0 · drop 0 · drift +12ms · 612 MB
    // Plus one muted glyph (mic off) as specified in the task.
    pill.updateMetrics(QStringLiteral("60.0"),   // fps
                       QStringLiteral("+12ms"),  // av_drift
                       QStringLiteral("0"),      // dropped_frames
                       QStringLiteral("612 MB"), // output_size
                       /*mic_muted=*/true,
                       /*sys_muted=*/false);
    pill.resize(pill.sizeHint());
    pill.setWindowFlags(pill.windowFlags() | Qt::WindowTransparentForInput);
    pill.setAttribute(Qt::WA_DontShowOnScreen, false);
    pill.show();
    QCoreApplication::processEvents();

    const bool saved = grabAndSave(pill, QStringLiteral("diagnostics-pill.png"));
    EXPECT_TRUE(saved) << "Failed to save diagnostics-pill.png";
    pill.hide();
}

// ── Countdown overlay proof ───────────────────────────────────────────────────
//
// Renders the countdown overlay at digit "3", with ring ~60% depleted (3 of 5
// seconds remaining).  The dark circle + amber ring + mono digit should be clearly
// visible against the mid-gray backdrop.

TEST_F(OverlayVisualProofTest, CountdownOverlay_Digit3_MidDepletion) {
    CountdownOverlayWindow overlay;
    // 3 remaining out of 5 total → ring at 3/5 = 60% progress (not yet shown;
    // updateCountdown is safe to call before show() since it just updates state).
    overlay.updateCountdown(3, 5);
    overlay.resize(overlay.sizeHint());

    // Force a paint; grab() bypasses the WDA_EXCLUDEFROMCAPTURE guard.
    overlay.setWindowFlags(overlay.windowFlags() | Qt::WindowTransparentForInput);
    overlay.setAttribute(Qt::WA_DontShowOnScreen, false);
    overlay.show();
    QCoreApplication::processEvents();

    const bool saved = grabAndSave(overlay, QStringLiteral("countdown-overlay.png"));
    EXPECT_TRUE(saved) << "Failed to save countdown-overlay.png";

    const QString full_path = output_dir_ + QStringLiteral("/countdown-overlay.png");
    std::printf("[overlay-proof] countdown-overlay.png path: %s\n", full_path.toUtf8().constData());
    std::fflush(stdout);

    overlay.hide();
}

// ── Output directory confirmation ─────────────────────────────────────────────

TEST_F(OverlayVisualProofTest, OutputDirectoryExists) {
    const QDir dir(output_dir_);
    EXPECT_TRUE(dir.exists()) << "Output directory does not exist: " << output_dir_.toStdString();
    std::printf("[overlay-proof] Proof PNGs written to:\n  %s\n",
                QDir::toNativeSeparators(output_dir_).toUtf8().constData());
    std::fflush(stdout);
}

} // namespace
} // namespace exosnap
