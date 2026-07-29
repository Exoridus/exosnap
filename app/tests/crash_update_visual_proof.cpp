// crash_update_visual_proof.cpp — CRASH-UPDATE-VISUAL-PROOF-R1
//
// Offscreen visual proof for the 0.4.0 crash + update UI surfaces. Mirrors the
// overlay_visual_proof.cpp pattern: builds a QApplication, applies the *real*
// ExoSnap theme (QSS + bundled fonts + palette) so the renders reflect shipping
// styling, constructs the real widgets with realistic sample data, then writes
// PNGs via QWidget rendering for human visual verification.
//
// Produces crash-state PNGs under .workspace/screenshots/0.4.0/:
//   01-crash-next-launch.png — CrashReportPanel, honest next-launch state
//                              (no exception/module/thread/stack), not recording.
//   02-crash-recording.png   — same panel with recording_was_active=true
//                              (green "recording secured" banner visible).
//   03-crash-privacy-expanded.png — expanded disclosure.
//   04-crash-no-dump.png     — limited-context path.
//   05-crash-remember-choice.png — uncommitted remember draft.
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
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "ui/dialogs/CrashReportOverlay.h"
#include "ui/dialogs/CrashReportPanel.h"
#include "ui/theme/ExoSnapTheme.h"
#include "ui/widgets/ExoCheckBox.h"

namespace exosnap {
namespace {

using ui::dialogs::CrashReportModel;
using ui::dialogs::CrashReportPanel;

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
        m.encoder = QStringLiteral("NVENC AV1 \xe2\x86\x92 MKV");
        m.crash_dir = QStringLiteral("C:/Users/test/AppData/Local/ExoSnap/crash");
        m.dmp_path = QStringLiteral("C:/Users/test/AppData/Local/ExoSnap/crash/report.dmp");
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

// The proofs above render the panel alone, where Qt paints a top-level window
// background regardless. In the app the panel is a child of CrashReportOverlay, and a
// plain QWidget child paints no stylesheet background unless WA_StyledBackground is set
// — so the card vanished and its contents sat loose on the backdrop. Render it as a
// child, because that is what the user sees on the next launch.
TEST_F(CrashUpdateVisualProofTest, Crash_InsideOverlayAsShownOnLaunch) {
    QWidget host;
    host.setObjectName(QStringLiteral("crashProofHost"));
    host.resize(1280, 860);

    auto* overlay = new ui::dialogs::CrashReportOverlay(baseModel(), &host);
    overlay->setGeometry(host.rect());
    overlay->openOverlay();

    const bool saved = renderAndSave(host, QStringLiteral("06-crash-overlay-in-window.png"));
    EXPECT_TRUE(saved);
}

// Pins the card's surface, not the attribute that produces it: a panel embedded in a
// parent must paint its own background over that parent, or it is not a card.
TEST_F(CrashUpdateVisualProofTest, Crash_CardPaintsItsSurfaceWhenEmbedded) {
    QWidget host;
    host.resize(700, 1100);
    // Palette, not stylesheet: a plain QWidget would not paint a stylesheet background
    // either — the very bug under test.
    host.setAutoFillBackground(true);
    QPalette host_palette = host.palette();
    host_palette.setColor(QPalette::Window, QColor(0xff, 0x00, 0xff)); // a colour the card never uses
    host.setPalette(host_palette);

    auto* panel = new CrashReportPanel(baseModel(), &host);
    panel->move(0, 0);
    settleLayout(host);
    host.move(-20000, -20000);
    host.show();
    settleLayout(host);

    const QImage shot = host.grab().toImage();
    const QColor magenta(0xff, 0x00, 0xff);
    // The card's own padding: inside its border, below the chrome bar, left of every
    // child widget. Only the card itself can paint here.
    for (const int x : {5, 8, 12}) {
        EXPECT_NE(shot.pixelColor(x, 120), magenta)
            << "at x=" << x << ": the card does not paint its surface; the parent shows through";
    }
    // The chrome bar shares the card's surface colour on purpose, so no tone comparison
    // can tell whether it paints — the card behind it would look the same.

    // The card has a fixed width, so a long label silently clips. Only a rendered,
    // laid-out panel knows the real widths.
    for (const auto* button : panel->findChildren<QPushButton*>()) {
        if (button->text().isEmpty())
            continue; // icon-only chrome close button
        EXPECT_GE(button->width(), button->sizeHint().width())
            << "button label is clipped: " << button->text().toStdString();
    }
}

TEST_F(CrashUpdateVisualProofTest, Crash_RecordingSecured) {
    CrashReportModel m = baseModel();
    m.recording_was_active = true;
    CrashReportPanel panel(m, nullptr);
    const bool saved = renderAndSave(panel, QStringLiteral("02-crash-recording.png"));
    EXPECT_TRUE(saved);
}

TEST_F(CrashUpdateVisualProofTest, Crash_PrivacyExpanded) {
    CrashReportModel m = baseModel();
    CrashReportPanel panel(m, nullptr);
    auto* toggle = panel.findChild<QPushButton*>(QStringLiteral("crashPrivacyDisclosure"));
    ASSERT_NE(toggle, nullptr) << "crashPrivacyDisclosure not found";
    toggle->click();
    QCoreApplication::processEvents();

    const bool saved = renderAndSave(panel, QStringLiteral("03-crash-privacy-expanded.png"));
    EXPECT_TRUE(saved);
}

TEST_F(CrashUpdateVisualProofTest, Crash_NoDumpAvailable) {
    CrashReportModel m = baseModel();
    m.dmp_path.clear();
    CrashReportPanel panel(m, nullptr);
    const bool saved = renderAndSave(panel, QStringLiteral("04-crash-no-dump.png"));
    EXPECT_TRUE(saved);
}

TEST_F(CrashUpdateVisualProofTest, Crash_RememberChoiceDraft) {
    CrashReportPanel panel(baseModel(), nullptr);
    auto* remember = panel.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("crashRememberChoiceCheck"));
    ASSERT_NE(remember, nullptr);
    remember->setChecked(true);
    QCoreApplication::processEvents();
    const bool saved = renderAndSave(panel, QStringLiteral("05-crash-remember-choice.png"));
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
