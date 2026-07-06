// recovery_overlay_visual_proof.cpp
//
// Offscreen visual proof for the startup Recovery surface. Mirrors
// crash_update_visual_proof.cpp / recording_error_visual_proof.cpp: builds a
// QApplication, applies the *real* ExoSnap theme (QSS + bundled fonts + palette)
// so the render reflects shipping styling, constructs the real RecoveryOverlay
// with realistic candidates, then writes a PNG of the card for human visual
// verification.
//
// Produces .workspace/screenshots/recovery-overlay.png.
//
// The test always passes as long as the PNG is non-empty; its real product is
// the PNG file for human visual verification.

#include <gtest/gtest.h>

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QEventLoop>
#include <QFrame>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QSize>
#include <QString>
#include <QTemporaryDir>
#include <QWidget>

#include "services/RecoveryService.h"
#include "settings/RecoveryManifestStore.h"
#include "ui/dialogs/RecoveryOverlay.h"
#include "ui/theme/ExoSnapTheme.h"

namespace exosnap {
namespace {

constexpr qreal kDpr = 2.0;

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "recovery_overlay_visual_proof";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

RecoveryCandidate MakeCandidate(const QString& id, const QString& artefact, qint64 size, const QString& container,
                                bool finalized) {
    RecoveryManifestEntry e;
    e.id = id;
    e.artefact_path = artefact;
    e.intended_container = container;
    e.final_output_path = artefact;
    e.started_at = QStringLiteral("2026-07-05T14:32:00Z");
    e.finalized = finalized;

    RecoveryCandidate c;
    c.entry = e;
    c.artefact_size_bytes = size;
    return c;
}

class RecoveryOverlayVisualProofTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        QApplication* app = EnsureApplication();
        ui::theme::ApplyExoSnapTheme(*app);
        output_dir_ = resolveOutputDir();
        QDir().mkpath(output_dir_);
    }

    static QString resolveOutputDir() {
        QDir d(QCoreApplication::applicationDirPath());
        for (int i = 0; i < 12; ++i) {
            if (d.exists(QStringLiteral(".git")))
                return d.absolutePath() + QStringLiteral("/.workspace/screenshots");
            if (!d.cdUp())
                break;
        }
        return QCoreApplication::applicationDirPath();
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
        std::printf(ok ? "[recovery-proof] Saved: %s\n" : "[recovery-proof] FAILED to save: %s\n",
                    full_path.toUtf8().constData());
        std::fflush(stdout);
        return ok;
    }

    static QString output_dir_;
};

QString RecoveryOverlayVisualProofTest::output_dir_;

TEST_F(RecoveryOverlayVisualProofTest, RecoveryCard) {
    QTemporaryDir tmp;
    RecoveryManifestStore store(tmp.path() + QStringLiteral("/manifest.json"));
    RecoveryService service(store);

    const QVector<RecoveryCandidate> candidates = {
        MakeCandidate(QStringLiteral("id-1"), QStringLiteral("C:/Users/test/Videos/Gameplay 2026-07-05.mkv"),
                      1024LL * 1024 * 842, QStringLiteral("mkv"), /*finalized=*/false),
        MakeCandidate(QStringLiteral("id-2"), QStringLiteral("C:/Users/test/Videos/Capture session 2.mp4"),
                      1024LL * 1024 * 233, QStringLiteral("mp4"), /*finalized=*/true),
    };

    // Host the overlay so the card lays out exactly as in the app, then render the
    // card (the styled surface) on the neutral proof backdrop.
    QWidget host;
    host.resize(960, 720);
    auto* overlay = new ui::dialogs::RecoveryOverlay(service, candidates, &host);
    host.move(-20000, -20000);
    host.show();
    overlay->openOverlay();
    settleLayout(host);

    auto* card = overlay->findChild<QFrame*>(QStringLiteral("recoveryCard"));
    ASSERT_NE(card, nullptr);
    EXPECT_TRUE(renderAndSave(*card, QStringLiteral("recovery-overlay.png")));
}

TEST_F(RecoveryOverlayVisualProofTest, OutputDirectoryExists) {
    const QDir dir(output_dir_);
    EXPECT_TRUE(dir.exists()) << "Output directory does not exist: " << output_dir_.toStdString();
}

} // namespace
} // namespace exosnap
