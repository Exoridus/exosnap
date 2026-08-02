#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QWidget>

#include "ui/dialogs/ExportOverlay.h"

namespace exosnap::ui::dialogs {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "export_overlay_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class ExportOverlayTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(ExportOverlayTest, StartsClosedInOptionsState) {
    QWidget host;
    ExportOverlay overlay(&host);
    EXPECT_FALSE(overlay.isCardOpen());
    EXPECT_EQ(overlay.state(), ExportOverlay::State::Options);
}

} // namespace
} // namespace exosnap::ui::dialogs
