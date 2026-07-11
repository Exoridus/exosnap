// theme_switch_visual_proof.cpp
//
// Pixel proof that switching themes at runtime restyles widgets which build
// inline stylesheets / theme-tinted pixmaps from ActiveTheme(). Historically
// those widgets baked their colours at construction time and survived
// ReapplyTheme() unchanged — the confirmed case being the CompareHint popover
// staying dark under a light theme.
//
// Mirrors the crash_update_visual_proof.cpp pattern: a QApplication with the
// *real* shipping theme (QSS + bundled fonts + palette), real widgets, PNGs
// under .workspace/screenshots/theme-switch/ for human verification — plus
// hard pixel assertions:
//
//   1. The CompareHint popover, opened after ReapplyTheme("light-paper"),
//      must render LIGHT (its construction-time theme was dark).
//   2. A dark → light → dark round trip must reproduce the original dark
//      render byte-identically (no drift, no half-applied styles).

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QImage>
#include <QLabel>
#include <QMetaObject>
#include <QPixmap>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

#include "ui/dialogs/UpdateSettingsPanel.h"
#include "ui/theme/ExoSnapTheme.h"
#include "ui/widgets/CompareHint.h"

namespace exosnap {
namespace {

using ui::dialogs::UpdateSettingsPanel;
using ui::dialogs::UpdateUiModel;
using ui::dialogs::UpdateUiState;
using ui::widgets::CompareHint;

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "theme_switch_visual_proof";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class ThemeSwitchVisualProofTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        app_ = EnsureApplication();
        // Startup path: dark-default, real QSS + fonts + palette.
        ui::theme::ApplyExoSnapTheme(*app_);
    }

    void SetUp() override {
        // Tests may run in one process; always start from the shipped default.
        ui::theme::ReapplyTheme(*app_, QStringLiteral("dark-default"));
    }

    static QString OutputDir() {
        QDir d(QCoreApplication::applicationDirPath());
        for (int i = 0; i < 12; ++i) {
            if (d.exists(QStringLiteral(".git")))
                return d.absolutePath() + QStringLiteral("/.workspace/screenshots/theme-switch");
            if (!d.cdUp())
                break;
        }
        return QCoreApplication::applicationDirPath() + QStringLiteral("/theme-switch");
    }

