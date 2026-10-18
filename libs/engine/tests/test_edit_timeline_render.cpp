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

#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
    // Two video cursors at most, plus one audio cursor per clip audible inside
    // the crossfade.
    EXPECT_LE(reader.PeakDecoders(), 4u);
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
    EXPECT_LE(stats.peak_decoders, 4u);
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

TEST(EditTimelineMedia, LayeredExportMatchesPreviewCompositionAndRendersBlankTimeBlack) {
    wchar_t directory[32768]{};
    if (!GetEnvironmentVariableW(L"EXOSNAP_EDIT_RENDER_FIXTURES", directory, 32768))
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated red/blue media.";
    auto snapshot = Fixture(std::filesystem::path(directory));
    // Red on the lower track for 2 s, half-transparent blue in front of it from
    // 0.5 s to 1.5 s, then one second of blank time before the timeline end.
    snapshot.timeline = {};
    snapshot.timeline.tracks = {{10, false}, {20, false}};
    TimelineClip red{1, 1, 0, false, 0, 0, 2'000'000};
    red.track = 20;
    TimelineClip blue{2, 2, 0, false, 500'000, 0, 1'000'000};
    blue.track = 10;
    blue.opacity = 0.5;
    snapshot.timeline.clips = {red, blue};
    snapshot.timeline.duration_us = 3'000'000;
    const auto plan = ClassifyEditExport(snapshot, {TimelineExportEligibility::RenderRequired, {}}, false);
    ASSERT_EQ(plan.path, EditExportPath::Render) << plan.reason;
    RecorderConfig config;
    config.video_codec = VideoCodec::H264;
    const auto output = std::filesystem::path(directory) / L"layered.mkv";
    std::error_code ignored;
    std::filesystem::remove(output, ignored);
    TimelineRenderMeasurements stats;
    const auto result = RenderEditTimeline(snapshot, plan, config, output, false, RemuxNoopCallback(), &stats);
    ASSERT_TRUE(result.success) << result.message;
    EXPECT_EQ(stats.video_frames, 90u);

    // The preview path: the same reader and compositor on a separate device.
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                            nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put())));
    EditTimelineCompositor compositor;
    std::string error;
    ASSERT_TRUE(compositor.Init(device.get(), context.get(), plan.output, error)) << error;
    EditTimelineReader reader;
    ASSERT_TRUE(reader.Open(snapshot, plan, error)) << error;
    const auto preview_pixel = [&](int64_t time_us) {
        TimelineVideoFrame frame;
        EXPECT_TRUE(reader.VideoAt(time_us, frame, error, VideoReadMode::RandomAccess)) << error;
        EXPECT_TRUE(compositor.Compose(frame.layers, error)) << error;
        D3D11_TEXTURE2D_DESC desc{};
        compositor.Result()->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::com_ptr<ID3D11Texture2D> staging;
        EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, staging.put())));
        context->CopyResource(staging.get(), compositor.Result());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        EXPECT_TRUE(SUCCEEDED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)));
        std::array<uint8_t, 3> pixel{};
        std::memcpy(pixel.data(), static_cast<const uint8_t*>(mapped.pData) + mapped.RowPitch * 90 + 160 * 4, 3);
        context->Unmap(staging.get(), 0);
        return pixel;
    };
    EditPlayerEngine exported;
    ASSERT_TRUE(exported.Open(output, error)) << error;
    const auto linear = [](uint8_t code) { return Bt709ToLinear(code / 255.0f); };
    std::array<float, 3> red_linear{};
    for (const int64_t time : {250'000LL, 1'000'000LL, 2'500'000LL}) {
        const auto decoded = exported.DecodeFrameAt(time);
        ASSERT_TRUE(decoded.has_value());
        const auto* pixel = decoded->bgra.get() + decoded->stride_bytes * 90 + 160 * 4;
        const auto preview = preview_pixel(decoded->pts_us);
        for (size_t channel = 0; channel < 3; ++channel)
            EXPECT_NEAR(pixel[channel], preview[channel], 6) << "time " << time << " channel " << channel;
        if (time == 250'000) {
            EXPECT_GT(pixel[2], 245);
            EXPECT_LT(pixel[0], 10);
            for (size_t channel = 0; channel < 3; ++channel)
                red_linear[channel] = linear(preview[channel]);
        }
        if (time == 1'000'000) {
            // Source-over in linear light: half blue in front of red, not added.
            EditPlayerEngine source;
            ASSERT_TRUE(source.Open(snapshot.sources[1].path, error)) << error;
            const auto blue_frame = source.DecodeFrameAt(0);
            ASSERT_TRUE(blue_frame.has_value());
            const auto* blue_pixel = blue_frame->bgra.get() + blue_frame->stride_bytes * 90 + 160 * 4;
            for (size_t channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(preview[channel],
                            LinearToBt709(0.5f * linear(blue_pixel[channel]) + 0.5f * red_linear[channel]) * 255, 3)
                    << "channel " << channel;
        }
        if (time == 2'500'000)
            for (size_t channel = 0; channel < 3; ++channel)
                EXPECT_LT(pixel[channel], 6) << "blank time renders black";
    }
}

TEST(EditTimelineReader, BlankTimeIsBlackSilenceAndAnEmptyUpperTrackRevealsTheLowerOne) {
    const auto path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
    const auto metadata = ProbeEditMedia(path);
    ASSERT_TRUE(metadata.error.empty()) << metadata.error;
    const auto d = metadata.duration_us;
    TimelineRenderSnapshot snapshot;
    snapshot.sources = {{1, path, metadata}};
    // Track 1 is drawn in front of track 2. Track 1 holds only a later clip, so
    // before it starts the lower track shows through; a gap follows both.
    snapshot.timeline.tracks = {{1, false}, {2, false}};
    TimelineClip lower{1, 1, 0, false, 0, 0, d};
    lower.track = 2;
    TimelineClip upper{2, 1, 0, false, d / 2, 0, d};
    upper.track = 1;
    TimelineClip after_gap{3, 1, 0, false, 2 * d + 1'000'000, 0, d};
    after_gap.track = 2;
    snapshot.timeline.clips = {lower, upper, after_gap};
    snapshot.timeline.duration_us = 3 * d + 1'000'000;
    const EditRenderPlan plan{EditExportPath::Render, {}, {64, 64}, 30, 1};
    EditTimelineReader reader;
    std::string error;
    ASSERT_TRUE(reader.Open(snapshot, plan, error)) << error;
    TimelineVideoFrame frame;
    ASSERT_TRUE(reader.VideoAt(d / 4, frame, error)) << error;
    ASSERT_EQ(frame.layers.size(), 1u);
    EXPECT_EQ(frame.layers.front().layer, 0u);
    ASSERT_TRUE(reader.VideoAt(d / 2 + d / 4, frame, error)) << error;
    ASSERT_EQ(frame.layers.size(), 2u);
    // Back to front: the lower track's clip first, the upper track's last.
    EXPECT_NEAR(static_cast<double>(frame.layers[0].frame.pts_us), static_cast<double>(d / 2 + d / 4), 40'000);
    EXPECT_NEAR(static_cast<double>(frame.layers[1].frame.pts_us), static_cast<double>(d / 4), 40'000);
    EXPECT_LT(frame.layers[0].layer, frame.layers[1].layer);
    for (const int64_t blank : {d + d / 2, 2 * d + 500'000}) {
        ASSERT_TRUE(reader.VideoAt(blank, frame, error, VideoReadMode::RandomAccess)) << error;
        EXPECT_TRUE(frame.layers.empty());
        EXPECT_EQ(frame.time_us, blank);
        EXPECT_EQ(frame.output.width, 64u);
    }
    ASSERT_TRUE(reader.VideoAt(2 * d + 1'000'000 + d / 4, frame, error)) << error;
    ASSERT_EQ(frame.layers.size(), 1u);
    std::vector<float> samples;
    ASSERT_TRUE(reader.AudioAt(AudioSampleIndex(2 * d + 100'000), 4800, samples, error)) << error;
    ASSERT_EQ(samples.size(), 9600u);
    EXPECT_TRUE(std::all_of(samples.begin(), samples.end(), [](float value) { return value == 0; }));
    EXPECT_LE(reader.PeakDecoders(), 2u);
}

TEST(EditTimelineReader, ThreeVisibleTracksDecodeAsThreeOrderedLayers) {
    const auto path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
    const auto metadata = ProbeEditMedia(path);
    ASSERT_TRUE(metadata.error.empty()) << metadata.error;
    TimelineRenderSnapshot snapshot;
    snapshot.sources = {{1, path, metadata}};
    snapshot.timeline.tracks = {{1, false}, {2, false}, {3, false}};
    for (uint64_t id = 1; id <= 3; ++id) {
        TimelineClip clip{id, 1, 0, false, 0, 0, metadata.duration_us};
        clip.track = id;
        clip.opacity = 0.5;
        snapshot.timeline.clips.push_back(clip);
    }
    snapshot.timeline.duration_us = metadata.duration_us;
    const EditRenderPlan plan{EditExportPath::Render, {}, {64, 64}, 30, 1};
    EditTimelineReader reader;
    std::string error;
    ASSERT_TRUE(reader.Open(snapshot, plan, error)) << error;
    TimelineVideoFrame frame;
    ASSERT_TRUE(reader.VideoAt(0, frame, error)) << error;
    ASSERT_EQ(frame.layers.size(), 3u);
    for (uint32_t index = 0; index < 3; ++index) {
        EXPECT_EQ(frame.layers[index].layer, index);
        EXPECT_DOUBLE_EQ(frame.layers[index].opacity, 0.5);
        EXPECT_DOUBLE_EQ(frame.layers[index].weight, 1);
    }
    EXPECT_EQ(reader.PeakDecoders(), 3u);
    // Disabling the middle track leaves two layers and retires its cursor on
    // the next frame instead of holding it open.
    snapshot.timeline.tracks[1].enabled = false;
    snapshot.timeline.revision = 2;
    ASSERT_TRUE(reader.UpdateSnapshot(snapshot, plan, error)) << error;
    ASSERT_TRUE(reader.VideoAt(33'333, frame, error)) << error;
    EXPECT_EQ(frame.layers.size(), 2u);
}

TEST(EditTimelineReader, PresentationRevisionRetainsSourceCursor) {
    const auto path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
    const auto metadata = ProbeEditMedia(path);
    ASSERT_TRUE(metadata.error.empty()) << metadata.error;
    ASSERT_TRUE(metadata.content_identity_valid);
    TimelineRenderSnapshot snapshot;
    snapshot.sources = {{1, path, metadata}};
    snapshot.timeline = {{{1, 1, 1, false, 0, 0, metadata.duration_us}}, {}, metadata.duration_us};
    const EditRenderPlan plan{EditExportPath::Render, {}, {64, 64}, 30, 1};
    EditTimelineReader reader;
    std::string error;
    ASSERT_TRUE(reader.Open(snapshot, plan, error)) << error;
    TimelineVideoFrame first;
    ASSERT_TRUE(reader.VideoAt(0, first, error)) << error;
    ASSERT_EQ(first.layers.size(), 1u);
    snapshot.timeline.revision = 2;
    snapshot.timeline.clips.front().gain_db = -6;
    snapshot.timeline.clips.front().opacity = 0.5;
    ASSERT_TRUE(reader.UpdateSnapshot(snapshot, plan, error)) << error;
    TimelineVideoFrame updated;
    ASSERT_TRUE(reader.VideoAt(0, updated, error)) << error;
    ASSERT_EQ(updated.layers.size(), 1u);
    EXPECT_EQ(first.layers.front().frame.backing_frame, updated.layers.front().frame.backing_frame);
    const auto stats = reader.VideoReadStats();
    EXPECT_EQ(stats.seeks, 1u);
    EXPECT_EQ(stats.selected, 2u);
    EXPECT_EQ(stats.held, 1u);
}

TEST(EditTimelineReader, MappingChangeRetiresCursorWithoutLosingMeasurements) {
    const auto path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
    const auto metadata = ProbeEditMedia(path);
    ASSERT_TRUE(metadata.error.empty()) << metadata.error;
    ASSERT_TRUE(metadata.content_identity_valid);
    TimelineRenderSnapshot snapshot;
    snapshot.sources = {{1, path, metadata}};
    snapshot.timeline = {{{1, 1, 1, false, 0, 0, metadata.duration_us}}, {}, metadata.duration_us};
    const EditRenderPlan plan{EditExportPath::Render, {}, {64, 64}, 30, 1};
    EditTimelineReader reader;
    std::string error;
    ASSERT_TRUE(reader.Open(snapshot, plan, error)) << error;
    TimelineVideoFrame frame;
    ASSERT_TRUE(reader.VideoAt(0, frame, error)) << error;
    snapshot.timeline.clips.front().source_in_us = 1'000'000;
    ASSERT_TRUE(reader.UpdateSnapshot(snapshot, plan, error)) << error;
    ASSERT_TRUE(reader.VideoAt(0, frame, error)) << error;
    ASSERT_EQ(frame.layers.size(), 1u);
    EXPECT_EQ(frame.layers.front().frame.pts_us, 1'000'000);
    EXPECT_EQ(reader.VideoReadStats().seeks, 2u);
    EXPECT_EQ(reader.VideoReadStats().selected, 2u);
    snapshot.timeline.clips.clear();
    ASSERT_TRUE(reader.UpdateSnapshot(snapshot, plan, error)) << error;
    EXPECT_EQ(reader.VideoReadStats().seeks, 2u);
    ASSERT_TRUE(reader.Open(snapshot, plan, error)) << error;
    EXPECT_EQ(reader.VideoReadStats().seeks, 0u);
}