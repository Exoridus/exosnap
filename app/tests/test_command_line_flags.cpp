#include "cli/CommandLineFlags.h"

#include <gtest/gtest.h>

#include <QSet>
#include <QString>
#include <QStringList>

namespace exosnap::cli {
namespace {

QStringList Cmd(std::initializer_list<const char*> args) {
    QStringList list{QStringLiteral("exosnap.exe")};
    for (const char* arg : args)
        list << QString::fromLatin1(arg);
    return list;
}

// ---------------------------------------------------------------------------
// The defect this exists for
//
// Every parser iterates the full argv and skips what it does not own, so a
// misspelled option was skipped by all of them: the harness ran, reported
// success, and never performed the check that was asked for.
// ---------------------------------------------------------------------------

TEST(CommandLineFlags, MisspelledOptionIsRejected) {
    QString error;
    EXPECT_FALSE(ValidateCommandLine(
        Cmd({"--auto-record", "--target", "monitor", "--duration", "4", "--capture-frmae-at", "2"}), &error));
    EXPECT_TRUE(error.contains(QStringLiteral("--capture-frmae-at"))) << error.toStdString();
    // The near-miss hint is what turns the rejection into a fix.
    EXPECT_TRUE(error.contains(QStringLiteral("--capture-frame-at"))) << error.toStdString();
}

TEST(CommandLineFlags, UnknownOptionWithNoNeighbourIsStillRejected) {
    QString error;
    EXPECT_FALSE(ValidateCommandLine(Cmd({"--zzz-not-a-flag"}), &error));
    EXPECT_FALSE(error.isEmpty());
}

// ---------------------------------------------------------------------------
// What must keep working
// ---------------------------------------------------------------------------

TEST(CommandLineFlags, RealAutoRecordInvocationIsAccepted) {
    QString error;
    EXPECT_TRUE(ValidateCommandLine(Cmd({"--auto-record", "--target", "monitor", "--duration", "6",
                                         "--capture-frame-at", "3", "--audio-rows", "sys"}),
                                    &error))
        << error.toStdString();
}

// The combined record-then-export run: two parsers, one argument list. Each
// skips the other's options, which is exactly why the check has to know both.
TEST(CommandLineFlags, CombinedAutoRecordAndAutoEditIsAccepted) {
    QString error;
    EXPECT_TRUE(ValidateCommandLine(
        Cmd({"--auto-record", "--target", "monitor", "--duration", "8", "--audio-rows", "sys", "--auto-edit",
             "--auto-edit-media", "clip.mkv", "--auto-edit-report", "report.json", "--auto-edit-trim", "0.1,0.9"}),
        &error))
        << error.toStdString();
}

TEST(CommandLineFlags, HarnessAndServiceInvocationsAreAccepted) {
    QString error;
    EXPECT_TRUE(ValidateCommandLine(Cmd({"--visual-test", "out.png", "--visual-page", "1", "--visual-test-size",
                                         "1600x1000", "--record-visual-menu"}),
                                    &error))
        << error.toStdString();
    EXPECT_TRUE(ValidateCommandLine(Cmd({"--live-verify-control", "run-12345678"}), &error)) << error.toStdString();
    EXPECT_TRUE(ValidateCommandLine(Cmd({"--verify-update-reinstall", "--relaunch-page", "settings",
                                         "--reenable-present-diag", "--update-base-url", "https://example.invalid/f"}),
                                    &error))
        << error.toStdString();
}

// Qt's own options are single-dash and consumed by QGuiApplication. Rejecting
// them here would break every offscreen test run.
TEST(CommandLineFlags, QtSingleDashOptionsArePassedThrough) {
    QString error;
    EXPECT_TRUE(
        ValidateCommandLine(Cmd({"-platform", "offscreen", "-style", "Basic", "-qmljsdebugger=port:1234"}), &error))
        << error.toStdString();
}

// A value is skipped as a value, never re-examined as an option -- otherwise a
// path or a title that begins with two dashes would be rejected as unknown.
TEST(CommandLineFlags, ValueThatLooksLikeAnOptionIsNotParsedAsOne) {
    QString error;
    EXPECT_TRUE(ValidateCommandLine(Cmd({"--target-window-title", "--not-a-flag"}), &error)) << error.toStdString();
}

TEST(CommandLineFlags, DoubleDashEndsOptionParsing) {
    QString error;
    EXPECT_TRUE(ValidateCommandLine(Cmd({"--smoke-test", "--", "--whatever-comes-after"}), &error))
        << error.toStdString();
}

// The three withdrawn preview-mode options stay registered on purpose: the
// auto-record parser owns their message, which names the replacement. If the
// generic validator claimed them first, that guidance would be lost.
TEST(CommandLineFlags, WithdrawnPreviewFlagsPassTheRegistryAndAreRefusedByTheirOwnParser) {
    QString error;
    EXPECT_TRUE(ValidateCommandLine(Cmd({"--auto-record", "--enable-preview"}), &error)) << error.toStdString();
    EXPECT_TRUE(ValidateCommandLine(Cmd({"--auto-record", "--screenshot-path", "shot.png"}), &error))
        << error.toStdString();
    EXPECT_TRUE(ValidateCommandLine(Cmd({"--auto-record", "--capture-frame-in-ready"}), &error)) << error.toStdString();
}

// ---------------------------------------------------------------------------
// Table hygiene
// ---------------------------------------------------------------------------

TEST(CommandLineFlags, TableHasNoDuplicatesAndIsWellFormed) {
    std::size_t count = 0;
    const KnownFlag* flags = KnownCommandLineFlags(&count);
    ASSERT_GT(count, 50u);

    QSet<QString> seen;
    for (std::size_t i = 0; i < count; ++i) {
        const QString name = QString::fromLatin1(flags[i].name);
        EXPECT_TRUE(name.startsWith(QStringLiteral("--"))) << name.toStdString();
        EXPECT_FALSE(seen.contains(name)) << "duplicate: " << name.toStdString();
        seen.insert(name);
    }
}

TEST(CommandLineFlags, EveryRegisteredFlagValidates) {
    std::size_t count = 0;
    const KnownFlag* flags = KnownCommandLineFlags(&count);
    for (std::size_t i = 0; i < count; ++i) {
        QStringList args{QStringLiteral("exosnap.exe"), QString::fromLatin1(flags[i].name)};
        if (flags[i].arity == FlagArity::Value)
            args << QStringLiteral("value");
        QString error;
        EXPECT_TRUE(ValidateCommandLine(args, &error)) << flags[i].name << ": " << error.toStdString();
    }
}

} // namespace
} // namespace exosnap::cli
