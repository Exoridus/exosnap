#include "test_unique_temp.h"
#include <exosnap/engine/codec_types.h>
#include <exosnap/engine/edit_export_plan.h>
#include <exosnap/engine/edit_timeline.h>
#include <exosnap/engine/edit_timeline_export.h>
#include <exosnap/engine/edit_timeline_render.h>
#include <exosnap/engine/edit_types.h>
#include <exosnap/engine/mp4_remuxer.h>
#include <exosnap/engine/recorder_session.h>
#include <exosnap/engine/recording_media_types.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/codec_id.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

using namespace exosnap::engine;

namespace {

constexpr int64_t Second = 1'000'000;

std::filesystem::path VideoFixture() {
    return std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
}

bool WriteConstantMonoPcm(const std::filesystem::path& path, int16_t level, int seconds) {
    AVFormatContext* context = nullptr;
    if (avformat_alloc_output_context2(&context, nullptr, "matroska", path.string().c_str()) < 0 || !context)
        return false;
    auto close_output = [](AVFormatContext* p) {
        if (p->pb)
            avio_closep(&p->pb);
        avformat_free_context(p);
    };
    std::unique_ptr<AVFormatContext, decltype(close_output)> output(context, close_output);
    auto* stream = avformat_new_stream(context, nullptr);
    if (!stream)
        return false;
    stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    stream->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
    stream->codecpar->sample_rate = 48000;
    stream->codecpar->bits_per_coded_sample = 16;
    av_channel_layout_default(&stream->codecpar->ch_layout, 1);
    stream->time_base = {1, 48000};
    if (avio_open(&context->pb, path.string().c_str(), AVIO_FLAG_WRITE) < 0 ||
        avformat_write_header(context, nullptr) < 0)
        return false;
    std::vector<uint8_t> payload(960);
    for (size_t index = 0; index < payload.size(); index += 2) {
        payload[index] = static_cast<uint8_t>(level);
        payload[index + 1] = static_cast<uint8_t>(static_cast<uint16_t>(level) >> 8);
    }
    for (int packet_index = 0; packet_index < seconds * 100; ++packet_index) {
        AVPacket packet{};
        packet.data = payload.data();
        packet.size = static_cast<int>(payload.size());
        packet.pts = av_rescale_q(static_cast<int64_t>(packet_index) * 480, {1, 48000}, stream->time_base);
        packet.dts = packet.pts;
        packet.duration = av_rescale_q(480, {1, 48000}, stream->time_base);
        packet.flags = AV_PKT_FLAG_KEY;
        if (av_write_frame(context, &packet) < 0)
            return false;
    }
    return av_write_trailer(context) >= 0 && avio_closep(&context->pb) >= 0;
}

struct StreamSummary {
    AVMediaType type = AVMEDIA_TYPE_UNKNOWN;
    AVCodecID codec = AV_CODEC_ID_NONE;
    int64_t packets = 0;
    int sample_rate = 0;
    int channels = 0;
    std::vector<int32_t> samples;
};

std::vector<StreamSummary> Summarize(const std::filesystem::path& path) {
    AVFormatContext* context = nullptr;
    if (avformat_open_input(&context, path.string().c_str(), nullptr, nullptr) < 0)
        return {};
    std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> owned(
        context, [](AVFormatContext* p) { avformat_close_input(&p); });
    if (avformat_find_stream_info(context, nullptr) < 0)
        return {};
    std::vector<StreamSummary> result(context->nb_streams);
    for (unsigned index = 0; index < context->nb_streams; ++index) {
        result[index].type = context->streams[index]->codecpar->codec_type;
        result[index].codec = context->streams[index]->codecpar->codec_id;
        result[index].sample_rate = context->streams[index]->codecpar->sample_rate;
        result[index].channels = context->streams[index]->codecpar->ch_layout.nb_channels;
    }
    AVPacket* packet = av_packet_alloc();
    while (av_read_frame(context, packet) >= 0) {
        auto& summary = result[static_cast<size_t>(packet->stream_index)];
        ++summary.packets;
        if (summary.codec == AV_CODEC_ID_PCM_S24LE)
            for (int offset = 0; offset + 2 < packet->size; offset += 3) {
                const int32_t value = packet->data[offset] | (packet->data[offset + 1] << 8) |
                                      (static_cast<int8_t>(packet->data[offset + 2]) << 16);
                summary.samples.push_back(value);
            }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    return result;
}

int DecodeFrames(const std::filesystem::path& path, std::vector<std::array<int, 3>>* pixels = nullptr) {
    AVFormatContext* input = nullptr;
    if (avformat_open_input(&input, path.string().c_str(), nullptr, nullptr) < 0)
        return -1;
    auto close_input = [](AVFormatContext* p) { avformat_close_input(&p); };
    std::unique_ptr<AVFormatContext, decltype(close_input)> input_guard(input, close_input);
    if (avformat_find_stream_info(input, nullptr) < 0)
        return -1;
    const AVCodec* codec = nullptr;
    const int video = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (video < 0)
        return -1;
    auto free_codec = [](AVCodecContext* p) { avcodec_free_context(&p); };
    std::unique_ptr<AVCodecContext, decltype(free_codec)> decoder(avcodec_alloc_context3(codec), free_codec);
    if (!decoder || avcodec_parameters_to_context(decoder.get(), input->streams[video]->codecpar) < 0 ||
        avcodec_open2(decoder.get(), codec, nullptr) < 0)
        return -1;
    auto free_packet = [](AVPacket* p) { av_packet_free(&p); };
    auto free_frame = [](AVFrame* p) { av_frame_free(&p); };
    std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
    std::unique_ptr<AVFrame, decltype(free_frame)> frame(av_frame_alloc(), free_frame);
    if (!packet || !frame)
        return -1;
    int count = 0;
    auto drain = [&] {
        int result = 0;
        while ((result = avcodec_receive_frame(decoder.get(), frame.get())) >= 0) {
            ++count;
            if (pixels) {
                if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P)
                    return false;
                pixels->push_back({frame->data[0][frame->height / 2 * frame->linesize[0] + frame->width / 2],
                                   frame->data[1][frame->height / 4 * frame->linesize[1] + frame->width / 4],
                                   frame->data[2][frame->height / 4 * frame->linesize[2] + frame->width / 4]});
            }
            av_frame_unref(frame.get());
        }
        return result == AVERROR(EAGAIN) || result == AVERROR_EOF;
    };
    int status = 0;
    while ((status = av_read_frame(input, packet.get())) >= 0) {
        if (packet->stream_index == video && (avcodec_send_packet(decoder.get(), packet.get()) < 0 || !drain()))
            return -1;
        av_packet_unref(packet.get());
    }
    if (status != AVERROR_EOF || avcodec_send_packet(decoder.get(), nullptr) < 0 || !drain())
        return -1;
    return count;
}

struct DecodedAudio {
    int64_t frames = 0;
    double middle_level = 0;
};

// Decodes the first audio stream, honouring the container's priming discard.
DecodedAudio DecodeAudio(const std::filesystem::path& path) {
    DecodedAudio result{-1, 0};
    AVFormatContext* input = nullptr;
    if (avformat_open_input(&input, path.string().c_str(), nullptr, nullptr) < 0)
        return result;
    std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> owned(
        input, [](AVFormatContext* p) { avformat_close_input(&p); });
    if (avformat_find_stream_info(input, nullptr) < 0)
        return result;
    const AVCodec* codec = nullptr;
    const int audio = av_find_best_stream(input, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (audio < 0)
        return result;
    std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)> decoder(
        avcodec_alloc_context3(codec), [](AVCodecContext* p) { avcodec_free_context(&p); });
    if (!decoder || avcodec_parameters_to_context(decoder.get(), input->streams[audio]->codecpar) < 0 ||
        avcodec_open2(decoder.get(), codec, nullptr) < 0)
        return result;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::vector<float> left;
    const auto drain = [&] {
        while (avcodec_receive_frame(decoder.get(), frame) >= 0) {
            if (frame->format == AV_SAMPLE_FMT_FLTP)
                for (int i = 0; i < frame->nb_samples; ++i)
                    left.push_back(reinterpret_cast<const float*>(frame->extended_data[0])[i]);
            else
                left.insert(left.end(), static_cast<size_t>(frame->nb_samples), 0.0f);
            av_frame_unref(frame);
        }
    };
    while (av_read_frame(input, packet) >= 0) {
        if (packet->stream_index == audio && avcodec_send_packet(decoder.get(), packet) >= 0)
            drain();
        av_packet_unref(packet);
    }
    avcodec_send_packet(decoder.get(), nullptr);
    drain();
    av_frame_free(&frame);
    av_packet_free(&packet);
    result.frames = static_cast<int64_t>(left.size());
    if (left.size() > 48000) {
        double sum = 0;
        for (size_t i = left.size() / 2 - 12000; i < left.size() / 2 + 12000; ++i)
            sum += left[i];
        result.middle_level = sum / 24000;
    }
    return result;
}

class EditorExportMedia : public ::testing::Test {
  protected:
    std::filesystem::path audio_source = exosnap_test::UniqueTempPath("export-audio.mkv");
    std::filesystem::path output = exosnap_test::UniqueTempPath("export-output.mkv");
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove(audio_source, error);
        std::filesystem::remove(output, error);
    }

