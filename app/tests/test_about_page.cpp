#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QWidget>

#include <update/update_types.h>
#include <vector>

#include "ExoSnapBuildInfo.h"
#include "models/AboutInfo.h"
#include "pages/AboutPage.h"

#ifndef EXOSNAP_BUILD_CONFIG
#define EXOSNAP_BUILD_CONFIG "Unknown"
#endif

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "about_page_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

// Pumps the Qt event loop until `predicate` is true or `timeout_ms` elapses.
// Used to observe the async executable-hash step behind "Copy details"
// without a hard sleep.
template <typename Predicate> bool pumpUntil(Predicate&& predicate, int timeout_ms = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.hasExpired(timeout_ms))
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

class AboutPageTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

// ── Pure helper tests (no widget, no QApplication dependency) ────────────────

TEST(AboutPageHelpersTest, FormatBuildTimestampForDisplay_ValidIso8601) {
    EXPECT_EQ(models::FormatBuildTimestampForDisplay(QStringLiteral("2026-07-28T12:34:56Z")),
              QStringLiteral("2026-07-28 12:34 UTC"));
}

TEST(AboutPageHelpersTest, FormatBuildTimestampForDisplay_InvalidInputPassesThrough) {
    const QString raw = QStringLiteral("not-a-timestamp");
    EXPECT_EQ(models::FormatBuildTimestampForDisplay(raw), raw);
}

TEST(AboutPageHelpersTest, ResolveInstallModeLabel_InstalledWinsOverScoop) {
    EXPECT_EQ(models::ResolveInstallModeLabel(exosnap::update::InstallMode::Installed, /*is_scoop=*/true),
              QStringLiteral("MSI"));
    EXPECT_EQ(models::ResolveInstallModeLabel(exosnap::update::InstallMode::Installed, /*is_scoop=*/false),
              QStringLiteral("MSI"));
}

TEST(AboutPageHelpersTest, ResolveInstallModeLabel_ScoopBeatsPlainPortable) {
    EXPECT_EQ(models::ResolveInstallModeLabel(exosnap::update::InstallMode::Portable, /*is_scoop=*/true),
              QStringLiteral("Scoop"));
}

TEST(AboutPageHelpersTest, ResolveInstallModeLabel_PlainPortable) {
    EXPECT_EQ(models::ResolveInstallModeLabel(exosnap::update::InstallMode::Portable, /*is_scoop=*/false),
              QStringLiteral("Portable"));
}

TEST(AboutPageHelpersTest, BuildAboutCopyText_OfficialCleanBuild) {
    models::AboutCopyFields f;
    f.version = QStringLiteral("0.9.0-rc4");
    f.official_build = true;
    f.git_commit_full = QStringLiteral("23aa1d210f0b5f23fca4c53d1f83021dd4cf6428");
    f.build_timestamp_utc = QStringLiteral("2026-07-28T12:34:56Z");
    f.build_id = QStringLiteral("123456");
    f.configuration = QStringLiteral("Release");
    f.dirty_source_tree = false;
    f.install_mode_label = QStringLiteral("MSI");
    f.channel = QStringLiteral("Stable");
    f.executable_path = QStringLiteral("C:\\Program Files\\ExoSnap\\ExoSnap.exe");
    f.executable_sha256 = QString(64, QLatin1Char('a'));

    const QString expected = QStringLiteral("ExoSnap\n"
                                            "Version: 0.9.0-rc4\n"
                                            "Tag: v0.9.0-rc4\n"
                                            "Commit: 23aa1d210f0b5f23fca4c53d1f83021dd4cf6428\n"
                                            "Build time: 2026-07-28T12:34:56Z\n"
                                            "Build ID: 123456\n"
                                            "Architecture: x64\n"
                                            "Configuration: Release\n"
                                            "Official build: yes\n"
                                            "Install mode: MSI\n"
                                            "Update channel: Stable\n"
                                            "Executable: C:\\Program Files\\ExoSnap\\ExoSnap.exe\n"
                                            "Executable SHA-256: %1")
                                 .arg(QString(64, QLatin1Char('a')));

    EXPECT_EQ(models::BuildAboutCopyText(f), expected);
}

