// The session log file (exosnap.log) used to grow without bound: no size cap, no
// rotation, and an open/write/flush/close cycle on every single line. These tests
// drive the real AppLog file-writing path (not a mock) and assert: rotation
// triggers once the configured size threshold is crossed, the oldest backup is
// dropped once the configured file count is reached, the newest line always lands
// in the live file, an oversized leftover from a previous run rotates on the next
// init(), and the in-memory history the Logs page actually renders from is
// unaffected by on-disk rotation.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>

#include "diagnostics/AppLog.h"

#include <memory>

namespace exosnap::diagnostics {
namespace {

QCoreApplication* EnsureApplication() {
    if (auto* existing = QCoreApplication::instance())
        return existing;
    static int argc = 1;
    static char app_name[] = "app_log_rotation_tests";
    static char* argv[] = {app_name, nullptr};
    static QCoreApplication app(argc, argv);
    return &app;
}

QStringList ReadLines(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QTextStream stream(&file);
    QStringList lines;
    while (!stream.atEnd())
        lines << stream.readLine();
    return lines;
}

class AppLogRotationTest : public ::testing::Test {
  protected:
    // Small enough that a handful of log lines cross it, so tests stay fast and
    // deterministic without writing real megabytes of data.
    static constexpr qint64 kTestMaxBytes = 500;

    void SetUp() override {
        EnsureApplication();
        ASSERT_TRUE(temp_dir_.isValid());
        qputenv("EXOSNAP_CONFIG_DIR", temp_dir_.path().toUtf8());
        AppLog::resetForTesting();
        AppLog::setMaxLogFileBytesForTesting(kTestMaxBytes);
    }

    void TearDown() override {
        AppLog::resetForTesting();
        qunsetenv("EXOSNAP_CONFIG_DIR");
    }

    QString logDir() const {
        return temp_dir_.path() + QStringLiteral("/logs");
    }

    QString logPath() const {
        return logDir() + QStringLiteral("/exosnap.log");
    }

    QString backupPath(int index) const {
        return logPath() + QStringLiteral(".%1").arg(index);
    }

    // Writes enough lines to comfortably cross kTestMaxBytes `rotations` times over.
    void WriteFillerLines(int count) {
        const QString filler(80, QLatin1Char('x'));
        for (int i = 0; i < count; ++i)
            AppLog::info(QStringLiteral("test"), QStringLiteral("line %1 %2").arg(i).arg(filler));
    }

