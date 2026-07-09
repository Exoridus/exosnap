// The engine's logger used to stay uninitialised in production: every log() call
// returned early, so the capture path's decisions never reached the user. These tests
// drive the real bridge — InitializeEngineLogging() plus a genuine
// recorder_core::logging::log() call — and assert the record lands in AppLog.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QString>

#include "diagnostics/AppLog.h"
#include "diagnostics/EngineLogBridge.h"

#include <recorder_core/logging/logging.h>

namespace exosnap {
namespace {

QCoreApplication* EnsureApplication() {
    if (auto* existing = QCoreApplication::instance())
        return existing;
    static int argc = 1;
    static char app_name[] = "engine_log_bridge_tests";
    static char* argv[] = {app_name, nullptr};
    static QCoreApplication app(argc, argv);
    return &app;
}

class EngineLogBridgeTest : public ::testing::Test {
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

    // Newest matching entry for a category, or nullopt.
    static std::optional<diagnostics::LogEntry> FindLast(const QString& category) {
        const auto entries = diagnostics::AppLog::history();
        for (auto it = entries.crbegin(); it != entries.crend(); ++it) {
            if (it->category == category)
                return *it;
        }
        return std::nullopt;
    }
};

TEST_F(EngineLogBridgeTest, EngineRecordsReachTheApplicationLog) {
    const recorder_core::logging::LogField field{"mode", "hdr10-native"};
    recorder_core::logging::log(recorder_core::logging::LogLevel::Info, "capture.hdr", "resolved", {&field, 1});

    const auto entry = FindLast(QStringLiteral("capture.hdr"));
    ASSERT_TRUE(entry.has_value()) << "an engine record must appear in AppLog";
    EXPECT_EQ(entry->severity, diagnostics::LogSeverity::Info);
    EXPECT_TRUE(entry->message.contains(QStringLiteral("resolved")));
    EXPECT_TRUE(entry->message.contains(QStringLiteral("mode=hdr10-native")))
        << "structured fields carry the meaning and must survive flattening";
}

TEST_F(EngineLogBridgeTest, EngineSeveritiesMapOntoApplicationSeverities) {
    recorder_core::logging::log(recorder_core::logging::LogLevel::Warn, "capture.warn", "degraded");
    recorder_core::logging::log(recorder_core::logging::LogLevel::Error, "capture.err", "lost");

    const auto warn = FindLast(QStringLiteral("capture.warn"));
    const auto err = FindLast(QStringLiteral("capture.err"));
    ASSERT_TRUE(warn.has_value());
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(warn->severity, diagnostics::LogSeverity::Warning);
    EXPECT_EQ(err->severity, diagnostics::LogSeverity::Error);
}

// After shutdown the sink is detached, so a late engine record cannot reach a
// half-destroyed AppLog during application teardown.
TEST_F(EngineLogBridgeTest, RecordsAfterShutdownDoNotReachTheApplicationLog) {
    ShutdownEngineLogging();
    recorder_core::logging::log(recorder_core::logging::LogLevel::Info, "capture.after", "late");
    EXPECT_FALSE(FindLast(QStringLiteral("capture.after")).has_value());

    InitializeEngineLogging(); // restore for TearDown symmetry
}

} // namespace
} // namespace exosnap