TEST(AboutPageHelpersTest, BuildAboutCopyText_UnofficialDirtyBuildOmitsTagAndAddsDirtyLine) {
    models::AboutCopyFields f;
    f.version = QStringLiteral("0.9.0-dev");
    f.official_build = false;
    f.git_commit_full = QStringLiteral("Unavailable");
    f.build_timestamp_utc = QStringLiteral("2026-07-28T00:00:00Z");
    f.build_id.clear(); // empty -> "(none)"
    f.configuration = QStringLiteral("Debug");
    f.dirty_source_tree = true;
    f.install_mode_label = QStringLiteral("Portable");
    f.channel = QStringLiteral("Preview");
    f.executable_path = QStringLiteral("C:\\dev\\exosnap.exe");
    f.executable_sha256 = QString(64, QLatin1Char('b'));

    const QString text = models::BuildAboutCopyText(f);

    EXPECT_TRUE(text.contains(QStringLiteral("Tag: (unofficial build)")));
    EXPECT_FALSE(text.contains(QStringLiteral("Tag: v0.9.0-dev")));
    EXPECT_TRUE(text.contains(QStringLiteral("Build ID: (none)")));
    EXPECT_TRUE(text.contains(QStringLiteral("Dirty source tree: yes")));
    EXPECT_TRUE(text.contains(QStringLiteral("Official build: no")));
}

TEST(AboutPageHelpersTest, BuildAboutCopyText_CleanBuildOmitsDirtyLine) {
    models::AboutCopyFields f;
    f.version = QStringLiteral("0.9.0-rc4");
    f.git_commit_full = QStringLiteral("deadbeef");
    f.build_timestamp_utc = QStringLiteral("2026-07-28T00:00:00Z");
    f.configuration = QStringLiteral("Release");
    f.dirty_source_tree = false;
    f.install_mode_label = QStringLiteral("Portable");
    f.channel = QStringLiteral("Stable");
    f.executable_path = QStringLiteral("exosnap.exe");
    f.executable_sha256 = QString(64, QLatin1Char('c'));

    EXPECT_FALSE(models::BuildAboutCopyText(f).contains(QStringLiteral("Dirty source tree")));
}

// ── AboutPage widget tests ────────────────────────────────────────────────────

// AboutPage is a plain QWidget (nav page), not a native QDialog or overlay.
TEST_F(AboutPageTest, IsPlainWidget) {
    pages::AboutPage page;

    EXPECT_EQ(qobject_cast<QDialog*>(&page), nullptr);
    EXPECT_NE(page.findChild<QFrame*>(QStringLiteral("aboutCard")), nullptr);
}

TEST_F(AboutPageTest, PermanentFieldsShowRealBuildMetadata) {
    pages::AboutPage page;

    auto* version = page.findChild<QLabel*>(QStringLiteral("aboutValueVersion"));
    auto* commit = page.findChild<QLabel*>(QStringLiteral("aboutValueCommit"));
    auto* built = page.findChild<QLabel*>(QStringLiteral("aboutValueBuilt"));
    auto* installation = page.findChild<QLabel*>(QStringLiteral("aboutValueInstallation"));
    auto* channel = page.findChild<QLabel*>(QStringLiteral("aboutValueChannel"));
    auto* author = page.findChild<QLabel*>(QStringLiteral("aboutValueAuthor"));

    ASSERT_NE(version, nullptr);
    ASSERT_NE(commit, nullptr);
    ASSERT_NE(built, nullptr);
    ASSERT_NE(installation, nullptr);
    ASSERT_NE(channel, nullptr);
    ASSERT_NE(author, nullptr);

    EXPECT_EQ(version->text(), QString::fromLatin1(build::kVersion));
    // Commit uses a rich-text hyperlink; verify the raw short SHA is embedded.
    EXPECT_TRUE(commit->text().contains(QString::fromLatin1(build::kGitCommit)));
    EXPECT_EQ(built->text(), models::FormatBuildTimestampForDisplay(QString::fromLatin1(build::kBuildTimestampUtc)));

    EXPECT_FALSE(installation->text().isEmpty());
    EXPECT_TRUE(installation->text() == QStringLiteral("Portable") || installation->text() == QStringLiteral("MSI") ||
                installation->text() == QStringLiteral("Scoop"));

    EXPECT_EQ(channel->text(), QStringLiteral("Stable"));
    EXPECT_TRUE(author->text().contains(QStringLiteral("Exoridus")));
}

