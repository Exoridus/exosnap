#include <gtest/gtest.h>

#include <chrono>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>

#include <QCoreApplication>

#include <recorder_core/recorder_session.h>

#include "models/FilenameBuilder.h"
#include "models/OutputPathPolicy.h"
#include "models/OutputPathValidator.h"
#include "models/OutputSettingsModel.h"
#include "services/RecordingCoordinator.h"

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

TEST(OutputSettingsTest, AppToken_IsReplacedFromContext) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    const auto filename = BuildFilename(L"{app}", capability::Container::Matroska, ts, WindowContext());
    EXPECT_EQ(filename, L"Brave.mkv");
}

TEST(OutputSettingsTest, ApplicationToken_IsNotRecognized) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    const auto filename = BuildFilename(L"{application}", capability::Container::Matroska, ts, WindowContext());
    EXPECT_EQ(filename.rfind(L"{application}", 0), 0u);
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

TEST(OutputSettingsTest, ResolveAvailableOutputPath_NoCollision_ReturnsBasePath) {
    const std::filesystem::path dir = UniqueTempPath(L"collision_no");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ASSERT_FALSE(ec);

    const auto base = dir / L"recording.mkv";
    const auto resolved = ResolveAvailableOutputPath(base);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(*resolved, base);

    std::filesystem::remove_all(dir, ec);
}

TEST(OutputSettingsTest, ResolveAvailableOutputPath_ExistingFile_AppendsSuffix) {
    const std::filesystem::path dir = UniqueTempPath(L"collision_suffix");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ASSERT_FALSE(ec);

    const auto base = dir / L"recording.mkv";
    {
        std::ofstream touch(base, std::ios::binary);
    }
    ASSERT_TRUE(std::filesystem::exists(base));

    const auto resolved = ResolveAvailableOutputPath(base);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_NE(*resolved, base);
    EXPECT_EQ(resolved->parent_path(), dir);
    EXPECT_EQ(resolved->extension(), L".mkv");
    const auto stem_str = resolved->stem().wstring();
    EXPECT_TRUE(stem_str.starts_with(L"recording ("));

    std::filesystem::remove_all(dir, ec);
}

TEST(OutputSettingsTest, ResolveAvailableOutputPath_MultipleCollisions_IncrementsSuffix) {
    const std::filesystem::path dir = UniqueTempPath(L"collision_multi");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ASSERT_FALSE(ec);

    const auto base = dir / L"recording.mkv";
    {
        std::ofstream touch(base, std::ios::binary);
    }
    {
        std::ofstream touch(dir / L"recording (1).mkv", std::ios::binary);
    }
    EXPECT_TRUE(std::filesystem::exists(base));
    EXPECT_TRUE(std::filesystem::exists(dir / L"recording (1).mkv"));

    const auto resolved = ResolveAvailableOutputPath(base);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->parent_path(), dir);
    EXPECT_EQ(resolved->extension(), L".mkv");
    EXPECT_EQ(resolved->stem(), L"recording (2)");

    std::filesystem::remove_all(dir, ec);
}

TEST(OutputSettingsTest, ResolveAvailableOutputPath_AlreadyUsedSuffix_SkipsToNext) {
    const std::filesystem::path dir = UniqueTempPath(L"collision_skip");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ASSERT_FALSE(ec);

    const auto base = dir / L"recording.mkv";
    const auto with_suffix = dir / L"recording (1).mkv";
    {
        std::ofstream touch(with_suffix, std::ios::binary);
    }
    EXPECT_FALSE(std::filesystem::exists(base));
    EXPECT_TRUE(std::filesystem::exists(with_suffix));

    const auto resolved = ResolveAvailableOutputPath(base);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(*resolved, base);

    std::filesystem::remove_all(dir, ec);
}

TEST(OutputSettingsTest, ResolveAvailableOutputPath_PreservesExtension_Case) {
    const std::filesystem::path dir = UniqueTempPath(L"collision_ext");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ASSERT_FALSE(ec);

    const auto base = dir / L"my clip.MP4";
    {
        std::ofstream touch(base, std::ios::binary);
    }
    ASSERT_TRUE(std::filesystem::exists(base));

    const auto resolved = ResolveAvailableOutputPath(base);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->extension(), L".MP4");

    std::filesystem::remove_all(dir, ec);
}