    // Flush pending events incl. DeferredDelete so widgets dropped during a
    // theme rebuild (deleteLater) are gone before we render.
    static void settle() {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    static QImage grabWidget(QWidget& w) {
        settle();
        return w.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    }

    static double averageLuminance(const QImage& img) {
        if (img.isNull())
            return 0.0;
        double sum = 0.0;
        qint64 count = 0;
        for (int y = 0; y < img.height(); y += 2) {
            for (int x = 0; x < img.width(); x += 2) {
                const QRgb px = img.pixel(x, y);
                sum += 0.299 * qRed(px) + 0.587 * qGreen(px) + 0.114 * qBlue(px);
                ++count;
            }
        }
        return count > 0 ? sum / static_cast<double>(count) : 0.0;
    }

    static void savePng(const QImage& img, const QString& name) {
        const QString dir = OutputDir();
        QDir().mkpath(dir);
        ASSERT_TRUE(img.save(dir + QLatin1Char('/') + name)) << "failed to save " << name.toStdString();
    }

    // The popover is a parentless Qt::Popup; locate it via its rows container.
    static QWidget* findPopover() {
        const QWidgetList tops = QApplication::topLevelWidgets();
        for (QWidget* top : tops) {
            if (top->findChild<QWidget*>(QStringLiteral("compareHintRows")) != nullptr)
                return top;
        }
        return nullptr;
    }

    static QApplication* app_;
};

QApplication* ThemeSwitchVisualProofTest::app_ = nullptr;

// The confirmed theme-switch bug: a CompareHint built under dark-default kept a
// dark popover after switching to a light theme. The popover opened after the
// switch must be light; switching back must restore the dark popover.
TEST_F(ThemeSwitchVisualProofTest, CompareHintPopover_FollowsThemeSwitch) {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto* hint = new CompareHint(QStringLiteral("container"), QStringLiteral("MKV"), &host);
    layout->addWidget(hint);
    host.resize(80, 40);
    // Show without activating and keep focus off the hint: window activation would
    // hand focus to the hint (focusInEvent opens the popover), and in this synchronous
    // test context that focus/popup interplay recurses. The popover is driven
    // explicitly below instead.
    host.setAttribute(Qt::WA_ShowWithoutActivating, true);
    hint->setFocusPolicy(Qt::NoFocus);
    host.show();
    settle();

    // Dark baseline (construction-time theme).
    ASSERT_TRUE(QMetaObject::invokeMethod(hint, "showPopover"));
    QWidget* popover = findPopover();
    ASSERT_NE(popover, nullptr) << "popover did not open under dark-default";
    const QImage dark_img = grabWidget(*popover);
    savePng(dark_img, QStringLiteral("compare-hint-popover-dark.png"));
    ASSERT_TRUE(QMetaObject::invokeMethod(hint, "hidePopover"));

    // Switch to a light theme — the real runtime path.
    ui::theme::ReapplyTheme(*app_, QStringLiteral("light-paper"));
    settle();

    ASSERT_TRUE(QMetaObject::invokeMethod(hint, "showPopover"));
    popover = findPopover();
    ASSERT_NE(popover, nullptr) << "popover did not open after the switch to light-paper";
    const QImage light_img = grabWidget(*popover);
    savePng(light_img, QStringLiteral("compare-hint-popover-light.png"));
    ASSERT_TRUE(QMetaObject::invokeMethod(hint, "hidePopover"));

    const double dark_lum = averageLuminance(dark_img);
    const double light_lum = averageLuminance(light_img);
    // dark surf2 is near-black (#1C1C1F); light-paper surf2 is white. A popover
    // that keeps its baked dark styling fails this hard.
    EXPECT_LT(dark_lum, 96.0) << "dark popover render unexpectedly light";
    EXPECT_GT(light_lum, 160.0) << "popover stayed dark after switching to light-paper";

    // Round trip back to dark: the popover must render dark again.
    ui::theme::ReapplyTheme(*app_, QStringLiteral("dark-default"));
    settle();
    ASSERT_TRUE(QMetaObject::invokeMethod(hint, "showPopover"));
    popover = findPopover();
    ASSERT_NE(popover, nullptr);
    const QImage dark_again = grabWidget(*popover);
    ASSERT_TRUE(QMetaObject::invokeMethod(hint, "hidePopover"));
    EXPECT_LT(averageLuminance(dark_again), 96.0) << "popover stayed light after switching back to dark";
}

// A panel full of inline stylesheets (UpdateSettingsPanel) must follow the
// switch, and a dark → light → dark round trip must be pixel-identical to the
// never-switched dark render — proving the switch is complete, not partial.
TEST_F(ThemeSwitchVisualProofTest, UpdatePanel_SwitchesAndRoundTripsDarkIdentically) {
    UpdateSettingsPanel panel;
    UpdateUiModel model;
    model.current_version = QStringLiteral("0.8.1");
    model.available_version = QStringLiteral("0.9.0");
    model.last_checked = QStringLiteral("Just now");
    model.whats_new = {QStringLiteral("System-audio loopback on any device rate"),
                       QStringLiteral("Failed remuxes keep the original recording")};
    model.channel = QStringLiteral("Stable");
    panel.setModel(model);
    panel.setState(UpdateUiState::Available);
    panel.setFixedWidth(520);
    panel.show();

    const QImage dark_before = grabWidget(panel);
    savePng(dark_before, QStringLiteral("update-panel-dark.png"));

    ui::theme::ReapplyTheme(*app_, QStringLiteral("light-paper"));
    const QImage light_img = grabWidget(panel);
    savePng(light_img, QStringLiteral("update-panel-light.png"));

    EXPECT_GT(averageLuminance(light_img), averageLuminance(dark_before) + 40.0)
        << "UpdateSettingsPanel did not visibly lighten after switching to light-paper";

    ui::theme::ReapplyTheme(*app_, QStringLiteral("dark-default"));
    const QImage dark_after = grabWidget(panel);
    savePng(dark_after, QStringLiteral("update-panel-dark-roundtrip.png"));

    EXPECT_EQ(dark_before, dark_after) << "dark render after a light round trip differs from the original dark render";
}

} // namespace
} // namespace exosnap
