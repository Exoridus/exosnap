#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QWidget>

#include "ExoSnapBuildInfo.h"
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

class AboutOverlayTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(AboutOverlayTest, RendersInWindowNotAsNativeDialog) {
    ui::dialogs::AboutOverlay overlay;

    // The About surface must live inside the app window — it is a plain QWidget
    // overlay, never a separate native QDialog/top-level OS window.
    EXPECT_EQ(qobject_cast<QDialog*>(&overlay), nullptr);
    EXPECT_NE(overlay.findChild<QFrame*>(QStringLiteral("aboutCard")), nullptr);
}

TEST_F(AboutOverlayTest, ShowsRealBuildMetadata) {
    ui::dialogs::AboutOverlay overlay;

    auto* version = overlay.findChild<QLabel*>(QStringLiteral("aboutValueVersion"));
    auto* build_config = overlay.findChild<QLabel*>(QStringLiteral("aboutValueBuild"));
    auto* commit = overlay.findChild<QLabel*>(QStringLiteral("aboutValueCommit"));
    auto* author = overlay.findChild<QLabel*>(QStringLiteral("aboutValueAuthor"));

    ASSERT_NE(version, nullptr);
    ASSERT_NE(build_config, nullptr);
    ASSERT_NE(commit, nullptr);
    ASSERT_NE(author, nullptr);

    EXPECT_EQ(version->text(), QString::fromLatin1(build::kVersion));
    EXPECT_EQ(build_config->text(), QString::fromLatin1(EXOSNAP_BUILD_CONFIG));
    // Commit and author use rich-text hyperlinks, so text() returns the full HTML;
    // verify the raw value is embedded rather than comparing the full HTML string.
    EXPECT_TRUE(commit->text().contains(QString::fromLatin1(build::kGitCommit)));
    EXPECT_TRUE(author->text().contains(QStringLiteral("Exoridus")));

    // Real metadata, not placeholders.
    EXPECT_FALSE(commit->text().isEmpty());
    EXPECT_FALSE(version->text().isEmpty());
}

TEST_F(AboutOverlayTest, GitHubButtonUsesConfiguredRepositoryUrl) {
    ui::dialogs::AboutOverlay overlay;

    auto* github = overlay.findChild<QPushButton*>(QStringLiteral("aboutGitHubButton"));
    ASSERT_NE(github, nullptr);
    EXPECT_EQ(github->property("url").toString(), QStringLiteral("https://github.com/Exoridus/exosnap"));
}

// v10: About card has three ghost/quiet action buttons (GitHub, Copy details, Release notes).
// None may carry the primary role — dismissal is via × (top-right), Escape, or backdrop click.
TEST_F(AboutOverlayTest, InfoCardHasNoReleaseNotesButtonWithPrimaryRole) {
    ui::dialogs::AboutOverlay overlay;

    auto* card = overlay.findChild<QFrame*>(QStringLiteral("aboutCard"));
    ASSERT_NE(card, nullptr);

    // The Release notes button is present but must not carry the primary role.
    auto* release_btn = overlay.findChild<QPushButton*>(QStringLiteral("aboutReleaseNotesButton"));
    ASSERT_NE(release_btn, nullptr);
    EXPECT_NE(release_btn->property("role").toString(), QStringLiteral("primary"));

    EXPECT_NE(overlay.findChild<QPushButton*>(QStringLiteral("aboutCopyButton")), nullptr);
    EXPECT_NE(overlay.findChild<QPushButton*>(QStringLiteral("aboutCloseButton")), nullptr);
}

TEST_F(AboutOverlayTest, ReleaseNotesButtonPresent) {
    ui::dialogs::AboutOverlay overlay;

    // v10: Release notes button is part of the info card's action row.
    auto* release_btn = overlay.findChild<QPushButton*>(QStringLiteral("aboutReleaseNotesButton"));
    ASSERT_NE(release_btn, nullptr);
    EXPECT_EQ(release_btn->text(), QStringLiteral("Release notes"));
}

TEST_F(AboutOverlayTest, EmbeddedUpdatePanelPresent) {
    ui::dialogs::AboutOverlay overlay;

    // The overlay keeps an UpdateSettingsPanel for MainWindow update-service wiring;
    // it is hidden from view but must be accessible via updatePanel().
    auto* panel = overlay.updatePanel();
    ASSERT_NE(panel, nullptr);

    // The panel is also findable via object hierarchy.
    auto* found = overlay.findChild<ui::dialogs::UpdateSettingsPanel*>(QStringLiteral("aboutUpdatePanel"));
    EXPECT_EQ(found, panel);

    // The panel must not be visible — it is an internal wiring node only.
    EXPECT_FALSE(panel->isVisible());
}

// v10: Channel row replaces the old QT row in the metadata table.
TEST_F(AboutOverlayTest, ChannelRowPresent) {
    ui::dialogs::AboutOverlay overlay;

    // A CHANNEL row must be present and non-empty.
    auto* channel_label = overlay.findChild<QLabel*>(QStringLiteral("aboutValueChannel"));
    ASSERT_NE(channel_label, nullptr);
    EXPECT_FALSE(channel_label->text().isEmpty());

    // Default value is "Stable" (no MainWindow seed yet).
    EXPECT_EQ(channel_label->text(), QStringLiteral("Stable"));
}