TEST(OutputSettingsTest, ResolveAvailableOutputPath_Exhausted_ReturnsNullopt) {
    const std::filesystem::path dir = UniqueTempPath(L"collision_exhausted");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ASSERT_FALSE(ec);

    const auto base = dir / L"recording.mkv";
    {
        std::ofstream touch(base, std::ios::binary);
    }
    ASSERT_TRUE(std::filesystem::exists(base));

    for (int i = 1; i < 1000; ++i) {
        const auto candidate = dir / (std::wstring(L"recording (") + std::to_wstring(i) + L").mkv");
        std::ofstream touch(candidate, std::ios::binary);
        ASSERT_TRUE(std::filesystem::exists(candidate, ec)) << "Failed to create suffix " << i;
    }

    const auto resolved = ResolveAvailableOutputPath(base);
    EXPECT_FALSE(resolved.has_value()) << "Should return nullopt when all suffixes are exhausted";

    std::filesystem::remove_all(dir, ec);
}

TEST(OutputSettingsTest, Defaults_FolderNotEmpty) {
    const OutputSettingsModel defaults = OutputSettingsModel::Defaults();
    EXPECT_FALSE(defaults.output_folder.empty());
    EXPECT_TRUE(defaults.output_folder.is_absolute());
}

TEST(OutputSettingsTest, Defaults_ContainerIsMatroska) {
    const OutputSettingsModel defaults = OutputSettingsModel::Defaults();
    EXPECT_EQ(defaults.container, capability::Container::Matroska);
}

TEST(OutputSettingsTest, Defaults_AudioCodecIsAac) {
    const OutputSettingsModel defaults = OutputSettingsModel::Defaults();
    EXPECT_EQ(defaults.audio_codec, capability::AudioCodec::AacMf);
}

TEST(OutputSettingsTest, Defaults_VideoCodecIsH264) {
    const OutputSettingsModel defaults = OutputSettingsModel::Defaults();
    EXPECT_EQ(defaults.video_codec, capability::VideoCodec::H264Nvenc);
}

TEST(OutputSettingsTest, DefaultResolutionIsNativeContain) {
    const OutputSettingsModel defaults = OutputSettingsModel::Defaults();
    EXPECT_EQ(defaults.resolution.mode, OutputResolutionMode::Native);
    EXPECT_EQ(defaults.resolution.fit, recorder_core::OutputFitMode::Contain);
}

TEST(OutputSettingsTest, FixedResolutionModesResolveToCanonicalSizes) {
    EXPECT_EQ(PresetOutputSize(OutputResolutionMode::UHD2160)->width, 3840u);
    EXPECT_EQ(PresetOutputSize(OutputResolutionMode::UHD2160)->height, 2160u);
    EXPECT_EQ(PresetOutputSize(OutputResolutionMode::QHD1440)->width, 2560u);
    EXPECT_EQ(PresetOutputSize(OutputResolutionMode::QHD1440)->height, 1440u);
    EXPECT_EQ(PresetOutputSize(OutputResolutionMode::FHD1080)->width, 1920u);
    EXPECT_EQ(PresetOutputSize(OutputResolutionMode::FHD1080)->height, 1080u);
    EXPECT_EQ(PresetOutputSize(OutputResolutionMode::HD720)->width, 1280u);
    EXPECT_EQ(PresetOutputSize(OutputResolutionMode::HD720)->height, 720u);
}

TEST(OutputSettingsTest, NativeResolutionUsesSourceSizeWithEncoderAlignment) {
    OutputResolutionSettings settings;
    settings.mode = OutputResolutionMode::Native;

    const auto resolved = ResolveRequestedOutputSize(settings, {1919, 1079});
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->width, 1918u);
    EXPECT_EQ(resolved->height, 1078u);
}

