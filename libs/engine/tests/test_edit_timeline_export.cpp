#include "exosnap/engine/edit_timeline_export.h"
#include "test_unique_temp.h"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
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
}

using namespace exosnap::engine;

namespace {

std::filesystem::path Fixture() {
    return std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
}

TimelineExportClip FullClip(int64_t start = 0) {
    return {Fixture(), 0, ProbeEditMedia(Fixture()).duration_us, start};
}

class TimelineExportTest : public ::testing::Test {
  protected:
    std::filesystem::path output = exosnap_test::UniqueTempPath("timeline.mp4");
    std::filesystem::path audio_source = exosnap_test::UniqueTempPath("source.mkv");
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove(output, error);
        std::filesystem::remove(audio_source, error);
    }
};

bool WritePcmMatroska(const std::filesystem::path& path) {
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
    std::array<uint8_t, 960> silence{};
    for (int i = 0; i < 100; ++i) {
        AVPacket packet{};
        packet.data = silence.data();
        packet.size = static_cast<int>(silence.size());
        packet.pts = av_rescale_q(i * 480, {1, 48000}, stream->time_base);
        packet.dts = packet.pts;
        packet.duration = av_rescale_q(480, {1, 48000}, stream->time_base);
        packet.flags = AV_PKT_FLAG_KEY;
        if (av_write_frame(context, &packet) < 0)
            return false;
    }
    return av_write_trailer(context) >= 0 && avio_closep(&context->pb) >= 0;
}

int DecodeFrames(const std::filesystem::path& path) {
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

} // namespace

TEST_F(TimelineExportTest, ProbeReadsRealMediaAndReportsMissingSource) {
    const auto metadata = ProbeEditMedia(Fixture());
    EXPECT_TRUE(metadata.error.empty()) << metadata.error;
    EXPECT_GT(metadata.duration_us, 0);
    EXPECT_GT(metadata.width, 0);
    EXPECT_GT(metadata.height, 0);
    EXPECT_GT(metadata.fps, 0);
    EXPECT_FALSE(ProbeEditMedia(output).error.empty());
}

TEST_F(TimelineExportTest, InvalidRecipesAreRejectedBeforeOpeningMedia) {
    EXPECT_EQ(AssessTimelineExport({}, true).eligibility, TimelineExportEligibility::Invalid);
    EXPECT_EQ(AssessTimelineExport({{output, -1, 100, 0}}, true).eligibility, TimelineExportEligibility::Invalid);
    EXPECT_EQ(AssessTimelineExport({{output, 0, 0, 0}}, true).eligibility, TimelineExportEligibility::Invalid);
    EXPECT_EQ(AssessTimelineExport({{output, 0, 100, 0}}, true).eligibility, TimelineExportEligibility::Invalid);
    EXPECT_EQ(AssessTimelineExport({{output, 0, 100, (std::numeric_limits<int64_t>::max)()}}, true).eligibility,
              TimelineExportEligibility::Invalid);
}

TEST_F(TimelineExportTest, GapsAndOverlapRequireRendering) {
    const auto first = FullClip();
    EXPECT_EQ(AssessTimelineExport({first, FullClip(first.source_out_us + 1)}, true).eligibility,
              TimelineExportEligibility::RenderRequired);
    EXPECT_EQ(AssessTimelineExport({first, FullClip(first.source_out_us - 1)}, true).eligibility,
              TimelineExportEligibility::RenderRequired);
    EXPECT_EQ(AssessTimelineExport({FullClip(1)}, true).eligibility, TimelineExportEligibility::RenderRequired);
}

TEST_F(TimelineExportTest, MultiplePartialSourcesRequireRendering) {
    auto first = FullClip();
    first.source_out_us /= 2;
    EXPECT_EQ(AssessTimelineExport({first, FullClip(first.source_out_us)}, true).eligibility,
              TimelineExportEligibility::RenderRequired);
}

TEST_F(TimelineExportTest, DifferentStreamCodecsRequireRendering) {
    ASSERT_TRUE(WritePcmMatroska(audio_source));
    const auto first = FullClip();
    const TimelineExportClip audio{audio_source, 0, ProbeEditMedia(audio_source).duration_us, first.source_out_us};
    EXPECT_EQ(AssessTimelineExport({first, audio}, false).eligibility, TimelineExportEligibility::RenderRequired);
}

TEST_F(TimelineExportTest, AdjacentSplitSlicesCoalesceAndRemainCopyable) {
    auto left = FullClip();
    auto right = left;
    left.source_out_us /= 2;
    right.source_in_us = left.source_out_us;
    right.timeline_start_us = left.source_out_us;
    EXPECT_EQ(AssessTimelineExport({left, right}, true).eligibility, TimelineExportEligibility::StreamCopy);
    ASSERT_TRUE(ExportTimelineStreamCopy({left, right}, output, true).success);
    EXPECT_EQ(DecodeFrames(output), DecodeFrames(Fixture()));
}

