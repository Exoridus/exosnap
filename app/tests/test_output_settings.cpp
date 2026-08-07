#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <QCoreApplication>

#include <recorder_core/recorder_session.h>

#include "diagnostics/DiskSpaceProvider.h"
#include "diagnostics/DiskSpaceThresholds.h"
#include "models/FilenameBuilder.h"
#include "models/OutputPathPolicy.h"
#include "models/OutputPathValidator.h"
#include "models/OutputSettingsModel.h"
#include "services/RecordingCoordinator.h"

#include <capability/capability_builder.h>
#include <capability/translation.h>

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
    EXPECT_EQ(defaults.audio_codec, capability::AudioCodec::Aac);
}

TEST(OutputSettingsTest, Defaults_VideoCodecIsH264) {
    const OutputSettingsModel defaults = OutputSettingsModel::Defaults();
    EXPECT_EQ(defaults.video_codec, capability::VideoCodec::H264);
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

// The resolver (via ToRecorderCoreConfig) stamps the container/codec decision
// into the RecorderConfig before ApplyOutputSettingsToRecorderConfig runs.
// The apply step must never overwrite that answer from the raw settings —
// doing so would silently undo a resolver fallback (e.g. an audio codec the
// validation replaced for the container) and record an invalid combination.

TEST(OutputSettingsTest, ApplyOutputSettings_DoesNotOverrideResolvedAudioCodec) {
    recorder_core::RecorderConfig config{};
    config.audio_codec = recorder_core::AudioCodec::Opus; // resolver's answer

    OutputSettingsModel settings = OutputSettingsModel::Defaults();
    settings.audio_codec = capability::AudioCodec::Aac; // raw, unresolved wish

    ApplyOutputSettingsToRecorderConfig(config, settings);
    EXPECT_EQ(config.audio_codec, recorder_core::AudioCodec::Opus)
        << "the apply step must keep the resolver-stamped audio codec, not the raw settings value";
}

TEST(OutputSettingsTest, ApplyOutputSettings_DoesNotOverrideResolvedContainerOrVideoCodec) {
    recorder_core::RecorderConfig config{};
    config.container = recorder_core::Container::Mp4; // resolver's answer
    config.video_codec = recorder_core::VideoCodec::H264;

    OutputSettingsModel settings = OutputSettingsModel::Defaults();
    settings.container = capability::Container::WebM; // raw, unresolved wish
    settings.video_codec = capability::VideoCodec::Av1;

    ApplyOutputSettingsToRecorderConfig(config, settings);
    EXPECT_EQ(config.container, recorder_core::Container::Mp4);
    EXPECT_EQ(config.video_codec, recorder_core::VideoCodec::H264);
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
    context.video_codec = capability::VideoCodec::H264;
    context.audio_codec = capability::AudioCodec::Aac;
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

// ── Split-by-size settings (SPLIT-BY-SIZE-R1) ────────────────────────────────

TEST(SplitSizeSettingsTest, DefaultsToOff) {
    SplitRecordingSettings s;
    EXPECT_EQ(s.size_mode, SplitSizeMode::Off);
    EXPECT_EQ(SplitSizeBytes(s), 0ull);
}

TEST(SplitSizeSettingsTest, CustomSizeModeReturnsBytesFromMb) {
    SplitRecordingSettings s;
    s.size_mode = SplitSizeMode::Custom;
    s.custom_size_mb = 1024; // 1 GiB
    EXPECT_EQ(SplitSizeBytes(s), 1024ull * 1024ull * 1024ull);
}

TEST(SplitSizeSettingsTest, SplitSizeBytesClampsTooSmall) {
    SplitRecordingSettings s;
    s.size_mode = SplitSizeMode::Custom;
    s.custom_size_mb = 0; // below min
    EXPECT_EQ(SplitSizeBytes(s), static_cast<uint64_t>(SplitRecordingSettings::kMinSizeMb) * 1024ull * 1024ull);
}

TEST(SplitSizeSettingsTest, SanitizeSplitSettingsClampsSizeMb) {
    SplitRecordingSettings lo;
    lo.size_mode = SplitSizeMode::Custom;
    lo.custom_size_mb = 0;
    SanitizeSplitSettings(lo);
    EXPECT_EQ(lo.custom_size_mb, SplitRecordingSettings::kMinSizeMb);

    SplitRecordingSettings hi;
    hi.size_mode = SplitSizeMode::Custom;
    hi.custom_size_mb = 0xFFFFFFFF; // above max
    SanitizeSplitSettings(hi);
    EXPECT_EQ(hi.custom_size_mb, SplitRecordingSettings::kMaxSizeMb);
}

TEST(SplitSizeSettingsTest, OffModeReturnsZeroRegardlessOfMb) {
    SplitRecordingSettings s;
    s.size_mode = SplitSizeMode::Off;
    s.custom_size_mb = 500;
    EXPECT_EQ(SplitSizeBytes(s), 0ull);
}

TEST(SplitSizeSettingsTest, EqualityIncludesSizeFields) {
    SplitRecordingSettings a;
    SplitRecordingSettings b;
    EXPECT_EQ(a, b);
    b.size_mode = SplitSizeMode::Custom;
    EXPECT_NE(a, b);
    a.size_mode = SplitSizeMode::Custom;
    EXPECT_EQ(a, b);
    b.custom_size_mb = 999;
    EXPECT_NE(a, b);
}

TEST(SplitSizeSettingsTest, SizeSplitPropagatesViaCoordinator) {
    RecordingCoordinator coordinator;
    OutputSettingsModel settings = OutputSettingsModel::Defaults();
    settings.split.size_mode = SplitSizeMode::Custom;
    settings.split.custom_size_mb = 2048;
    coordinator.SetOutputSettings(settings);

    const auto split = coordinator.SplitSettings();
    EXPECT_EQ(split.size_bytes, 2048ull * 1024ull * 1024ull);
}

TEST(SplitSizeSettingsTest, SizeSplitOffPropagatesZeroViaCoordinator) {
    RecordingCoordinator coordinator;
    OutputSettingsModel settings = OutputSettingsModel::Defaults();
    settings.split.size_mode = SplitSizeMode::Off;
    coordinator.SetOutputSettings(settings);

    const auto split = coordinator.SplitSettings();
    EXPECT_EQ(split.size_bytes, 0ull);
}

// SETTINGS-HONESTY-R1 (red-proof): MainWindow's formatSettingsChanged handler routes
// every live ConfigPage format-editor field through MergeFormatSelection() into the
// output_settings_ MainWindow holds (see OutputSettingsModel.h/.cpp). A live edit of
// the Settings > Output Split card (mode / custom minutes / size mode / custom MB)
// reaches ConfigPage::format_settings_ and is emitted via formatSettingsChanged, but
// MergeFormatSelection did not carry `.split` — so the live edit never reached
// output_settings_ (only preset-apply/startup loaded it), and a recording started
// right after a split edit silently used the OLD split configuration. Without the
// `.split` line in MergeFormatSelection, this test fails: `live.split` stays at its
// pre-merge Off/default values instead of picking up `incoming.split`.
TEST(SplitSizeSettingsTest, MergeFormatSelection_CarriesSplitSettings) {
    OutputSettingsModel live = OutputSettingsModel::Defaults();
    live.split.mode = SplitRecordingMode::Off;
    live.split.size_mode = SplitSizeMode::Off;

    OutputSettingsModel incoming = live;
    incoming.split.mode = SplitRecordingMode::Custom;
    incoming.split.custom_minutes = 42;
    incoming.split.size_mode = SplitSizeMode::Custom;
    incoming.split.custom_size_mb = 777;

    MergeFormatSelection(live, incoming);

    EXPECT_EQ(live.split.mode, SplitRecordingMode::Custom)
        << "MergeFormatSelection must carry a live split-mode edit into output_settings_";
    EXPECT_EQ(live.split.custom_minutes, 42u);
    EXPECT_EQ(live.split.size_mode, SplitSizeMode::Custom);
    EXPECT_EQ(live.split.custom_size_mb, 777u);

    // Same downstream path the app takes at recording start (RecordingCoordinator::Start).
    RecordingCoordinator coordinator;
    coordinator.SetOutputSettings(live);
    const auto split = coordinator.SplitSettings();
    EXPECT_EQ(split.size_bytes, 777ull * 1024ull * 1024ull);
}

// NVENC-PRESET-R1: the NVENC encoder speed/quality preset (P1..P7) is a real
// expert setting, default P4 (balanced) — matches the prior AV1/HEVC hardcoded
// default (the default profile is AV1, so a fresh install is unaffected);
// H.264 previously used P6 (visible default change — see ADR 0039).
TEST(OutputSettingsTest, Defaults_NvencPresetIsP4) {
    const OutputSettingsModel defaults = OutputSettingsModel::Defaults();
    EXPECT_EQ(defaults.nvenc_preset, recorder_core::NvencPreset::P4);
}

// Red-proof (same class of bug as MergeFormatSelection_CarriesSplitSettings):
// without the `.nvenc_preset` line in MergeFormatSelection, a live edit of the
// Container & codecs card's "Encoder preset (NVENC)" combo would reach
// ConfigPage::format_settings_ and be emitted via formatSettingsChanged, but
// never reach output_settings_ — the recording would silently keep using the
// OLD preset. Delete the `.nvenc_preset` line in MergeFormatSelection to watch
// this test fail.
TEST(OutputSettingsTest, MergeFormatSelection_CarriesNvencPreset) {
    OutputSettingsModel live = OutputSettingsModel::Defaults();
    live.nvenc_preset = recorder_core::NvencPreset::P4;

    OutputSettingsModel incoming = live;
    incoming.nvenc_preset = recorder_core::NvencPreset::P7;

    MergeFormatSelection(live, incoming);

    EXPECT_EQ(live.nvenc_preset, recorder_core::NvencPreset::P7)
        << "MergeFormatSelection must carry a live encoder-preset edit into output_settings_";
}

// Same class of bug as MergeFormatSelection_CarriesNvencPreset. hdr_mode has
// no UI control yet, but the model field must still survive MergeFormatSelection
// so the future expert HDR control / preset-apply plumbing doesn't silently
// drop it, same as every other output field.
TEST(OutputSettingsTest, MergeFormatSelection_CarriesHdrMode) {
    OutputSettingsModel live = OutputSettingsModel::Defaults();
    live.hdr_mode = recorder_core::HdrMode::TonemapSdr;

    OutputSettingsModel incoming = live;
    incoming.hdr_mode = recorder_core::HdrMode::Hdr10;

    MergeFormatSelection(live, incoming);

    EXPECT_EQ(live.hdr_mode, recorder_core::HdrMode::Hdr10)
        << "MergeFormatSelection must carry hdr_mode through like every other output field";
}

// Verifies the last-mile wiring from OutputSettingsModel into the engine's
// RecorderConfig (RecordingCoordinator.cpp, ApplyOutputSettingsToRecorderConfig).
// Without this line the combo could be wired end-to-end through the UI and
// still never reach the NVENC encoder at recording start.
TEST(OutputSettingsTest, ApplyOutputSettingsToRecorderConfig_CarriesNvencPreset) {
    recorder_core::RecorderConfig config{};
    config.nvenc_preset = recorder_core::NvencPreset::P4;

    OutputSettingsModel settings = OutputSettingsModel::Defaults();
    settings.nvenc_preset = recorder_core::NvencPreset::P1;

    ApplyOutputSettingsToRecorderConfig(config, settings);
    EXPECT_EQ(config.nvenc_preset, recorder_core::NvencPreset::P1);
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

// ---------------------------------------------------------------------------
// Display HDR facts must not go stale (the startup capability query runs once)
// ---------------------------------------------------------------------------

namespace {

capability::DisplayHdrFacts MakeDisplay(const char* name, bool hdr_active) {
    capability::DisplayHdrFacts d;
    d.name = name;
    d.hdr_active = hdr_active;
    return d;
}

} // namespace

TEST(DisplayFactsRefreshTest, RefreshReplacesTheStartupSnapshot) {
    RecordingCoordinator coordinator;

    capability::CapabilitySet caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    caps.probed = true; // simulate a completed hardware probe — required by OnCapabilitiesReady
    caps.runtime.displays = {MakeDisplay("\\\\.\\DISPLAY1", /*hdr_active=*/false)};
    coordinator.OnCapabilitiesReady(caps);
    ASSERT_EQ(coordinator.DisplayFacts().size(), 1u);
    EXPECT_FALSE(coordinator.DisplayFacts()[0].hdr_active);

    // The user turns Windows HDR on. Screen geometry does not change, so nothing notices.
    coordinator.SetDisplayFactsProvider(
        [] { return std::vector<capability::DisplayHdrFacts>{MakeDisplay("\\\\.\\DISPLAY1", /*hdr_active=*/true)}; });
    coordinator.RefreshDisplayFacts();

    ASSERT_EQ(coordinator.DisplayFacts().size(), 1u);
    EXPECT_TRUE(coordinator.DisplayFacts()[0].hdr_active) << "the refresh must reach the facts the reconcile reads";
}

// A failed DXGI query returns nothing. Treating that as "every display went SDR" would
// silently drop HDR metadata from a recording that should carry it.
TEST(DisplayFactsRefreshTest, AnEmptyQueryKeepsThePreviousFacts) {
    RecordingCoordinator coordinator;

    capability::CapabilitySet caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    caps.probed = true; // simulate a completed hardware probe — required by OnCapabilitiesReady
    caps.runtime.displays = {MakeDisplay("\\\\.\\DISPLAY1", /*hdr_active=*/true)};
    coordinator.OnCapabilitiesReady(caps);

    coordinator.SetDisplayFactsProvider([] { return std::vector<capability::DisplayHdrFacts>{}; });
    coordinator.RefreshDisplayFacts();

    ASSERT_EQ(coordinator.DisplayFacts().size(), 1u);
    EXPECT_TRUE(coordinator.DisplayFacts()[0].hdr_active);
}

// ---------------------------------------------------------------------------
// OnCapabilitiesReady must validate (and never clobber) settings already
// applied via SetOutputSettings/SetVideoSettings
// ---------------------------------------------------------------------------
// Regression for a live-verify finding (2026-08-07): RecordPage::initCoordinator()
// always applies the persisted profile via SetOutputSettings/SetVideoSettings BEFORE
// the async hardware capability probe resolves. OnCapabilitiesReady used to be handed
// a validation computed by the caller against a *different* config (RecordPage fed it
// a hardcoded MKV+H264+AAC baseline meant only as a "can anything record at all" gate)
// and unconditionally applied that validation's resolved config to
// resolved_user_config_ — silently discarding the AV1/Opus profile that had just been
// applied. Invisible until the user touched any Settings control (which re-syncs via
// RevalidateCapabilities), so it hit every "fresh app start, immediate record" session.

TEST(CapabilitiesReadyTest, DoesNotOverwriteAlreadyAppliedOutputSettings) {
    RecordingCoordinator coordinator;

    OutputSettingsModel settings = OutputSettingsModel::Defaults();
    settings.container = capability::Container::Matroska;
    settings.video_codec = capability::VideoCodec::Av1;
    settings.audio_codec = capability::AudioCodec::Opus;
    coordinator.SetOutputSettings(settings);

    capability::CapabilitySet caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    caps.probed = true; // simulate a completed hardware probe — required by OnCapabilitiesReady
    coordinator.OnCapabilitiesReady(caps);

    EXPECT_EQ(coordinator.ResolvedVideoCodecLabel(), L"AV1 NVENC encoder");
}

// ---------------------------------------------------------------------------
// A start failure must report the configured format, not the struct defaults
// ---------------------------------------------------------------------------
// The error dialog shows the result's container/codec context. The early
// failure paths in StartRecording used to post results without filling it, so
// the dialog claimed "WebM · AV1 · Opus" (UiRecordingResult's defaults) while
// the footer and the output path said MKV.

TEST(StartFailureFormatTest, FailureResultCarriesTheConfiguredFormat) {
    int argc = 0;
    QCoreApplication app(argc, nullptr);

    RecordingCoordinator coordinator;
    capability::CapabilitySet caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    caps.probed = true; // simulate a completed hardware probe — required by OnCapabilitiesReady

    // Point the output at a FILE so the folder guard rejects the start before
    // any engine work — the earliest of the failure paths that used to leave
    // the format fields at their defaults.
    const std::filesystem::path file_as_folder = UniqueTempPath(L"not_a_folder.bin");
    {
        std::ofstream out(file_as_folder);
        out << "x";
    }
    OutputSettingsModel settings;
    settings.output_folder = file_as_folder;
    // MP4 + H.264 + AAC: validated as-is by the static baseline, and distinct
    // from every UiRecordingResult default, so a leak of the defaults cannot
    // pass by coincidence.
    settings.container = capability::Container::Mp4;
    settings.video_codec = capability::VideoCodec::H264;
    settings.audio_codec = capability::AudioCodec::Aac;
    // Settings must be applied BEFORE OnCapabilitiesReady: the coordinator validates
    // whatever resolved_user_config_ already holds (see RecordingCoordinator::
    // OnCapabilitiesReady) — it is never handed a caller-supplied stand-in.
    coordinator.SetOutputSettings(settings);
    coordinator.OnCapabilitiesReady(caps);

    std::optional<UiRecordingResult> failure;
    coordinator.SetResultReadyCallback([&](const UiRecordingResult& r) { failure = r; });

    recorder_core::CaptureTarget target;
    target.kind = recorder_core::CaptureTarget::Kind::Monitor;
    target.native_id = 1; // never touched: the folder guard fires first
    target.description = "\\\\.\\DISPLAY1";
    // The device work (and the folder guard) now runs on the preparation worker
    // thread, so StartRecording returns true immediately (accepted) and the Failed
    // result arrives asynchronously via the event loop — proof it is off-thread.
    EXPECT_TRUE(coordinator.StartRecording(target, capability::AudioUiState{}, std::nullopt));

    for (int i = 0; i < 500 && !failure.has_value(); ++i) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(failure.has_value());
    EXPECT_FALSE(failure->succeeded);
    // The contract: the dialog's format context equals what the recording
    // itself would have used — the same translation StartRecording runs.
    capability::UserRecorderConfig expected_config;
    expected_config.container = capability::Container::Mp4;
    expected_config.video_codec = capability::VideoCodec::H264;
    expected_config.audio_codec = capability::AudioCodec::Aac;
    const auto expected = capability::ToRecorderCoreConfig(expected_config, caps);
    EXPECT_EQ(failure->container, expected.container);
    EXPECT_EQ(failure->video_codec, expected.video_codec);
    EXPECT_EQ(failure->audio_codec, expected.audio_codec);
    EXPECT_EQ(failure->container, recorder_core::Container::Mp4); // not the WebM default

    std::error_code cleanup_ec;
    std::filesystem::remove(file_as_folder, cleanup_ec);
}

// ---------------------------------------------------------------------------
// Preparing-state (off-thread StartRecording) behaviour.
// ---------------------------------------------------------------------------

namespace {

// A ready coordinator: freshly-probed baseline caps, a valid temp output folder,
// and a validated MKV/AV1/Opus format. `out_folder` receives the temp folder so
// the caller can clean it up.
void MakeReadyCoordinator(RecordingCoordinator& coordinator, const std::filesystem::path& out_folder) {
    std::filesystem::create_directories(out_folder);

    OutputSettingsModel settings;
    settings.output_folder = out_folder;
    settings.container = capability::Container::Matroska;
    settings.video_codec = capability::VideoCodec::Av1;
    settings.audio_codec = capability::AudioCodec::Opus;
    coordinator.SetOutputSettings(settings);

    capability::CapabilitySet caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    caps.probed = true;
    coordinator.OnCapabilitiesReady(caps);
}

recorder_core::CaptureTarget MonitorTarget() {
    recorder_core::CaptureTarget target;
    target.kind = recorder_core::CaptureTarget::Kind::Monitor;
    target.native_id = 1;
    target.description = "\\\\.\\DISPLAY1";
    return target;
}

// A disk provider that parks the caller (the preparation worker) inside
// FreeBytesForPath until the test releases it — making the "worker is mid-prepare"
// window deterministic without touching a real device.
class GatedDiskSpaceProvider final : public diagnostics::IDiskSpaceProvider {
  public:
    explicit GatedDiskSpaceProvider(uint64_t free_bytes) : free_bytes_(free_bytes) {
    }

    std::optional<uint64_t> FreeBytesForPath(const std::filesystem::path&) const override {
        {
            std::unique_lock<std::mutex> lock(m_);
            entered_ = true;
            entered_cv_.notify_all();
            release_cv_.wait(lock, [this] { return released_; });
        }
        return free_bytes_;
    }

    void WaitEntered() {
        std::unique_lock<std::mutex> lock(m_);
        entered_cv_.wait(lock, [this] { return entered_; });
    }
    void Release() {
        {
            std::lock_guard<std::mutex> lock(m_);
            released_ = true;
        }
        release_cv_.notify_all();
    }

  private:
    uint64_t free_bytes_;
    mutable std::mutex m_;
    mutable std::condition_variable entered_cv_;
    mutable std::condition_variable release_cv_;
    mutable bool entered_ = false;
    bool released_ = false;
};

class StubFreeSpace final : public diagnostics::IDiskSpaceProvider {
  public:
    explicit StubFreeSpace(uint64_t free_bytes) : free_bytes_(free_bytes) {
    }
    std::optional<uint64_t> FreeBytesForPath(const std::filesystem::path&) const override {
        return free_bytes_;
    }

  private:
    uint64_t free_bytes_;
};

// Pump the event loop until `predicate` holds or the timeout elapses.
template <typename Pred> bool PumpUntil(Pred predicate, int max_iterations = 1000) {
    for (int i = 0; i < max_iterations && !predicate(); ++i) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

} // namespace

// The start is accepted immediately (returns true, off-thread) and the low-disk
// block is delivered asynchronously as Preparing → Failed(DiskSpace).
TEST(PreparingStateTest, DiskBlockPostsFailedOffThread) {
    int argc = 0;
    QCoreApplication app(argc, nullptr);

    RecordingCoordinator coordinator;
    const std::filesystem::path folder = UniqueTempPath(L"prep_disk");
    MakeReadyCoordinator(coordinator, folder);

    StubFreeSpace stub(diagnostics::kHardStopFreeBytes / 2); // at/under hard stop
    coordinator.SetDiskSpaceProvider(&stub);

    std::vector<UiRecordingState> states;
    coordinator.SetStateChangedCallback([&](UiRecordingState s) { states.push_back(s); });
    std::optional<UiRecordingResult> failure;
    coordinator.SetResultReadyCallback([&](const UiRecordingResult& r) { failure = r; });

    EXPECT_TRUE(coordinator.StartRecording(MonitorTarget(), capability::AudioUiState{}, std::nullopt));

    ASSERT_TRUE(PumpUntil([&] { return failure.has_value(); }));
    EXPECT_FALSE(failure->succeeded);
    EXPECT_EQ(failure->error_phase, std::wstring(L"DiskSpace"));
    // The state machine passed through Preparing before Failed — the whole point.
    ASSERT_GE(states.size(), 2u);
    EXPECT_EQ(states.front(), UiRecordingState::Preparing);
    EXPECT_EQ(states.back(), UiRecordingState::Failed);

    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
}

// A second StartRecording while a prepare is in flight is rejected (no double
// start), and no second Preparing is posted.
TEST(PreparingStateTest, SecondStartRejectedWhilePreparing) {
    int argc = 0;
    QCoreApplication app(argc, nullptr);

    RecordingCoordinator coordinator;
    const std::filesystem::path folder = UniqueTempPath(L"prep_reentrancy");
    MakeReadyCoordinator(coordinator, folder);

    GatedDiskSpaceProvider gate(50ULL * 1024 * 1024 * 1024); // 50 GB free
    coordinator.SetDiskSpaceProvider(&gate);

    int preparing_posts = 0;
    coordinator.SetStateChangedCallback([&](UiRecordingState s) {
        if (s == UiRecordingState::Preparing)
            ++preparing_posts;
    });

    EXPECT_TRUE(coordinator.StartRecording(MonitorTarget(), capability::AudioUiState{}, std::nullopt));
    gate.WaitEntered(); // worker is parked mid-prepare, prepare_in_flight_ is set

    // Second start must be rejected outright.
    EXPECT_FALSE(coordinator.StartRecording(MonitorTarget(), capability::AudioUiState{}, std::nullopt));

    // Cancel and let the worker unwind so the thread joins cleanly.
    coordinator.CancelPreparing();
    gate.Release();
    ASSERT_TRUE(PumpUntil([&] { return coordinator.State() == UiRecordingState::Ready; }));

    QCoreApplication::processEvents();
    EXPECT_EQ(preparing_posts, 1);

    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
}

// Cancelling during Preparing (before the recording commits) returns to Ready
// without ever posting Recording, and without leaking a recovery-manifest entry.
TEST(PreparingStateTest, CancelBeforeRecordReturnsToReady) {
    int argc = 0;
    QCoreApplication app(argc, nullptr);

    RecordingCoordinator coordinator;
    const std::filesystem::path folder = UniqueTempPath(L"prep_cancel");
    MakeReadyCoordinator(coordinator, folder);

    RecoveryManifestStore manifest_store(QString::fromStdWString((folder / L"recovery.json").wstring()));
    coordinator.SetRecoveryManifestStore(&manifest_store);

    GatedDiskSpaceProvider gate(50ULL * 1024 * 1024 * 1024);
    coordinator.SetDiskSpaceProvider(&gate);

    std::vector<UiRecordingState> states;
    coordinator.SetStateChangedCallback([&](UiRecordingState s) { states.push_back(s); });

    EXPECT_TRUE(coordinator.StartRecording(MonitorTarget(), capability::AudioUiState{}, std::nullopt));
    gate.WaitEntered();
    // Request the cancel while the worker is parked in the disk query, so the flag
    // is set before the worker reaches its first checkpoint.
    coordinator.CancelPreparing();
    gate.Release();

    ASSERT_TRUE(PumpUntil([&] { return coordinator.State() == UiRecordingState::Ready; }));
    QCoreApplication::processEvents();

    // Never reached Recording.
    for (UiRecordingState s : states)
        EXPECT_NE(s, UiRecordingState::Recording);
    EXPECT_EQ(states.back(), UiRecordingState::Ready);
    EXPECT_FALSE(coordinator.IsArmedFromRecovery());
    // No orphaned manifest entry from the cancelled prepare.
    EXPECT_TRUE(manifest_store.Entries().isEmpty());

    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
}

} // namespace
} // namespace exosnap