    TimelineRenderSnapshot Snapshot(bool with_audio, int64_t video_in = 0) {
        TimelineRenderSnapshot snapshot;
        const auto video = ProbeEditMedia(VideoFixture());
        snapshot.sources.push_back({1, VideoFixture(), video, std::nullopt});
        snapshot.timeline.tracks = {{1, false}};
        TimelineClip clip{1, 1, 1, false, 0, video_in, video.duration_us};
        clip.track = 1;
        snapshot.timeline.clips.push_back(clip);
        snapshot.timeline.duration_us = video.duration_us - video_in;
        if (with_audio) {
            EXPECT_TRUE(WriteConstantMonoPcm(audio_source, 8192, 5));
            const auto audio = ProbeEditMedia(audio_source);
            snapshot.sources.push_back({2, audio_source, audio, std::nullopt});
            snapshot.timeline.tracks.push_back({2, true, true, -6.0206});
            TimelineClip mixed{2, 2, 2, true, 0, 0, snapshot.timeline.duration_us};
            mixed.track = 2;
            mixed.stream = TimelineStreamRef{2, audio.streams.front().stream_identity.value_or(AudioTrackId{{1}}), 0,
                                             TimelineStreamKind::Audio};
            snapshot.timeline.clips.push_back(mixed);
        }
        return snapshot;
    }