// The old permanent BUILD (Debug/Release) row is gone -- it now only surfaces
// conditionally as the "Debug build" notice.
TEST_F(AboutPageTest, PermanentBuildRowIsGone) {
    pages::AboutPage page;
    EXPECT_EQ(page.findChild<QLabel*>(QStringLiteral("aboutValueBuild")), nullptr);
}

TEST_F(AboutPageTest, CommitRowLinkMatchesFullShaAvailability) {
    pages::AboutPage page;
    auto* commit_label = page.findChild<QLabel*>(QStringLiteral("aboutValueCommit"));
    ASSERT_NE(commit_label, nullptr);

    const QString full = QString::fromLatin1(build::kGitCommitFull);
    if (full == QStringLiteral("Unavailable")) {
        EXPECT_FALSE(commit_label->textInteractionFlags().testFlag(Qt::LinksAccessibleByMouse));
    } else {
        EXPECT_TRUE(commit_label->textInteractionFlags().testFlag(Qt::LinksAccessibleByMouse));
        EXPECT_TRUE(commit_label->openExternalLinks());
        EXPECT_TRUE(commit_label->text().contains(full));
        EXPECT_TRUE(commit_label->text().contains(QStringLiteral("github.com/Exoridus/exosnap/commit/")));
    }
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

// Conditional notices must track the actual build flags of this test binary,
// not a hardcoded expectation -- CI / official builds and local dev builds
// disagree on kOfficialBuild and EXOSNAP_BUILD_CONFIG.
TEST_F(AboutPageTest, ConditionalNoticesMatchBuildFlags) {
    pages::AboutPage page;

    auto* unofficial = page.findChild<QLabel*>(QStringLiteral("aboutNoticeUnofficial"));
    auto* debug = page.findChild<QLabel*>(QStringLiteral("aboutNoticeDebug"));
    auto* dirty = page.findChild<QLabel*>(QStringLiteral("aboutNoticeDirty"));

    EXPECT_EQ(unofficial != nullptr, !build::kOfficialBuild);
    EXPECT_EQ(debug != nullptr, QString::fromLatin1(EXOSNAP_BUILD_CONFIG) == QStringLiteral("Debug"));
    EXPECT_EQ(dirty != nullptr, build::kDirtySourceTree);

    if (unofficial != nullptr)
        EXPECT_EQ(unofficial->text(), QStringLiteral("Unofficial build"));
    if (debug != nullptr)
        EXPECT_EQ(debug->text(), QStringLiteral("Debug build"));
    if (dirty != nullptr)
        EXPECT_EQ(dirty->text(), QStringLiteral("Dirty source tree"));
}

// ── Copy details: async executable hash ───────────────────────────────────────

TEST_F(AboutPageTest, CopyDetailsProducesExpectedFormatWithRealExecutableHash) {
    pages::AboutPage page;
    page.setChannelHint(QStringLiteral("Preview"));

    auto* copy_btn = page.findChild<QPushButton*>(QStringLiteral("aboutCopyButton"));
    ASSERT_NE(copy_btn, nullptr);
    EXPECT_EQ(copy_btn->text(), QStringLiteral("Copy details"));

    std::vector<QString> emitted;
    QObject::connect(&page, &pages::AboutPage::copyDetailsFinished,
                     [&emitted](const QString& text) { emitted.push_back(text); });

    copy_btn->click();

    ASSERT_TRUE(pumpUntil([&]() { return !emitted.empty(); })) << "copyDetailsFinished did not fire in time";

    const QString& text = emitted.front();
    EXPECT_TRUE(text.startsWith(QStringLiteral("ExoSnap\n")));
    EXPECT_TRUE(text.contains(QStringLiteral("Version: %1").arg(QString::fromLatin1(build::kVersion))));
    EXPECT_TRUE(text.contains(QStringLiteral("Commit: %1").arg(QString::fromLatin1(build::kGitCommitFull))));
    EXPECT_TRUE(text.contains(QStringLiteral("Update channel: Preview")));
    EXPECT_TRUE(text.contains(QStringLiteral("Install mode: Portable")) ||
                text.contains(QStringLiteral("Install mode: MSI")) ||
                text.contains(QStringLiteral("Install mode: Scoop")));

    // The "running executable" under test IS the test binary itself -- hash it
    // independently here and require the copied text to carry the same digest.
    QFile exe(QCoreApplication::applicationFilePath());
    ASSERT_TRUE(exe.open(QIODevice::ReadOnly));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    ASSERT_TRUE(hash.addData(&exe));
    const QString expected_sha256 = QString::fromLatin1(hash.result().toHex());

    EXPECT_TRUE(text.contains(QStringLiteral("Executable SHA-256: %1").arg(expected_sha256)));

    // The button must return to its idle state once the async hash completes.
    EXPECT_EQ(copy_btn->text(), QStringLiteral("Copy details"));
    EXPECT_TRUE(copy_btn->isEnabled());
}

TEST_F(AboutPageTest, CopyDetailsSecondClickIsCachedAndConsistent) {
    pages::AboutPage page;
    auto* copy_btn = page.findChild<QPushButton*>(QStringLiteral("aboutCopyButton"));
    ASSERT_NE(copy_btn, nullptr);

    std::vector<QString> emitted;
    QObject::connect(&page, &pages::AboutPage::copyDetailsFinished,
                     [&emitted](const QString& text) { emitted.push_back(text); });

    copy_btn->click();
    ASSERT_TRUE(pumpUntil([&]() { return emitted.size() >= 1; }));

    copy_btn->click();
    // Cached: the second click must not spin up another hash computation, so it
    // resolves far faster than the first (bounded generously to stay non-flaky).
    ASSERT_TRUE(pumpUntil([&]() { return emitted.size() >= 2; }, /*timeout_ms=*/2000));

    EXPECT_EQ(emitted[0], emitted[1]);
}

TEST_F(AboutPageTest, RapidDoubleClickWhileHashingIsSwallowed) {
    pages::AboutPage page;
    auto* copy_btn = page.findChild<QPushButton*>(QStringLiteral("aboutCopyButton"));
    ASSERT_NE(copy_btn, nullptr);

    std::vector<QString> emitted;
    QObject::connect(&page, &pages::AboutPage::copyDetailsFinished,
                     [&emitted](const QString& text) { emitted.push_back(text); });

    // Two clicks back-to-back, synchronously, before the event loop runs at all:
    // QAbstractButton::click() emits clicked() synchronously, so the second call
    // must observe the in-flight guard and be swallowed rather than queuing a
    // second hash computation.
    copy_btn->click();
    copy_btn->click();

    ASSERT_TRUE(pumpUntil([&]() { return !emitted.empty(); }));
    // Give any (unexpected) second completion a chance to land before asserting.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

    EXPECT_EQ(emitted.size(), 1u);
}

} // namespace
} // namespace exosnap
