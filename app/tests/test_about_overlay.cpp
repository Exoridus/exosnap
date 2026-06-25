#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QWidget>

#include "ExoSnapBuildInfo.h"
#include "pages/AboutPage.h"
#include "ui/dialogs/AboutOverlay.h"
#include "ui/dialogs/UpdateSettingsPanel.h"

#ifndef EXOSNAP_BUILD_CONFIG
#define EXOSNAP_BUILD_CONFIG "Unknown"
#endif

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "about_overlay_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class AboutPageTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

// AboutPage is a plain QWidget (nav page), not a native QDialog or overlay.
TEST_F(AboutPageTest, IsPlainWidget) {
    pages::AboutPage page;

    EXPECT_EQ(qobject_cast<QDialog*>(&page), nullptr);
    EXPECT_NE(page.findChild<QFrame*>(QStringLiteral("aboutCard")), nullptr);
}

TEST_F(AboutPageTest, ShowsRealBuildMetadata) {
    pages::AboutPage page;

    auto* version = page.findChild<QLabel*>(QStringLiteral("aboutValueVersion"));
    auto* build_config = page.findChild<QLabel*>(QStringLiteral("aboutValueBuild"));
    auto* commit = page.findChild<QLabel*>(QStringLiteral("aboutValueCommit"));
    auto* author = page.findChild<QLabel*>(QStringLiteral("aboutValueAuthor"));

    ASSERT_NE(version, nullptr);
    ASSERT_NE(build_config, nullptr);
    ASSERT_NE(commit, nullptr);
    ASSERT_NE(author, nullptr);

    EXPECT_EQ(version->text(), QString::fromLatin1(build::kVersion));
    EXPECT_EQ(build_config->text(), QString::fromLatin1(EXOSNAP_BUILD_CONFIG));
    // Commit and author use rich-text hyperlinks; verify the raw value is embedded.
    EXPECT_TRUE(commit->text().contains(QString::fromLatin1(build::kGitCommit)));
    EXPECT_TRUE(author->text().contains(QStringLiteral("Exoridus")));

    EXPECT_FALSE(commit->text().isEmpty());
    EXPECT_FALSE(version->text().isEmpty());
}

TEST_F(AboutPageTest, GitHubButtonUsesConfiguredRepositoryUrl) {
    pages::AboutPage page;

    auto* github = page.findChild<QPushButton*>(QStringLiteral("aboutGitHubButton"));
    ASSERT_NE(github, nullptr);
    EXPECT_EQ(github->property("url").toString(), QStringLiteral("https://github.com/Exoridus/exosnap"));
}

// v10: About card has three ghost/quiet action buttons (GitHub, Copy details, Release notes).
// None may carry the primary role.
TEST_F(AboutPageTest, InfoCardHasNoButtonWithPrimaryRole) {
    pages::AboutPage page;

    auto* card = page.findChild<QFrame*>(QStringLiteral("aboutCard"));
    ASSERT_NE(card, nullptr);

    auto* release_btn = page.findChild<QPushButton*>(QStringLiteral("aboutReleaseNotesButton"));
    ASSERT_NE(release_btn, nullptr);
    EXPECT_NE(release_btn->property("role").toString(), QStringLiteral("primary"));

    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("aboutCopyButton")), nullptr);

    // No × close/dismiss button — About is a normal nav page, not an overlay.
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("aboutCloseButton")), nullptr);
}

TEST_F(AboutPageTest, ReleaseNotesButtonPresent) {
    pages::AboutPage page;

    auto* release_btn = page.findChild<QPushButton*>(QStringLiteral("aboutReleaseNotesButton"));
    ASSERT_NE(release_btn, nullptr);
    EXPECT_EQ(release_btn->text(), QStringLiteral("Release notes"));
}

// v10: Channel row in the metadata table (replaces the old QT row).
TEST_F(AboutPageTest, ChannelRowPresent) {
    pages::AboutPage page;

    auto* channel_label = page.findChild<QLabel*>(QStringLiteral("aboutValueChannel"));
    ASSERT_NE(channel_label, nullptr);
    EXPECT_FALSE(channel_label->text().isEmpty());

    // Default value is "Stable" before MainWindow calls setChannelHint().
    EXPECT_EQ(channel_label->text(), QStringLiteral("Stable"));
}

// v10: QT row must not appear — it was replaced by the Channel row.
TEST_F(AboutPageTest, QtVersionRowAbsent) {
    pages::AboutPage page;

    auto* qt_label = page.findChild<QLabel*>(QStringLiteral("aboutValueQt"));
    EXPECT_EQ(qt_label, nullptr);
}

TEST_F(AboutPageTest, SetChannelHintUpdatesChannelRow) {
    pages::AboutPage page;

    page.setChannelHint(QStringLiteral("Preview"));
    auto* channel_label = page.findChild<QLabel*>(QStringLiteral("aboutValueChannel"));
    ASSERT_NE(channel_label, nullptr);
    EXPECT_EQ(channel_label->text(), QStringLiteral("Preview"));
}

// v10: No update-status line in About — update info lives exclusively in Settings.
TEST_F(AboutPageTest, NoUpdateStatusLine) {
    pages::AboutPage page;

    auto* status_line = page.findChild<QLabel*>(QStringLiteral("aboutUpdateStatusLine"));
    EXPECT_EQ(status_line, nullptr);
}

TEST_F(AboutPageTest, CommitRowValueIsClickableLink) {
    pages::AboutPage page;
    auto* commit_label = page.findChild<QLabel*>(QStringLiteral("aboutValueCommit"));
    ASSERT_NE(commit_label, nullptr);
    EXPECT_TRUE(commit_label->textInteractionFlags().testFlag(Qt::LinksAccessibleByMouse));
    EXPECT_TRUE(commit_label->openExternalLinks());
}

TEST_F(AboutPageTest, AuthorRowValueIsClickableLink) {
    pages::AboutPage page;
    auto* author_label = page.findChild<QLabel*>(QStringLiteral("aboutValueAuthor"));
    ASSERT_NE(author_label, nullptr);
    EXPECT_TRUE(author_label->textInteractionFlags().testFlag(Qt::LinksAccessibleByMouse));
    EXPECT_TRUE(author_label->openExternalLinks());
}

TEST_F(AboutPageTest, NoPrimaryRoleButton) {
    pages::AboutPage page;
    for (auto* btn : page.findChildren<QPushButton*>())
        EXPECT_NE(btn->property("role").toString(), QStringLiteral("primary"));
}

// ── AboutOverlay shim tests ──────────────────────────────────────────────────
// AboutOverlay is now a thin hidden-panel holder (no overlay behavior).

class AboutOverlayShimTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(AboutOverlayShimTest, HoldsUpdatePanel) {
    ui::dialogs::AboutOverlay overlay;

    auto* panel = overlay.updatePanel();
    ASSERT_NE(panel, nullptr);

    auto* found = overlay.findChild<ui::dialogs::UpdateSettingsPanel*>(QStringLiteral("aboutUpdatePanel"));
    EXPECT_EQ(found, panel);

    // The panel must never be visible — it is an internal wiring node only.
    EXPECT_FALSE(panel->isVisible());
}

TEST_F(AboutOverlayShimTest, IsHiddenByDefault) {
    ui::dialogs::AboutOverlay overlay;
    EXPECT_TRUE(overlay.isHidden());
}

} // namespace
} // namespace exosnap
