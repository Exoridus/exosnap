#include <gtest/gtest.h>

#include <chrono>
#include <ctime>
#include <cwctype>
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

FilenameTargetContext WindowContext(const std::wstring& app = L"Brave", const std::wstring& title = L"Claude Design",
                                    const std::wstring& process = L"brave") {
    FilenameTargetContext context;
    context.app_name = app;
    context.window_title = title;
    context.process_name = process;
    context.target_name = title.empty() ? app : (app + L" - " + title);
    return context;
}

FilenameTargetContext DisplayContext() {
    FilenameTargetContext context;
    context.target_name = L"Desktop - Display 1";
    context.app_name = L"Desktop";
    context.window_title = L"Display 1";
    context.process_name = L"desktop";
    return context;
}

bool EqualPathElementCaseInsensitive(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
    const std::wstring a = lhs.native();
    const std::wstring b = rhs.native();
    if (a.size() != b.size()) {
        return false;
    }

    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) {
            return false;
        }
    }

    return true;
}

bool IsPathUnderFolder(const std::filesystem::path& path, const std::filesystem::path& folder) {
    const std::filesystem::path normalized_path = path.lexically_normal();
    const std::filesystem::path normalized_folder = folder.lexically_normal();

    auto path_it = normalized_path.begin();
    for (auto folder_it = normalized_folder.begin(); folder_it != normalized_folder.end(); ++folder_it, ++path_it) {
        if (path_it == normalized_path.end()) {
            return false;
        }
        if (!EqualPathElementCaseInsensitive(*path_it, *folder_it)) {
            return false;
        }
    }

    return true;
}

TEST(OutputSettingsTest, DateToken_UsesIsoDateFormat) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    const auto filename = BuildFilename(L"{date}", capability::Container::Matroska, ts, WindowContext());
    EXPECT_EQ(filename, L"2026-05-22.mkv");
}

TEST(OutputSettingsTest, TimeToken_UsesDashSeparatedTimeFormat) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    const auto filename = BuildFilename(L"{time}", capability::Container::Matroska, ts, WindowContext());
    EXPECT_EQ(filename, L"14-37-09.mkv");
}

TEST(OutputSettingsTest, DateTimeToken_UsesIsoDateTimeFormat) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    const auto filename = BuildFilename(L"{datetime}", capability::Container::Matroska, ts, WindowContext());
    EXPECT_EQ(filename, L"2026-05-22_14-37-09.mkv");
}

TEST(OutputSettingsTest, TimestampToken_UsesUnixTimestampInteger) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    const auto filename = BuildFilename(L"{timestamp}", capability::Container::Matroska, ts, WindowContext());
    EXPECT_EQ(filename, std::to_wstring(static_cast<long long>(ts)) + L".mkv");
}

TEST(OutputSettingsTest, SplitDateTimeTokens_ExpandIndividually) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    const auto filename =
        BuildFilename(L"{YYYY}_{YY}_{MM}_{DD}_{hh}_{mm}_{ss}", capability::Container::Matroska, ts, WindowContext());
    EXPECT_EQ(filename, L"2026_26_05_22_14_37_09.mkv");
}

TEST(OutputSettingsTest, TargetToken_IsReplacedFromContext) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    const auto filename = BuildFilename(L"{target}", capability::Container::Matroska, ts, WindowContext());
    EXPECT_EQ(filename, L"Brave - Claude Design.mkv");
}

TEST(OutputSettingsTest, AppAndApplicationTokens_AreAliases) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    const auto filename = BuildFilename(L"{app}_{application}", capability::Container::Matroska, ts, WindowContext());
    EXPECT_EQ(filename, L"Brave_Brave.mkv");
}

TEST(OutputSettingsTest, ProcessToken_IsReplacedFromContext) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    const auto filename = BuildFilename(L"{process}", capability::Container::Matroska, ts, WindowContext());
    EXPECT_EQ(filename, L"brave.mkv");
}

TEST(OutputSettingsTest, TitleToken_IsReplacedFromContext) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    const auto filename = BuildFilename(L"{title}", capability::Container::Matroska, ts, WindowContext());
    EXPECT_EQ(filename, L"Claude Design.mkv");
}

TEST(OutputSettingsTest, AppAndTitleTokens_AreSanitizedBeforeInsertion) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    const auto filename = BuildFilename(L"{app}_{title}_{date}", capability::Container::Matroska, ts,
                                        WindowContext(L"Br:ave", L"Clau/de|Design", L"brave"));
    EXPECT_EQ(filename, L"Br_ave_Clau_de_Design_2026-05-22.mkv");
}

TEST(OutputSettingsTest, LegacyOverloadWithoutContext_RemainsFunctional) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    const auto filename = BuildFilename(L"rec_{datetime}", capability::Container::Matroska, ts);
    EXPECT_EQ(filename, L"rec_2026-05-22_14-37-09.mkv");
}