TEST(OutputSettingsTest, InvalidCustomResolutionSanitizesToNative) {
    OutputResolutionSettings settings;
    settings.mode = OutputResolutionMode::Custom;
    settings.custom_width = 0;
    settings.custom_height = 1080;

    SanitizeOutputResolution(settings);
    EXPECT_EQ(settings.mode, OutputResolutionMode::Native);
    EXPECT_EQ(settings.custom_width, 0u);
    EXPECT_EQ(settings.custom_height, 0u);
}

TEST(OutputSettingsTest, OddCustomResolutionAlignsDeterministically) {
    OutputResolutionSettings settings;
    settings.mode = OutputResolutionMode::Custom;
    settings.custom_width = 1919;
    settings.custom_height = 1079;

    SanitizeOutputResolution(settings);
    EXPECT_EQ(settings.mode, OutputResolutionMode::Custom);
    EXPECT_EQ(settings.custom_width, 1918u);
    EXPECT_EQ(settings.custom_height, 1078u);
}

TEST(OutputSettingsTest, CustomResolutionBounds_WidthBelowMinimum_SanitizesToNative) {
    OutputResolutionSettings settings;
    settings.mode = OutputResolutionMode::Custom;
    settings.custom_width = 100;
    settings.custom_height = 720;

    SanitizeOutputResolution(settings);
    EXPECT_EQ(settings.mode, OutputResolutionMode::Native);
    EXPECT_EQ(settings.custom_width, 0u);
    EXPECT_EQ(settings.custom_height, 0u);
}

TEST(OutputSettingsTest, CustomResolutionBounds_HeightBelowMinimum_SanitizesToNative) {
    OutputResolutionSettings settings;
    settings.mode = OutputResolutionMode::Custom;
    settings.custom_width = 1920;
    settings.custom_height = 100;

    SanitizeOutputResolution(settings);
    EXPECT_EQ(settings.mode, OutputResolutionMode::Native);
    EXPECT_EQ(settings.custom_width, 0u);
    EXPECT_EQ(settings.custom_height, 0u);
}

TEST(OutputSettingsTest, CustomResolutionBounds_WidthExceedsMaximum_SanitizesToNative) {
    OutputResolutionSettings settings;
    settings.mode = OutputResolutionMode::Custom;
    settings.custom_width = 8000;
    settings.custom_height = 4320;

    SanitizeOutputResolution(settings);
    EXPECT_EQ(settings.mode, OutputResolutionMode::Native);
    EXPECT_EQ(settings.custom_width, 0u);
    EXPECT_EQ(settings.custom_height, 0u);
}

TEST(OutputSettingsTest, CustomResolutionBounds_ValidValues_RemainCustom) {
    OutputResolutionSettings settings;
    settings.mode = OutputResolutionMode::Custom;
    settings.custom_width = 1920;
    settings.custom_height = 1080;

    SanitizeOutputResolution(settings);
    EXPECT_EQ(settings.mode, OutputResolutionMode::Custom);
    EXPECT_EQ(settings.custom_width, 1920u);
    EXPECT_EQ(settings.custom_height, 1080u);
}

TEST(OutputSettingsTest, CustomResolutionBounds_MinimumBounds_Accepted) {
    OutputResolutionSettings settings;
    settings.mode = OutputResolutionMode::Custom;
    settings.custom_width = 320;
    settings.custom_height = 180;

    SanitizeOutputResolution(settings);
    EXPECT_EQ(settings.mode, OutputResolutionMode::Custom);
    EXPECT_EQ(settings.custom_width, 320u);
    EXPECT_EQ(settings.custom_height, 180u);
}

TEST(OutputSettingsTest, CustomResolutionBounds_MaximumBounds_Accepted) {
    OutputResolutionSettings settings;
    settings.mode = OutputResolutionMode::Custom;
    settings.custom_width = 7680;
    settings.custom_height = 4320;

    SanitizeOutputResolution(settings);
    EXPECT_EQ(settings.mode, OutputResolutionMode::Custom);
    EXPECT_EQ(settings.custom_width, 7680u);
    EXPECT_EQ(settings.custom_height, 4320u);
}

