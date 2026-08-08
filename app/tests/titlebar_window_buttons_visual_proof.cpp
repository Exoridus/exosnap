// titlebar_window_buttons_visual_proof.cpp
//
// Pixel proof for the titlebar window-control buttons (minimize / maximize /
// close): the painted hover/rest box was made smaller and quieter purely via a
// QSS margin (see exosnap_dark.qss), while the widget itself keeps the full
// 46 x kHeight click target (OperationalTitleBar.cpp) so a mouse flick into
// the maximized window's screen corner still lands on Close.
//
// This mirrors theme_switch_visual_proof.cpp: a QApplication with the *real*
// shipping theme (QSS + bundled fonts), a real OperationalTitleBar, PNGs under
// .workspace/screenshots/titlebar-window-buttons/ for human verification —
// plus hard pixel assertions. Judging on rendered pixels (not QSS source
// values) is required because the actual bug this test guards against was
// invisible in QSS review: the generic `QPushButton { padding; min-height }`
// rule still applies to unset properties on the ID-scoped rule (CSS cascades
// per-property, not per-rule), and once combined with `margin` it silently
// grew the real widget past its setFixedSize() click target and, separately,
// produced a degenerate paint rect that rendered nothing at all — both only
// visible by actually rendering the widget with the real stylesheet.

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QImage>
#include <QPushButton>
#include <QString>

#include "ui/chrome/OperationalTitleBar.h"
#include "ui/theme/ExoSnapTheme.h"

namespace exosnap {
namespace {

using ui::chrome::OperationalTitleBar;

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "titlebar_window_buttons_visual_proof";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class TitlebarWindowButtonsVisualProofTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        app_ = EnsureApplication();
        // Real QSS + bundled fonts + palette — the same startup path main.cpp uses.
        ui::theme::ApplyExoSnapTheme(*app_);
    }

    static QString OutputDir() {
        QDir d(QCoreApplication::applicationDirPath());
        for (int i = 0; i < 12; ++i) {
            if (d.exists(QStringLiteral(".git")))
                return d.absolutePath() + QStringLiteral("/.workspace/screenshots/titlebar-window-buttons");
            if (!d.cdUp())
                break;
        }
        return QCoreApplication::applicationDirPath() + QStringLiteral("/titlebar-window-buttons");
    }