    std::shared_ptr<const EditExportPlan> Plan(const TimelineRenderSnapshot& snapshot) {
        EditExportIntent intent;
        intent.span = EditExportSpan::EntireTimeline;
        intent.destination = output;
        return PrepareEditExport(snapshot, intent, AssessEditExportFacts(snapshot, intent, true));
    }
};

class EditorExportScopeMedia : public EditorExportMedia {
  protected:
    std::filesystem::path fixture_directory;

    void SetUp() override {
        const char* directory = std::getenv("EXOSNAP_EDIT_RENDER_FIXTURES");
        if (!directory)
            GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated red/blue media.";
        fixture_directory = directory;
        ASSERT_TRUE(std::filesystem::is_regular_file(fixture_directory / "red.mkv"));
        ASSERT_TRUE(std::filesystem::is_regular_file(fixture_directory / "blue.mkv"));
    }

    TimelineRenderSnapshot GappedSnapshot(bool red_only = false) {
        TimelineRenderSnapshot snapshot;
        for (uint64_t id : {1ULL, 2ULL}) {
            const auto path = fixture_directory / (id == 1 ? "red.mkv" : "blue.mkv");
            const auto metadata = ProbeEditMedia(path);
            EXPECT_TRUE(metadata.error.empty()) << metadata.error;
            snapshot.sources.push_back({id, path, metadata, std::nullopt});
        }
        snapshot.timeline.tracks = {{1, false}, {2, true}};
        snapshot.timeline.duration_us = 5 * Second;
        snapshot.timeline.output_fps_num = 30;
        snapshot.timeline.output_fps_den = 1;
        for (uint64_t index : {0ULL, 1ULL}) {
            const uint64_t asset = red_only ? 1 : index + 1;
            TimelineClip video{index + 1, asset, index + 1, false, (1 + 2 * static_cast<int64_t>(index)) * Second,
                               0,         Second};
            video.track = 1;
            auto audio = video;
            audio.id = index + 3;
            audio.audio = true;
            audio.track = 2;
            const auto& metadata = snapshot.sources[asset - 1].metadata;
            const auto stream = std::find_if(metadata.streams.begin(), metadata.streams.end(), [](const auto& value) {
                return value.kind == TimelineStreamKind::Audio && value.stream_identity.has_value();
            });
            EXPECT_NE(stream, metadata.streams.end());
            if (stream == metadata.streams.end())
                return {};
            audio.stream =
                TimelineStreamRef{asset, *stream->stream_identity, stream->container_index, TimelineStreamKind::Audio};
            snapshot.timeline.clips.push_back(video);
            snapshot.timeline.clips.push_back(audio);
        }
        return snapshot;
    }