TEST(OutputSettingsTest, Defaults_NamingPatternCorrect) {
    const OutputSettingsModel defaults = OutputSettingsModel::Defaults();
    EXPECT_EQ(defaults.naming_pattern, L"{datetime}_{app}_{title}");
}

TEST(OutputSettingsTest, EmptyTitleToken_CleansSeparatorArtifact) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    const auto filename =
        BuildFilename(L"{app}_{title}_{datetime}", capability::Container::Matroska, ts, WindowContext(L"Brave", L""));
    EXPECT_EQ(filename, L"Brave_2026-05-22_14-37-09.mkv");
}

TEST(OutputSettingsTest, IntentionalLiteralSeparatorRun_PreservedWhenTokensPresent) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    const auto filename = BuildFilename(L"{app}__{datetime}", capability::Container::Matroska, ts, WindowContext());
    EXPECT_EQ(filename, L"Brave__2026-05-22_14-37-09.mkv");
}

TEST(OutputSettingsTest, RelativePatternPath_CreatesSubfolder) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    const std::filesystem::path folder = UniqueTempPath(L"relative_path");

    const auto output =
        BuildOutputPath(folder, L"{app}/{datetime}", capability::Container::Matroska, ts, WindowContext());

    EXPECT_EQ(output.lexically_normal(), (folder / L"Brave" / L"2026-05-22_14-37-09.mkv").lexically_normal());
}

TEST(OutputSettingsTest, EmptySegmentFromToken_IsRemovedFromRelativePath) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    const std::filesystem::path folder = UniqueTempPath(L"empty_segment");

    const auto output = BuildOutputPath(folder, L"{app}/{title}/{datetime}", capability::Container::Matroska, ts,
                                        WindowContext(L"Brave", L""));

    EXPECT_EQ(output.lexically_normal(), (folder / L"Brave" / L"2026-05-22_14-37-09.mkv").lexically_normal());
}

TEST(OutputSettingsTest, ParentTraversalPattern_DoesNotEscapeOutputFolder) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    const std::filesystem::path folder = UniqueTempPath(L"dotdot_prefix");

    const auto output =
        BuildOutputPath(folder, L"../{app}/{datetime}", capability::Container::Matroska, ts, WindowContext());

    EXPECT_TRUE(IsPathUnderFolder(output, folder));
}

TEST(OutputSettingsTest, DrivePrefixPattern_DoesNotBecomeAbsoluteDrivePath) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    const std::filesystem::path folder = UniqueTempPath(L"drive_prefix");

    const auto output =
        BuildOutputPath(folder, L"C:/{app}/{datetime}", capability::Container::Matroska, ts, WindowContext());

    EXPECT_TRUE(IsPathUnderFolder(output, folder));
}

TEST(OutputSettingsTest, MidPathParentTraversal_DoesNotEscapeOutputFolder) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    const std::filesystem::path folder = UniqueTempPath(L"dotdot_mid");

    const auto output =
        BuildOutputPath(folder, L"{app}/../{datetime}", capability::Container::Matroska, ts, WindowContext());

    EXPECT_TRUE(IsPathUnderFolder(output, folder));
}

TEST(OutputSettingsTest, DisplayContext_TargetTokensRenderDesktopDefaults) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    const auto filename =
        BuildFilename(L"{target}_{app}_{title}_{process}", capability::Container::Matroska, ts, DisplayContext());
    EXPECT_EQ(filename, L"Desktop - Display 1_Desktop_Display 1_desktop.mkv");
}

TEST(OutputSettingsTest, LiteralPattern_NoTokens) {
    const std::time_t ts = LocalTimestamp(2024, 1, 1, 12, 0, 0);
    const auto filename = BuildFilename(L"myrecord", capability::Container::Matroska, ts);
    EXPECT_EQ(filename, L"myrecord.mkv");
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

TEST(OutputSettingsTest, UnknownTokenPreserved) {
    const std::time_t ts = LocalTimestamp(2024, 1, 1, 12, 0, 0);
    const auto filename = BuildFilename(L"{unknown}_rec", capability::Container::Matroska, ts);
    EXPECT_EQ(filename.rfind(L"{unknown}_rec", 0), 0u);
}

TEST(OutputSettingsTest, ReservedDeviceName_Literal_GetsUnderscore1Suffix) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    EXPECT_EQ(BuildFilename(L"CON", capability::Container::Matroska, ts), L"CON_1.mkv");
    EXPECT_EQ(BuildFilename(L"NUL", capability::Container::Matroska, ts), L"NUL_1.mkv");
}

