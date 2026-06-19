#include "ui/widgets/CornerCaptureButton.h"
#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <gtest/gtest.h>

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "corner_capture_button_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class CornerCaptureButtonTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(CornerCaptureButtonTest, Constructs_WithoutCrash) {
    ui::widgets::CornerCaptureButton btn;
    EXPECT_NE(&btn, nullptr);
}

TEST_F(CornerCaptureButtonTest, FixedSize_40x40) {
    ui::widgets::CornerCaptureButton btn;
    EXPECT_EQ(btn.width(), 40);
    EXPECT_EQ(btn.height(), 40);
}

TEST_F(CornerCaptureButtonTest, IsQAbstractButton) {
    ui::widgets::CornerCaptureButton btn;
    EXPECT_NE(qobject_cast<QAbstractButton*>(&btn), nullptr);
}

TEST_F(CornerCaptureButtonTest, SetIcon_DoesNotCrash) {
    ui::widgets::CornerCaptureButton btn;
    btn.setIcon(QStringLiteral("camera"));
    btn.setIcon(QStringLiteral("image"));
}

TEST_F(CornerCaptureButtonTest, SetIconColor) {
    ui::widgets::CornerCaptureButton btn;
    btn.setIconColor(QColor(Qt::white));
    EXPECT_TRUE(btn.isEnabled());
}

TEST_F(CornerCaptureButtonTest, Clicked_Signal_Emitted) {
    // Manual signal recorder — avoids Qt6::Test / QSignalSpy dependency
    ui::widgets::CornerCaptureButton btn;
    int click_count = 0;
    QObject::connect(&btn, &ui::widgets::CornerCaptureButton::clicked, [&]() { ++click_count; });
    btn.click();
    EXPECT_EQ(click_count, 1);
}

TEST_F(CornerCaptureButtonTest, Disabled_DoesNotEmitClick) {
    // Manual signal recorder — avoids Qt6::Test / QSignalSpy dependency
    ui::widgets::CornerCaptureButton btn;
    btn.setEnabled(false);
    int click_count = 0;
    QObject::connect(&btn, &ui::widgets::CornerCaptureButton::clicked, [&]() { ++click_count; });
    btn.click();
    EXPECT_EQ(click_count, 0);
}

TEST_F(CornerCaptureButtonTest, UnknownIcon_DoesNotCrash) {
    ui::widgets::CornerCaptureButton btn;
    // Unknown icon returns transparent pixmap, must not crash
    btn.setIcon(QStringLiteral("nonexistent-icon-xyz"));
    btn.update();
}

} // namespace
} // namespace exosnap