    void ExportAndCheck(const TimelineRenderSnapshot& snapshot, EditExportIntent intent,
                        const std::vector<int>& seconds) {
        intent.destination = output;
        intent.video_profile = EditVideoProfile::H264;
        intent.fps_num = 30;
        intent.fps_den = 1;
        const auto plan = PrepareEditExport(snapshot, intent, AssessEditExportFacts(snapshot, intent, true));
        ASSERT_TRUE(plan->ready()) << "issue count " << plan->issues.size();
        ASSERT_EQ(plan->span_out_us - plan->span_in_us, static_cast<int64_t>(seconds.size()) * Second);
        RecorderConfig config;
        config.video_codec = VideoCodec::H264;
        const auto result = ExecuteEditExport(*plan, config, output, RemuxNoopCallback());
        ASSERT_TRUE(result.success) << result.message;
        const auto streams = Summarize(output);
        ASSERT_EQ(streams.size(), 2u);
        EXPECT_EQ(streams[0].type, AVMEDIA_TYPE_VIDEO);
        EXPECT_EQ(streams[0].codec, AV_CODEC_ID_H264);
        ASSERT_EQ(streams[1].codec, AV_CODEC_ID_PCM_S24LE);
        ASSERT_EQ(streams[1].sample_rate, 48000);
        ASSERT_EQ(streams[1].channels, 2);
        ASSERT_EQ(streams[1].samples.size(), seconds.size() * 48000 * 2);
        std::vector<std::array<int, 3>> pixels;
        ASSERT_EQ(DecodeFrames(output, &pixels), static_cast<int>(seconds.size() * 30));
        ASSERT_EQ(pixels.size(), seconds.size() * 30);
        for (size_t second = 0; second < seconds.size(); ++second) {
            const int color = seconds[second];
            for (size_t frame = second * 30; frame < (second + 1) * 30; ++frame) {
                const auto& yuv = pixels[frame];
                if (color == 0) {
                    ASSERT_LT(yuv[0], 25) << "frame " << frame;
                    ASSERT_NEAR(yuv[1], 128, 4) << "frame " << frame;
                    ASSERT_NEAR(yuv[2], 128, 4) << "frame " << frame;
                } else if (color == 1) {
                    ASSERT_GT(yuv[2], yuv[1] + 60) << "red frame " << frame;
                } else {
                    ASSERT_GT(yuv[1], yuv[2] + 60) << "blue frame " << frame;
                }
            }
            const int expected = color == 0 ? 0 : (color == 1 ? 1 << 21 : -(1 << 21));
            for (size_t sample = second * 48000 * 2; sample < (second + 1) * 48000 * 2; ++sample)
                ASSERT_NEAR(streams[1].samples[sample], expected, 64) << "sample " << sample / 2;
        }
    }
};