TEST(OutputSettingsTest, ReservedDeviceName_CaseInsensitive_PreservesOriginalCase) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    EXPECT_EQ(BuildFilename(L"con", capability::Container::Matroska, ts), L"con_1.mkv");
    EXPECT_EQ(BuildFilename(L"nul", capability::Container::Matroska, ts), L"nul_1.mkv");
}

TEST(OutputSettingsTest, ReservedDeviceName_ViaToken_SanitizedAfterExpansion) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    const auto filename =
        BuildFilename(L"{app}", capability::Container::Matroska, ts, WindowContext(L"CON", L"Claude Design", L"con"));
    EXPECT_EQ(filename, L"CON_1.mkv");
}

TEST(OutputSettingsTest, ReservedDeviceName_ComAndLpt_SanitizedWithSuffix) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    EXPECT_EQ(BuildFilename(L"COM1", capability::Container::Matroska, ts), L"COM1_1.mkv");
    EXPECT_EQ(BuildFilename(L"LPT9", capability::Container::Matroska, ts), L"LPT9_1.mkv");
}

TEST(OutputSettingsTest, EmptyOnlyToken_FallsBackToRecordingPrefix) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    const auto filename = BuildFilename(L"{title}", capability::Container::Matroska, ts, WindowContext(L"Brave", L""));
    EXPECT_EQ(filename.rfind(L"recording_", 0), 0u);
    EXPECT_EQ(filename.substr(filename.size() - 4), L".mkv");
}

TEST(OutputSettingsTest, LeadingSlashPattern_CreatesExpectedSubfolderPath) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    const std::filesystem::path folder = UniqueTempPath(L"leading_slash");
    const auto output =
        BuildOutputPath(folder, L"/{app}/{datetime}", capability::Container::Matroska, ts, WindowContext());
    EXPECT_TRUE(IsPathUnderFolder(output, folder));
    EXPECT_EQ(output.lexically_normal(), (folder / L"Brave" / L"2026-05-22_14-37-09.mkv").lexically_normal());
}

TEST(OutputSettingsTest, TrailingSlashPattern_FinalFilenameIsLastNonEmptySegment) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    const std::filesystem::path folder = UniqueTempPath(L"trailing_slash");
    const auto output =
        BuildOutputPath(folder, L"{app}/{datetime}/", capability::Container::Matroska, ts, WindowContext());
    EXPECT_TRUE(IsPathUnderFolder(output, folder));
    EXPECT_EQ(output.lexically_normal(), (folder / L"Brave" / L"2026-05-22_14-37-09.mkv").lexically_normal());
}

TEST(OutputSettingsTest, DuplicatePathSeparators_Collapsed) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    const std::filesystem::path folder = UniqueTempPath(L"double_slash");
    const auto output =
        BuildOutputPath(folder, L"{app}//{datetime}", capability::Container::Matroska, ts, WindowContext());
    EXPECT_TRUE(IsPathUnderFolder(output, folder));
    EXPECT_EQ(output.lexically_normal(), (folder / L"Brave" / L"2026-05-22_14-37-09.mkv").lexically_normal());
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

TEST(OutputSettingsTest, Defaults_AudioCodecIsAac) {
    const OutputSettingsModel defaults = OutputSettingsModel::Defaults();
    EXPECT_EQ(defaults.audio_codec, capability::AudioCodec::AacMf);
}

TEST(OutputSettingsTest, Mp4Profile_ExtensionIsMp4) {
    const std::time_t ts = LocalTimestamp(2026, 5, 23, 10, 0, 0);
    const auto filename = BuildFilename(L"rec_{datetime}", capability::Container::Mp4, ts);
    EXPECT_EQ(filename.substr(filename.size() - 4), L".mp4");
}

TEST(OutputSettingsTest, Mp4Profile_ApplyAudioCodec_UsesAac) {
    recorder_core::RecorderConfig config{};
    config.audio_codec = recorder_core::AudioCodec::Opus;

    OutputSettingsModel settings = OutputSettingsModel::Defaults();
    settings.container = capability::Container::Mp4;
    settings.audio_codec = capability::AudioCodec::AacMf;

    ApplyOutputSettingsToRecorderConfig(config, settings);
    EXPECT_EQ(config.audio_codec, recorder_core::AudioCodec::AacMf);
}

TEST(OutputSettingsTest, WebMProfile_ApplyAudioCodec_UsesOpus) {
    recorder_core::RecorderConfig config{};
    config.audio_codec = recorder_core::AudioCodec::AacMf;

    OutputSettingsModel settings = OutputSettingsModel::Defaults();
    settings.container = capability::Container::WebM;
    settings.audio_codec = capability::AudioCodec::Opus;

    ApplyOutputSettingsToRecorderConfig(config, settings);
    EXPECT_EQ(config.audio_codec, recorder_core::AudioCodec::Opus);
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