TEST_F(TimelineExportTest, SingleTrimRetainsProductionKeyframeContract) {
    auto clip = FullClip();
    clip.source_in_us = 500000;
    clip.source_out_us = 1500000;
    EXPECT_EQ(AssessTimelineExport({clip}, true).eligibility, TimelineExportEligibility::StreamCopy);
    ASSERT_TRUE(ExportTimelineStreamCopy({clip}, output, true).success);
    const auto count = DecodeFrames(output);
    EXPECT_GT(count, 0);
    EXPECT_LT(count, DecodeFrames(Fixture()));
}

TEST_F(TimelineExportTest, HardCutExportsBothCompleteSourcesAndDecodesEveryFrame) {
    const auto first = FullClip();
    const auto second = FullClip(first.source_out_us);
    const auto assessment = AssessTimelineExport({first, second}, true);
    ASSERT_EQ(assessment.eligibility, TimelineExportEligibility::StreamCopy) << assessment.reason;
    float last = 0;
    const auto result = ExportTimelineStreamCopy({first, second}, output, true, [&](float value) {
        EXPECT_GE(value, last);
        last = value;
        return true;
    });
    ASSERT_TRUE(result.success) << result.message;
    EXPECT_EQ(last, 1);
    EXPECT_EQ(DecodeFrames(output), 2 * DecodeFrames(Fixture()));
    EXPECT_LE(std::abs(ProbeEditMedia(output).duration_us - 2 * first.source_out_us), 1000);
}

TEST_F(TimelineExportTest, HardCutExportsToMatroska) {
    const auto first = FullClip();
    const auto result = ExportTimelineStreamCopy({first, FullClip(first.source_out_us)}, output, false);
    ASSERT_TRUE(result.success) << result.message;
    EXPECT_EQ(DecodeFrames(output), 2 * DecodeFrames(Fixture()));
}

TEST_F(TimelineExportTest, AudioHardCutPreservesEverySampleAndMonotoneTimestamps) {
    ASSERT_TRUE(WritePcmMatroska(audio_source));
    const auto metadata = ProbeEditMedia(audio_source);
    ASSERT_TRUE(metadata.error.empty()) << metadata.error;
    ASSERT_TRUE(metadata.has_audio);
    ASSERT_EQ(metadata.duration_us, 1000000);
    const std::vector<TimelineExportClip> clips{{audio_source, 0, metadata.duration_us, 0},
                                                {audio_source, 0, metadata.duration_us, metadata.duration_us}};
    const auto result = ExportTimelineStreamCopy(clips, output, false);
    ASSERT_TRUE(result.success) << result.message;
    AVFormatContext* input = nullptr;
    ASSERT_GE(avformat_open_input(&input, output.string().c_str(), nullptr, nullptr), 0);
    auto close_input = [](AVFormatContext* p) { avformat_close_input(&p); };
    std::unique_ptr<AVFormatContext, decltype(close_input)> guard(input, close_input);
    ASSERT_GE(avformat_find_stream_info(input, nullptr), 0);
    ASSERT_EQ(input->nb_streams, 1u);
    EXPECT_EQ(input->streams[0]->codecpar->codec_id, AV_CODEC_ID_PCM_S16LE);
    auto free_packet = [](AVPacket* p) { av_packet_free(&p); };
    std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
    ASSERT_TRUE(packet);
    int64_t bytes = 0;
    int64_t last = AV_NOPTS_VALUE;
    int status = 0;
    while ((status = av_read_frame(input, packet.get())) >= 0) {
        EXPECT_GT(packet->dts, last);
        EXPECT_EQ(packet->pts, packet->dts);
        last = packet->dts;
        bytes += packet->size;
        av_packet_unref(packet.get());
    }
    EXPECT_EQ(status, AVERROR_EOF);
    EXPECT_EQ(bytes, 2 * 48000 * 2);
    EXPECT_LE(std::abs(input->duration - 2000000), 1000);
}

TEST_F(TimelineExportTest, CancellationRemovesPartialOutput) {
    const auto first = FullClip();
    const auto result = ExportTimelineStreamCopy({first, FullClip(first.source_out_us)}, output, true,
                                                 [](float progress) { return progress < 0.2f; });
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST_F(TimelineExportTest, ExistingDestinationIsNeverTruncated) {
    {
        std::ofstream existing(output);
        existing << "preserved";
    }
    EXPECT_FALSE(ExportTimelineStreamCopy({FullClip()}, output, true).success);
    std::ifstream existing(output);
    std::string contents;
    existing >> contents;
    EXPECT_EQ(contents, "preserved");
}
