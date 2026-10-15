#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/backend_tuning.h>
#include <exosnap/engine/codec_types.h>
#include <exosnap/engine/edit_player_engine.h>
#include <exosnap/engine/edit_timeline.h>
#include <exosnap/engine/edit_timeline_compositor.h>
#include <exosnap/engine/edit_timeline_export.h>
#include <exosnap/engine/edit_timeline_render.h>
#include <exosnap/engine/mp4_remuxer.h>
#include <exosnap/engine/recorder_session.h>
#include <exosnap/engine/recording_media_types.h>
#include <exosnap/engine/sdr_transfer.h>

#include "test_unique_temp.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <ratio>
#include <system_error>
#include <vector>

using namespace exosnap::engine;

namespace {
TimelineRenderSnapshot Fixture(const std::filesystem::path& directory) {
    TimelineRenderSnapshot result;
    for (uint64_t id : {1ULL, 2ULL}) {
        const auto path = directory / (id == 1 ? L"red.mkv" : L"blue.mkv");
        result.sources.push_back({id, path, ProbeEditMedia(path)});
    }
    result.timeline = {{{1, 1, 1, false, 0, 0, 2'000'000},
                        {2, 2, 2, false, 1'500'000, 0, 2'000'000},
                        {3, 1, 1, true, 0, 0, 2'000'000},
                        {4, 2, 2, true, 1'500'000, 0, 2'000'000}},
                       {{1, 2, 500'000}},
                       3'500'000};
    return result;
}
} // namespace

class EditCopyBaseline : public testing::Test {
  protected:
    std::filesystem::path output = exosnap_test::UniqueTempPath("hdr-copy.mp4");
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove(output, error);
    }
};

TEST_F(EditCopyBaseline, BaselinePreviewFailureCopy) {
    const auto source = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
    const auto metadata = ProbeEditMedia(source);
    ASSERT_TRUE(metadata.error.empty()) << metadata.error;
    const TimelineExportClip clip{source, 0, metadata.duration_us, 0};
    ASSERT_EQ(AssessTimelineExport({clip}, true).eligibility, TimelineExportEligibility::StreamCopy);
    EditPlayerEngine player;
    std::string error;
    std::atomic<bool> cancelled{true};
    ASSERT_FALSE(player.Open(source, error, &cancelled));
    ASSERT_FALSE(error.empty());
    EXPECT_FALSE(player.HasVideoStream());
    const auto assessment = AssessTimelineExport({clip}, true);
    ASSERT_EQ(assessment.eligibility, TimelineExportEligibility::StreamCopy) << assessment.reason;
    TimelineRenderSnapshot snapshot;
    snapshot.sources = {{1, source, metadata}};
    snapshot.timeline = {{{1, 1, 1, false, 0, 0, metadata.duration_us}}, {}, metadata.duration_us};
    EXPECT_EQ(ClassifyEditExport(snapshot, assessment, true).path, EditExportPath::StreamCopy);
    const auto copied = ExportTimelineStreamCopy({clip}, output, true);
    ASSERT_TRUE(copied.success) << copied.message;
    ASSERT_TRUE(player.Open(output, error)) << error;
    EXPECT_TRUE(player.DecodeFrameAtRaw(0));
    EXPECT_TRUE(player.DecodeFrameAtRaw(metadata.duration_us / 2));
}

TEST_F(EditCopyBaseline, BaselineHdrCopy) {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
    const auto source = root / ".workspace" / "test-fixtures" / "hdr10_test_1080p60.mp4";
    if (!std::filesystem::exists(source))
        GTEST_SKIP() << "No native HDR10 fixture present: " << source.string();
    const auto metadata = ProbeEditMedia(source);
    ASSERT_TRUE(metadata.error.empty()) << metadata.error;
    ASSERT_FALSE(metadata.render_color_supported);
    ASSERT_FALSE(metadata.render_color_reason.empty());
    EditPlayerEngine original;
    std::string error;
    ASSERT_TRUE(original.Open(source, error)) << error;
    const auto original_frame = original.DecodeFrameAtRaw(0);
    ASSERT_TRUE(original_frame);
    ASSERT_TRUE(original_frame->is_pq_source);
    ASSERT_EQ(original_frame->format, DecodedPixelFormat::Yuv420P10);
    TimelineRenderSnapshot snapshot;
    snapshot.sources = {{1, source, metadata}};
    snapshot.timeline = {{{1, 1, 1, false, 0, 0, metadata.duration_us}}, {}, metadata.duration_us};
    const TimelineExportClip clip{source, 0, metadata.duration_us, 0};
    const auto assessment = AssessTimelineExport({clip}, true);
    ASSERT_EQ(assessment.eligibility, TimelineExportEligibility::StreamCopy) << assessment.reason;
    EXPECT_EQ(ClassifyEditExport(snapshot, assessment, true).path, EditExportPath::StreamCopy);
    const auto rendered = ClassifyEditExport(snapshot, {TimelineExportEligibility::RenderRequired, {}}, false);
    EXPECT_EQ(rendered.path, EditExportPath::Unsupported);
    EXPECT_EQ(rendered.reason, metadata.render_color_reason);
    const auto result = ExportTimelineStreamCopy({clip}, output, true);
    ASSERT_TRUE(result.success) << result.message;
    EditPlayerEngine copied;
    ASSERT_TRUE(copied.Open(output, error)) << error;
    const auto copied_frame = copied.DecodeFrameAtRaw(0);
    ASSERT_TRUE(copied_frame);
    EXPECT_TRUE(copied_frame->is_pq_source);
    EXPECT_EQ(copied_frame->format, original_frame->format);
    EXPECT_EQ(copied_frame->primaries, original_frame->primaries);
    EXPECT_EQ(copied_frame->transfer, original_frame->transfer);
    EXPECT_EQ(copied_frame->matrix, original_frame->matrix);
    EXPECT_EQ(copied_frame->range, original_frame->range);
    EXPECT_EQ(copied_frame->width, original_frame->width);
    EXPECT_EQ(copied_frame->height, original_frame->height);
}

TEST(EditRenderPlan, StreamCopyRemainsIndependentOfRenderColorAndContainerLimits) {
    TimelineRenderSnapshot snapshot;
    const TimelineExportAssessment copy{TimelineExportEligibility::StreamCopy, {}};
    EXPECT_EQ(ClassifyEditExport(snapshot, copy, true).path, EditExportPath::StreamCopy);
    EXPECT_EQ(ClassifyEditExport(snapshot, {TimelineExportEligibility::RenderRequired, {}}, false).path,
              EditExportPath::Unsupported);
    snapshot.timeline.crossfades.push_back({1, 2, 500000});
    EXPECT_EQ(ClassifyEditExport(snapshot, copy, true).path, EditExportPath::Unsupported);
}

TEST(EditRenderPlan, CrossfadeRequiresHonestColorAndKeepsExactFrameRate) {
    TimelineRenderSnapshot snapshot;
    EditMediaMetadata metadata;
    metadata.duration_us = 2'000'000;
    metadata.width = 320;
    metadata.height = 180;
    metadata.fps_num = 30000;
    metadata.fps_den = 1001;
    metadata.render_color_supported = true;
    snapshot.sources = {{1, L"a.mkv", metadata}, {2, L"b.mkv", metadata}};
    snapshot.timeline = {
        {{1, 1, 1, false, 0, 0, 2'000'000}, {2, 2, 2, false, 1'500'000, 0, 2'000'000}}, {{1, 2, 500'000}}, 3'500'000};
    const TimelineExportAssessment copy{TimelineExportEligibility::RenderRequired, {}};
    const auto plan = ClassifyEditExport(snapshot, copy, false);
    EXPECT_EQ(plan.path, EditExportPath::Render);
    EXPECT_EQ(plan.fps_num, 30000);
    EXPECT_EQ(plan.fps_den, 1001);
    snapshot.sources[1].metadata.render_color_supported = false;
    snapshot.sources[1].metadata.render_color_reason = "HDR render is unsupported.";
    const auto unsupported = ClassifyEditExport(snapshot, copy, false);
    EXPECT_EQ(unsupported.path, EditExportPath::Unsupported);
    EXPECT_EQ(unsupported.reason, "HDR render is unsupported.");
}

TEST(EditTimelineMedia, SharedReaderSeeksAudioAndRealExportReopens) {
    wchar_t directory[32768]{};
    if (!GetEnvironmentVariableW(L"EXOSNAP_EDIT_RENDER_FIXTURES", directory, 32768))
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated red/blue media.";
    auto snapshot = Fixture(std::filesystem::path(directory));
    for (auto& source : snapshot.sources) {
        ASSERT_TRUE(source.metadata.error.empty()) << source.metadata.error;
        const auto& audio = source.metadata.audio_metadata;
        ASSERT_EQ(audio.actual_tracks.size(), 1u);
        ASSERT_TRUE(audio.unverified_asset_binding);
        const auto& track = audio.actual_tracks.front();
        ASSERT_EQ(track.stream_index.value, 0u);
        ASSERT_NE(track.track_id, AudioTrackId{});
        source.monitor_selection = MonitorSelectionSnapshot{
            audio.selection_media_id, {1}, {{track.track_id, 1.0f, false}}, true, audio.unverified_asset_binding};
    }
    const auto plan = ClassifyEditExport(snapshot, {TimelineExportEligibility::RenderRequired, {}}, false);
    ASSERT_EQ(plan.path, EditExportPath::Render) << plan.reason;
    EditTimelineReader reader;
    std::string error;
    ASSERT_TRUE(reader.Open(snapshot, plan, error)) << error;
    const auto begin = std::chrono::steady_clock::now();
    size_t delivered = 0;
    double maximum_seek_ms = 0;
    for (int64_t time :
         {0LL, 1'499'999LL, 1'500'000LL, 1'750'000LL, 1'999'999LL, 2'000'000LL, 3'000'000LL, 1'750'000LL, 0LL}) {
        const auto seek = std::chrono::steady_clock::now();
        TimelineVideoFrame frame;
        ASSERT_TRUE(reader.VideoAt(time, frame, error)) << error;
        const auto expected = EvaluateTimeline(snapshot.timeline, time);
        ASSERT_EQ(frame.layers.size(), expected.video.size());
        for (size_t i = 0; i < frame.layers.size(); ++i) {
            EXPECT_EQ(frame.layers[i].weight, expected.video[i].weight);
            EXPECT_GE(frame.layers[i].frame.pts_us, expected.video[i].source_us - 33334);
            EXPECT_LE(frame.layers[i].frame.pts_us, expected.video[i].source_us + 33334);
        }
        maximum_seek_ms =
            std::max(maximum_seek_ms,
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - seek).count());
        ++delivered;
    }
    const double seek_elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    std::vector<float> samples;
    ASSERT_TRUE(reader.AudioAt(0, 48000, samples, error)) << error;
    for (size_t i = 0; i < samples.size(); ++i)
        ASSERT_NEAR(samples[i], 0.25, 0.0001) << "PCM continuity at sample " << i;
    ASSERT_TRUE(reader.AudioAt(84000, 480, samples, error)) << error;
    EXPECT_NEAR(samples[0], 0, 0.0001);
    EXPECT_NEAR(samples[958], -0.00998, 0.0001);
    ASSERT_TRUE(reader.AudioAt(84480, 480, samples, error)) << error;
    EXPECT_NEAR(samples[0], -0.01, 0.0001);
    EXPECT_LE(reader.PeakDecoders(), 2u);
    auto split = snapshot;
    split.timeline = {{{1, 1, 1, false, 0, 0, 1'000'000},
                       {2, 1, 2, false, 500'000, 1'000'000, 2'000'000},
                       {3, 1, 1, true, 0, 0, 1'000'000},
                       {4, 1, 2, true, 500'000, 1'000'000, 2'000'000}},
                      {{1, 2, 500'000}},
                      1'500'000};
    EditTimelineReader split_reader;
    ASSERT_TRUE(split_reader.Open(
        split, ClassifyEditExport(split, {TimelineExportEligibility::RenderRequired, {}}, false), error));
    TimelineVideoFrame split_frame;
    ASSERT_TRUE(split_reader.VideoAt(750000, split_frame, error)) << error;
    ASSERT_EQ(split_frame.layers.size(), 2u);
    EXPECT_NEAR(static_cast<double>(split_frame.layers[0].frame.pts_us), 750000, 33334);
    EXPECT_NEAR(static_cast<double>(split_frame.layers[1].frame.pts_us), 1250000, 33334);
    EXPECT_EQ(split_reader.PeakDecoders(), 2u);
    ASSERT_TRUE(split_reader.AudioAt(36000, 480, samples, error)) << error;
    EXPECT_NEAR(samples.front(), 0.25, 0.0001);
    RecorderConfig config;
    config.video_codec = VideoCodec::H264;
    NvencTuning tuning;
    tuning.bframes = 2;
    config.backend_tuning = tuning;
    const auto output = std::filesystem::path(directory) / L"crossfade.mkv";
    std::error_code ignored;
    std::filesystem::remove(output, ignored);
    TimelineRenderMeasurements stats;
    const auto result = RenderEditTimeline(snapshot, plan, config, output, false, RemuxNoopCallback(), &stats);
    ASSERT_TRUE(result.success) << result.message;
    EXPECT_EQ(stats.video_frames, 105u);
    EXPECT_EQ(stats.audio_frames, 168000u);
    EXPECT_LE(stats.peak_decoders, 2u);
    auto unavailable_device = config;
    unavailable_device.resolved_encoder_device.valid = true;
    unavailable_device.resolved_encoder_device.adapter_luid = -1;
    const auto unavailable_output = std::filesystem::path(directory) / L"unavailable-adapter.mkv";
    std::filesystem::remove(unavailable_output, ignored);
    const auto unavailable =
        RenderEditTimeline(snapshot, plan, unavailable_device, unavailable_output, false, RemuxNoopCallback());
    EXPECT_FALSE(unavailable.success);
    EXPECT_EQ(unavailable.message, "No supported encoder adapter is available for timeline rendering.");
    EXPECT_FALSE(std::filesystem::exists(unavailable_output));
    unavailable_device.resolved_encoder_device.valid = false;
    unavailable_device.resolved_encoder_device.reason = "The selected adapter capability probe did not complete.";
    const auto unprobed =
        RenderEditTimeline(snapshot, plan, unavailable_device, unavailable_output, false, RemuxNoopCallback());
    EXPECT_FALSE(unprobed.success);
    EXPECT_EQ(unprobed.message, unavailable_device.resolved_encoder_device.reason);
    EXPECT_FALSE(std::filesystem::exists(unavailable_output));
    EditPlayerEngine reopened;
    ASSERT_TRUE(reopened.Open(output, error)) << error;
    for (int64_t time : {500000LL, 1750000LL, 3000000LL}) {
        const auto frame = reopened.DecodeFrameAt(time);
        ASSERT_TRUE(frame.has_value());
        const auto* pixel = frame->bgra.get() + frame->stride_bytes * 90 + 160 * 4;
        if (time == 500000) {
            EXPECT_GT(pixel[2], 245);
            EXPECT_LT(pixel[0], 10);
        }
        if (time == 1750000) {
            const auto evaluated = EvaluateTimeline(snapshot.timeline, frame->pts_us);
            ASSERT_EQ(evaluated.video.size(), 2u);
            EXPECT_NEAR(pixel[2], LinearToBt709(static_cast<float>(evaluated.video[0].weight)) * 255, 5);
            EXPECT_NEAR(pixel[0], LinearToBt709(static_cast<float>(evaluated.video[1].weight)) * 255, 5);
        }
        if (time == 3000000) {
            EXPECT_GT(pixel[0], 245);
            EXPECT_LT(pixel[2], 10);
        }
    }
    const auto cancelled = std::filesystem::path(directory) / L"cancelled.mkv";
    std::filesystem::remove(cancelled, ignored);
    auto cancelled_at = std::chrono::steady_clock::now();
    const auto cancellation = RenderEditTimeline(snapshot, plan, config, cancelled, false, [&](float fraction) {
        if (fraction < 0.1f)
            return true;
        cancelled_at = std::chrono::steady_clock::now();
        return false;
    });
    const double cancel_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cancelled_at).count();
    EXPECT_FALSE(cancellation.success);
    EXPECT_FALSE(std::filesystem::exists(cancelled));
    std::cout << "timeline measurements: delivered=" << delivered << " seek_fps=" << delivered / seek_elapsed
              << " max_seek_ms=" << maximum_seek_ms << " render_fps=" << stats.video_frames * 1000 / stats.elapsed_ms
              << " compositor_textures=" << stats.compositor_texture_creations
              << " peak_decoders=" << stats.peak_decoders << " peak_encoder_backlog=" << stats.peak_encoder_backlog
              << " cancel_ms=" << cancel_ms << '\n';
}