TEST_F(EditorExportScopeMedia, EntireTimelinePreservesLeadingInteriorAndFixedTrailingBlank) {
    EditExportIntent intent;
    intent.span = EditExportSpan::EntireTimeline;
    ExportAndCheck(GappedSnapshot(), intent, {0, 1, 0, 2, 0});
}

TEST_F(EditorExportScopeMedia, OccupiedContentRebasesAndRetainsInteriorBlank) {
    EditExportIntent intent;
    intent.span = EditExportSpan::OccupiedContent;
    ExportAndCheck(GappedSnapshot(), intent, {1, 0, 2});
}

TEST_F(EditorExportScopeMedia, TimeRangeDoesNotLeaseOrDecodeMissingSourcesOutsideTheOutput) {
    auto snapshot = GappedSnapshot();
    snapshot.timeline.tracks.push_back({99, false});
    TimelineClip excluded{99, 99, 99, false, 0, 0, Second};
    excluded.track = 99;
    snapshot.timeline.clips.insert(snapshot.timeline.clips.begin(), excluded);
    EditExportIntent intent;
    intent.span = EditExportSpan::TimeRange;
    intent.selection_in_us = Second;
    intent.selection_out_us = 4 * Second;
    ExportAndCheck(snapshot, intent, {1, 0, 2});
}

TEST_F(EditorExportScopeMedia, DisabledMissingSourcesDoNotBlockRendering) {
    auto snapshot = GappedSnapshot();
    snapshot.timeline.tracks.push_back({99, false});
    TimelineClip excluded{99, 99, 99, false, 0, 0, Second};
    excluded.track = 99;
    excluded.enabled = false;
    snapshot.timeline.clips.insert(snapshot.timeline.clips.begin(), excluded);
    EditExportIntent intent;
    intent.span = EditExportSpan::EntireTimeline;
    ExportAndCheck(snapshot, intent, {0, 1, 0, 2, 0});
}

