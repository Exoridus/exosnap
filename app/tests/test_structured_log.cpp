// logEvent() is the app-side entry point for structured events. It must forward
// ONLY to the engine JSONL: the EngineLogBridge sink is the single source of the
// flattened text line, so one logEvent must produce exactly one AppLog entry (no
// double) and one engine record. And because the engine's minimumLevel is Info, a
// Debug event must vanish entirely rather than reach either stream.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QString>

#include "diagnostics/AppLog.h"
#include "diagnostics/EngineLogBridge.h"
#include "diagnostics/StructuredLog.h"

#include <exosnap/engine/logging/logging.h>

namespace exosnap {
namespace {

QCoreApplication* EnsureApplication() {
    if (auto* existing = QCoreApplication::instance())
        return existing;
    static int argc = 1;
    static char app_name[] = "structured_log_tests";
    static char* argv[] = {app_name, nullptr};
    static QCoreApplication app(argc, argv);
    return &app;
}

class StructuredLogTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
        diagnostics::AppLog::init();
    }

    void SetUp() override {
        InitializeEngineLogging();
    }

    void TearDown() override {
        ShutdownEngineLogging();
    }

    static int CountEntries(const QString& category, const QString& needle) {
        int count = 0;
        for (const auto& entry : diagnostics::AppLog::history()) {
            if (entry.category == category && entry.message.contains(needle))
                ++count;
        }
        return count;
    }

    static int CountEngineRecords(const std::string& message) {
        int count = 0;
        for (const auto& record : exosnap::engine::logging::snapshot_ring_buffer()) {
            if (record.message == message)
                ++count;
        }
        return count;
    }
};

TEST_F(StructuredLogTest, EmitsExactlyOneAppLogEntryAndOneEngineRecord) {
    diagnostics::logEvent(diagnostics::LogSeverity::Info, "record", "record.start.unique-token", {{"backend", "dxgi"}});

    EXPECT_EQ(CountEntries(QStringLiteral("record"), QStringLiteral("record.start.unique-token")), 1)
        << "logEvent must not double-write: the bridge is the only source of the text line";
    EXPECT_EQ(CountEngineRecords("record.start.unique-token"), 1);
}

TEST_F(StructuredLogTest, StructuredFieldsSurviveInTheTextLine) {
    diagnostics::logEvent(diagnostics::LogSeverity::Warning, "encoder", "encoder.init.token",
                          {{"codec", "av1"}, {"preset", "p5"}});

    const auto history = diagnostics::AppLog::history();
    bool found = false;
    for (const auto& entry : history) {
        if (entry.category == QStringLiteral("encoder") &&
            entry.message.contains(QStringLiteral("encoder.init.token"))) {
            found = true;
            EXPECT_EQ(entry.severity, diagnostics::LogSeverity::Warning);
            EXPECT_TRUE(entry.message.contains(QStringLiteral("codec=av1")));
            EXPECT_TRUE(entry.message.contains(QStringLiteral("preset=p5")));
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(StructuredLogTest, DebugEventIsDroppedAtInfoMinimumLevel) {
    const int before = CountEngineRecords("record.debug.dropped-token");
    diagnostics::logEvent(diagnostics::LogSeverity::Debug, "record", "record.debug.dropped-token");

    EXPECT_EQ(CountEntries(QStringLiteral("record"), QStringLiteral("record.debug.dropped-token")), 0);
    EXPECT_EQ(CountEngineRecords("record.debug.dropped-token"), before);
}

} // namespace
} // namespace exosnap
