#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QWidget>

#include "ui/widgets/EditDetailsRail.h"

namespace exosnap::ui::widgets {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "edit_details_rail_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class EditDetailsRailTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(EditDetailsRailTest, CarriesItsObjectName) {
    QWidget host;
    EditDetailsRail rail(&host);
    EXPECT_EQ(rail.objectName(), QStringLiteral("editDetailsRail"));
}

} // namespace
} // namespace exosnap::ui::widgets