TEST_F(EditorExportScopeMedia, OriginalStreamsCannotCopyAudioExcludedFromSelectedClips) {
    TimelineRenderSnapshot snapshot;
    const auto path = fixture_directory / "red.mkv";
    const auto metadata = ProbeEditMedia(path);
    ASSERT_TRUE(metadata.error.empty()) << metadata.error;
    snapshot.sources.push_back({1, path, metadata});
    snapshot.timeline.duration_us = metadata.duration_us;
    snapshot.timeline.tracks = {{1, false}, {2, true}};
    for (const auto& stream : metadata.streams) {
        const bool audio = stream.kind == TimelineStreamKind::Audio;
        TimelineClip clip{audio ? 2ULL : 1ULL, 1, 1, audio, 0, 0, metadata.duration_us};
        clip.track = audio ? 2 : 1;
        clip.stream =
            TimelineStreamRef{1, stream.stream_identity.value_or(AudioTrackId{}), stream.container_index, stream.kind};
        snapshot.timeline.clips.push_back(clip);
    }
    EditExportIntent intent;
    intent.mode = EditExportMode::OriginalStreams;
    intent.span = EditExportSpan::SelectedClips;
    intent.selected_clip_ids = {1};
    intent.destination = output;
    const auto plan = PrepareEditExport(snapshot, intent, AssessEditExportFacts(snapshot, intent, true));
    EXPECT_FALSE(plan->ready());
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST_F(EditorExportScopeMedia, FixedBlankTimelineUsesItsRetainedOutputFormatWithoutAnySource) {
    TimelineRenderSnapshot snapshot;
    snapshot.timeline.duration_us = 2 * Second;
    snapshot.timeline.output_width = 320;
    snapshot.timeline.output_height = 180;
    snapshot.timeline.output_fps_num = 30;
    snapshot.timeline.output_fps_den = 1;
    EditExportIntent intent;
    intent.span = EditExportSpan::EntireTimeline;
    intent.destination = output;
    const auto plan = PrepareEditExport(snapshot, intent, AssessEditExportFacts(snapshot, intent, true));
    ASSERT_TRUE(plan->ready());
    RecorderConfig config;
    config.video_codec = VideoCodec::H264;
    const auto result = ExecuteEditExport(*plan, config, output, RemuxNoopCallback());
    ASSERT_TRUE(result.success) << result.message;
    const auto streams = Summarize(output);
    ASSERT_EQ(streams.size(), 1u);
    EXPECT_EQ(streams.front().type, AVMEDIA_TYPE_VIDEO);
    std::vector<std::array<int, 3>> pixels;
    ASSERT_EQ(DecodeFrames(output, &pixels), 60);
    for (const auto& pixel : pixels) {
        EXPECT_LT(pixel[0], 25);
        EXPECT_NEAR(pixel[1], 128, 4);
        EXPECT_NEAR(pixel[2], 128, 4);
    }
}

TEST_F(EditorExportScopeMedia, SelectedClipsExcludeSimultaneousUnselectedVideoAndAudio) {
    auto snapshot = GappedSnapshot(true);
    ASSERT_EQ(snapshot.timeline.clips.size(), 4u);
    snapshot.timeline.tracks.insert(snapshot.timeline.tracks.begin(), {3, false});
    snapshot.timeline.tracks.push_back({4, true});
    TimelineClip video{5, 2, 3, false, Second, 0, 2 * Second};
    video.track = 3;
    auto audio = video;
    audio.id = 6;
    audio.audio = true;
    audio.track = 4;
    const auto& streams = snapshot.sources[1].metadata.streams;
    const auto stream = std::find_if(streams.begin(), streams.end(), [](const auto& value) {
        return value.kind == TimelineStreamKind::Audio && value.stream_identity.has_value();
    });
    ASSERT_NE(stream, streams.end());
    audio.stream = TimelineStreamRef{2, *stream->stream_identity, stream->container_index, TimelineStreamKind::Audio};
    snapshot.timeline.clips.push_back(video);
    snapshot.timeline.clips.push_back(audio);
    EditExportIntent intent;
    intent.span = EditExportSpan::SelectedClips;
    intent.selected_clip_ids = {1, 2, 3, 4};
    ExportAndCheck(snapshot, intent, {1, 0, 1});
}

TEST_F(EditorExportMedia, MissingInitialMatroskaDtsRendersFinishedVideoButPreservesOriginalStreamCopy) {
    const auto remux = RemuxToMkv(VideoFixture(), audio_source);
    ASSERT_TRUE(remux.success) << remux.message;
    {
        AVFormatContext* context = nullptr;
        ASSERT_GE(avformat_open_input(&context, audio_source.string().c_str(), nullptr, nullptr), 0);
        std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> input(
            context, [](AVFormatContext* p) { avformat_close_input(&p); });
        ASSERT_GE(avformat_find_stream_info(context, nullptr), 0);
        auto free_packet = [](AVPacket* packet) { av_packet_free(&packet); };
        std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
        ASSERT_NE(packet, nullptr);
        ASSERT_GE(av_read_frame(context, packet.get()), 0);
        ASSERT_NE(packet->pts, AV_NOPTS_VALUE);
        ASSERT_EQ(packet->dts, AV_NOPTS_VALUE);
    }
    auto snapshot = Snapshot(false);
    snapshot.sources.front().path = audio_source;
    snapshot.sources.front().metadata = ProbeEditMedia(audio_source);
    ASSERT_TRUE(snapshot.sources.front().metadata.error.empty());
    snapshot.timeline.duration_us = snapshot.sources.front().metadata.duration_us;
    snapshot.timeline.clips.front().source_out_us = snapshot.timeline.duration_us;
    EditExportIntent intent;
    intent.span = EditExportSpan::EntireTimeline;
    intent.destination = output;
    const auto original_duration = ProbeEditMedia(VideoFixture()).duration_us;
    const std::vector<TimelineExportClip> consecutive{{VideoFixture(), 0, original_duration, 0},
                                                      {VideoFixture(), 0, original_duration, original_duration}};
    EXPECT_TRUE(HasUsableCopiedVideoTimestamps(consecutive));
    auto overlapping = consecutive;
    overlapping.back().timeline_start_us = 0;
    EXPECT_FALSE(HasUsableCopiedVideoTimestamps(overlapping));
    std::atomic<bool> cancelled{true};
    EXPECT_FALSE(HasUsableCopiedVideoTimestamps(consecutive, &cancelled));
    const auto facts = AssessEditExportFacts(snapshot, intent, true);
    EXPECT_TRUE(facts.original_copy_qualified);
    EXPECT_FALSE(facts.video_copy_qualified);
    const auto finished = PrepareEditExport(snapshot, intent, facts);
    ASSERT_TRUE(finished->ready());
    EXPECT_EQ(finished->video_path, EditVideoPath::Render);
    intent.mode = EditExportMode::OriginalStreams;
    auto& video = snapshot.timeline.clips.front();
    const auto& stream = snapshot.sources.front().metadata.streams.front();
    video.stream = TimelineStreamRef{1, stream.stream_identity.value_or(AudioTrackId{{1}}), stream.container_index,
                                     TimelineStreamKind::Video};
    const auto original = PrepareEditExport(snapshot, intent, AssessEditExportFacts(snapshot, intent, true));
    ASSERT_TRUE(original->ready()) << "issue count " << original->issues.size();
    EXPECT_EQ(original->video_path, EditVideoPath::Copy);
    const auto result = ExecuteEditExport(*original, {}, output, RemuxNoopCallback());
    ASSERT_TRUE(result.success) << result.message;
    EXPECT_EQ(DecodeFrames(output), DecodeFrames(VideoFixture()));
}

TEST_F(EditorExportMedia, UnchangedVideoIsCopiedBesideARenderedStereoMix) {
    const auto snapshot = Snapshot(true);
    const auto plan = Plan(snapshot);
    ASSERT_TRUE(plan->ready());
    EXPECT_EQ(plan->video_path, EditVideoPath::Copy);
    EXPECT_EQ(plan->audio_path, EditAudioPath::RenderStereo);
    const auto result = ExecuteEditExport(*plan, {}, output, RemuxNoopCallback());
    ASSERT_TRUE(result.success) << result.message;
    const auto source = Summarize(VideoFixture());
    const auto exported = Summarize(output);
    ASSERT_EQ(exported.size(), 2u);
    EXPECT_EQ(exported[0].type, AVMEDIA_TYPE_VIDEO);
    EXPECT_EQ(exported[0].codec, AV_CODEC_ID_H264);
    ASSERT_EQ(source.size(), 1u);
    EXPECT_EQ(exported[0].packets, source[0].packets);
    const auto decoded = DecodeFrames(output);
    EXPECT_GT(decoded, 0);
    EXPECT_EQ(decoded, DecodeFrames(VideoFixture()));
    EXPECT_EQ(exported[1].codec, AV_CODEC_ID_PCM_S24LE);
    const auto expected_frames = (snapshot.timeline.duration_us * 48000 + Second / 2) / Second;
    EXPECT_EQ(static_cast<int64_t>(exported[1].samples.size()), expected_frames * 2);
    for (size_t index = 0; index < exported[1].samples.size(); ++index)
        ASSERT_NEAR(exported[1].samples[index], 1 << 20, 64) << "sample " << index / 2;
}

TEST_F(EditorExportMedia, FinishedMp4CopiesVideoBesideAnAacMix) {
    const auto mp4 = exosnap_test::UniqueTempPath("export-output.mp4");
    const auto snapshot = Snapshot(true);
    EditExportIntent intent;
    intent.span = EditExportSpan::EntireTimeline;
    intent.container = EditExportContainer::Mp4;
    intent.destination = mp4;
    const auto plan = PrepareEditExport(snapshot, intent, AssessEditExportFacts(snapshot, intent, true));
    ASSERT_TRUE(plan->ready());
    EXPECT_EQ(plan->video_path, EditVideoPath::Copy);
    const auto result = ExecuteEditExport(*plan, {}, mp4, RemuxNoopCallback());
    ASSERT_TRUE(result.success) << result.message;
    auto intermediate = mp4;
    intermediate += L".mix.mkv";
    EXPECT_FALSE(std::filesystem::exists(intermediate));
    const auto exported = Summarize(mp4);
    ASSERT_EQ(exported.size(), 2u);
    EXPECT_EQ(exported[0].codec, AV_CODEC_ID_H264);
    EXPECT_EQ(exported[1].codec, AV_CODEC_ID_AAC);
    EXPECT_EQ(DecodeFrames(mp4), DecodeFrames(VideoFixture()));
    const auto audio = DecodeAudio(mp4);
    const auto expected = (snapshot.timeline.duration_us * 48000 + Second / 2) / Second;
    EXPECT_NEAR(static_cast<double>(audio.frames), static_cast<double>(expected), 1024);
    EXPECT_NEAR(audio.middle_level, 0.125, 0.01);
    std::error_code ignored;
    std::filesystem::remove(mp4, ignored);
}

TEST_F(EditorExportMedia, TimelineWithoutEnabledAudioHasNoAudioStream) {
    const auto snapshot = Snapshot(false);
    const auto plan = Plan(snapshot);
    ASSERT_TRUE(plan->ready());
    EXPECT_EQ(plan->audio_path, EditAudioPath::Absent);
    const auto result = ExecuteEditExport(*plan, {}, output, RemuxNoopCallback());
    ASSERT_TRUE(result.success) << result.message;
    const auto exported = Summarize(output);
    ASSERT_EQ(exported.size(), 1u);
    EXPECT_EQ(exported[0].type, AVMEDIA_TYPE_VIDEO);
}

TEST_F(EditorExportMedia, TrimmedVideoIsNeverCopiedWithExpandedBoundaries) {
    const auto plan = Plan(Snapshot(true, Second / 2));
    EXPECT_FALSE(plan->facts.video_copy_qualified);
    EXPECT_NE(plan->video_path, EditVideoPath::Copy);
}

TEST_F(EditorExportMedia, DestinationThatAliasesASourceIsRejected) {
    const auto snapshot = Snapshot(false);
    EditExportIntent intent;
    intent.span = EditExportSpan::EntireTimeline;
    intent.destination = VideoFixture();
    const auto plan = PrepareEditExport(snapshot, intent, AssessEditExportFacts(snapshot, intent, true));
    EXPECT_FALSE(plan->ready());
    EXPECT_FALSE(plan->facts.destination_admitted);
}

TEST_F(EditorExportMedia, CancelledHybridExportLeavesNoOutput) {
    const auto plan = Plan(Snapshot(true));
    ASSERT_TRUE(plan->ready());
    int calls = 0;
    const auto result = ExecuteEditExport(*plan, {}, output, [&calls](float) { return ++calls < 3; });
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(std::filesystem::exists(output));
}

} // namespace