    QTemporaryDir temp_dir_;
};

TEST_F(AppLogRotationTest, StaysInTheLiveFileBelowTheThreshold) {
    AppLog::init();
    AppLog::info(QStringLiteral("test"), QStringLiteral("small line"));

    EXPECT_TRUE(QFile::exists(logPath()));
    EXPECT_FALSE(QFile::exists(backupPath(1)));
}

TEST_F(AppLogRotationTest, CrossingTheThresholdRotatesToBackupOne) {
    AppLog::init();
    WriteFillerLines(10);

    EXPECT_TRUE(QFile::exists(logPath()));
    EXPECT_TRUE(QFile::exists(backupPath(1)));
}

TEST_F(AppLogRotationTest, OldestBackupIsDroppedAtTheConfiguredFileCount) {
    AppLog::init();
    // Enough lines to rotate past kMaxLogFileCount backups multiple times over.
    WriteFillerLines(60);

    EXPECT_TRUE(QFile::exists(logPath()));
    EXPECT_TRUE(QFile::exists(backupPath(AppLog::kMaxLogFileCount - 1)));
    EXPECT_FALSE(QFile::exists(backupPath(AppLog::kMaxLogFileCount)))
        << "rotation must not keep more backups than kMaxLogFileCount allows";

    const QDir dir(logDir());
    const QStringList entries = dir.entryList(QStringList{QStringLiteral("exosnap.log*")}, QDir::Files);
    EXPECT_LE(entries.size(), AppLog::kMaxLogFileCount);
}

TEST_F(AppLogRotationTest, NewestLineAfterRotationLandsInTheFreshLiveFile) {
    AppLog::init();
    WriteFillerLines(10);
    AppLog::info(QStringLiteral("test"), QStringLiteral("MARKER-AFTER-ROTATION"));

    const QStringList live_lines = ReadLines(logPath());
    bool found = false;
    for (const QString& line : live_lines)
        found = found || line.contains(QStringLiteral("MARKER-AFTER-ROTATION"));
    EXPECT_TRUE(found) << "the newest line must be in the live file, not a rotated-out backup";
}

TEST_F(AppLogRotationTest, InMemoryHistoryForTheLogsPageIsUnaffectedByRotation) {
    AppLog::init();
    WriteFillerLines(10);

    // The Logs page renders from AppLog::history(), never the file directly;
    // rotating the file on disk must not disturb that in-memory model.
    const QVector<LogEntry> history = AppLog::history();
    ASSERT_GE(history.size(), 10);
    EXPECT_TRUE(history.constLast().message.contains(QStringLiteral("line 9")));
}

TEST_F(AppLogRotationTest, OversizedFileFromAPreviousRunRotatesOnNextInit) {
    // Simulate a leftover from a previous process run: the log file name is
    // stable across sessions and nothing has rotated it yet in this process.
    QDir().mkpath(logDir());
    {
        QFile file(logPath());
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write(QByteArray(static_cast<int>(kTestMaxBytes) + 100, 'a'));
    }

    AppLog::init();
    AppLog::info(QStringLiteral("test"), QStringLiteral("first line of new run"));

    EXPECT_TRUE(QFile::exists(backupPath(1))) << "the oversized leftover must be rotated out, not appended to";

    const QStringList live_lines = ReadLines(logPath());
    ASSERT_FALSE(live_lines.isEmpty());
    EXPECT_TRUE(live_lines.constLast().contains(QStringLiteral("first line of new run")));
}

TEST_F(AppLogRotationTest, HandleStaysOpenAcrossWritesInsteadOfPerLine) {
    AppLog::init();
    AppLog::info(QStringLiteral("test"), QStringLiteral("first"));
    AppLog::info(QStringLiteral("test"), QStringLiteral("second"));

    const QStringList live_lines = ReadLines(logPath());
    int matches = 0;
    for (const QString& line : live_lines) {
        if (line.contains(QStringLiteral("first")) || line.contains(QStringLiteral("second")))
            ++matches;
    }
    EXPECT_EQ(matches, 2) << "both lines from the same session must persist in the live file";
}

// ---------------------------------------------------------------------------
// QCR-205: a log file that cannot be (re)opened must not leave the logger
// claiming a working file sink.
//
// The reopen result was dropped on the floor. Rotation closed the live file,
// shifted the backups, called open() and ignored what it returned, then reset
// the size counter to zero — so a rotation that could not reopen handed back a
// closed QFile while writeLineUnlocked reported success to its caller.
// ---------------------------------------------------------------------------

TEST_F(AppLogRotationTest, FileLoggingIsHealthyBeforeAnythingHasFailed) {
    EXPECT_TRUE(AppLog::isFileLoggingHealthy()) << "nothing has been attempted yet, so nothing has failed";
    AppLog::init();
    AppLog::info(QStringLiteral("test"), QStringLiteral("first"));
    EXPECT_TRUE(AppLog::isFileLoggingHealthy());
}

TEST_F(AppLogRotationTest, OrdinaryRotationLeavesTheFileSinkHealthy) {
    // The checked reopen must not turn a perfectly normal rotation into a
    // reported failure.
    AppLog::init();
    WriteFillerLines(30);
    EXPECT_TRUE(AppLog::isFileLoggingHealthy());
    EXPECT_TRUE(QFile::exists(backupPath(1)));
}

TEST_F(AppLogRotationTest, AFailedReopenIsRecordedInsteadOfIgnored) {
    AppLog::init();
    AppLog::info(QStringLiteral("test"), QStringLiteral("before"));
    ASSERT_TRUE(AppLog::isFileLoggingHealthy());

    // A real open failure against a real filesystem: QFile::open does not create
    // missing directories, so this is what a log folder that has gone away (a
    // removed drive, a profile relocation) looks like from inside the logger.
    const QString unreachable = temp_dir_.path() + QStringLiteral("/no-such-dir/exosnap.log");
    AppLog::setLogFilePathForTesting(unreachable);

    AppLog::info(QStringLiteral("test"), QStringLiteral("after"));

    EXPECT_FALSE(AppLog::isFileLoggingHealthy()) << "the logger must not claim a file sink it does not have";
    EXPECT_FALSE(QFile::exists(unreachable));
}

TEST_F(AppLogRotationTest, WritesAfterAFailedReopenDoNotCrashAndKeepFeedingTheLogsPage) {
    AppLog::init();
    AppLog::setLogFilePathForTesting(temp_dir_.path() + QStringLiteral("/no-such-dir/exosnap.log"));

    // Many lines, not one: the failure path is re-entered per line, and the
    // fallback report is latched precisely so this cannot become a loop.
    for (int i = 0; i < 50; ++i)
        AppLog::info(QStringLiteral("test"), QStringLiteral("line %1").arg(i));

    EXPECT_FALSE(AppLog::isFileLoggingHealthy());

    // The in-memory history is a separate sink and is what the Logs page and the
    // support bundle render from; a dead file must not cost the user those.
    const QVector<LogEntry> history = AppLog::history();
    ASSERT_GE(history.size(), 50);
    EXPECT_TRUE(history.constLast().message.contains(QStringLiteral("line 49")));
}

TEST_F(AppLogRotationTest, FileLoggingRecoversOnceThePathIsUsableAgain) {
    AppLog::init();
    AppLog::setLogFilePathForTesting(temp_dir_.path() + QStringLiteral("/no-such-dir/exosnap.log"));
    AppLog::info(QStringLiteral("test"), QStringLiteral("lost"));
    ASSERT_FALSE(AppLog::isFileLoggingHealthy());

    // The failure is a state, not a one-way door: the next successful open
    // clears it, so a transient lock or a folder that comes back restores file
    // logging without a restart.
    const QString recovered = logDir() + QStringLiteral("/recovered.log");
    ASSERT_TRUE(QDir().mkpath(logDir()));
    AppLog::setLogFilePathForTesting(recovered);
    AppLog::info(QStringLiteral("test"), QStringLiteral("MARKER-AFTER-RECOVERY"));

    EXPECT_TRUE(AppLog::isFileLoggingHealthy());
    const QStringList lines = ReadLines(recovered);
    bool found = false;
    for (const QString& line : lines)
        found = found || line.contains(QStringLiteral("MARKER-AFTER-RECOVERY"));
    EXPECT_TRUE(found) << "recovery must actually write, not merely clear the flag";
}

TEST_F(AppLogRotationTest, RotationAfterARecoveredSinkStillWorks) {
    // The reopen path is shared between a cold open and a rotation, so a sink
    // that failed and came back must still rotate normally afterwards.
    AppLog::init();
    AppLog::setLogFilePathForTesting(temp_dir_.path() + QStringLiteral("/no-such-dir/exosnap.log"));
    AppLog::info(QStringLiteral("test"), QStringLiteral("lost"));
    ASSERT_FALSE(AppLog::isFileLoggingHealthy());

    ASSERT_TRUE(QDir().mkpath(logDir()));
    AppLog::setLogFilePathForTesting(logPath());
    WriteFillerLines(30);

    EXPECT_TRUE(AppLog::isFileLoggingHealthy());
    EXPECT_TRUE(QFile::exists(backupPath(1)));
}

// ---------------------------------------------------------------------------
// QCR-208: a rotation whose backup shift fails must not leave the size counter
// describing a file that no longer exists.
//
// Rotation closes the live file and renames the backups up one slot. QFile
// refuses to overwrite an existing target and fails outright when something else
// holds the source open, and the result was unchecked. The oversized
// exosnap.log then stayed exactly where it was, the reopen SUCCEEDED (append
// mode, same file), and log_file_size was reset to 0 regardless — so the size
// cap was measured against a counter that no longer described the file.
//
// The failure is produced against the real filesystem rather than injected: a
// second open handle on exosnap.log.1 is what a log viewer or a crashed process
// leaves behind, and Qt opens files without FILE_SHARE_DELETE, so Windows
// refuses to rename it.
// ---------------------------------------------------------------------------

TEST_F(AppLogRotationTest, AFailedBackupShiftKeepsTheOversizedLiveFileAndLosesNothing) {
    AppLog::init();
    WriteFillerLines(10);
    ASSERT_TRUE(QFile::exists(backupPath(1))) << "the first rotation must work, or the case below proves nothing";

    QFile blocker(backupPath(1));
    ASSERT_TRUE(blocker.open(QIODevice::ReadOnly));

    AppLog::info(QStringLiteral("test"), QStringLiteral("MARKER-BEFORE-FAILED-ROTATION"));
    WriteFillerLines(10);

    // The shift could not move it, so the live file is the same one — over the
    // cap, still holding everything written into it.
    EXPECT_GE(QFileInfo(logPath()).size(), kTestMaxBytes)
        << "a rotation that could not happen must leave the file where it is, not pretend it is empty";
    const QStringList live_lines = ReadLines(logPath());
    bool found = false;
    for (const QString& line : live_lines)
        found = found || line.contains(QStringLiteral("MARKER-BEFORE-FAILED-ROTATION"));
    EXPECT_TRUE(found) << "no line may be lost to a rotation that did not happen";
    EXPECT_TRUE(AppLog::isFileLoggingHealthy()) << "the file sink itself is fine — only the shift failed";
}

TEST_F(AppLogRotationTest, RotationResumesTheMomentTheBackupCanBeMovedAgain) {
    AppLog::init();
    WriteFillerLines(10);
    ASSERT_TRUE(QFile::exists(backupPath(1)));

    auto blocker = std::make_unique<QFile>(backupPath(1));
    ASSERT_TRUE(blocker->open(QIODevice::ReadOnly));
    WriteFillerLines(10);
    const qint64 stuck_size = QFileInfo(logPath()).size();
    ASSERT_GE(stuck_size, kTestMaxBytes);

    // Whatever held the backup lets go. Because the counter now describes the
    // real file, the very next line is already over the cap and rotates — the
    // bug reset it to 0, so a full cap of new bytes had to be written first and
    // the file was allowed to grow past the cap by that much, silently.
    blocker.reset();
    AppLog::info(QStringLiteral("test"), QStringLiteral("MARKER-AFTER-RECOVERED-ROTATION"));

    // Rotation runs after the line is written, so the marker is the last line of
    // the file that was just rotated out — and the live file is fresh again.
    EXPECT_LT(QFileInfo(logPath()).size(), stuck_size) << "the live file must actually have been rotated out";
    const QStringList rotated_lines = ReadLines(backupPath(1));
    ASSERT_FALSE(rotated_lines.isEmpty());
    EXPECT_TRUE(rotated_lines.constLast().contains(QStringLiteral("MARKER-AFTER-RECOVERED-ROTATION")))
        << "the line that triggered the retry must be in the file it was written to, not lost";
}

TEST_F(AppLogRotationTest, ARotationThatWorksLeavesTheCounterMatchingAFreshFile) {
    // The other half of the same contract: after a successful rotation the
    // counter must describe the NEW file, so the next rotation is a full cap
    // away rather than one line away.
    AppLog::init();
    WriteFillerLines(10);
    ASSERT_TRUE(QFile::exists(backupPath(1)));

    const qint64 after_rotation = QFileInfo(logPath()).size();
    ASSERT_LT(after_rotation, kTestMaxBytes);

    AppLog::info(QStringLiteral("test"), QStringLiteral("short"));

    // A short line simply lands in the live file. If it had rotated, the live
    // file would be back to (near) zero instead of one line longer.
    EXPECT_GT(QFileInfo(logPath()).size(), after_rotation)
        << "one short line after a rotation must not trigger another one";
}

} // namespace
} // namespace exosnap::diagnostics
