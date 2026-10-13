#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/codec_types.h>
#include <exosnap/engine/edit_player_engine.h>
#include <exosnap/engine/edit_timeline_compositor.h>
#include <exosnap/engine/edit_timeline_export.h>
#include <exosnap/engine/edit_timeline_render.h>
#include <exosnap/engine/mp4_remuxer.h>
#include <exosnap/engine/recorder_session.h>
#include <exosnap/engine/recording_media_types.h>

#include "media_identity_sidecar.h"
#include "test_unique_temp.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <windows.h>

using namespace exosnap::engine;

namespace {

TimelineRenderSnapshot LinkedHardCut() {
    EditMediaMetadata metadata;
    metadata.duration_us = 4'000'000;
    metadata.width = 64;
    metadata.height = 64;
    metadata.fps_num = 30;
    metadata.fps_den = 1;
    metadata.render_color_supported = true;
    TimelineRenderSnapshot snapshot;
    snapshot.sources = {{1, L"first.mkv", metadata}, {2, L"second.mkv", metadata}};
    snapshot.timeline = {{{1, 1, 1, false, 0, 0, 1'000'000},
                          {2, 2, 2, false, 1'000'000, 2'000'000, 3'000'000},
                          {3, 1, 1, true, 0, 0, 1'000'000},
                          {4, 2, 2, true, 1'000'000, 2'000'000, 3'000'000}},
                         {},
                         2'000'000};
    return snapshot;
}

TEST(EditHardCutRender, IneligibleCopyUsesRenderWithoutInventingCrossfade) {
    const auto snapshot = LinkedHardCut();
    const auto plan =
        ClassifyEditExport(snapshot, {TimelineExportEligibility::RenderRequired, "Trim requires render."}, false);
    EXPECT_EQ(plan.path, EditExportPath::Render) << plan.reason;
    EXPECT_TRUE(snapshot.timeline.crossfades.empty());
    EXPECT_EQ(plan.output.width, 64u);
    EXPECT_EQ(plan.fps_num, 30);
}

TEST(EditHardCutRender, EligibleCopyKeepsPriority) {
    const auto plan = ClassifyEditExport(LinkedHardCut(), {TimelineExportEligibility::StreamCopy, {}}, false);
    EXPECT_EQ(plan.path, EditExportPath::StreamCopy);
}

TEST(EditHardCutRender, RenderRetainsSourceLinkColorGeometryAndContainerGuards) {
    const auto needs_render = TimelineExportAssessment{TimelineExportEligibility::RenderRequired, {}};
    auto snapshot = LinkedHardCut();
    snapshot.timeline.clips[3].source_in_us += 1;
    EXPECT_EQ(ClassifyEditExport(snapshot, needs_render, false).path, EditExportPath::Unsupported);
    snapshot = LinkedHardCut();
    snapshot.sources[1].metadata.render_color_supported = false;
    snapshot.sources[1].metadata.render_color_reason = "HDR render is unsupported.";
    EXPECT_EQ(ClassifyEditExport(snapshot, needs_render, false).reason, "HDR render is unsupported.");
    snapshot = LinkedHardCut();
    snapshot.sources[1].metadata.width = 63;
    EXPECT_EQ(ClassifyEditExport(snapshot, needs_render, false).path, EditExportPath::Unsupported);
    snapshot = LinkedHardCut();
    snapshot.timeline.clips[1].source_out_us = 4'000'001;
    EXPECT_EQ(ClassifyEditExport(snapshot, needs_render, false).path, EditExportPath::Unsupported);
    snapshot = LinkedHardCut();
    snapshot.sources[0].metadata.fps_num = 0;
    EXPECT_EQ(ClassifyEditExport(snapshot, needs_render, false).path, EditExportPath::Unsupported);
    EXPECT_EQ(ClassifyEditExport(LinkedHardCut(), needs_render, true).path, EditExportPath::Unsupported);
}

TEST(EditHardCutRender, ReaderDecodesBothSidesOfCutFromRealCpuFixture) {
    const auto source = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
    auto snapshot = LinkedHardCut();
    snapshot.sources[0].path = source;
    snapshot.sources[1].path = source;
    EditRenderPlan plan;
    plan.path = EditExportPath::Render;
    plan.output = {64, 64};
    plan.fps_num = 30;
    EditTimelineReader reader;
    std::string error;
    ASSERT_TRUE(reader.Open(snapshot, plan, error)) << error;
    const std::pair<int64_t, int64_t> probes[]{{500'000, 500'000}, {1'500'000, 2'500'000}};
    for (const auto [timeline_us, source_us] : probes) {
        TimelineVideoFrame frame;
        ASSERT_TRUE(reader.VideoAt(timeline_us, frame, error)) << error;
        ASSERT_EQ(frame.layers.size(), 1u);
        EXPECT_GE(frame.layers[0].frame.pts_us, source_us - 34'000);
        EXPECT_LE(frame.layers[0].frame.pts_us, source_us + 34'000);
        EXPECT_DOUBLE_EQ(frame.layers[0].weight, 1);
    }
}

TEST(EditHardCutRender, RealSdrRecordingRendersLinkedHardCutWithSelectedPcmAudio) {
    wchar_t source_name[32768]{};
    if (!GetEnvironmentVariableW(L"EXOSNAP_EDIT_HARD_CUT_SOURCE", source_name, 32768))
        GTEST_SKIP() << "Set EXOSNAP_EDIT_HARD_CUT_SOURCE to an SDR recording with one audible audio track.";
    struct NativeDecodeMode {
        std::optional<std::string> previous;
        NativeDecodeMode() {
            if (const char* value = std::getenv("EXOSNAP_EDIT_SOFTWARE_DECODE"))
                previous = value;
            _putenv_s("EXOSNAP_EDIT_SOFTWARE_DECODE", "");
        }
        ~NativeDecodeMode() {
            if (previous)
                _putenv_s("EXOSNAP_EDIT_SOFTWARE_DECODE", previous->c_str());
            else
                _putenv_s("EXOSNAP_EDIT_SOFTWARE_DECODE", "");
        }
    } native_decode_mode;
    const std::filesystem::path source(source_name);
    ASSERT_TRUE(std::filesystem::exists(source));
    const auto original_size = std::filesystem::file_size(source);
    const auto metadata = ProbeEditMedia(source);
    ASSERT_TRUE(metadata.error.empty()) << metadata.error;
    ASSERT_TRUE(metadata.render_color_supported) << metadata.render_color_reason;
    ASSERT_GE(metadata.duration_us, 3'000'000);
    EditPlayerEngine original;
    std::string error;
    ASSERT_TRUE(original.Open(source, error)) << error;
    const auto source_audio = original.AudioMetadata();
    ASSERT_EQ(source_audio.actual_tracks.size(), 1u);
    const auto source_track = source_audio.actual_tracks.front().track_id;
    MonitorSelectionSnapshot selection{source_audio.selection_media_id,
                                       {1},
                                       {{source_track, 1.0f, false}},
                                       true,
                                       source_audio.unverified_asset_binding};
    ASSERT_TRUE(original.SetMonitorSelection(selection, error)) << error;

    TimelineRenderSnapshot snapshot;
    snapshot.sources = {{1, source, metadata, selection}, {2, source, metadata, selection}};
    snapshot.timeline = {{{1, 1, 1, false, 0, 0, 500'000},
                          {2, 2, 2, false, 500'000, 2'000'000, 2'500'000},
                          {3, 1, 1, true, 0, 0, 500'000},
                          {4, 2, 2, true, 500'000, 2'000'000, 2'500'000}},
                         {},
                         1'000'000};
    const auto assessment =
        AssessTimelineExport({{source, 0, 500'000, 0}, {source, 2'000'000, 2'500'000, 500'000}}, false);
    ASSERT_EQ(assessment.eligibility, TimelineExportEligibility::RenderRequired);
    const auto plan = ClassifyEditExport(snapshot, assessment, false);
    ASSERT_EQ(plan.path, EditExportPath::Render) << plan.reason;
    const auto output = exosnap_test::UniqueTempPath("hard-cut-render.mkv");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            std::filesystem::remove(MediaIdentitySidecarPath(path), ignored);
        }
    } cleanup{output};
    RecorderConfig config;
    config.video_codec = VideoCodec::H264;
    TimelineRenderMeasurements stats;
    const auto result = RenderEditTimeline(snapshot, plan, config, output, false, RemuxNoopCallback(), &stats);
    ASSERT_TRUE(result.success) << result.message;
    EXPECT_EQ(stats.video_frames, static_cast<uint64_t>(plan.fps_num / plan.fps_den));
    EXPECT_EQ(stats.audio_frames, 48000u);
    EXPECT_EQ(std::filesystem::file_size(source), original_size);

    EditPlayerEngine rendered;
    ASSERT_TRUE(rendered.Open(output, error)) << error;
    EXPECT_TRUE(rendered.DecodeFrameAt(250'000).has_value());
    EXPECT_TRUE(rendered.DecodeFrameAt(750'000).has_value());
    const auto output_audio = rendered.AudioMetadata();
    ASSERT_EQ(output_audio.status, AudioMetadataStatus::Verified);
    ASSERT_TRUE(output_audio.manifest);
    ASSERT_EQ(output_audio.actual_tracks.size(), 1u);
    ASSERT_EQ(output_audio.manifest->tracks.size(), 1u);
    const auto& mix = output_audio.manifest->tracks.front();
    EXPECT_EQ(mix.container.codec.codec_id, "A_PCM/INT/LIT");
    EXPECT_EQ(mix.container.codec.bit_depth, 24u);
    EXPECT_EQ(mix.container.codec.sample_rate, 48000u);
    EXPECT_EQ(mix.container.codec.channels, 2u);
    EXPECT_EQ(mix.derived_from_track_ids, std::vector<AudioTrackId>{source_track});
    EXPECT_NE(mix.container.track_id, source_track);
    EXPECT_EQ(output_audio.manifest->derived_from_media_ids, std::vector<MediaId>{source_audio.selection_media_id});
    ASSERT_TRUE(rendered.SetMonitorSelection(
        {output_audio.selection_media_id, {1}, {{mix.container.track_id, 1.0f, false}}, true}, error))
        << error;
    for (const auto [render_sample, source_sample] : {std::pair<int64_t, int64_t>{4'800, 4'800}, {28'800, 100'800}}) {
        std::vector<float> source_samples;
        std::vector<float> rendered_samples;
        ASSERT_TRUE(original.DecodeAudioRange(source_sample, 480, source_samples, error)) << error;
        ASSERT_TRUE(rendered.DecodeAudioRange(render_sample, 480, rendered_samples, error)) << error;
        ASSERT_EQ(rendered_samples.size(), source_samples.size());
        double energy = 0;
        double difference = 0;
        for (size_t i = 0; i < source_samples.size(); ++i) {
            energy += std::abs(source_samples[i]);
            difference += std::abs(source_samples[i] - rendered_samples[i]);
        }
        EXPECT_GT(energy / source_samples.size(), 0.0001);
        EXPECT_LT(difference / source_samples.size(), 0.02);
    }
}

} // namespace