TEST(OutputSettingsTest, ResolveRequestedOutputSize_Custom_ReturnsAlignedDimensions) {
    OutputResolutionSettings settings;
    settings.mode = OutputResolutionMode::Custom;
    settings.custom_width = 1919;
    settings.custom_height = 1079;

    const auto resolved = ResolveRequestedOutputSize(settings, {2560, 1440});
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->width, 1918u);
    EXPECT_EQ(resolved->height, 1078u);
}

TEST(OutputSettingsTest, ResolveRequestedOutputSize_Custom_InvalidBounds_ReturnsNullopt) {
    OutputResolutionSettings settings;
    settings.mode = OutputResolutionMode::Custom;
    settings.custom_width = 100;
    settings.custom_height = 100;

    const auto resolved = ResolveRequestedOutputSize(settings, {2560, 1440});
    EXPECT_FALSE(resolved.has_value());
}

TEST(OutputSettingsTest, ApplyOutputResolution_Custom_PassesAlignedSizeToRecorderConfig) {
    recorder_core::RecorderConfig config{};

    OutputSettingsModel settings = OutputSettingsModel::Defaults();
    settings.resolution.mode = OutputResolutionMode::Custom;
    settings.resolution.custom_width = 1919;
    settings.resolution.custom_height = 1079;
    SanitizeOutputResolution(settings.resolution);

    ApplyOutputSettingsToRecorderConfig(config, settings);
    EXPECT_EQ(config.output_width, 1918u);
    EXPECT_EQ(config.output_height, 1078u);
    EXPECT_EQ(config.output_fit, recorder_core::OutputFitMode::Contain);
}

TEST(OutputSettingsTest, FixedModesStillBehavior_Unchanged) {
    EXPECT_EQ(PresetOutputSize(OutputResolutionMode::UHD2160)->width, 3840u);
    EXPECT_EQ(PresetOutputSize(OutputResolutionMode::FHD1080)->width, 1920u);
    EXPECT_EQ(PresetOutputSize(OutputResolutionMode::HD720)->height, 720u);

    EXPECT_EQ(std::wstring(OutputResolutionModeName(OutputResolutionMode::Native)), L"Native");
    EXPECT_EQ(std::wstring(OutputResolutionModeName(OutputResolutionMode::Custom)), L"Custom");
}

TEST(OutputGeometryTest, ContainRect_16x9Into16x9FillsOutput) {
    const auto rect = recorder_core::ResolveContainRect({1920, 1080}, {1280, 720});
    ASSERT_TRUE(rect.has_value());
    EXPECT_EQ(rect->x, 0u);
    EXPECT_EQ(rect->y, 0u);
    EXPECT_EQ(rect->width, 1280u);
    EXPECT_EQ(rect->height, 720u);
}

TEST(OutputGeometryTest, ContainRect_4x3Into16x9LetterboxesHorizontally) {
    const auto rect = recorder_core::ResolveContainRect({1024, 768}, {1920, 1080});
    ASSERT_TRUE(rect.has_value());
    EXPECT_EQ(rect->width, 1440u);
    EXPECT_EQ(rect->height, 1080u);
    EXPECT_EQ(rect->x, 240u);
    EXPECT_EQ(rect->y, 0u);
}

TEST(OutputGeometryTest, ContainRect_PortraitIntoLandscapeCenters) {
    const auto rect = recorder_core::ResolveContainRect({1080, 1920}, {1920, 1080});
    ASSERT_TRUE(rect.has_value());
    EXPECT_EQ(rect->width, 608u);
    EXPECT_EQ(rect->height, 1080u);
    EXPECT_EQ(rect->x, 656u);
    EXPECT_EQ(rect->y, 0u);
}

