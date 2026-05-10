#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <recorder_core/logging/logging.h>

#include <fstream>
#include <string>

namespace {

using recorder_core::logging::LogField;
using recorder_core::logging::LogLevel;
using recorder_core::logging::LoggerConfig;

class LoggingTest : public ::testing::Test {
protected:
    void SetUp() override {
        recorder_core::logging::shutdown();
    }

    void TearDown() override {
        recorder_core::logging::shutdown();
        for (const auto& p : tempFiles_) {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
    }

    std::filesystem::path makeTempPath() {
        auto name =
            "exosnap_test_" + std::to_string(counter_++) + ".jsonl";
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
    EXPECT_EQ(recorder_core::logging::to_string(LogLevel::Trace), "trace");
    EXPECT_EQ(recorder_core::logging::to_string(LogLevel::Debug), "debug");
    EXPECT_EQ(recorder_core::logging::to_string(LogLevel::Info), "info");
    EXPECT_EQ(recorder_core::logging::to_string(LogLevel::Warn), "warn");
    EXPECT_EQ(recorder_core::logging::to_string(LogLevel::Error), "error");
    EXPECT_EQ(recorder_core::logging::to_string(LogLevel::Critical),
              "critical");
}

TEST_F(LoggingTest, InitializeRejectsZeroRingCapacity) {
    LoggerConfig cfg;
    cfg.filePath = makeTempPath();
    cfg.ringCapacity = 0;
    EXPECT_THROW(recorder_core::logging::initialize(cfg),
                 std::invalid_argument);
}

TEST_F(LoggingTest, RingBufferKeepsNewestRecordsInChronologicalOrder) {
    auto path = makeTempPath();
    LoggerConfig cfg;
    cfg.filePath = path;
    cfg.ringCapacity = 2;
    cfg.minimumLevel = LogLevel::Info;
    recorder_core::logging::initialize(cfg);

    recorder_core::logging::log(LogLevel::Info, "comp", "first");
    recorder_core::logging::log(LogLevel::Info, "comp", "second");
    recorder_core::logging::log(LogLevel::Info, "comp", "third");

    auto snapshot = recorder_core::logging::snapshot_ring_buffer();
    ASSERT_EQ(snapshot.size(), 2);
    EXPECT_EQ(snapshot[0].message, "second");
    EXPECT_EQ(snapshot[1].message, "third");
}

TEST_F(LoggingTest, MinimumLevelFiltersBothSinks) {
    auto path = makeTempPath();
    LoggerConfig cfg;
    cfg.filePath = path;
    cfg.minimumLevel = LogLevel::Warn;
    recorder_core::logging::initialize(cfg);

    recorder_core::logging::log(LogLevel::Info, "comp", "info_msg");
    recorder_core::logging::log(LogLevel::Error, "comp", "error_msg");

    auto snapshot = recorder_core::logging::snapshot_ring_buffer();
    ASSERT_EQ(snapshot.size(), 1);
    EXPECT_EQ(snapshot[0].level, LogLevel::Error);
    EXPECT_EQ(snapshot[0].message, "error_msg");

    recorder_core::logging::shutdown();

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
    recorder_core::logging::initialize(cfg);

    std::vector<LogField> fields;
    fields.push_back({"key1", "value1"});
    fields.push_back({"key2", "value2"});
    recorder_core::logging::log(LogLevel::Info, "test_comp", "test message",
                                fields);

    recorder_core::logging::shutdown();

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
    recorder_core::logging::initialize(cfgA);
    recorder_core::logging::log(LogLevel::Info, "comp", "msg_a");

    EXPECT_EQ(recorder_core::logging::snapshot_ring_buffer().size(), 1);

    LoggerConfig cfgB;
    cfgB.filePath = pathB;
    cfgB.ringCapacity = 10;
    recorder_core::logging::initialize(cfgB);

    EXPECT_TRUE(recorder_core::logging::snapshot_ring_buffer().empty());

    recorder_core::logging::log(LogLevel::Info, "comp", "msg_b");
    EXPECT_EQ(recorder_core::logging::snapshot_ring_buffer().size(), 1);

    recorder_core::logging::shutdown();

    auto contentB = readFile(pathB);
    auto jB = nlohmann::json::parse(contentB);
    EXPECT_EQ(jB["message"], "msg_b");

    auto contentA = readFile(pathA);
    EXPECT_EQ(contentA.find("msg_b"), std::string::npos);
}

} // namespace
