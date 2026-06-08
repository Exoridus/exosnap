#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QPlainTextEdit>
#include <QPushButton>

#include "pages/LogsPage.h"

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "logs_page_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class LogsPageTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(LogsPageTest, ContainedViewerAndCoreActionsExist) {
    LogsPage page;

    auto* viewer = page.findChild<QPlainTextEdit*>(QStringLiteral("logViewer"));
    ASSERT_NE(viewer, nullptr);
    EXPECT_TRUE(viewer->isReadOnly());
    // Monospace, contained surface — no line wrapping so columns stay readable.
    EXPECT_EQ(viewer->lineWrapMode(), QPlainTextEdit::NoWrap);

    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("logRefreshBtn")), nullptr);
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("logOpenFolderBtn")), nullptr);
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("logCopyBtn")), nullptr);
}

TEST_F(LogsPageTest, NoFakeLevelFiltersArePresent) {
    LogsPage page;

    // The AppLog text has no per-line level field, so the page must not present All/Info/Issues
    // filters. Guard against accidental reintroduction of a fake filter bar.
    for (auto* btn : page.findChildren<QPushButton*>()) {
        const QString text = btn->text();
        EXPECT_NE(text, QStringLiteral("Info"));
        EXPECT_NE(text, QStringLiteral("Issues"));
        EXPECT_NE(text, QStringLiteral("All"));
    }
}

TEST_F(LogsPageTest, CopyDisabledWhenNoLogContent) {
    LogsPage page;

    auto* viewer = page.findChild<QPlainTextEdit*>(QStringLiteral("logViewer"));
    auto* copy = page.findChild<QPushButton*>(QStringLiteral("logCopyBtn"));
    ASSERT_NE(viewer, nullptr);
    ASSERT_NE(copy, nullptr);

    // No AppLogInit() in this test → no log file → empty viewer → Copy stays disabled.
    if (viewer->toPlainText().isEmpty())
        EXPECT_FALSE(copy->isEnabled());
}

} // namespace
} // namespace exosnap
