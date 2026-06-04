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
    EXPECT_EQ(commit->text(), QString::fromLatin1(build::kGitCommit));
    EXPECT_EQ(author->text(), QStringLiteral("Exoridus"));

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

TEST_F(AboutOverlayTest, NoFakeReleaseNotesAction) {
    ui::dialogs::AboutOverlay overlay;

    // No published release feed is configured, so the About card must not offer a Release notes
    // action. Copy details / GitHub / Close are the only real actions.
    for (auto* btn : overlay.findChildren<QPushButton*>())
        EXPECT_FALSE(btn->text().contains(QStringLiteral("Release"), Qt::CaseInsensitive));

    EXPECT_NE(overlay.findChild<QPushButton*>(QStringLiteral("aboutCopyButton")), nullptr);
    EXPECT_NE(overlay.findChild<QPushButton*>(QStringLiteral("aboutCloseButton")), nullptr);
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