// v10: QT row must not appear — it was replaced by the Channel row.
TEST_F(AboutOverlayTest, QtVersionRowAbsent) {
    ui::dialogs::AboutOverlay overlay;

    auto* qt_label = overlay.findChild<QLabel*>(QStringLiteral("aboutValueQt"));
    EXPECT_EQ(qt_label, nullptr);
}

TEST_F(AboutOverlayTest, SetChannelHintUpdatesChannelRow) {
    ui::dialogs::AboutOverlay overlay;

    overlay.setChannelHint(QStringLiteral("Preview"));
    auto* channel_label = overlay.findChild<QLabel*>(QStringLiteral("aboutValueChannel"));
    ASSERT_NE(channel_label, nullptr);
    EXPECT_EQ(channel_label->text(), QStringLiteral("Preview"));
}

TEST_F(AboutOverlayTest, UpdateStatusLineHiddenByDefault) {
    ui::dialogs::AboutOverlay overlay;

    auto* status_line = overlay.findChild<QLabel*>(QStringLiteral("aboutUpdateStatusLine"));
    ASSERT_NE(status_line, nullptr);
    // Before any setUpdateStatusText() call the line must be hidden.
    EXPECT_TRUE(status_line->isHidden());
}

TEST_F(AboutOverlayTest, SetUpdateStatusTextShowsAndHidesLine) {
    ui::dialogs::AboutOverlay overlay;

    auto* status_line = overlay.findChild<QLabel*>(QStringLiteral("aboutUpdateStatusLine"));
    ASSERT_NE(status_line, nullptr);

    overlay.setUpdateStatusText(QStringLiteral("Up to date."));
    EXPECT_FALSE(status_line->isHidden());
    EXPECT_EQ(status_line->text(), QStringLiteral("Up to date."));

    overlay.setUpdateStatusText(QString());
    EXPECT_TRUE(status_line->isHidden());
}

TEST_F(AboutOverlayTest, DismissButtonIsXSymbol) {
    ui::dialogs::AboutOverlay overlay;
    auto* close_btn = overlay.findChild<QPushButton*>(QStringLiteral("aboutCloseButton"));
    ASSERT_NE(close_btn, nullptr);
    // Compact top-right dismiss button uses × rather than the word "Close"
    EXPECT_EQ(close_btn->text(), QString::fromLatin1("\xd7"));
}

TEST_F(AboutOverlayTest, NoPrimaryRoleCloseButton) {
    ui::dialogs::AboutOverlay overlay;
    // The dominant primary-role Close button was replaced by the compact × dismiss button
    for (auto* btn : overlay.findChildren<QPushButton*>())
        EXPECT_NE(btn->property("role").toString(), QStringLiteral("primary"));
}

TEST_F(AboutOverlayTest, CommitRowValueIsClickableLink) {
    ui::dialogs::AboutOverlay overlay;
    auto* commit_label = overlay.findChild<QLabel*>(QStringLiteral("aboutValueCommit"));
    ASSERT_NE(commit_label, nullptr);
    EXPECT_TRUE(commit_label->textInteractionFlags().testFlag(Qt::LinksAccessibleByMouse));
    EXPECT_TRUE(commit_label->openExternalLinks());
}

TEST_F(AboutOverlayTest, AuthorRowValueIsClickableLink) {
    ui::dialogs::AboutOverlay overlay;
    auto* author_label = overlay.findChild<QLabel*>(QStringLiteral("aboutValueAuthor"));
    ASSERT_NE(author_label, nullptr);
    EXPECT_TRUE(author_label->textInteractionFlags().testFlag(Qt::LinksAccessibleByMouse));
    EXPECT_TRUE(author_label->openExternalLinks());
}

TEST_F(AboutOverlayTest, OpenThenCloseTogglesOpenState) {
    // Parent to a (hidden) host so the overlay tracks its geometry; the open state
    // is derived from the overlay's own show/hide flag, not ancestor visibility.
    QWidget host;
    auto* overlay = new ui::dialogs::AboutOverlay(&host);

    EXPECT_FALSE(overlay->isOpen());

    overlay->openOverlay();
    EXPECT_TRUE(overlay->isOpen());

    overlay->closeOverlay();
    EXPECT_FALSE(overlay->isOpen());
}

TEST_F(AboutOverlayTest, CloseButtonDismissesAndEmitsClosed) {
    QWidget host;
    auto* overlay = new ui::dialogs::AboutOverlay(&host);
    overlay->openOverlay();
    ASSERT_TRUE(overlay->isOpen());

    int closed_count = 0;
    QObject::connect(overlay, &ui::dialogs::AboutOverlay::closed, overlay, [&closed_count]() { ++closed_count; });

    auto* close_btn = overlay->findChild<QPushButton*>(QStringLiteral("aboutCloseButton"));
    ASSERT_NE(close_btn, nullptr);
    close_btn->click();

    EXPECT_FALSE(overlay->isOpen());
    EXPECT_EQ(closed_count, 1);
}

} // namespace
} // namespace exosnap