TEST(OutputGeometryTest, ContainRect_NeverLeavesOutput) {
    const auto rect = recorder_core::ResolveContainRect({1234, 321}, {1280, 720});
    ASSERT_TRUE(rect.has_value());
    EXPECT_LE(rect->x + rect->width, 1280u);
    EXPECT_LE(rect->y + rect->height, 720u);
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

TEST(OutputSettingsTest, ApplyOutputResolution_PassesFixedSizeToRecorderConfig) {
    recorder_core::RecorderConfig config{};

    OutputSettingsModel settings = OutputSettingsModel::Defaults();
    settings.resolution.mode = OutputResolutionMode::FHD1080;

    ApplyOutputSettingsToRecorderConfig(config, settings);
    EXPECT_EQ(config.output_width, 1920u);
    EXPECT_EQ(config.output_height, 1080u);
    EXPECT_EQ(config.output_fit, recorder_core::OutputFitMode::Contain);
}

TEST(OutputSettingsTest, ApplyOutputResolution_NativeUsesRuntimeSourceSize) {
    recorder_core::RecorderConfig config{};
    config.output_width = 1920;
    config.output_height = 1080;

    OutputSettingsModel settings = OutputSettingsModel::Defaults();
    settings.resolution.mode = OutputResolutionMode::Native;

    ApplyOutputSettingsToRecorderConfig(config, settings);
    EXPECT_EQ(config.output_width, 0u);
    EXPECT_EQ(config.output_height, 0u);
}

TEST(OutputSettingsTest, OutputFolderPolicy_HomeAliasResolvesToAbsolutePath) {
    const auto normalized = NormalizeOutputFolderInput(L"~/Videos/ExoSnap");
    EXPECT_EQ(normalized.result, OutputFolderPolicyResult::Ok);
    EXPECT_TRUE(normalized.resolved_path.is_absolute());
}

TEST(OutputSettingsTest, OutputFolderPolicy_UserProfileEnvResolvesToAbsolutePath) {
    const auto normalized = NormalizeOutputFolderInput(L"%USERPROFILE%\\Videos\\ExoSnap");
    EXPECT_EQ(normalized.result, OutputFolderPolicyResult::Ok);
    EXPECT_TRUE(normalized.resolved_path.is_absolute());
}

TEST(OutputSettingsTest, OutputFolderPolicy_UnknownEnvironmentVariableRejected) {
    const auto normalized = NormalizeOutputFolderInput(L"%UNKNOWN%\\Captures");
    EXPECT_EQ(normalized.result, OutputFolderPolicyResult::UnsupportedEnvironmentVariable);
}

TEST(OutputSettingsTest, OutputFolderPolicy_TrailingSlashesAreStrippedForNonRoot) {
    const auto normalized = NormalizeOutputFolderInput(L"C:\\Recordings\\\\");
    EXPECT_EQ(normalized.result, OutputFolderPolicyResult::Ok);
    EXPECT_EQ(normalized.normalized_input, L"C:\\Recordings");
}

TEST(OutputSettingsTest, OutputFolderPolicy_RootPathPreserved) {
    const auto normalized = NormalizeOutputFolderInput(L"C:\\");
    EXPECT_EQ(normalized.result, OutputFolderPolicyResult::Ok);
    EXPECT_EQ(normalized.normalized_input, L"C:\\");
}

TEST(OutputSettingsTest, FilenamePatternPolicy_StripsLeadingPrefixes) {
    const auto normalized_a = NormalizeFilenamePatternInput(L"/{app}/{datetime}");
    const auto normalized_b = NormalizeFilenamePatternInput(L"\\{app}\\{datetime}");
    const auto normalized_c = NormalizeFilenamePatternInput(L"./{app}/{datetime}");
    const auto normalized_d = NormalizeFilenamePatternInput(L".\\{app}\\{datetime}");
    EXPECT_EQ(normalized_a.result, FilenamePatternPolicyResult::Ok);
    EXPECT_EQ(normalized_b.result, FilenamePatternPolicyResult::Ok);
    EXPECT_EQ(normalized_c.result, FilenamePatternPolicyResult::Ok);
    EXPECT_EQ(normalized_d.result, FilenamePatternPolicyResult::Ok);
    EXPECT_EQ(normalized_a.normalized_pattern, L"{app}/{datetime}");
    EXPECT_EQ(normalized_b.normalized_pattern, L"{app}/{datetime}");
    EXPECT_EQ(normalized_c.normalized_pattern, L"{app}/{datetime}");
    EXPECT_EQ(normalized_d.normalized_pattern, L"{app}/{datetime}");
}

TEST(OutputSettingsTest, FilenamePatternPolicy_AllowsSubfolders) {
    const auto normalized = NormalizeFilenamePatternInput(L"{profile}/{app}/{datetime}");
    EXPECT_EQ(normalized.result, FilenamePatternPolicyResult::Ok);
    EXPECT_EQ(normalized.normalized_pattern, L"{profile}/{app}/{datetime}");
}

TEST(OutputSettingsTest, FilenamePatternPolicy_RejectsParentTraversal) {
    const auto normalized = NormalizeFilenamePatternInput(L"{app}/../{datetime}");
    EXPECT_EQ(normalized.result, FilenamePatternPolicyResult::ParentTraversalSegment);
}

TEST(OutputSettingsTest, FilenamePatternPolicy_RejectsAbsoluteDrivePath) {
    const auto normalized = NormalizeFilenamePatternInput(L"C:\\captures\\{datetime}");
    EXPECT_EQ(normalized.result, FilenamePatternPolicyResult::AbsolutePath);
}

TEST(OutputSettingsTest, FilenamePatternPolicy_RejectsEnvironmentVariables) {
    const auto normalized = NormalizeFilenamePatternInput(L"%USERPROFILE%/{datetime}");
    EXPECT_EQ(normalized.result, FilenamePatternPolicyResult::UnsupportedEnvironmentVariable);
}

TEST(OutputSettingsTest, FilenamePatternPolicy_RejectsHomeAlias) {
    const auto normalized = NormalizeFilenamePatternInput(L"~/captures/{datetime}");
    EXPECT_EQ(normalized.result, FilenamePatternPolicyResult::UnsupportedHomeAlias);
}

TEST(OutputSettingsTest, FilenameTokens_ProfileContainerVideoAudioRender) {
    const std::time_t ts = LocalTimestamp(2026, 5, 22, 14, 37, 9);
    FilenameTargetContext context = WindowContext();
    context.profile_name = L"MKV H264 AAC";
    context.video_codec = capability::VideoCodec::H264Nvenc;
    context.audio_codec = capability::AudioCodec::AacMf;
    const auto filename =
        BuildFilename(L"{profile}_{container}_{video}_{audio}", capability::Container::Matroska, ts, context);
    EXPECT_EQ(filename, L"MKV H264 AAC_mkv_h264_aac.mkv");
}

TEST(OutputSettingsTest, PasteSplit_TokenPathAutoSplits) {
    const auto decision = AnalyzeOutputPasteInput(L"D:\\Captures\\{app}\\{datetime}");
    EXPECT_EQ(decision.kind, OutputPasteSplitKind::AutoSplitTokenPath);
    EXPECT_EQ(decision.folder_input, L"D:/Captures");
    EXPECT_EQ(decision.pattern_input, L"{app}/{datetime}");
}

TEST(OutputSettingsTest, PasteSplit_FullFilePathIsSplitOffer) {
    const auto decision = AnalyzeOutputPasteInput(L"D:\\Captures\\recording.mp4");
    EXPECT_EQ(decision.kind, OutputPasteSplitKind::OfferSplitFullFilePath);
}

TEST(OutputSettingsTest, PasteSplit_AbsolutePathWithoutTokenOrExtensionIsFolder) {
    const auto decision = AnalyzeOutputPasteInput(L"D:\\Captures\\Sessions");
    EXPECT_EQ(decision.kind, OutputPasteSplitKind::TreatAsFolder);
    EXPECT_EQ(decision.folder_input, L"D:\\Captures\\Sessions");
}

// ── Split recording settings (SPLIT-RECORDING-R1) ────────────────────────────

TEST(SplitSettingsTest, DefaultsToOffSingleFile) {
    SplitRecordingSettings s;
    EXPECT_EQ(s.mode, SplitRecordingMode::Off);
    EXPECT_EQ(SplitDurationMs(s), 0ull);
}

TEST(SplitSettingsTest, PresetDurationsMapToMilliseconds) {
    SplitRecordingSettings s;
    s.mode = SplitRecordingMode::Every15Min;
    EXPECT_EQ(SplitDurationMs(s), 15ull * 60ull * 1000ull);
    s.mode = SplitRecordingMode::Every30Min;
    EXPECT_EQ(SplitDurationMs(s), 30ull * 60ull * 1000ull);
    s.mode = SplitRecordingMode::Every60Min;
    EXPECT_EQ(SplitDurationMs(s), 60ull * 60ull * 1000ull);
}

TEST(SplitSettingsTest, CustomDurationUsesMinutes) {
    SplitRecordingSettings s;
    s.mode = SplitRecordingMode::Custom;
    s.custom_minutes = 42;
    EXPECT_EQ(SplitDurationMs(s), 42ull * 60ull * 1000ull);
}

TEST(SplitSettingsTest, CustomMinutesClampedToBounds) {
    SplitRecordingSettings lo;
    lo.custom_minutes = 0;
    SanitizeSplitSettings(lo);
    EXPECT_EQ(lo.custom_minutes, SplitRecordingSettings::kMinMinutes);

    SplitRecordingSettings hi;
    hi.custom_minutes = 100000;
    SanitizeSplitSettings(hi);
    EXPECT_EQ(hi.custom_minutes, SplitRecordingSettings::kMaxMinutes); // 24h
    EXPECT_EQ(hi.custom_minutes, 24u * 60u);
}

TEST(SplitSettingsTest, SplitDurationClampsCustomEvenIfUnsanitized) {
    SplitRecordingSettings s;
    s.mode = SplitRecordingMode::Custom;
    s.custom_minutes = 0; // below min; SplitDurationMs must still clamp
    EXPECT_EQ(SplitDurationMs(s), static_cast<uint64_t>(SplitRecordingSettings::kMinMinutes) * 60ull * 1000ull);
}

// ── EXOSNAP_OUTPUT_DIR override (DF-HISTORY) ─────────────────────────────────
//
// When EXOSNAP_OUTPUT_DIR is set to a non-empty path, EffectiveOutputFolder()
// must return the override instead of the configured output_folder.  When the
// variable is absent or empty, it must fall through to the configured folder.

TEST(OutputDirOverrideTest, OverrideSet_ReturnsOverrideDir) {
    const std::filesystem::path configured(L"C:\\Users\\User\\Videos\\ExoSnap");
    const std::filesystem::path override_dir(L"C:\\Temp\\exosnap-test-output");

    qputenv("EXOSNAP_OUTPUT_DIR", override_dir.string().c_str());

    RecordingCoordinator coordinator;
    OutputSettingsModel settings;
    settings.output_folder = configured;
    coordinator.SetOutputSettings(settings);

    EXPECT_EQ(coordinator.EffectiveOutputFolder(), override_dir);

    qunsetenv("EXOSNAP_OUTPUT_DIR");
}

TEST(OutputDirOverrideTest, OverrideUnset_ReturnsConfiguredDir) {
    const std::filesystem::path configured(L"C:\\Users\\User\\Videos\\ExoSnap");

    qunsetenv("EXOSNAP_OUTPUT_DIR");

    RecordingCoordinator coordinator;
    OutputSettingsModel settings;
    settings.output_folder = configured;
    coordinator.SetOutputSettings(settings);

    EXPECT_EQ(coordinator.EffectiveOutputFolder(), configured);
}

TEST(OutputDirOverrideTest, OverrideEmptyString_ReturnsConfiguredDir) {
    const std::filesystem::path configured(L"C:\\Users\\User\\Videos\\ExoSnap");

    qputenv("EXOSNAP_OUTPUT_DIR", "");

    RecordingCoordinator coordinator;
    OutputSettingsModel settings;
    settings.output_folder = configured;
    coordinator.SetOutputSettings(settings);

    EXPECT_EQ(coordinator.EffectiveOutputFolder(), configured);

    qunsetenv("EXOSNAP_OUTPUT_DIR");
}

} // namespace
} // namespace exosnap
