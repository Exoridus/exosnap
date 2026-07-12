// The engine JSONL is the canonical structured stream. These tests pin the two
// schema guarantees the support channel depends on: every record carries the
// configured base fields (the launch session id), and the on-disk file rotates
// (appends + bounded) rather than being truncated on every launch.

#include <gtest/gtest.h>

#include <recorder_core/logging/logging.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::filesystem::path UniqueTempFile(const char* stem) {
    const auto dir =
        std::filesystem::temp_directory_path() /
        ("exosnap_logtest_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" + stem);
    std::filesystem::create_directories(dir);
    return dir / "engine.jsonl";
}

} // namespace

TEST(LoggingSchema, BaseFieldsAreStampedOntoEveryRecord) {
    const auto path = UniqueTempFile("basefields");
    std::filesystem::remove(path);

    recorder_core::logging::LoggerConfig config;
    config.filePath = path;
    config.baseFields = {{"session", "launch-abc-123"}};
    recorder_core::logging::initialize(config);

    const recorder_core::logging::LogField field{"target", "monitor-1"};
    recorder_core::logging::log(recorder_core::logging::LogLevel::Info, "record", "record.start", {&field, 1});
    recorder_core::logging::shutdown();

    const std::string contents = ReadAll(path);
    EXPECT_NE(contents.find("\"session\":\"launch-abc-123\""), std::string::npos)
        << "the base session field must appear in fields{}; got: " << contents;
    EXPECT_NE(contents.find("\"target\":\"monitor-1\""), std::string::npos);
    EXPECT_NE(contents.find("\"message\":\"record.start\""), std::string::npos);
}

TEST(LoggingSchema, PerCallFieldWinsOverBaseFieldOnCollision) {
    const auto path = UniqueTempFile("collision");
    std::filesystem::remove(path);

    recorder_core::logging::LoggerConfig config;
    config.filePath = path;
    config.baseFields = {{"session", "base"}};
    recorder_core::logging::initialize(config);

    const recorder_core::logging::LogField field{"session", "override"};
    recorder_core::logging::log(recorder_core::logging::LogLevel::Info, "c", "e", {&field, 1});
    recorder_core::logging::shutdown();

    const std::string contents = ReadAll(path);
    EXPECT_NE(contents.find("\"session\":\"override\""), std::string::npos) << contents;
    EXPECT_EQ(contents.find("\"session\":\"base\""), std::string::npos);
}

TEST(LoggingSchema, FileRotatesAtASmallThreshold) {
    const auto path = UniqueTempFile("rotation");
    // spdlog's rotating sink names the backup by inserting the index before the
    // extension: engine.jsonl -> engine.1.jsonl (not engine.jsonl.1).
    const auto backup = path.parent_path() / "engine.1.jsonl";
    std::filesystem::remove(path);
    std::filesystem::remove(backup);

    recorder_core::logging::LoggerConfig config;
    config.filePath = path;
    config.maxFileBytes = 512; // tiny, so a handful of lines forces a rotation
    config.maxFileCount = 3;
    recorder_core::logging::initialize(config);

    for (int i = 0; i < 50; ++i) {
        recorder_core::logging::log(recorder_core::logging::LogLevel::Info, "component.name",
                                    "a reasonably sized message so lines exceed the threshold quickly");
    }
    recorder_core::logging::shutdown();

    EXPECT_TRUE(std::filesystem::exists(backup))
        << "a second (rotated) file must appear once the live file exceeds maxFileBytes";
}
