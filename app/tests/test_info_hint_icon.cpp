#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QFocusEvent>
#include <QString>
#include <QToolButton>

#include "ui/widgets/InfoHintIcon.h"

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "info_hint_icon_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class InfoHintIconTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

// ---- InfoHintIcon widget tests ----

TEST_F(InfoHintIconTest, Constructs_WithHintText) {
    const QString hint = QStringLiteral("MKV safest \xC2\xB7 MP4 most compatible");
    ui::widgets::InfoHintIcon icon(hint);

    EXPECT_EQ(icon.hintText(), hint);
}

TEST_F(InfoHintIconTest, ToolTip_IsSetToHintText) {
    const QString hint = QStringLiteral("Constant rate \xC2\xB7 editor-friendly");
    ui::widgets::InfoHintIcon icon(hint);

    EXPECT_EQ(icon.toolTip(), hint);
}

TEST_F(InfoHintIconTest, AccessibleName_ContainsHintText) {
    const QString hint = QStringLiteral("Best compression \xC2\xB7 newer players");
    ui::widgets::InfoHintIcon icon(hint);

    EXPECT_TRUE(icon.accessibleName().contains(hint))
        << "Accessible name must include the hint text for screen readers";
}

TEST_F(InfoHintIconTest, IsFocusable_TabFocusPolicy) {
    ui::widgets::InfoHintIcon icon(QStringLiteral("Include this source"));

    EXPECT_EQ(icon.focusPolicy(), Qt::TabFocus) << "InfoHintIcon must be keyboard-focusable via Tab";
}

TEST_F(InfoHintIconTest, IsQToolButton) {
    ui::widgets::InfoHintIcon icon(QStringLiteral("Where recordings are saved"));

    // Verify it is a QToolButton (the design recommends a flat button).
    EXPECT_NE(qobject_cast<QToolButton*>(&icon), nullptr);
}

TEST_F(InfoHintIconTest, HasIcon_AtConstruction) {
    ui::widgets::InfoHintIcon icon(QStringLiteral("Downscale to save size \xC2\xB7 re-encodes"));

    // The icon is set via LucideIcon; verify it is not null.
    EXPECT_FALSE(icon.icon().isNull()) << "InfoHintIcon must have an icon at construction time";
}

TEST_F(InfoHintIconTest, FixedSize_Is18x18) {
    ui::widgets::InfoHintIcon icon(QStringLiteral("Tokens for auto-naming"));

    EXPECT_EQ(icon.width(), 18);
    EXPECT_EQ(icon.height(), 18);
}

TEST_F(InfoHintIconTest, LabelRole_IsInfoGlyph) {
    ui::widgets::InfoHintIcon icon(QStringLiteral("Combine into one track"));

    EXPECT_EQ(icon.property("labelRole").toString(), QStringLiteral("infoGlyph"))
        << "labelRole must be 'infoGlyph' for QSS styling";
}

TEST_F(InfoHintIconTest, AutoRaise_IsTrue) {
    ui::widgets::InfoHintIcon icon(QStringLiteral("Show the mouse pointer"));

    EXPECT_TRUE(icon.autoRaise()) << "InfoHintIcon must be flat (autoRaise = true)";
}

TEST_F(InfoHintIconTest, EmptyHintText_DoesNotCrash) {
    // Robust against empty strings (though not expected in production).
    const QString empty;
    EXPECT_NO_FATAL_FAILURE({
        ui::widgets::InfoHintIcon icon(empty);
        EXPECT_TRUE(icon.hintText().isEmpty());
    });
}

} // namespace
} // namespace exosnap
