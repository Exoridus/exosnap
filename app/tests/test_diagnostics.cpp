#include <gtest/gtest.h>

#include <filesystem>

#include "diagnostics/CapabilitySummary.h"
#include "diagnostics/ConfigSummary.h"
#include "diagnostics/DiagnosticResult.h"
#include "diagnostics/DiagnosticsPresentation.h"
#include "diagnostics/RecommendationEngine.h"
#include "diagnostics/SelfTestRunner.h"

#include "models/OutputSettingsModel.h"
#include "models/VideoSettingsModel.h"

#include <capability/capability_builder.h>
#include <capability/capability_set.h>
#include <capability/resolver.h>
#include <capability/user_config.h>

namespace exosnap::diagnostics {
namespace {

// --- DiagnosticResult tests ---

TEST(DiagnosticsTest, DiagnosticChecklist_Empty_HasNoBlocker) {
    DiagnosticChecklist checklist;
    EXPECT_FALSE(checklist.has_blocker);
    EXPECT_FALSE(checklist.has_notice);
    EXPECT_EQ(checklist.worst_severity(), DiagnosticSeverity::Pass);
}

TEST(DiagnosticsTest, DiagnosticChecklist_WithBlocker) {
    DiagnosticChecklist checklist;
    DiagnosticResult r;
    r.severity = DiagnosticSeverity::Blocker;
    r.title = "Test blocker";
    checklist.results.push_back(r);
    checklist.has_blocker = true;

    EXPECT_TRUE(checklist.has_blocker);
    EXPECT_EQ(checklist.worst_severity(), DiagnosticSeverity::Blocker);
}

TEST(DiagnosticsTest, DiagnosticChecklist_WithNotice) {
    DiagnosticChecklist checklist;
    DiagnosticResult r;
    r.severity = DiagnosticSeverity::Notice;
    r.title = "Test notice";
    checklist.results.push_back(r);
    checklist.has_notice = true;

    EXPECT_TRUE(checklist.has_notice);
    EXPECT_EQ(checklist.worst_severity(), DiagnosticSeverity::Notice);
}

// --- CapabilitySummary tests ---

TEST(CapabilitySummaryTest, FromCapabilitySet_HasOsInfo) {
    capability::CapabilitySet caps;
    caps.runtime.os.version_string = "Windows 10 Pro";
    caps.runtime.os.build_number = 19045;

    auto summary = CapabilitySummary::FromCapabilitySet(caps);
    EXPECT_GE(summary.entries.size(), 1u);
}

TEST(CapabilitySummaryTest, FromCapabilitySet_ReportsNvenc) {
    capability::CapabilitySet caps;
    caps.nvenc_dll_present = true;
    caps.runtime.nvidia.nvenc_api_version = 1200;

    auto summary = CapabilitySummary::FromCapabilitySet(caps);
    bool found_nvenc = false;
    for (const auto& entry : summary.entries) {
        if (entry.label == "NVENC Available") {
            found_nvenc = true;
            EXPECT_EQ(entry.value, "Yes");
            EXPECT_TRUE(entry.available);
        }
    }
    EXPECT_TRUE(found_nvenc);
}

TEST(CapabilitySummaryTest, FromCapabilitySet_ReportsPerDisplayHdr) {
    capability::CapabilitySet caps;
    capability::DisplayHdrFacts disp;
    disp.name = "\\\\.\\DISPLAY7";
    disp.hdr_active = false;
    disp.bits_per_color = 10;
    disp.max_luminance_nits = 1499.0f;
    caps.runtime.displays.push_back(disp);

    auto summary = CapabilitySummary::FromCapabilitySet(caps);
    bool found = false;
    for (const auto& entry : summary.entries) {
        if (entry.label == "\\\\.\\DISPLAY7") {
            found = true;
            EXPECT_NE(entry.value.find("HDR off"), std::string::npos);
            EXPECT_NE(entry.value.find("10-bit"), std::string::npos);
            EXPECT_NE(entry.value.find("1499 nits"), std::string::npos);
        }
    }
    EXPECT_TRUE(found);
}

TEST(CapabilitySummaryTest, FromCapabilitySet_ReportsVideoCodecs) {
    capability::CapabilitySet caps;
    caps.video_codecs[capability::VideoCodec::H264Nvenc] = {capability::SupportLevel::Available, ""};
    caps.video_codecs[capability::VideoCodec::HevcNvenc] = {capability::SupportLevel::NotImplemented,
                                                            "No HEVC support"};
    caps.video_codecs[capability::VideoCodec::Av1Nvenc] = {capability::SupportLevel::Invalid, "NVENC missing"};

    auto summary = CapabilitySummary::FromCapabilitySet(caps);
    int video_count = 0;
    for (const auto& entry : summary.entries) {
        if (entry.label.find("H.264") != std::string::npos || entry.label.find("HEVC") != std::string::npos ||
            entry.label.find("AV1") != std::string::npos) {
            ++video_count;
        }
    }
    EXPECT_GE(video_count, 3);
}

TEST(CapabilitySummaryTest, FromCapabilitySet_ReportsAudioCodecs) {
    capability::CapabilitySet caps;
    caps.audio_codecs[capability::AudioCodec::Opus] = {capability::SupportLevel::Available, ""};
    caps.audio_codecs[capability::AudioCodec::AacMf] = {capability::SupportLevel::Available, ""};
    caps.audio_codecs[capability::AudioCodec::Pcm] = {capability::SupportLevel::NotImplemented, ""};

    auto summary = CapabilitySummary::FromCapabilitySet(caps);
    int audio_count = 0;
    for (const auto& entry : summary.entries) {
        if (entry.label.find("Opus") != std::string::npos || entry.label.find("AAC") != std::string::npos ||
            entry.label.find("PCM") != std::string::npos) {
            ++audio_count;
        }
    }
    EXPECT_GE(audio_count, 3);
}

TEST(CapabilitySummaryTest, FromCapabilitySet_ReportsContainers) {
    capability::CapabilitySet caps;
    caps.containers[capability::Container::Matroska] = {capability::SupportLevel::Available, ""};
    caps.containers[capability::Container::Mp4] = {capability::SupportLevel::Available, ""};
    caps.containers[capability::Container::WebM] = {capability::SupportLevel::Available, ""};

    auto summary = CapabilitySummary::FromCapabilitySet(caps);
    int container_count = 0;
    for (const auto& entry : summary.entries) {
        if (entry.label.find("MKV") != std::string::npos || entry.label.find("MP4") != std::string::npos ||
            entry.label.find("WebM") != std::string::npos) {
            ++container_count;
        }
    }
    EXPECT_GE(container_count, 3);
}

// --- ConfigSummary tests ---

TEST(ConfigSummaryTest, FromCurrentSettings_HasExpectedFields) {
    OutputSettingsModel output;
    output.container = capability::Container::Matroska;
    output.video_codec = capability::VideoCodec::H264Nvenc;
    output.audio_codec = capability::AudioCodec::AacMf;
    output.output_folder = std::filesystem::path(L"C:/Videos/ExoSnap");
    output.naming_pattern = L"{datetime}_{app}";

    VideoSettingsModel video;
    video.cfr = true;
    video.capture_cursor = true;

    capability::AudioUiState audio;
    audio.source_rows = {
        {recorder_core::AudioSourceKind::SystemOutput, true, false},
        {recorder_core::AudioSourceKind::Mic, true, true},
    };

    auto summary = ConfigSummary::FromCurrentSettings(output, video, audio, std::filesystem::path(L"C:/settings.ini"),
                                                      "MKV H.264 AAC", "Start/Stop: Alt+F9");

    // Verify key fields exist
    bool has_container = false, has_video = false, has_audio = false, has_cfr = false, has_path = false,
         has_profile = false;
    for (const auto& entry : summary.entries) {
        if (entry.label == "Container") {
            has_container = true;
            EXPECT_EQ(entry.value, "MKV");
        }
        if (entry.label == "Video Codec") {
            has_video = true;
        }
        if (entry.label == "Audio Codec") {
            has_audio = true;
        }
        if (entry.label == "CFR/VFR") {
            has_cfr = true;
            EXPECT_EQ(entry.value, "CFR");
        }
        if (entry.label == "Output Folder") {
            has_path = true;
        }
        if (entry.label == "Recording Profile") {
            has_profile = true;
        }
    }
    EXPECT_TRUE(has_container);
    EXPECT_TRUE(has_video);
    EXPECT_TRUE(has_audio);
    EXPECT_TRUE(has_cfr);
    EXPECT_TRUE(has_path);
    EXPECT_TRUE(has_profile);
}

TEST(ConfigSummaryTest, FromCurrentSettings_VfrReportsCorrectly) {
    OutputSettingsModel output;
    VideoSettingsModel video;
    video.cfr = false;

    capability::AudioUiState audio;
    auto summary = ConfigSummary::FromCurrentSettings(output, video, audio, std::filesystem::path(), "test", "");

    for (const auto& entry : summary.entries) {
        if (entry.label == "CFR/VFR") {
            EXPECT_EQ(entry.value, "VFR");
            return;
        }
    }
    FAIL() << "CFR/VFR entry not found";
}

TEST(ConfigSummaryTest, FromCurrentSettings_AudioRoutingWithMerge) {
    OutputSettingsModel output;
    VideoSettingsModel video;

    capability::AudioUiState audio;
    audio.source_rows = {
        {recorder_core::AudioSourceKind::SystemOutput, true, false},
        {recorder_core::AudioSourceKind::Mic, false, false},
        {recorder_core::AudioSourceKind::Sys, true, true},
    };

    auto summary = ConfigSummary::FromCurrentSettings(output, video, audio, std::filesystem::path(), "test", "");

    for (const auto& entry : summary.entries) {
        if (entry.label == "Audio Routing") {
            EXPECT_NE(entry.value.find("MERGED"), std::string::npos);
            EXPECT_NE(entry.value.find("OFF"), std::string::npos);
            return;
        }
    }
    FAIL() << "Audio Routing entry not found";
}

TEST(ConfigSummaryTest, UserConfigFromSettings_UsesActiveOutputSelection) {
    OutputSettingsModel output;
    output.container = capability::Container::WebM;
    output.video_codec = capability::VideoCodec::Av1Nvenc;
    output.audio_codec = capability::AudioCodec::Opus;

    VideoSettingsModel video;
    video.cfr = false;

    const capability::UserRecorderConfig config = UserConfigFromSettings(output, video);
    EXPECT_EQ(config.container, capability::Container::WebM);
    EXPECT_EQ(config.video_codec, capability::VideoCodec::Av1Nvenc);
    EXPECT_EQ(config.audio_codec, capability::AudioCodec::Opus);
    EXPECT_EQ(config.chroma, capability::ChromaSubsampling::Cs420);
    EXPECT_EQ(config.bit_depth, capability::BitDepth::Bit8);
    EXPECT_EQ(config.frame_rate_num, 60u);
    EXPECT_EQ(config.frame_rate_den, 1u);
}

// 0.7.0 — S7: the selected video bit depth flows into UserRecorderConfig.bit_depth
// (was hardcoded to Bit8 before this slice).
TEST(ConfigSummaryTest, UserConfigFromSettings_UsesSelectedVideoBitDepth) {
    OutputSettingsModel output;
    output.container = capability::Container::Matroska;
    output.video_codec = capability::VideoCodec::HevcNvenc;
    output.audio_codec = capability::AudioCodec::Opus;
    output.bit_depth = capability::BitDepth::Bit10;

    VideoSettingsModel video;

    const capability::UserRecorderConfig config = UserConfigFromSettings(output, video);
    EXPECT_EQ(config.video_codec, capability::VideoCodec::HevcNvenc);
    EXPECT_EQ(config.bit_depth, capability::BitDepth::Bit10);
}

TEST(ConfigSummaryTest, UserConfigFromSettings_MapsMp4H264AacProfileSelection) {
    OutputSettingsModel output;
    output.container = capability::Container::Mp4;
    output.video_codec = capability::VideoCodec::H264Nvenc;
    output.audio_codec = capability::AudioCodec::AacMf;

    VideoSettingsModel video;
    video.cfr = true;

    const capability::UserRecorderConfig config = UserConfigFromSettings(output, video);
    EXPECT_EQ(config.container, capability::Container::Mp4);
    EXPECT_EQ(config.video_codec, capability::VideoCodec::H264Nvenc);
    EXPECT_EQ(config.audio_codec, capability::AudioCodec::AacMf);
    EXPECT_EQ(config.frame_rate_num, 60u);
    EXPECT_EQ(config.frame_rate_den, 1u);
}

// --- DiagnosticsPresentation tests ---

TEST(DiagnosticsPresentationTest, InvalidFieldMappings_ReturnExpectedDisplayNamesAndActionHints) {
    EXPECT_EQ(InvalidFieldDisplayName("container"), "Container");
    EXPECT_EQ(InvalidFieldDisplayName("video_codec"), "Video codec");
    EXPECT_EQ(InvalidFieldDisplayName("audio_codec"), "Audio codec");
    EXPECT_EQ(InvalidFieldDisplayName("frame_rate"), "Frame rate");
    EXPECT_EQ(InvalidFieldDisplayName("output_width"), "Output width");
    EXPECT_EQ(InvalidFieldDisplayName("output_height"), "Output height");
    EXPECT_EQ(InvalidFieldDisplayName("config"), "Setting combination");
    EXPECT_EQ(InvalidFieldDisplayName("unknown_field"), "Configuration");

    EXPECT_EQ(InvalidFieldActionHint("frame_rate"), "Adjust this value in Format & Encoding settings.");
    EXPECT_EQ(InvalidFieldActionHint("output_width"), "Adjust this value in Output settings.");
    EXPECT_EQ(InvalidFieldActionHint("output_height"), "Adjust this value in Output settings.");
    EXPECT_EQ(InvalidFieldActionHint("video_codec"), "Adjust the selected profile in Output or Video settings.");
}

TEST(DiagnosticsPresentationTest, ResolverInvalidityField_MapsToSpecificFrameRateGuidance) {
    capability::CapabilitySet caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    capability::SettingsResolver resolver(caps);

    capability::UserRecorderConfig invalid_config;
    invalid_config.frame_rate_num = 0;
    invalid_config.frame_rate_den = 1;

    const capability::ResolveResult result = resolver.ValidateConfig(invalid_config);
    ASSERT_FALSE(result.succeeded);
    ASSERT_FALSE(result.invalidity.empty());
    EXPECT_EQ(result.invalidity.front().field, "frame_rate");
    EXPECT_EQ(InvalidFieldDisplayName(result.invalidity.front().field), "Frame rate");
    EXPECT_EQ(InvalidFieldActionHint(result.invalidity.front().field),
              "Adjust this value in Format & Encoding settings.");
}

TEST(DiagnosticsPresentationTest, BuildTopIssueRecommendations_BlockerFirstAndRec006SuppressedWhenInvalidityExists) {
    DiagnosticChecklist recommendations;

    DiagnosticResult notice_one;
    notice_one.id = "rec.001";
    notice_one.severity = DiagnosticSeverity::Notice;
    recommendations.results.push_back(notice_one);

    DiagnosticResult blocker_video;
    blocker_video.id = "rec.003";
    blocker_video.severity = DiagnosticSeverity::Blocker;
    recommendations.results.push_back(blocker_video);

    DiagnosticResult blocker_audio;
    blocker_audio.id = "rec.004";
    blocker_audio.severity = DiagnosticSeverity::Blocker;
    recommendations.results.push_back(blocker_audio);

    DiagnosticResult blocker_profile;
    blocker_profile.id = "rec.006";
    blocker_profile.severity = DiagnosticSeverity::Blocker;
    recommendations.results.push_back(blocker_profile);

    DiagnosticResult notice_two;
    notice_two.id = "rec.005";
    notice_two.severity = DiagnosticSeverity::Notice;
    recommendations.results.push_back(notice_two);

    DiagnosticResult pass_item;
    pass_item.id = "rec.pass";
    pass_item.severity = DiagnosticSeverity::Pass;
    recommendations.results.push_back(pass_item);

    const std::vector<DiagnosticResult> ordered_without_suppression =
        BuildTopIssueRecommendations(recommendations, false);
    ASSERT_EQ(ordered_without_suppression.size(), 5u);
    EXPECT_EQ(ordered_without_suppression[0].id, "rec.003");
    EXPECT_EQ(ordered_without_suppression[1].id, "rec.004");
    EXPECT_EQ(ordered_without_suppression[2].id, "rec.006");
    EXPECT_EQ(ordered_without_suppression[3].id, "rec.001");
    EXPECT_EQ(ordered_without_suppression[4].id, "rec.005");

    const std::vector<DiagnosticResult> ordered_with_suppression = BuildTopIssueRecommendations(recommendations, true);
    ASSERT_EQ(ordered_with_suppression.size(), 4u);
    EXPECT_EQ(ordered_with_suppression[0].id, "rec.003");
    EXPECT_EQ(ordered_with_suppression[1].id, "rec.004");
    EXPECT_EQ(ordered_with_suppression[2].id, "rec.001");
    EXPECT_EQ(ordered_with_suppression[3].id, "rec.005");
}

// --- RecommendationEngine tests ---

TEST(RecommendationEngineTest, Generate_EmptyNoFlag) {
    capability::CapabilitySet caps;
    caps.video_codecs[capability::VideoCodec::Av1Nvenc] = {capability::SupportLevel::Available, ""};
    caps.audio_codecs[capability::AudioCodec::Opus] = {capability::SupportLevel::Available, ""};

    capability::UserRecorderConfig config;
    config.container = capability::Container::Matroska;
    config.video_codec = capability::VideoCodec::Av1Nvenc;
    config.audio_codec = capability::AudioCodec::Opus;

    RecommendationEngine engine(caps, config, 0, 0, true);
    auto checklist = engine.Generate();
    // MKV selected, no rate mismatch, no drive space warning, profile supported
    // Should have zero results
    EXPECT_TRUE(checklist.results.empty());
}

TEST(RecommendationEngineTest, Generate_Mp4_Warns) {
    capability::CapabilitySet caps;
    caps.video_codecs[capability::VideoCodec::H264Nvenc] = {capability::SupportLevel::Available, ""};
    caps.audio_codecs[capability::AudioCodec::AacMf] = {capability::SupportLevel::Available, ""};
    caps.containers[capability::Container::Mp4] = {capability::SupportLevel::Available, ""};

    capability::UserRecorderConfig config;
    config.container = capability::Container::Mp4;
    config.video_codec = capability::VideoCodec::H264Nvenc;
    config.audio_codec = capability::AudioCodec::AacMf;

    RecommendationEngine engine(caps, config, 0, 0, true);
    auto checklist = engine.Generate();

    bool found_mp4_warning = false;
    for (const auto& r : checklist.results) {
        if (r.id == "rec.002") {
            found_mp4_warning = true;
            EXPECT_EQ(r.severity, DiagnosticSeverity::Notice);
        }
    }
    EXPECT_TRUE(found_mp4_warning);
}

TEST(RecommendationEngineTest, Generate_Mp4_HasFixAction_Assisted) {
    // rec.002 must carry a typed FixAction with Safety::Assisted — the UI will offer
    // a one-click path that opens Output settings (user still performs the last step).
    capability::CapabilitySet caps;
    caps.video_codecs[capability::VideoCodec::H264Nvenc] = {capability::SupportLevel::Available, ""};
    caps.audio_codecs[capability::AudioCodec::AacMf] = {capability::SupportLevel::Available, ""};
    caps.containers[capability::Container::Mp4] = {capability::SupportLevel::Available, ""};

    capability::UserRecorderConfig config;
    config.container = capability::Container::Mp4;
    config.video_codec = capability::VideoCodec::H264Nvenc;
    config.audio_codec = capability::AudioCodec::AacMf;

    RecommendationEngine engine(caps, config, 0, 0, true);
    auto checklist = engine.Generate();

    bool found = false;
    for (const auto& r : checklist.results) {
        if (r.id == "rec.002") {
            found = true;
            EXPECT_TRUE(r.fix_action.has_value());
            EXPECT_EQ(r.fix_action->id, "fix.container.mkv");
            EXPECT_EQ(r.fix_action->safety, FixAction::Safety::Assisted);
            EXPECT_TRUE(r.fix_action->reversible);
            EXPECT_FALSE(r.fix_action->changes_summary.empty());
        }
    }
    EXPECT_TRUE(found);
}

TEST(RecommendationEngineTest, Generate_RefreshRateMismatch) {
    capability::CapabilitySet caps;
    caps.video_codecs[capability::VideoCodec::Av1Nvenc] = {capability::SupportLevel::Available, ""};
    caps.audio_codecs[capability::AudioCodec::Opus] = {capability::SupportLevel::Available, ""};

    capability::UserRecorderConfig config;
    config.frame_rate_num = 60;
    config.frame_rate_den = 1;

    // 144 Hz monitor + 60 fps = warn
    RecommendationEngine engine(caps, config, 144, 0, true);
    auto checklist = engine.Generate();

    bool found_mismatch = false;
    for (const auto& r : checklist.results) {
        if (r.id == "rec.001") {
            found_mismatch = true;
            EXPECT_EQ(r.severity, DiagnosticSeverity::Notice);
        }
    }
    EXPECT_TRUE(found_mismatch);
}

TEST(RecommendationEngineTest, Generate_RefreshRateMatch_NoWarn) {
    capability::CapabilitySet caps;
    caps.video_codecs[capability::VideoCodec::Av1Nvenc] = {capability::SupportLevel::Available, ""};
    caps.audio_codecs[capability::AudioCodec::Opus] = {capability::SupportLevel::Available, ""};

    capability::UserRecorderConfig config;
    config.frame_rate_num = 60;
    config.frame_rate_den = 1;

    // 60 Hz monitor + 60 fps = no warn
    RecommendationEngine engine(caps, config, 60, 0, true);
    auto checklist = engine.Generate();

    bool found_mismatch = false;
    for (const auto& r : checklist.results) {
        if (r.id == "rec.001")
            found_mismatch = true;
    }
    EXPECT_FALSE(found_mismatch);
}

// --- Live present-cadence correlation (v0.8.0 / ADR 0033) ---

namespace {
recorder_core::RecordingDiagnosticsSnapshot MakeJudderSnapshot(bool cfr, double jitter_ms, double coalesce_ratio) {
    recorder_core::RecordingDiagnosticsSnapshot live;
    live.valid = true;
    live.video_encoder.cfr = cfr;
    live.capture.present_cadence_availability = recorder_core::MetricAvailability::Available;
    live.capture.source_present_jitter_ms = jitter_ms;
    live.capture.source_coalesce_ratio = coalesce_ratio;
    return live;
}
} // namespace

TEST(RecommendationEngineTest, Generate_LiveJitter_HighConfidenceJudder) {
    capability::CapabilitySet caps;
    caps.video_codecs[capability::VideoCodec::Av1Nvenc] = {capability::SupportLevel::Available, ""};
    caps.audio_codecs[capability::AudioCodec::Opus] = {capability::SupportLevel::Available, ""};
    capability::UserRecorderConfig config;
    config.frame_rate_num = 60;
    config.frame_rate_den = 1;

    // monitor_refresh = 0 (unknown) → static arm suppressed; the live judder arm fires alone.
    const auto live = MakeJudderSnapshot(/*cfr=*/true, /*jitter_ms=*/9.0, /*coalesce_ratio=*/1.0);
    RecommendationEngine engine(caps, config, 0, 0, true, "", &live);
    auto checklist = engine.Generate();

    bool found = false;
    for (const auto& r : checklist.results) {
        if (r.id == "rec.001") {
            found = true;
            EXPECT_EQ(r.severity, DiagnosticSeverity::Notice);
            EXPECT_NE(r.title.find("judder"), std::string::npos);
            ASSERT_TRUE(r.fix_action.has_value());
            EXPECT_EQ(r.fix_action->id, "fix.fps.cap");
        }
    }
    EXPECT_TRUE(found);
}

TEST(RecommendationEngineTest, Generate_LiveCoalesce_HighConfidenceJudder) {
    capability::CapabilitySet caps;
    caps.video_codecs[capability::VideoCodec::Av1Nvenc] = {capability::SupportLevel::Available, ""};
    caps.audio_codecs[capability::AudioCodec::Opus] = {capability::SupportLevel::Available, ""};
    capability::UserRecorderConfig config;
    config.frame_rate_num = 60;
    config.frame_rate_den = 1;

    // Low jitter but sustained coalescing (> 1.5) → judder arm fires.
    const auto live = MakeJudderSnapshot(/*cfr=*/true, /*jitter_ms=*/1.0, /*coalesce_ratio=*/2.0);
    RecommendationEngine engine(caps, config, 0, 0, true, "", &live);
    auto checklist = engine.Generate();

    bool found = false;
    for (const auto& r : checklist.results)
        if (r.id == "rec.001")
            found = true;
    EXPECT_TRUE(found);
}

TEST(RecommendationEngineTest, Generate_LiveJudder_VfrDoesNotFire) {
    capability::CapabilitySet caps;
    caps.video_codecs[capability::VideoCodec::Av1Nvenc] = {capability::SupportLevel::Available, ""};
    caps.audio_codecs[capability::AudioCodec::Opus] = {capability::SupportLevel::Available, ""};
    capability::UserRecorderConfig config;
    config.frame_rate_num = 60;
    config.frame_rate_den = 1;

    // VFR capture: present jitter is expected and not a CFR-judder signal. monitor=0 → no
    // static arm either, so rec.001 must not appear.
    const auto live = MakeJudderSnapshot(/*cfr=*/false, /*jitter_ms=*/9.0, /*coalesce_ratio=*/3.0);
    RecommendationEngine engine(caps, config, 0, 0, true, "", &live);
    auto checklist = engine.Generate();

    for (const auto& r : checklist.results)
        EXPECT_NE(r.id, "rec.001");
}

TEST(RecommendationEngineTest, Generate_LiveBelowThreshold_DoesNotFire) {
    capability::CapabilitySet caps;
    caps.video_codecs[capability::VideoCodec::Av1Nvenc] = {capability::SupportLevel::Available, ""};
    caps.audio_codecs[capability::AudioCodec::Opus] = {capability::SupportLevel::Available, ""};
    capability::UserRecorderConfig config;
    config.frame_rate_num = 60;
    config.frame_rate_den = 1;

    // CFR but within conservative thresholds (jitter <= 4 ms, coalesce <= 1.5) → no diagnosis.
    const auto live = MakeJudderSnapshot(/*cfr=*/true, /*jitter_ms=*/2.0, /*coalesce_ratio=*/1.1);
    RecommendationEngine engine(caps, config, 0, 0, true, "", &live);
    auto checklist = engine.Generate();

    for (const auto& r : checklist.results)
        EXPECT_NE(r.id, "rec.001");
}

TEST(RecommendationEngineTest, Generate_StaticMismatchStillFiresWithoutLive) {
    capability::CapabilitySet caps;
    caps.video_codecs[capability::VideoCodec::Av1Nvenc] = {capability::SupportLevel::Available, ""};
    caps.audio_codecs[capability::AudioCodec::Opus] = {capability::SupportLevel::Available, ""};
    capability::UserRecorderConfig config;
    config.frame_rate_num = 60;
    config.frame_rate_den = 1;

    // 144 Hz monitor, no live snapshot → original static heuristic wording (no "judder").
    RecommendationEngine engine(caps, config, 144, 0, true, "", nullptr);
    auto checklist = engine.Generate();

    bool found = false;
    for (const auto& r : checklist.results) {
        if (r.id == "rec.001") {
            found = true;
            EXPECT_EQ(r.title.find("judder"), std::string::npos);
        }
    }
    EXPECT_TRUE(found);
}

TEST(RecommendationEngineTest, Generate_CodecUnavailable_Blocker) {
    capability::CapabilitySet caps;
    caps.video_codecs[capability::VideoCodec::Av1Nvenc] = {capability::SupportLevel::Invalid, "NVENC not found"};
    caps.audio_codecs[capability::AudioCodec::Opus] = {capability::SupportLevel::Available, ""};

    capability::UserRecorderConfig config;
    config.container = capability::Container::WebM;
    config.video_codec = capability::VideoCodec::Av1Nvenc;
    config.audio_codec = capability::AudioCodec::Opus;

    RecommendationEngine engine(caps, config, 0, 0, true);
    auto checklist = engine.Generate();

    bool found_blocker = false;
    for (const auto& r : checklist.results) {
        if (r.id == "rec.003") {
            found_blocker = true;
            EXPECT_EQ(r.severity, DiagnosticSeverity::Blocker);
        }
    }
    EXPECT_TRUE(found_blocker);
}

TEST(RecommendationEngineTest, Generate_Rec009_FlacMp4_Blocker) {
    capability::CapabilitySet caps;
    caps.audio_codecs[capability::AudioCodec::Flac] = {capability::SupportLevel::Available, ""};
    caps.video_codecs[capability::VideoCodec::H264Nvenc] = {capability::SupportLevel::Available, ""};

    capability::UserRecorderConfig config;
    config.container = capability::Container::Mp4;
    config.video_codec = capability::VideoCodec::H264Nvenc;
    config.audio_codec = capability::AudioCodec::Flac;

    RecommendationEngine engine(caps, config, 0, 10ULL * 1024 * 1024 * 1024, true);
    auto checklist = engine.Generate();

    bool found = false;
    for (const auto& r : checklist.results) {
        if (r.id == "rec.009") {
            found = true;
            EXPECT_EQ(r.severity, DiagnosticSeverity::Blocker);
            ASSERT_TRUE(r.fix_action.has_value());
            EXPECT_EQ(r.fix_action->id, "fix.audio.flac_to_mkv");
            EXPECT_EQ(r.fix_action->safety, FixAction::Safety::Assisted);
            EXPECT_TRUE(r.fix_action->reversible);
            EXPECT_FALSE(r.fix_action->changes_summary.empty());
        }
    }
    EXPECT_TRUE(found);
    EXPECT_TRUE(checklist.has_blocker);
}

TEST(RecommendationEngineTest, Generate_Rec009_OpusMp4_Notice) {
    capability::CapabilitySet caps;
    caps.audio_codecs[capability::AudioCodec::Opus] = {capability::SupportLevel::Available, ""};
    caps.video_codecs[capability::VideoCodec::H264Nvenc] = {capability::SupportLevel::Available, ""};
    caps.containers[capability::Container::Mp4] = {capability::SupportLevel::Available, ""};

    capability::UserRecorderConfig config;
    config.container = capability::Container::Mp4;
    config.video_codec = capability::VideoCodec::H264Nvenc;
    config.audio_codec = capability::AudioCodec::Opus;

    RecommendationEngine engine(caps, config, 0, 0, true);
    auto checklist = engine.Generate();

    bool found = false;
    for (const auto& r : checklist.results) {
        if (r.id == "rec.009") {
            found = true;
            EXPECT_EQ(r.severity, DiagnosticSeverity::Notice);
            ASSERT_TRUE(r.fix_action.has_value());
            EXPECT_EQ(r.fix_action->id, "fix.audio.opus_to_aac");
            EXPECT_EQ(r.fix_action->safety, FixAction::Safety::Auto);
            EXPECT_TRUE(r.fix_action->reversible);
        }
    }
    EXPECT_TRUE(found);
}

TEST(RecommendationEngineTest, Generate_Rec009_OpusMkv_NoFire) {
    capability::CapabilitySet caps;
    caps.audio_codecs[capability::AudioCodec::Opus] = {capability::SupportLevel::Available, ""};
    caps.video_codecs[capability::VideoCodec::Av1Nvenc] = {capability::SupportLevel::Available, ""};

    capability::UserRecorderConfig config;
    config.container = capability::Container::Matroska;
    config.video_codec = capability::VideoCodec::Av1Nvenc;
    config.audio_codec = capability::AudioCodec::Opus;

    RecommendationEngine engine(caps, config, 0, 0, true);
    auto checklist = engine.Generate();

    for (const auto& r : checklist.results) {
        EXPECT_NE(r.id, "rec.009") << "rec.009 must not fire for Opus+MKV";
    }
}

TEST(RecommendationEngineTest, Generate_Rec010_HevcWebm_Blocker) {
    capability::CapabilitySet caps;
    caps.video_codecs[capability::VideoCodec::HevcNvenc] = {capability::SupportLevel::Available, ""};
    caps.audio_codecs[capability::AudioCodec::Opus] = {capability::SupportLevel::Available, ""};

    capability::UserRecorderConfig config;
    config.container = capability::Container::WebM;
    config.video_codec = capability::VideoCodec::HevcNvenc;
    config.audio_codec = capability::AudioCodec::Opus;

    RecommendationEngine engine(caps, config, 0, 10ULL * 1024 * 1024 * 1024, true);
    auto checklist = engine.Generate();

    bool found = false;
    for (const auto& r : checklist.results) {
        if (r.id == "rec.010") {
            found = true;
            EXPECT_EQ(r.severity, DiagnosticSeverity::Blocker);
            ASSERT_TRUE(r.fix_action.has_value());
            EXPECT_EQ(r.fix_action->id, "fix.video.hevc_webm");
            EXPECT_EQ(r.fix_action->safety, FixAction::Safety::Assisted);
            EXPECT_TRUE(r.fix_action->reversible);
            EXPECT_FALSE(r.fix_action->changes_summary.empty());
        }
    }
    EXPECT_TRUE(found);
    EXPECT_TRUE(checklist.has_blocker);
}

TEST(RecommendationEngineTest, Generate_Rec010_H264Webm_Blocker) {
    capability::CapabilitySet caps;
    caps.video_codecs[capability::VideoCodec::H264Nvenc] = {capability::SupportLevel::Available, ""};
    caps.audio_codecs[capability::AudioCodec::Opus] = {capability::SupportLevel::Available, ""};

    capability::UserRecorderConfig config;
    config.container = capability::Container::WebM;
    config.video_codec = capability::VideoCodec::H264Nvenc;
    config.audio_codec = capability::AudioCodec::Opus;

    RecommendationEngine engine(caps, config, 0, 10ULL * 1024 * 1024 * 1024, true);
    auto checklist = engine.Generate();

    bool found = false;
    for (const auto& r : checklist.results) {
        if (r.id == "rec.010") {
            found = true;
            EXPECT_EQ(r.severity, DiagnosticSeverity::Blocker);
            ASSERT_TRUE(r.fix_action.has_value());
            EXPECT_EQ(r.fix_action->id, "fix.video.h264_webm");
            EXPECT_EQ(r.fix_action->safety, FixAction::Safety::Assisted);
            EXPECT_TRUE(r.fix_action->reversible);
            EXPECT_FALSE(r.fix_action->changes_summary.empty());
        }
    }
    EXPECT_TRUE(found);
    EXPECT_TRUE(checklist.has_blocker);
}

TEST(RecommendationEngineTest, Generate_Rec010_Av1Webm_NoFire) {
    capability::CapabilitySet caps;
    caps.video_codecs[capability::VideoCodec::Av1Nvenc] = {capability::SupportLevel::Available, ""};
    caps.audio_codecs[capability::AudioCodec::Opus] = {capability::SupportLevel::Available, ""};

    capability::UserRecorderConfig config;
    config.container = capability::Container::WebM;
    config.video_codec = capability::VideoCodec::Av1Nvenc;
    config.audio_codec = capability::AudioCodec::Opus;

    RecommendationEngine engine(caps, config, 0, 0, true);
    auto checklist = engine.Generate();

    for (const auto& r : checklist.results) {
        EXPECT_NE(r.id, "rec.010") << "rec.010 must not fire for AV1+WebM";
    }
}

TEST(RecommendationEngineTest, Generate_LowDiskSpace_Warns) {
    capability::CapabilitySet caps;
    caps.video_codecs[capability::VideoCodec::Av1Nvenc] = {capability::SupportLevel::Available, ""};
    caps.audio_codecs[capability::AudioCodec::Opus] = {capability::SupportLevel::Available, ""};

    capability::UserRecorderConfig config;

    // 1 GB free: above the 500 MB hard-stop, below the 2 GB soft-warn → rec.005 Notice.
    // (LOW-DISK-GUARD-R1: 100 MB would now produce a rec.007 Blocker instead.)
    RecommendationEngine engine(caps, config, 0, 1ULL * 1024 * 1024 * 1024, true);
    auto checklist = engine.Generate();

    bool found_space = false;
    for (const auto& r : checklist.results) {
        if (r.id == "rec.005") {
            found_space = true;
            EXPECT_EQ(r.severity, DiagnosticSeverity::Notice);
        }
    }
    EXPECT_TRUE(found_space);
}

TEST(RecommendationEngineTest, Generate_UnsupportedProfile_Blocker) {
    capability::CapabilitySet caps;
    capability::UserRecorderConfig config;

    RecommendationEngine engine(caps, config, 0, 0, false);
    auto checklist = engine.Generate();

    bool found_blocker = false;
    for (const auto& r : checklist.results) {
        if (r.id == "rec.006") {
            found_blocker = true;
            EXPECT_EQ(r.severity, DiagnosticSeverity::Blocker);
        }
    }
    EXPECT_TRUE(found_blocker);
}

TEST(RecommendationEngineTest, GetAllRecommendationCodes_ReturnsExpected) {
    auto codes = RecommendationEngine::GetAllRecommendationCodes();
    // v0.8.0-D added rec.009 (audio/container compat) and rec.010 (video/container compat) — expect 10 codes now.
    EXPECT_EQ(codes.size(), 10u);
    EXPECT_NE(std::find(codes.begin(), codes.end(), "rec.001"), codes.end());
    EXPECT_NE(std::find(codes.begin(), codes.end(), "rec.005"), codes.end());
    EXPECT_NE(std::find(codes.begin(), codes.end(), "rec.006"), codes.end());
    EXPECT_NE(std::find(codes.begin(), codes.end(), "rec.007"), codes.end());
    EXPECT_NE(std::find(codes.begin(), codes.end(), "rec.008"), codes.end());
    EXPECT_NE(std::find(codes.begin(), codes.end(), "rec.009"), codes.end());
    EXPECT_NE(std::find(codes.begin(), codes.end(), "rec.010"), codes.end());
}

// ─── REC-R10: Active output config → ValidateConfig wiring ───────────────────
//
// Verifies the data path used by RevalidateCapabilities(): UserConfigFromSettings()
// produces a config that, when fed to ValidateConfig(), yields the correct Ready/Blocked
// result for the active profile.

TEST(ConfigSummaryTest, UserConfigFromSettings_DefaultMkvH264Aac_MatchesPrimaryConfig) {
    // The default profile (MKV + H264Nvenc + AAC) must produce a UserRecorderConfig that
    // matches the hardcoded primaryRecorderConfig() used at startup, so startup validation
    // and post-setOutputSettings revalidation produce consistent results.
    OutputSettingsModel output;
    output.container = capability::Container::Matroska;
    output.video_codec = capability::VideoCodec::H264Nvenc;
    output.audio_codec = capability::AudioCodec::AacMf;
    VideoSettingsModel video;

    const capability::UserRecorderConfig config = UserConfigFromSettings(output, video);
    EXPECT_EQ(config.container, capability::Container::Matroska);
    EXPECT_EQ(config.video_codec, capability::VideoCodec::H264Nvenc);
    EXPECT_EQ(config.audio_codec, capability::AudioCodec::AacMf);
    EXPECT_EQ(config.chroma, capability::ChromaSubsampling::Cs420);
    EXPECT_EQ(config.bit_depth, capability::BitDepth::Bit8);
    EXPECT_EQ(config.frame_rate_num, 60u);
    EXPECT_EQ(config.frame_rate_den, 1u);
}

TEST(BlockedScenarioTest, FailedStartupValidation_ResolvedConfig_HasSafeDimensionsForRevalidation) {
    // When startup validation fails (NVENC unavailable), validation.resolved_config must not carry
    // problematic output_width/output_height values that would mask the real codec blocker in
    // subsequent RevalidateCapabilities() calls (REC-R10 follow-up fix).
    capability::CapabilitySet caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    const std::string nvenc_reason = "NVENC unavailable: simulated";
    caps.video_codecs[capability::VideoCodec::H264Nvenc] = {capability::SupportLevel::NotImplemented, nvenc_reason};
    caps.video_codecs[capability::VideoCodec::Av1Nvenc] = {capability::SupportLevel::NotImplemented, nvenc_reason};
    const capability::ComboKey mkv_h264{capability::Container::Matroska, capability::VideoCodec::H264Nvenc,
                                        capability::AudioCodec::AacMf, capability::ChromaSubsampling::Cs420,
                                        capability::BitDepth::Bit8};
    caps.combo_overrides[mkv_h264] = {capability::SupportLevel::NotImplemented, nvenc_reason};

    capability::SettingsResolver resolver(caps);
    capability::UserRecorderConfig primary;
    primary.container = capability::Container::Matroska;
    primary.video_codec = capability::VideoCodec::H264Nvenc;
    primary.audio_codec = capability::AudioCodec::AacMf;
    primary.chroma = capability::ChromaSubsampling::Cs420;
    primary.bit_depth = capability::BitDepth::Bit8;
    primary.frame_rate_num = 60;
    primary.frame_rate_den = 1;

    const capability::ResolveResult result = resolver.ValidateConfig(primary);
    ASSERT_FALSE(result.succeeded);

    // resolved_config dimensions must remain safe (native-resolution 0/0) so that
    // OnCapabilitiesReady's resolved_user_config_ assignment does not introduce dimension
    // blockers that would shadow the video_codec root-cause in later revalidation.
    EXPECT_EQ(result.resolved_config.output_width, 0u);
    EXPECT_EQ(result.resolved_config.output_height, 0u);
    EXPECT_EQ(result.resolved_config.frame_rate_num, 60u);
    EXPECT_EQ(result.resolved_config.frame_rate_den, 1u);
}

TEST(BlockedScenarioTest, ActiveOutputConfig_WebmAv1Opus_ValidatesSuccessfullyOnCapableHardware) {
    // When the user selects WebM + AV1 + Opus and NVENC is available,
    // UserConfigFromSettings + ValidateConfig must succeed — mirrors RevalidateCapabilities().
    capability::CapabilitySet caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();

    OutputSettingsModel output;
    output.container = capability::Container::WebM;
    output.video_codec = capability::VideoCodec::Av1Nvenc;
    output.audio_codec = capability::AudioCodec::Opus;
    VideoSettingsModel video;

    const capability::UserRecorderConfig config = UserConfigFromSettings(output, video);
    capability::SettingsResolver resolver(caps);
    const capability::ResolveResult result = resolver.ValidateConfig(config);

    EXPECT_TRUE(result.succeeded);
    EXPECT_TRUE(result.invalidity.empty());
}

TEST(BlockedScenarioTest, ActiveOutputConfig_Av1NvencUnavailable_ValidateFails_VideoCodecBlocker) {
    // When AV1 (NVENC) is selected but NVENC is unavailable, UserConfigFromSettings +
    // ValidateConfig must fail and report video_codec as the blocker.
    capability::CapabilitySet caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    const std::string nvenc_reason = "NVENC unavailable: simulated";
    caps.video_codecs[capability::VideoCodec::Av1Nvenc] = {capability::SupportLevel::NotImplemented, nvenc_reason};

    OutputSettingsModel output;
    output.container = capability::Container::WebM;
    output.video_codec = capability::VideoCodec::Av1Nvenc;
    output.audio_codec = capability::AudioCodec::Opus;
    VideoSettingsModel video;

    const capability::UserRecorderConfig config = UserConfigFromSettings(output, video);
    capability::SettingsResolver resolver(caps);
    const capability::ResolveResult result = resolver.ValidateConfig(config);

    EXPECT_FALSE(result.succeeded);
    ASSERT_FALSE(result.invalidity.empty());
    EXPECT_EQ(result.invalidity.front().field, "video_codec");
    EXPECT_EQ(result.invalidity.front().message, nvenc_reason);
}

// --- SelfTestRunner tests ---

TEST(SelfTestTest, SelfTest_Run_ReturnsAllResults) {
    SelfTestRunner runner;
    auto checklist = runner.Run();
    // Always returns exactly 5 results; pass/fail mix is hardware-dependent.
    EXPECT_EQ(checklist.results.size(), 5u);
}

// Integration smoke: verify the probe runs without crashing and returns a coherent result.
// DXGI is available on any DirectX-capable Windows system, so pass is expected,
// but the assertion is on structure rather than hardware outcome.
TEST(SelfTestTest, SelfTest_CaptureAvailable_Smoke) {
    auto result = SelfTestRunner::CheckCaptureAvailability();
    EXPECT_EQ(result.category, "Capture");
    EXPECT_FALSE(result.detail.empty());
}

// Encoder probe depends on NVENC hardware; do not assert passed=true/false here.
TEST(SelfTestTest, SelfTest_EncoderAvailable_Smoke) {
    auto result = SelfTestRunner::CheckEncoderAvailability();
    EXPECT_EQ(result.category, "Encoder");
    EXPECT_FALSE(result.detail.empty());
}

// Muxer probe writes to the OS temp directory, which is always writable.
TEST(SelfTestTest, SelfTest_MuxerAvailable_PassesOnWritableTemp) {
    auto result = SelfTestRunner::CheckMuxerAvailability();
    EXPECT_TRUE(result.passed);
    EXPECT_EQ(result.category, "Muxer");
}

// Output path probe with empty path falls back to the temp directory (always writable).
TEST(SelfTestTest, SelfTest_OutputPathWritable_EmptyPath_Passes) {
    auto result = SelfTestRunner::CheckOutputPathWritable("");
    EXPECT_TRUE(result.passed);
    EXPECT_EQ(result.category, "Output Path");
}

// Output path probe with an explicit, known-writable temp directory.
TEST(SelfTestTest, SelfTest_OutputPath_ValidTempPath_Passes) {
    std::string tmp = std::filesystem::temp_directory_path().string();
    auto result = SelfTestRunner::CheckOutputPathWritable(tmp);
    EXPECT_TRUE(result.passed);
    EXPECT_EQ(result.category, "Output Path");
    EXPECT_NE(result.detail.find("Output path is writable"), std::string::npos);
}

// Output path probe with a path that cannot exist — must fail gracefully, never throw.
TEST(SelfTestTest, SelfTest_OutputPath_InvalidPath_FailsGracefully) {
    auto result = SelfTestRunner::CheckOutputPathWritable("Z:\\nonexistent\\xyzzy\\selftest\\abc");
    EXPECT_FALSE(result.passed);
    EXPECT_EQ(result.category, "Output Path");
    EXPECT_FALSE(result.detail.empty());
}

// Audio device probe is hardware-dependent; just verify it returns a coherent result.
TEST(SelfTestTest, SelfTest_AudioDeviceAvailable_Smoke) {
    auto result = SelfTestRunner::CheckAudioDeviceAvailability();
    EXPECT_EQ(result.category, "Audio Devices");
    EXPECT_FALSE(result.detail.empty());
}

// --- Display name helper tests ---

TEST(DisplayNameTest, VideoCodecDisplayNames) {
    EXPECT_EQ(VideoCodecDisplayName(capability::VideoCodec::H264Nvenc), "H.264 (NVENC)");
    EXPECT_EQ(VideoCodecDisplayName(capability::VideoCodec::HevcNvenc), "HEVC (NVENC)");
    EXPECT_EQ(VideoCodecDisplayName(capability::VideoCodec::Av1Nvenc), "AV1 (NVENC)");
}

TEST(DisplayNameTest, AudioCodecDisplayNames) {
    EXPECT_EQ(AudioCodecDisplayName(capability::AudioCodec::Opus), "Opus");
    EXPECT_EQ(AudioCodecDisplayName(capability::AudioCodec::AacMf), "AAC (Media Foundation)");
    EXPECT_EQ(AudioCodecDisplayName(capability::AudioCodec::Pcm), "PCM");
}

TEST(DisplayNameTest, ContainerDisplayNames) {
    EXPECT_EQ(ContainerDisplayName(capability::Container::Matroska), "MKV / Matroska");
    EXPECT_EQ(ContainerDisplayName(capability::Container::Mp4), "MP4");
    EXPECT_EQ(ContainerDisplayName(capability::Container::WebM), "WebM");
}

TEST(DisplayNameTest, SupportLevelStrings) {
    EXPECT_EQ(SupportLevelString(capability::SupportLevel::Available), "Available");
    EXPECT_EQ(SupportLevelString(capability::SupportLevel::NotImplemented), "Not implemented");
    EXPECT_EQ(SupportLevelString(capability::SupportLevel::Invalid), "Invalid");
}

// ─── Blocked Scenario Tests ───────────────────────────────────────────────────
//
// BLOCKED state sources:
//   1. Startup: RecordingCoordinator::OnCapabilitiesReady() validates primaryRecorderConfig()
//      (MKV + H264Nvenc + AAC + Cs420 + Bit8 + 60 fps) — fails when NVENC is unavailable.
//   2. Post-profile-change (REC-R10): RecordingCoordinator::RevalidateCapabilities() is called
//      from RecordPage::setOutputSettings() whenever the active profile or output settings
//      change. It validates the current resolved_user_config_ (fed by SetOutputSettings).
//
// Both paths use the same ValidateConfig() call. These tests prove the data path via
// synthetic CapabilitySets that mirror NVENC absence or codec unavailability.

TEST(BlockedScenarioTest, StartupConfig_NvencUnavailable_ValidateFails) {
    capability::CapabilitySet caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    const std::string nvenc_reason = "NVENC unavailable: simulated";
    caps.video_codecs[capability::VideoCodec::H264Nvenc] = {capability::SupportLevel::NotImplemented, nvenc_reason};
    caps.video_codecs[capability::VideoCodec::Av1Nvenc] = {capability::SupportLevel::NotImplemented, nvenc_reason};
    const capability::ComboKey mkv_h264{capability::Container::Matroska, capability::VideoCodec::H264Nvenc,
                                        capability::AudioCodec::AacMf, capability::ChromaSubsampling::Cs420,
                                        capability::BitDepth::Bit8};
    caps.combo_overrides[mkv_h264] = {capability::SupportLevel::NotImplemented, nvenc_reason};

    capability::SettingsResolver resolver(caps);

    // Matches the hardcoded primaryRecorderConfig() used by RecordPage::initCoordinator().
    capability::UserRecorderConfig primary;
    primary.container = capability::Container::Matroska;
    primary.video_codec = capability::VideoCodec::H264Nvenc;
    primary.audio_codec = capability::AudioCodec::AacMf;
    primary.chroma = capability::ChromaSubsampling::Cs420;
    primary.bit_depth = capability::BitDepth::Bit8;
    primary.frame_rate_num = 60;
    primary.frame_rate_den = 1;

    const capability::ResolveResult result = resolver.ValidateConfig(primary);
    // Validation failure drives state_ = Blocked in RecordingCoordinator::OnCapabilitiesReady().
    EXPECT_FALSE(result.succeeded);
    ASSERT_FALSE(result.invalidity.empty());
    // Must report video_codec as the root cause — not audio_codec via fallback path.
    EXPECT_EQ(result.invalidity.front().field, "video_codec");
}

TEST(BlockedScenarioTest, StartupConfig_NvencAvailable_ValidateSucceeds) {
    // Regression canary: NVENC-capable hardware must stay READY, not BLOCKED.
    capability::CapabilitySet caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    capability::SettingsResolver resolver(caps);

    capability::UserRecorderConfig primary;
    primary.container = capability::Container::Matroska;
    primary.video_codec = capability::VideoCodec::H264Nvenc;
    primary.audio_codec = capability::AudioCodec::AacMf;
    primary.chroma = capability::ChromaSubsampling::Cs420;
    primary.bit_depth = capability::BitDepth::Bit8;
    primary.frame_rate_num = 60;
    primary.frame_rate_den = 1;

    const capability::ResolveResult result = resolver.ValidateConfig(primary);
    EXPECT_TRUE(result.succeeded);
    EXPECT_TRUE(result.invalidity.empty());
}

TEST(BlockedScenarioTest, StartupConfig_NvencUnavailable_InvalidityDisplayMapsToVideoCodec) {
    // End-to-end check: the invalidity field reported when NVENC is missing must map to
    // the "Video codec" display name and the Output/Video settings action hint — not
    // "Audio codec", which was the previous misleading message (REC-R7 fix).
    capability::CapabilitySet caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    const std::string nvenc_reason = "NVENC unavailable: simulated";
    caps.video_codecs[capability::VideoCodec::H264Nvenc] = {capability::SupportLevel::NotImplemented, nvenc_reason};
    caps.video_codecs[capability::VideoCodec::Av1Nvenc] = {capability::SupportLevel::NotImplemented, nvenc_reason};
    const capability::ComboKey mkv_h264{capability::Container::Matroska, capability::VideoCodec::H264Nvenc,
                                        capability::AudioCodec::AacMf, capability::ChromaSubsampling::Cs420,
                                        capability::BitDepth::Bit8};
    caps.combo_overrides[mkv_h264] = {capability::SupportLevel::NotImplemented, nvenc_reason};

    capability::SettingsResolver resolver(caps);
    capability::UserRecorderConfig primary;
    primary.container = capability::Container::Matroska;
    primary.video_codec = capability::VideoCodec::H264Nvenc;
    primary.audio_codec = capability::AudioCodec::AacMf;
    primary.chroma = capability::ChromaSubsampling::Cs420;
    primary.bit_depth = capability::BitDepth::Bit8;
    primary.frame_rate_num = 60;
    primary.frame_rate_den = 1;

    const capability::ResolveResult result = resolver.ValidateConfig(primary);
    ASSERT_FALSE(result.succeeded);
    ASSERT_FALSE(result.invalidity.empty());

    const std::string& field = result.invalidity.front().field;
    EXPECT_EQ(field, "video_codec");
    // Diagnostics card title: "Video codec is not supported" (not "Audio codec is not supported").
    EXPECT_EQ(InvalidFieldDisplayName(field), "Video codec");
    // Action hint must point to Output/Video settings.
    const std::string hint = InvalidFieldActionHint(field);
    EXPECT_NE(hint.find("settings"), std::string::npos);
    // The message should carry the NVENC reason.
    EXPECT_EQ(result.invalidity.front().message, nvenc_reason);
}

TEST(BlockedScenarioTest, RecommendationEngine_H264NvencUnavailable_ProducesVideoCodecBlocker) {
    // When H264Nvenc is not selectable, DiagnosticsPage's RecommendationEngine should
    // produce a rec.003 (video codec unavailable) Blocker that appears in Top Issues.
    capability::CapabilitySet caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    caps.video_codecs[capability::VideoCodec::H264Nvenc] = {capability::SupportLevel::NotImplemented,
                                                            "NVENC unavailable"};

    capability::UserRecorderConfig config;
    config.container = capability::Container::Matroska;
    config.video_codec = capability::VideoCodec::H264Nvenc;
    config.audio_codec = capability::AudioCodec::AacMf;
    config.frame_rate_num = 60;
    config.frame_rate_den = 1;

    RecommendationEngine engine(caps, config, 0, 0, /*is_profile_supported=*/true);
    const DiagnosticChecklist checklist = engine.Generate();

    bool found_rec003 = false;
    for (const auto& r : checklist.results) {
        if (r.id == "rec.003") {
            found_rec003 = true;
            EXPECT_EQ(r.severity, DiagnosticSeverity::Blocker);
        }
    }
    EXPECT_TRUE(found_rec003);
    EXPECT_TRUE(checklist.has_blocker);
}

TEST(RecommendationEngineTest, ExclusiveFullscreenRaisesBorderlessFixAction) {
    capability::CapabilitySet caps;
    capability::UserRecorderConfig config;
    PresentSample present;
    present.available = true;
    present.mode = PresentMode::ExclusiveFullscreen;

    RecommendationEngine engine(caps, config, /*monitor_refresh_rate=*/0,
                                /*output_drive_free_bytes=*/0, /*is_profile_supported=*/true,
                                /*output_filesystem_name=*/"NTFS",
                                /*live_snapshot=*/nullptr, /*present=*/&present);
    const DiagnosticChecklist list = engine.Generate();

    const auto it = std::find_if(list.results.begin(), list.results.end(),
                                 [](const DiagnosticResult& r) { return r.id == "rec.present.exclusive"; });
    ASSERT_NE(it, list.results.end());
    ASSERT_TRUE(it->fix_action.has_value());
    EXPECT_EQ(it->fix_action->id, "fix.present.borderless");
    EXPECT_EQ(it->fix_action->safety, FixAction::Safety::Assisted);
}

TEST(RecommendationEngineTest, ComposedPresentRaisesNoExclusiveCheck) {
    capability::CapabilitySet caps;
    capability::UserRecorderConfig config;
    PresentSample present;
    present.available = true;
    present.mode = PresentMode::Composed;
    RecommendationEngine engine(caps, config, 0, 0, true, "NTFS", nullptr, &present);
    const DiagnosticChecklist list = engine.Generate();
    EXPECT_TRUE(std::none_of(list.results.begin(), list.results.end(),
                             [](const DiagnosticResult& r) { return r.id == "rec.present.exclusive"; }));
}

TEST(RecommendationEngineTest, JudderDetailNamesPresentModeAttribution) {
    using namespace exosnap::diagnostics;
    recorder_core::RecordingDiagnosticsSnapshot snap;
    snap.valid = true;
    snap.video_encoder.cfr = true;
    snap.capture.present_cadence_availability = recorder_core::MetricAvailability::Available;
    snap.capture.source_present_jitter_ms = 6.0; // > kJitterMs
    snap.capture.source_coalesce_ratio = 2.0;    // > kCoalesceRatio
    capability::CapabilitySet caps;
    capability::UserRecorderConfig config;
    config.frame_rate_num = 60;
    config.frame_rate_den = 1;
    PresentSample present;
    present.available = true;
    present.mode = PresentMode::IndependentFlip;

    RecommendationEngine engine(caps, config, 0, 0, true, "NTFS", &snap, &present);
    const DiagnosticChecklist list = engine.Generate();
    const auto it = std::find_if(list.results.begin(), list.results.end(),
                                 [](const DiagnosticResult& r) { return r.id == "rec.001"; });
    ASSERT_NE(it, list.results.end());
    EXPECT_NE(it->detail.find("independent flip"), std::string::npos);
}

TEST(RecommendationEngineTest, HighDpcLatencyNamesDriverExternalFix) {
    using namespace exosnap::diagnostics;
    capability::CapabilitySet caps;
    capability::UserRecorderConfig config;
    RecommendationEngine engine(caps, config, 0, 0, true, "NTFS", nullptr, nullptr);
    DpcLatencyReading dpc{/*max*/ 2500.0, /*avg*/ 180.0, "nvlddmkm.sys", /*available*/ true};
    engine.SetDpcLatency(dpc);
    const DiagnosticChecklist list = engine.Generate();
    const auto it = std::find_if(list.results.begin(), list.results.end(),
                                 [](const DiagnosticResult& r) { return r.id == "rec.dpc.latency"; });
    ASSERT_NE(it, list.results.end());
    EXPECT_NE(it->detail.find("nvlddmkm.sys"), std::string::npos);
    ASSERT_TRUE(it->fix_action.has_value());
    EXPECT_EQ(it->fix_action->safety, FixAction::Safety::External);
}

TEST(RecommendationEngineTest, LowDpcLatencyRaisesNothing) {
    using namespace exosnap::diagnostics;
    capability::CapabilitySet caps;
    capability::UserRecorderConfig config;
    RecommendationEngine engine(caps, config, 0, 0, true, "NTFS", nullptr, nullptr);
    engine.SetDpcLatency({200.0, 60.0, "", true});
    const DiagnosticChecklist list = engine.Generate();
    EXPECT_TRUE(std::none_of(list.results.begin(), list.results.end(),
                             [](const DiagnosticResult& r) { return r.id == "rec.dpc.latency"; }));
}

// --- ADR 0035: Smooth pacing Auto FixAction when judder fires + Newest pacing ---

TEST(RecommendationEngineTest, JudderInNewestOffersSmoothPacingAutoFix) {
    // Live judder + config.frame_pacing = Newest → rec.pacing.smooth result
    // with fix_action id=="fix.frame_pacing.smooth", safety==Auto.
    capability::CapabilitySet caps;
    caps.video_codecs[capability::VideoCodec::Av1Nvenc] = {capability::SupportLevel::Available, ""};
    caps.audio_codecs[capability::AudioCodec::Opus] = {capability::SupportLevel::Available, ""};

    capability::UserRecorderConfig config;
    config.frame_rate_num = 60;
    config.frame_rate_den = 1;
    config.frame_pacing = recorder_core::FramePacingMode::Newest; // triggers the pacing result

    const auto live = MakeJudderSnapshot(/*cfr=*/true, /*jitter_ms=*/9.0, /*coalesce_ratio=*/1.0);
    RecommendationEngine engine(caps, config, 0, 0, true, "", &live);
    const auto checklist = engine.Generate();

    const auto it = std::find_if(checklist.results.begin(), checklist.results.end(),
                                 [](const DiagnosticResult& r) { return r.id == "rec.pacing.smooth"; });
    ASSERT_NE(it, checklist.results.end()) << "rec.pacing.smooth must be emitted for judder+Newest";
    ASSERT_TRUE(it->fix_action.has_value());
    EXPECT_EQ(it->fix_action->id, "fix.frame_pacing.smooth");
    EXPECT_EQ(it->fix_action->safety, FixAction::Safety::Auto);
    EXPECT_TRUE(it->fix_action->reversible);
    EXPECT_FALSE(it->fix_action->changes_summary.empty());
}

TEST(RecommendationEngineTest, JudderInSmoothOffersNoPacingFix) {
    // Live judder + config.frame_pacing = Smooth → NO rec.pacing.smooth result
    // (pacing is already correct — nothing to fix).
    capability::CapabilitySet caps;
    caps.video_codecs[capability::VideoCodec::Av1Nvenc] = {capability::SupportLevel::Available, ""};
    caps.audio_codecs[capability::AudioCodec::Opus] = {capability::SupportLevel::Available, ""};

    capability::UserRecorderConfig config;
    config.frame_rate_num = 60;
    config.frame_rate_den = 1;
    config.frame_pacing = recorder_core::FramePacingMode::Smooth; // already correct — no fix offered

    const auto live = MakeJudderSnapshot(/*cfr=*/true, /*jitter_ms=*/9.0, /*coalesce_ratio=*/1.0);
    RecommendationEngine engine(caps, config, 0, 0, true, "", &live);
    const auto checklist = engine.Generate();

    EXPECT_TRUE(std::none_of(checklist.results.begin(), checklist.results.end(), [](const DiagnosticResult& r) {
        return r.id == "rec.pacing.smooth";
    })) << "rec.pacing.smooth must NOT be emitted when pacing is already Smooth";
}

} // namespace
} // namespace exosnap::diagnostics
