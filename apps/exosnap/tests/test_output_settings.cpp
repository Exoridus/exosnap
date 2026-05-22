#include <gtest/gtest.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <string>

#include <recorder_core/recorder_session.h>

#include "models/FilenameBuilder.h"
#include "models/OutputPathValidator.h"
#include "models/OutputSettingsModel.h"

namespace exosnap {

void ApplyOutputSettingsToRecorderConfig(recorder_core::RecorderConfig& config, const OutputSettingsModel& settings);

namespace {

std::time_t LocalTimestamp(int year, int month, int day, int hour, int minute, int second) {
    std::tm tm_local{};
    tm_local.tm_year = year - 1900;
    tm_local.tm_mon = month - 1;
    tm_local.tm_mday = day;
    tm_local.tm_hour = hour;
    tm_local.tm_min = minute;
    tm_local.tm_sec = second;
    tm_local.tm_isdst = -1;
    return std::mktime(&tm_local);
}

std::filesystem::path UniqueTempPath(const std::wstring& suffix) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           (L"exosnap_output_settings_" + std::to_wstring(now) + L"_" + suffix);
}

TEST(OutputSettingsTest, KnownTimestamp_MKV) {
    const std::time_t ts = LocalTimestamp(2024, 1, 1, 12, 0, 0);
    const auto filename = BuildFilename(L"exosnap_{date}_{time}", capability::Container::Matroska, ts);
    EXPECT_EQ(filename, L"exosnap_20240101_120000.mkv");
}

TEST(OutputSettingsTest, LiteralPattern_NoTokens) {
    const std::time_t ts = LocalTimestamp(2024, 1, 1, 12, 0, 0);
    const auto filename = BuildFilename(L"myrecord", capability::Container::Matroska, ts);
    EXPECT_EQ(filename, L"myrecord.mkv");
}

TEST(OutputSettingsTest, DateOnlyToken) {
    const std::time_t ts = LocalTimestamp(2024, 1, 1, 12, 0, 0);
    const auto filename = BuildFilename(L"rec_{date}", capability::Container::Matroska, ts);
    EXPECT_EQ(filename, L"rec_20240101.mkv");
}

TEST(OutputSettingsTest, TimeOnlyToken) {
    const std::time_t ts = LocalTimestamp(2024, 1, 1, 12, 0, 0);
    const auto filename = BuildFilename(L"rec_{time}", capability::Container::Matroska, ts);
    EXPECT_EQ(filename, L"rec_120000.mkv");
}

TEST(OutputSettingsTest, Mp4Extension) {
    const std::time_t ts = LocalTimestamp(2024, 1, 1, 12, 0, 0);
    const auto filename = BuildFilename(L"rec", capability::Container::Mp4, ts);
    EXPECT_EQ(filename, L"rec.mp4");
}

TEST(OutputSettingsTest, WebMExtension) {
    const std::time_t ts = LocalTimestamp(2024, 1, 1, 12, 0, 0);
    const auto filename = BuildFilename(L"rec", capability::Container::WebM, ts);
    EXPECT_EQ(filename, L"rec.webm");
}

TEST(OutputSettingsTest, InvalidCharsReplaced) {
    const std::time_t ts = LocalTimestamp(2024, 1, 1, 12, 0, 0);
    const auto filename = BuildFilename(L"rec<name>", capability::Container::Matroska, ts);
    EXPECT_EQ(filename, L"rec_name_.mkv");
}

TEST(OutputSettingsTest, UnknownTokenPreserved) {
    const std::time_t ts = LocalTimestamp(2024, 1, 1, 12, 0, 0);
    const auto filename = BuildFilename(L"{unknown}_rec", capability::Container::Matroska, ts);
    EXPECT_EQ(filename.rfind(L"{unknown}_rec", 0), 0u);
}

TEST(OutputSettingsTest, ValidTempDir) {
    const std::filesystem::path dir = UniqueTempPath(L"valid");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ASSERT_FALSE(ec);

    EXPECT_EQ(ValidateOutputFolder(dir), FolderValidationResult::Ok);

    std::filesystem::remove_all(dir, ec);
}

TEST(OutputSettingsTest, NonExistentNestedDir) {
    const std::filesystem::path base = UniqueTempPath(L"nested");
    const std::filesystem::path nested = base / L"a" / L"b" / L"c";

    std::error_code ec;
    std::filesystem::remove_all(base, ec);

    EXPECT_EQ(ValidateOutputFolder(nested), FolderValidationResult::Ok);
    EXPECT_TRUE(std::filesystem::exists(nested));

    std::filesystem::remove_all(base, ec);
}

TEST(OutputSettingsTest, EmptyPath) {
    EXPECT_EQ(ValidateOutputFolder(std::filesystem::path{}), FolderValidationResult::InvalidPath);
}

TEST(OutputSettingsTest, Defaults_FolderNotEmpty) {
    const OutputSettingsModel defaults = OutputSettingsModel::Defaults();
    EXPECT_FALSE(defaults.output_folder.empty());
}

TEST(OutputSettingsTest, Defaults_ContainerIsMatroska) {
    const OutputSettingsModel defaults = OutputSettingsModel::Defaults();
    EXPECT_EQ(defaults.container, capability::Container::Matroska);
}

TEST(OutputSettingsTest, Defaults_AudioCodecIsOpus) {
    const OutputSettingsModel defaults = OutputSettingsModel::Defaults();
    EXPECT_EQ(defaults.audio_codec, capability::AudioCodec::Opus);
}

TEST(OutputSettingsTest, Defaults_NamingPatternCorrect) {
    const OutputSettingsModel defaults = OutputSettingsModel::Defaults();
    EXPECT_EQ(defaults.naming_pattern, L"exosnap_{date}_{time}");
}

TEST(OutputSettingsTest, ApplyOutputAudioCodec_UsesOpusWhenSelected) {
    recorder_core::RecorderConfig config{};
    config.audio_codec = recorder_core::AudioCodec::AacMf;

    OutputSettingsModel settings = OutputSettingsModel::Defaults();
    settings.audio_codec = capability::AudioCodec::Opus;

    ApplyOutputSettingsToRecorderConfig(config, settings);
    EXPECT_EQ(config.audio_codec, recorder_core::AudioCodec::Opus);
}

TEST(OutputSettingsTest, ApplyOutputAudioCodec_UsesAacWhenSelected) {
    recorder_core::RecorderConfig config{};
    config.audio_codec = recorder_core::AudioCodec::Opus;

    OutputSettingsModel settings = OutputSettingsModel::Defaults();
    settings.audio_codec = capability::AudioCodec::AacMf;

    ApplyOutputSettingsToRecorderConfig(config, settings);
    EXPECT_EQ(config.audio_codec, recorder_core::AudioCodec::AacMf);
}

} // namespace
} // namespace exosnap
