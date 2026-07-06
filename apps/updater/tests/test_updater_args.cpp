// Unit tests for the updater command-line parser.

#include <gtest/gtest.h>

#include "UpdaterArgs.h"

namespace {

using exosnap::update::InstallMode;
using exosnap::update::UpdateChannel;

QStringList FullArgLine() {
    return QStringList{
        QStringLiteral("exosnap-updater.exe"),
        QStringLiteral("--channel"),         QStringLiteral("preview"),
        QStringLiteral("--install-mode"),    QStringLiteral("portable"),
        QStringLiteral("--install-dir"),     QStringLiteral("C:/Apps/ExoSnap"),
        QStringLiteral("--app-pid"),         QStringLiteral("4321"),
        QStringLiteral("--current-version"), QStringLiteral("0.8.1"),
        QStringLiteral("--base-url"),        QStringLiteral("http://localhost:8080"),
        QStringLiteral("--preview-state"),   QStringLiteral("amber"),
    };
}

TEST(UpdaterArgs, FullArgLineParses) {
    auto args = ParseUpdaterArgs(FullArgLine());
    ASSERT_TRUE(args.has_value());
    EXPECT_EQ(args->channel, UpdateChannel::Preview);
    EXPECT_EQ(args->install_mode, InstallMode::Portable);
    EXPECT_EQ(args->install_dir, QStringLiteral("C:/Apps/ExoSnap"));
    EXPECT_EQ(args->app_pid, 4321u);
    EXPECT_EQ(args->current_version, QStringLiteral("0.8.1"));
    EXPECT_EQ(args->base_url, QStringLiteral("http://localhost:8080"));
    EXPECT_EQ(args->preview_state, QStringLiteral("amber"));
}

TEST(UpdaterArgs, MissingInstallDirInPortableModeIsRejected) {
    QStringList argv{
        QStringLiteral("exosnap-updater.exe"),
        QStringLiteral("--install-mode"), QStringLiteral("portable"),
        QStringLiteral("--current-version"), QStringLiteral("0.8.1"),
    };
    auto args = ParseUpdaterArgs(argv);
    EXPECT_FALSE(args.has_value());
}

TEST(UpdaterArgs, InstalledModeDoesNotRequireInstallDir) {
    QStringList argv{
        QStringLiteral("exosnap-updater.exe"),
        QStringLiteral("--install-mode"), QStringLiteral("installed"),
    };
    auto args = ParseUpdaterArgs(argv);
    ASSERT_TRUE(args.has_value());
    EXPECT_EQ(args->install_mode, InstallMode::Installed);
}

TEST(UpdaterArgs, ChannelParsing) {
    {
        QStringList argv{QStringLiteral("u"), QStringLiteral("--channel"),
                         QStringLiteral("stable"), QStringLiteral("--install-mode"),
                         QStringLiteral("installed")};
        auto a = ParseUpdaterArgs(argv);
        ASSERT_TRUE(a.has_value());
        EXPECT_EQ(a->channel, UpdateChannel::Stable);
    }
    {
        QStringList argv{QStringLiteral("u"), QStringLiteral("--channel"),
                         QStringLiteral("bogus"), QStringLiteral("--install-mode"),
                         QStringLiteral("installed")};
        auto a = ParseUpdaterArgs(argv);
        EXPECT_FALSE(a.has_value());
    }
}

TEST(UpdaterArgs, PidParsing) {
    {
        QStringList argv{QStringLiteral("u"), QStringLiteral("--install-mode"),
                         QStringLiteral("installed"), QStringLiteral("--app-pid"),
                         QStringLiteral("99999")};
        auto a = ParseUpdaterArgs(argv);
        ASSERT_TRUE(a.has_value());
        EXPECT_EQ(a->app_pid, 99999u);
    }
    {
        QStringList argv{QStringLiteral("u"), QStringLiteral("--install-mode"),
                         QStringLiteral("installed"), QStringLiteral("--app-pid"),
                         QStringLiteral("notanumber")};
        auto a = ParseUpdaterArgs(argv);
        EXPECT_FALSE(a.has_value());
    }
    {
        // Absent --app-pid means the app is not running (0).
        QStringList argv{QStringLiteral("u"), QStringLiteral("--install-mode"),
                         QStringLiteral("installed")};
        auto a = ParseUpdaterArgs(argv);
        ASSERT_TRUE(a.has_value());
        EXPECT_EQ(a->app_pid, 0u);
    }
}

TEST(UpdaterArgs, MissingValueForFlagIsRejected) {
    QStringList argv{QStringLiteral("u"), QStringLiteral("--install-mode")};
    auto a = ParseUpdaterArgs(argv);
    EXPECT_FALSE(a.has_value());
}

TEST(UpdaterArgs, InvalidPreviewStateIsRejected) {
    QStringList argv{QStringLiteral("u"), QStringLiteral("--install-mode"),
                     QStringLiteral("installed"), QStringLiteral("--preview-state"),
                     QStringLiteral("teal")};
    auto a = ParseUpdaterArgs(argv);
    EXPECT_FALSE(a.has_value());
}

} // namespace
