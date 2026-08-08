#include "ui/theme/ExoSnapTheme.h"
#include "ui/widgets/NotificationBell.h"
#include <QApplication>
#include <QCoreApplication>
#include <QImage>
#include <QToolButton>
#include <gtest/gtest.h>

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "notification_bell_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class NotificationBellTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

// Renders the bell and returns the pixel at the centre of where the unread dot
// sits, so the three severity colours are asserted on real output rather than on
// the stored status string.
QColor DotPixel(ui::widgets::NotificationBell& bell) {
    QImage image(bell.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    bell.render(&image);
    return image.pixelColor(bell.dotCenter());
}

TEST_F(NotificationBellTest, Constructs_WithNothingUnread) {
    ui::widgets::NotificationBell bell;
    EXPECT_TRUE(bell.unreadStatus().isEmpty());
    EXPECT_FALSE(bell.hasUnread());
}

TEST_F(NotificationBellTest, SetUnreadStatus_IsRetrievable) {
    ui::widgets::NotificationBell bell;
    bell.setUnreadStatus(QStringLiteral("caution"));
    EXPECT_EQ(bell.unreadStatus(), QStringLiteral("caution"));
    EXPECT_TRUE(bell.hasUnread());
}

TEST_F(NotificationBellTest, SetUnreadStatus_Empty_ClearsUnread) {
    ui::widgets::NotificationBell bell;
    bell.setUnreadStatus(QStringLiteral("error"));
    bell.setUnreadStatus(QString());
    EXPECT_FALSE(bell.hasUnread());
}

TEST_F(NotificationBellTest, Dot_PaintsSeverityColours) {
    const auto& t = exosnap::ui::theme::ActiveTheme();
    ui::widgets::NotificationBell bell;

    bell.setUnreadStatus(QStringLiteral("error"));
    EXPECT_EQ(DotPixel(bell), QColor(QString::fromUtf8(t.error)));

    bell.setUnreadStatus(QStringLiteral("caution"));
    EXPECT_EQ(DotPixel(bell), QColor(QString::fromUtf8(t.caution)));

    // "info" and "success" share the neutral accent: unread mail is not a warning.
    bell.setUnreadStatus(QStringLiteral("info"));
    EXPECT_EQ(DotPixel(bell), QColor(QString::fromUtf8(t.ac)));

    bell.setUnreadStatus(QStringLiteral("success"));
    EXPECT_EQ(DotPixel(bell), QColor(QString::fromUtf8(t.ac)));
}

TEST_F(NotificationBellTest, Dot_UnrecognisedStatus_FallsBackToNeutral) {
    const auto& t = exosnap::ui::theme::ActiveTheme();
    ui::widgets::NotificationBell bell;
    bell.setUnreadStatus(QStringLiteral("not-a-status"));
    EXPECT_TRUE(bell.hasUnread());
    EXPECT_EQ(DotPixel(bell), QColor(QString::fromUtf8(t.ac)));
}

TEST_F(NotificationBellTest, Dot_NothingUnread_PaintsNoDot) {
    const auto& t = exosnap::ui::theme::ActiveTheme();
    ui::widgets::NotificationBell bell;

    // Compared against the widget's own resting output rather than against
    // transparency: QToolButton paints its panel through the active QStyle, so
    // the corner pixel is opaque either way and its exact value is style- (and
    // therefore machine-) dependent.
    const QColor resting = DotPixel(bell);
    EXPECT_NE(resting, QColor(QString::fromUtf8(t.error)));

    bell.setUnreadStatus(QStringLiteral("error"));
    EXPECT_NE(DotPixel(bell), resting) << "an unread status must change the corner";

    bell.setUnreadStatus(QString());
    EXPECT_EQ(DotPixel(bell), resting) << "clearing the status must remove the dot again";
}

TEST_F(NotificationBellTest, IsQToolButton) {
    ui::widgets::NotificationBell bell;
    EXPECT_NE(qobject_cast<QToolButton*>(&bell), nullptr);
}

TEST_F(NotificationBellTest, FixedSize_MatchesTheDeclaredMetric) {
    ui::widgets::NotificationBell bell;
    EXPECT_EQ(bell.width(), ui::widgets::NotificationBell::kSize);
    EXPECT_EQ(bell.height(), ui::widgets::NotificationBell::kSize);
    // Pinned: the 40 px title bar has no room for the old 34 px bell.
    EXPECT_EQ(ui::widgets::NotificationBell::kSize, 28);
}

TEST_F(NotificationBellTest, NoFocusPolicy) {
    ui::widgets::NotificationBell bell;
    EXPECT_EQ(bell.focusPolicy(), Qt::NoFocus);
}

TEST_F(NotificationBellTest, Clicked_Signal_Emitted) {
    // Manual signal recorder — avoids Qt6::Test / QSignalSpy dependency
    ui::widgets::NotificationBell bell;
    int click_count = 0;
    QObject::connect(&bell, &ui::widgets::NotificationBell::clicked, [&]() { ++click_count; });
    bell.click();
    EXPECT_EQ(click_count, 1);
}

TEST_F(NotificationBellTest, DotCenter_SitsOnTheIconCornerNotTheWidgetEdge) {
    ui::widgets::NotificationBell bell;
    const QPoint c = bell.dotCenter();
    const int radius = ui::widgets::NotificationBell::kDotDiameter / 2;

    // Fully inside the widget, ring included.
    EXPECT_GE(c.x() - radius - 1, 0);
    EXPECT_LE(c.x() + radius + 1, bell.width());
    EXPECT_GE(c.y() - radius - 1, 0);
    EXPECT_LE(c.y() + radius + 1, bell.height());

    // Anchored to the glyph, not the widget corner: a dot pinned to the widget
    // edge floats away from the bell, because the icon is centred in a larger
    // button. Rendered proof of that is in the visual-test screenshots.
    EXPECT_LT(c.x(), bell.width() - radius) << "dot is hugging the widget's right edge";
    EXPECT_GT(c.y(), radius - 1) << "dot is hugging the widget's top edge";
}

TEST_F(NotificationBellTest, RepeatedSameStatus_IsIdempotent) {
    ui::widgets::NotificationBell bell;
    bell.setUnreadStatus(QStringLiteral("error"));
    bell.setUnreadStatus(QStringLiteral("error"));
    EXPECT_EQ(bell.unreadStatus(), QStringLiteral("error"));
}

} // namespace
} // namespace exosnap
