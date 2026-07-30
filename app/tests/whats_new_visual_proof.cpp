// whats_new_visual_proof.cpp -- offscreen visual proof for the redesigned
// What's-new overlay (single scrolling notes view, relaid-out footer).
//
// Mirrors crash_update_visual_proof.cpp's pattern: real theme, real widgets embedded in
// a host (the overlay only paints its card/scrim correctly as a child), rendered and
// saved as PNG under .workspace/screenshots/whats-new/ for human visual verification.

#include <gtest/gtest.h>

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QString>
#include <QWidget>

#include "ui/dialogs/WhatsNewOverlay.h"
#include "ui/theme/ExoSnapTheme.h"

namespace exosnap {
namespace {

constexpr qreal kDpr = 2.0;

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "whats_new_visual_proof";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class WhatsNewVisualProofTest : public ::testing::Test {
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
                return d.absolutePath() + QStringLiteral("/.workspace/screenshots/whats-new");
            if (!d.cdUp())
                break;
        }
        return QCoreApplication::applicationDirPath() + QStringLiteral("/whats-new");
    }

    static bool renderAndSave(QWidget& widget, const QString& filename) {
        widget.resize(1280, 860);
        widget.move(-20000, -20000);
        widget.show();
        for (int i = 0; i < 3; ++i)
            QCoreApplication::processEvents(QEventLoop::AllEvents);

        QPixmap shot(widget.size() * kDpr);
        shot.setDevicePixelRatio(kDpr);
        widget.render(&shot);

        const QString full_path = output_dir_ + QStringLiteral("/") + filename;
        return shot.save(full_path, "PNG");
    }

    static QVector<WhatsNewNote> SampleNotes() {
        return {
            {QStringLiteral("0.9.0-rc5"),
             QStringLiteral("## 0.9.0-rc5\n- Hardened crash consent\n- Updater handoff fixes"),
             QStringLiteral("https://gh/r/rc5")},
            {QStringLiteral("0.9.0-rc4"), QStringLiteral("## 0.9.0-rc4\n- Full release-version identity"),
             QStringLiteral("https://gh/r/rc4")},
            {QStringLiteral("0.9.0-rc3"), QStringLiteral("## 0.9.0-rc3\n- VFR epoch clamp"),
             QStringLiteral("https://gh/r/rc3")},
        };
    }

    static QString output_dir_;
};

QString WhatsNewVisualProofTest::output_dir_;

TEST_F(WhatsNewVisualProofTest, PreUpdate_NoCheckbox) {
    QWidget host;
    host.resize(1280, 860);
    auto* overlay =
        new ui::dialogs::WhatsNewOverlay(SampleNotes(), /*post_update_mode=*/false,
                                         QStringLiteral("https://github.com/Exoridus/exosnap/releases"), &host);
    overlay->setGeometry(host.rect());
    overlay->openOverlay();
    EXPECT_TRUE(renderAndSave(host, QStringLiteral("01-pre-update.png")));
}

TEST_F(WhatsNewVisualProofTest, PostUpdate_WithCheckbox) {
    QWidget host;
    host.resize(1280, 860);
    auto* overlay =
        new ui::dialogs::WhatsNewOverlay(SampleNotes(), /*post_update_mode=*/true,
                                         QStringLiteral("https://github.com/Exoridus/exosnap/releases"), &host);
    overlay->setGeometry(host.rect());
    overlay->openOverlay();
    EXPECT_TRUE(renderAndSave(host, QStringLiteral("02-post-update.png")));
}

} // namespace
} // namespace exosnap
