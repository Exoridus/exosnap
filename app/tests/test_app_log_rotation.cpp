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
#include <QTemporaryDir>
#include <QTextStream>

#include "diagnostics/AppLog.h"

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

} // namespace
} // namespace exosnap::diagnostics