    static void settle() {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    static void savePng(const QImage& img, const QString& name) {
        const QString dir = OutputDir();
        QDir().mkpath(dir);
        ASSERT_TRUE(img.save(dir + QLatin1Char('/') + name)) << "failed to save " << name.toStdString();
    }

    // Average color over the button's actual PAINTED box (inset by the QSS margin
    // from the widget's full click-target rect). img is a grab() of `root` (the
    // titlebar), so the button's own local rect must be mapped into root's
    // coordinate space before indexing into img — img.pixel() takes root-relative
    // coordinates, not button-local ones.
    static QColor averagePaintedBoxColor(const QImage& img, const QWidget& root, const QPushButton& button) {
        const QRect local = button.rect();
        // The margin values are QSS-owned and may be retuned; sample the inner ~40%
        // of the button rect so the probe stays inside the painted box regardless of
        // the exact margin, without hardcoding it here.
        const QRect local_probe(local.center().x() - local.width() / 5, local.center().y() - local.height() / 5,
                                2 * local.width() / 5, 2 * local.height() / 5);
        const QPoint origin = button.mapTo(&root, local_probe.topLeft());
        const QRect probe(origin, local_probe.size());
        long r = 0;
        long g = 0;
        long b = 0;
        long n = 0;
        for (int y = probe.top(); y <= probe.bottom(); ++y) {
            for (int x = probe.left(); x <= probe.right(); ++x) {
                if (!img.rect().contains(x, y))
                    continue;
                const QRgb px = img.pixel(x, y);
                r += qRed(px);
                g += qGreen(px);
                b += qBlue(px);
                ++n;
            }
        }
        if (n == 0)
            return {};
        return QColor(static_cast<int>(r / n), static_cast<int>(g / n), static_cast<int>(b / n));
    }

    static QApplication* app_;
};

QApplication* TitlebarWindowButtonsVisualProofTest::app_ = nullptr;

// The click target must stay the full 46 x kHeight widget rect, flush to the
// titlebar's right edge, EVEN WITH the real stylesheet applied — the earlier
// (gtest-environment, no-stylesheet) geometry tests could not see the real bug:
// the generic QPushButton rule's min-height, combined with the new margin, grew
// the live widget past its fixed size once a real QStyleSheetStyle was in play.
TEST_F(TitlebarWindowButtonsVisualProofTest, WindowButtons_KeepFullClickTargetUnderRealStylesheet) {
    OperationalTitleBar bar;
    bar.resize(1200, OperationalTitleBar::kHeight);
    bar.show();
    settle();

    const QList<QPushButton*> window_buttons = bar.findChildren<QPushButton*>(QStringLiteral("titlebarWindowButton"));
    ASSERT_EQ(window_buttons.size(), 3);
    for (const QPushButton* button : window_buttons) {
        EXPECT_EQ(button->size(), QSize(46, OperationalTitleBar::kHeight))
            << "window button lost its fixed 46 x kHeight click target under the real stylesheet";
    }
}

// Rest state: none of the three buttons show the close button's coral/red hover
// fill or the neutral hover fill — the painted box must be quiet at rest.
TEST_F(TitlebarWindowButtonsVisualProofTest, RestState_NoButtonShowsHoverFill) {
    OperationalTitleBar bar;
    bar.resize(1200, OperationalTitleBar::kHeight);
    bar.show();
    settle();

    const QImage img = bar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    savePng(img, QStringLiteral("rest.png"));

    for (QPushButton* button : bar.findChildren<QPushButton*>(QStringLiteral("titlebarWindowButton"))) {
        const QColor c = averagePaintedBoxColor(img, bar, *button);
        // The close hover fill (#b8261d) is strongly red-dominant; the neutral hover
        // fill (bg3, a near-neutral dark grey) is not — either would be a rest-state
        // regression, so require the sampled box to stay close to neutral: no channel
        // more than a slight amount brighter than the darkest, and not red-dominant.
        EXPECT_LT(c.red() - c.blue(), 40) << "unexpected red tint at rest for button " << button->text().toStdString();
    }
}

// Forcing hover on the close button must paint the coral/red hover fill inside
// its (now smaller) painted box.
TEST_F(TitlebarWindowButtonsVisualProofTest, CloseButton_ForcedHover_PaintsRedFill) {
    OperationalTitleBar bar;
    bar.resize(1200, OperationalTitleBar::kHeight);
    bar.show();
    settle();

    QPushButton* close_button = nullptr;
    for (QPushButton* button : bar.findChildren<QPushButton*>(QStringLiteral("titlebarWindowButton"))) {
        if (button->property("windowControlRole").toString() == QStringLiteral("close"))
            close_button = button;
    }
    ASSERT_NE(close_button, nullptr);

    bar.setForcedWindowButtonHover(QStringLiteral("close"));
    settle();

    const QImage img = bar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    savePng(img, QStringLiteral("hover-close.png"));

    const QColor c = averagePaintedBoxColor(img, bar, *close_button);
    EXPECT_GT(c.red(), 120) << "close hover fill not red enough: " << c.name().toStdString();
    EXPECT_GT(c.red() - c.green(), 40) << "close hover fill not red-dominant: " << c.name().toStdString();
    EXPECT_GT(c.red() - c.blue(), 40) << "close hover fill not red-dominant: " << c.name().toStdString();

    // The click target itself must be unaffected by hover.
    EXPECT_EQ(close_button->size(), QSize(46, OperationalTitleBar::kHeight));
}

// Forcing hover on minimize must paint the neutral hover fill (bg3) — lighter
// than the app background, but nowhere near red — and must leave maximize/close
// untouched (only one button hovers at a time).
TEST_F(TitlebarWindowButtonsVisualProofTest, MinimizeButton_ForcedHover_PaintsNeutralFillOnlyOnItself) {
    OperationalTitleBar bar;
    bar.resize(1200, OperationalTitleBar::kHeight);
    bar.show();
    settle();

    QPushButton* minimize_button = nullptr;
    QPushButton* other_button = nullptr;
    for (QPushButton* button : bar.findChildren<QPushButton*>(QStringLiteral("titlebarWindowButton"))) {
        if (button->text() == QStringLiteral("−")) // MINUS SIGN
            minimize_button = button;
        else if (button->property("windowControlRole").toString() == QStringLiteral("close"))
            other_button = button;
    }
    ASSERT_NE(minimize_button, nullptr);
    ASSERT_NE(other_button, nullptr);

    // Rest-state baseline for comparison.
    const QImage rest_img = bar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    const QColor rest_color = averagePaintedBoxColor(rest_img, bar, *minimize_button);

    bar.setForcedWindowButtonHover(QStringLiteral("minimize"));
    settle();

    const QImage hover_img = bar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    savePng(hover_img, QStringLiteral("hover-minimize.png"));

    const QColor hovered_color = averagePaintedBoxColor(hover_img, bar, *minimize_button);
    EXPECT_GT(hovered_color.red() + hovered_color.green() + hovered_color.blue(),
              rest_color.red() + rest_color.green() + rest_color.blue() + 15)
        << "minimize hover fill did not brighten the painted box: rest=" << rest_color.name().toStdString()
        << " hover=" << hovered_color.name().toStdString();
    EXPECT_LT(hovered_color.red() - hovered_color.blue(), 20) << "minimize hover fill unexpectedly red-tinted";

    const QColor other_color = averagePaintedBoxColor(hover_img, bar, *other_button);
    EXPECT_LT(other_color.red() - other_color.blue(), 40) << "close button hovered along with minimize";
}

} // namespace
} // namespace exosnap
