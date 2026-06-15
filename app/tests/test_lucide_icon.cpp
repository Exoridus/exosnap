#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QPixmap>

#include "ui/theme/ExoSnapPalette.h"
#include "ui/theme/LucideIcon.h"

namespace exosnap {
namespace {

using ui::theme::ExoSnapPalette;

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "lucide_icon_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class LucideIconTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(LucideIconTest, KnownNameYieldsNonNullPixmapOfRequestedSize) {
    const QPixmap pix =
        ui::theme::lucidePixmap(QStringLiteral("download"), QString::fromLatin1(ExoSnapPalette::kInfo), 16);
    EXPECT_FALSE(pix.isNull());
    // Logical size is honoured even on HiDPI: deviceIndependentSize == requested size.
    EXPECT_EQ(pix.deviceIndependentSize().width(), 16);
    EXPECT_EQ(pix.deviceIndependentSize().height(), 16);
}

TEST_F(LucideIconTest, UnknownNameYieldsEmptyTransparentPixmap) {
    const QPixmap pix =
        ui::theme::lucidePixmap(QStringLiteral("not-a-real-icon"), QString::fromLatin1(ExoSnapPalette::kAccent), 18);
    // Must not crash, and must produce a transparent pixmap (no drawn pixels), still
    // sized to the request so layouts don't shift.
    EXPECT_FALSE(pix.isNull());
    EXPECT_EQ(pix.deviceIndependentSize().width(), 18);
    const QImage img = pix.toImage();
    bool any_opaque = false;
    for (int y = 0; y < img.height() && !any_opaque; ++y)
        for (int x = 0; x < img.width(); ++x)
            if (qAlpha(img.pixel(x, y)) != 0) {
                any_opaque = true;
                break;
            }
    EXPECT_FALSE(any_opaque);
}

TEST_F(LucideIconTest, HiDpiPixmapRendersAtScaledDevicePixels) {
    const QPixmap pix =
        ui::theme::lucidePixmap(QStringLiteral("check"), QString::fromLatin1(ExoSnapPalette::kOk), 12, 2.0);
    EXPECT_FALSE(pix.isNull());
    EXPECT_DOUBLE_EQ(pix.devicePixelRatio(), 2.0);
    EXPECT_EQ(pix.width(), 24); // 12 logical × 2.0 dpr
    EXPECT_EQ(pix.height(), 24);
}

TEST_F(LucideIconTest, ColorIsHonoredAndProducesOpaquePixels) {
    // A stroked icon tinted with a known colour must draw some opaque pixels.
    const QPixmap pix = ui::theme::lucidePixmap(QStringLiteral("x"), QString::fromLatin1(ExoSnapPalette::kErr), 16);
    const QImage img = pix.toImage();
    bool any_opaque = false;
    for (int y = 0; y < img.height() && !any_opaque; ++y)
        for (int x = 0; x < img.width(); ++x)
            if (qAlpha(img.pixel(x, y)) != 0) {
                any_opaque = true;
                break;
            }
    EXPECT_TRUE(any_opaque);
}

TEST_F(LucideIconTest, IconWrapperIsNonNull) {
    const QIcon icon = ui::theme::lucideIcon(QStringLiteral("layers"), QString::fromLatin1(ExoSnapPalette::kText2), 14);
    EXPECT_FALSE(icon.isNull());
}

} // namespace
} // namespace exosnap
