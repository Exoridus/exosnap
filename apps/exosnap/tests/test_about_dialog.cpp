#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <QPushButton>

#include "ExoSnapBuildInfo.h"
#include "ui/dialogs/AboutDialog.h"

#ifndef EXOSNAP_BUILD_CONFIG
#define EXOSNAP_BUILD_CONFIG "Unknown"
#endif

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "about_dialog_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class AboutDialogTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(AboutDialogTest, ShowsRealBuildMetadata) {
    ui::dialogs::AboutDialog dlg;

    auto* version = dlg.findChild<QLabel*>(QStringLiteral("aboutValueVersion"));
    auto* build_config = dlg.findChild<QLabel*>(QStringLiteral("aboutValueBuild"));
    auto* commit = dlg.findChild<QLabel*>(QStringLiteral("aboutValueCommit"));
    auto* author = dlg.findChild<QLabel*>(QStringLiteral("aboutValueAuthor"));

    ASSERT_NE(version, nullptr);
    ASSERT_NE(build_config, nullptr);
    ASSERT_NE(commit, nullptr);
    ASSERT_NE(author, nullptr);

    EXPECT_EQ(version->text(), QString::fromLatin1(build::kVersion));
    EXPECT_EQ(build_config->text(), QString::fromLatin1(EXOSNAP_BUILD_CONFIG));
    EXPECT_EQ(commit->text(), QString::fromLatin1(build::kGitCommit));
    EXPECT_EQ(author->text(), QStringLiteral("Exoridus"));

    // Real metadata, not placeholders.
    EXPECT_FALSE(commit->text().isEmpty());
    EXPECT_FALSE(version->text().isEmpty());
}

TEST_F(AboutDialogTest, GitHubButtonUsesConfiguredRepositoryUrl) {
    ui::dialogs::AboutDialog dlg;

    auto* github = dlg.findChild<QPushButton*>(QStringLiteral("aboutGitHubButton"));
    ASSERT_NE(github, nullptr);
    EXPECT_EQ(github->property("url").toString(), QStringLiteral("https://github.com/Exoridus/exosnap"));
}

TEST_F(AboutDialogTest, NoFakeReleaseNotesAction) {
    ui::dialogs::AboutDialog dlg;

    // No published release feed is configured, so the About card must not offer a Release notes
    // action. Copy details / GitHub / Close are the only real actions.
    for (auto* btn : dlg.findChildren<QPushButton*>())
        EXPECT_FALSE(btn->text().contains(QStringLiteral("Release"), Qt::CaseInsensitive));

    EXPECT_NE(dlg.findChild<QPushButton*>(QStringLiteral("aboutCopyButton")), nullptr);
    EXPECT_NE(dlg.findChild<QPushButton*>(QStringLiteral("aboutCloseButton")), nullptr);
}

} // namespace
} // namespace exosnap
