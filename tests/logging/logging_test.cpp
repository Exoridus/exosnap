#include <exosnap/engine/logging/logging.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace {

using exosnap::engine::logging::LogField;
using exosnap::engine::logging::LoggerConfig;
using exosnap::engine::logging::LogLevel;

class LoggingTest : public ::testing::Test {
  protected:
    void SetUp() override {
        exosnap::engine::logging::shutdown();
    }

    void TearDown() override {
        exosnap::engine::logging::shutdown();
        for (const auto& p : tempFiles_) {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
    }

    std::filesystem::path makeTempPath() {
        // A per-process random token disambiguates concurrent processes and
        // worktrees that share the one system temp dir; without it, a fixed
        // `exosnap_test_<n>.jsonl` name races (both truncate + write the same
        // file, corrupting the JSON one side reads back).
        static const unsigned token = [] {
            std::random_device rd;
            return rd();
        }();
        auto name = "exosnap_logtest_" + std::to_string(token) + "_" + std::to_string(counter_++) + ".jsonl";
        auto path = std::filesystem::temp_directory_path() / name;
        tempFiles_.push_back(path);
        return path;
    }

    static std::string readFile(const std::filesystem::path& path) {
        std::ifstream file(path);
        std::string content;
        std::string line;
        while (std::getline(file, line)) {
            if (!content.empty()) {
                content += '\n';
            }
            content += line;
        }
        return content;
    }

    std::vector<std::filesystem::path> tempFiles_;
    inline static int counter_ = 0;
};

TEST_F(LoggingTest, ToStringReturnsStableLevelNames) {
    EXPECT_EQ(exosnap::engine::logging::to_string(LogLevel::Trace), "trace");
    EXPECT_EQ(exosnap::engine::logging::to_string(LogLevel::Debug), "debug");
    EXPECT_EQ(exosnap::engine::logging::to_string(LogLevel::Info), "info");
    EXPECT_EQ(exosnap::engine::logging::to_string(LogLevel::Warn), "warn");
    EXPECT_EQ(exosnap::engine::logging::to_string(LogLevel::Error), "error");
    EXPECT_EQ(exosnap::engine::logging::to_string(LogLevel::Critical), "critical");
}

TEST_F(LoggingTest, InitializeRejectsZeroRingCapacity) {
    LoggerConfig cfg;
    cfg.filePath = makeTempPath();
    cfg.ringCapacity = 0;
    EXPECT_THROW(exosnap::engine::logging::initialize(cfg), std::invalid_argument);
}

TEST_F(LoggingTest, RingBufferKeepsNewestRecordsInChronologicalOrder) {
    auto path = makeTempPath();
    LoggerConfig cfg;
    cfg.filePath = path;
    cfg.ringCapacity = 2;
    cfg.minimumLevel = LogLevel::Info;
    exosnap::engine::logging::initialize(cfg);

    exosnap::engine::logging::log(LogLevel::Info, "comp", "first");
    exosnap::engine::logging::log(LogLevel::Info, "comp", "second");
    exosnap::engine::logging::log(LogLevel::Info, "comp", "third");

    auto snapshot = exosnap::engine::logging::snapshot_ring_buffer();
    ASSERT_EQ(snapshot.size(), 2);
    EXPECT_EQ(snapshot[0].message, "second");
    EXPECT_EQ(snapshot[1].message, "third");
}

TEST_F(LoggingTest, MinimumLevelFiltersBothSinks) {
    auto path = makeTempPath();
    LoggerConfig cfg;
    cfg.filePath = path;
    cfg.minimumLevel = LogLevel::Warn;
    exosnap::engine::logging::initialize(cfg);

    exosnap::engine::logging::log(LogLevel::Info, "comp", "info_msg");
    exosnap::engine::logging::log(LogLevel::Error, "comp", "error_msg");

    auto snapshot = exosnap::engine::logging::snapshot_ring_buffer();
    ASSERT_EQ(snapshot.size(), 1);
    EXPECT_EQ(snapshot[0].level, LogLevel::Error);
    EXPECT_EQ(snapshot[0].message, "error_msg");

    exosnap::engine::logging::shutdown();

    auto content = readFile(path);
    auto j = nlohmann::json::parse(content);
    EXPECT_EQ(j["level"], "error");
    EXPECT_EQ(j["message"], "error_msg");
}

TEST_F(LoggingTest, FileSinkWritesStructuredJsonLines) {
    auto path = makeTempPath();
    LoggerConfig cfg;
    cfg.filePath = path;
    cfg.minimumLevel = LogLevel::Info;
    exosnap::engine::logging::initialize(cfg);

    std::vector<LogField> fields;
    fields.push_back({"key1", "value1"});
    fields.push_back({"key2", "value2"});
    exosnap::engine::logging::log(LogLevel::Info, "test_comp", "test message", fields);

    exosnap::engine::logging::shutdown();

    auto content = readFile(path);
    auto j = nlohmann::json::parse(content);

    EXPECT_EQ(j["level"], "info");
    EXPECT_EQ(j["component"], "test_comp");
    EXPECT_EQ(j["message"], "test message");
    EXPECT_TRUE(j.contains("timestamp_unix_ms"));
    EXPECT_TRUE(j["timestamp_unix_ms"].is_number());
    EXPECT_EQ(j["fields"]["key1"], "value1");
    EXPECT_EQ(j["fields"]["key2"], "value2");
}

TEST_F(LoggingTest, ReinitializeClearsPreviousRingBuffer) {
    auto pathA = makeTempPath();
    auto pathB = makeTempPath();

    LoggerConfig cfgA;
    cfgA.filePath = pathA;
    cfgA.ringCapacity = 10;
    exosnap::engine::logging::initialize(cfgA);
    exosnap::engine::logging::log(LogLevel::Info, "comp", "msg_a");

    EXPECT_EQ(exosnap::engine::logging::snapshot_ring_buffer().size(), 1);

    LoggerConfig cfgB;
    cfgB.filePath = pathB;
    cfgB.ringCapacity = 10;
    exosnap::engine::logging::initialize(cfgB);

    EXPECT_TRUE(exosnap::engine::logging::snapshot_ring_buffer().empty());

    exosnap::engine::logging::log(LogLevel::Info, "comp", "msg_b");
    EXPECT_EQ(exosnap::engine::logging::snapshot_ring_buffer().size(), 1);

    exosnap::engine::logging::shutdown();

    auto contentB = readFile(pathB);
    auto jB = nlohmann::json::parse(contentB);
    EXPECT_EQ(jB["message"], "msg_b");

    auto contentA = readFile(pathA);
    EXPECT_EQ(contentA.find("msg_b"), std::string::npos);
}

// Without a sink the engine's decisions never reach the host application: they land in
// a JSONL file nobody reads. The host installs one to mirror them into its own log.
TEST_F(LoggingTest, SinkReceivesEveryAcceptedRecord) {
    std::vector<exosnap::engine::logging::LogRecord> seen;

    LoggerConfig cfg;
    cfg.filePath = makeTempPath();
    cfg.minimumLevel = exosnap::engine::logging::LogLevel::Info;
    cfg.sink = [&seen](const exosnap::engine::logging::LogRecord& r) { seen.push_back(r); };
    exosnap::engine::logging::initialize(cfg);

    const LogField field{"mode", "hdr10-native"};
    exosnap::engine::logging::log(exosnap::engine::logging::LogLevel::Info, "capture", "resolved", {&field, 1});
    exosnap::engine::logging::log(exosnap::engine::logging::LogLevel::Debug, "capture", "below_minimum");

    exosnap::engine::logging::shutdown();

    ASSERT_EQ(seen.size(), 1u) << "records below the minimum level must not reach the sink";
    EXPECT_EQ(seen[0].component, "capture");
    EXPECT_EQ(seen[0].message, "resolved");
    ASSERT_EQ(seen[0].fields.size(), 1u);
    EXPECT_EQ(seen[0].fields[0].key, "mode");
    EXPECT_EQ(seen[0].fields[0].value, "hdr10-native");
}

// The sink runs outside the logger's lock, so a sink that logs again — or blocks on a
// mutex of its own — must not deadlock the thread that produced the record.
TEST_F(LoggingTest, SinkMayLogReentrantlyWithoutDeadlock) {
    int depth = 0;
    int calls = 0;

    LoggerConfig cfg;
    cfg.filePath = makeTempPath();
    cfg.sink = [&](const exosnap::engine::logging::LogRecord&) {
        ++calls;
        if (depth == 0) {
            ++depth;
            exosnap::engine::logging::log(exosnap::engine::logging::LogLevel::Info, "sink", "reentrant");
        }
    };
    exosnap::engine::logging::initialize(cfg);

    exosnap::engine::logging::log(exosnap::engine::logging::LogLevel::Info, "capture", "outer");
    exosnap::engine::logging::shutdown();

    EXPECT_EQ(calls, 2) << "the reentrant record must also reach the sink";
}

} // namespace
