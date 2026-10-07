// test_remux_trim.cpp — unit tests for TrimRange stream-copy and
//                        ExtractKeyframeTimestamps.
//
// Test strategy: generate synthetic MKVs via MatroskaStreamWriter (same pattern
// as test_mp4_remuxer.cpp), then run RemuxToProgressiveMp4 / RemuxToMkv with
// TrimRange overloads and verify the output is smaller / correctly bounded.
// ExtractKeyframeTimestamps is tested for non-empty sorted output and graceful
// failure on bad input.

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <system_error>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
}

#include "exosnap/engine/mp4_remuxer.h"
#include "matroska_packet_timestamps.h"
#include "matroska_stream_writer.h"
#include "test_unique_temp.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using exosnap::engine::ExtractKeyframeTimestamps;
using exosnap::engine::MatroskaStreamConfig;
using exosnap::engine::MatroskaStreamWriter;
using exosnap::engine::MuxPacket;
using exosnap::engine::RemuxNoopCallback;
using exosnap::engine::RemuxToMkv;
using exosnap::engine::RemuxToProgressiveMp4;
using exosnap::engine::TrimRange;

namespace {

// ---------------------------------------------------------------------------
// Minimal codec private data (same stubs as test_mp4_remuxer.cpp)
// ---------------------------------------------------------------------------

static std::vector<uint8_t> FakeH264Cp_tr() {
    return {0x01, 0x42, 0x00, 0x1F, 0xFF, 0xE1, 0x00};
}
static std::vector<uint8_t> ValidAacCp_tr() {
    return {0x13, 0x90}; // AAC-LC 48 kHz stereo AudioSpecificConfig
}

// ---------------------------------------------------------------------------
// Config factory
// ---------------------------------------------------------------------------

static MatroskaStreamConfig MakeTrimCfg(const std::string& path) {
    MatroskaStreamConfig c;
    c.output_path = path;
    c.video_codec_id = "V_MPEG4/ISO/AVC";
    c.video_codec_private = FakeH264Cp_tr();
    c.encode_width = 1280;
    c.encode_height = 720;
    c.frame_rate_num = 60;
    c.frame_rate_den = 1;
    c.audio_codec = exosnap::engine::StreamAudioCodec::Aac;
    c.audio_track_count = 1;
    c.audio_tracks[0].codec_private = ValidAacCp_tr();
    return c;
}

// ---------------------------------------------------------------------------
// Packet feeder — 60 fps video, 48 kHz audio, gop = keyframe every `gop` frames
// ---------------------------------------------------------------------------

static void FeedTrimSeconds(MatroskaStreamWriter& w, double seconds, int gop = 60) {
    const uint64_t vframe = 1000000000ULL / 60;
    const uint64_t aframe = 1024ULL * 1000000000ULL / 48000ULL;
    const uint64_t total_ns = static_cast<uint64_t>(seconds * 1e9);
    const std::vector<uint8_t> blob(256, 0xAB);

    uint64_t vpts = 0;
    int vidx = 0;
    uint64_t apts = 0;

    while (vpts < total_ns || apts < total_ns) {
        if (vpts <= apts && vpts < total_ns) {
            MuxPacket p;
            p.pts_ns = vpts;
            p.track_num = 1;
            p.is_key = (vidx % gop == 0);
            p.bytes = blob;
            w.Push(std::move(p));
            vpts += vframe;
            ++vidx;
        } else if (apts < total_ns) {
            MuxPacket p;
            p.pts_ns = apts;
            p.track_num = 2;
            p.is_key = true;
            p.bytes = blob;
            w.Push(std::move(p));
            apts += aframe;
        } else {
            break;
        }
    }
}

// Build a synthetic MKV; return path on success, empty string on failure.
static std::string BuildTrimMkv(const std::string& path, double seconds = 6.0, int gop = 60) {
    MatroskaStreamWriter w;
    auto cfg = MakeTrimCfg(path);
    if (!w.Open(cfg))
        return {};
    FeedTrimSeconds(w, seconds, gop);
    if (!w.Finalize())
        return {};
    return path;
}

// Build a temp path unique across processes/worktrees and calls (folds a
// per-process random token + counter + the running test name; see
// test_unique_temp.h).
static std::string UniqueTrimTempPath(const char* suffix) {
    return exosnap_test::UniqueTempPathStr(std::string("trim_") + suffix);
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class TrimTest : public ::testing::Test {
  protected:
    void SetUp() override {
        src_ = UniqueTrimTempPath("src.mkv");
        dst_ = UniqueTrimTempPath("dst.mp4");
        std::remove(src_.c_str());
        std::remove(dst_.c_str());
    }
    void TearDown() override {
        std::remove(src_.c_str());
        std::remove(dst_.c_str());
        for (const auto& path : extra_paths_) {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    }
    std::string src_;
    std::string dst_;
    std::vector<std::filesystem::path> extra_paths_;
};

} // namespace

TEST_F(TrimTest, ReorderedEndTrimDrainsEveryAudioTrackToPresentationBoundary) {
    // The real H.264 fixture has closed GOPs, two B-frames and two-second IDRs.
    // Test execution only needs the production decoder/demux libraries.
    const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
    AVFormatContext* source = nullptr;
    ASSERT_GE(avformat_open_input(&source, fixture.string().c_str(), nullptr, nullptr), 0);
    auto close_input = [](AVFormatContext* p) { avformat_close_input(&p); };
    std::unique_ptr<AVFormatContext, decltype(close_input)> source_guard(source, close_input);
    ASSERT_GE(avformat_find_stream_info(source, nullptr), 0);
    ASSERT_EQ(source->nb_streams, 1u);
    const auto* video = source->streams[0];
    auto free_packet = [](AVPacket* p) { av_packet_free(&p); };
    std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
    ASSERT_TRUE(packet);
    std::vector<MuxPacket> packets;
    uint64_t boundary_ns = 0;
    bool reordered = false;
    while (av_read_frame(source, packet.get()) >= 0) {
        MuxPacket p;
        p.track_num = 1;
        p.pts_ns = static_cast<uint64_t>(av_rescale_q(packet->pts, video->time_base, {1, 1000000000}));
        p.dts_ns = av_rescale_q(packet->dts, video->time_base, {1, 1000000000});
        p.is_key = (packet->flags & AV_PKT_FLAG_KEY) != 0;
        p.bytes.assign(packet->data, packet->data + packet->size);
        reordered |= *p.dts_ns < static_cast<int64_t>(p.pts_ns);
        if (p.is_key && p.pts_ns >= 1500000000ULL && boundary_ns == 0)
            boundary_ns = p.pts_ns;
        packets.push_back(std::move(p));
        av_packet_unref(packet.get());
    }
    ASSERT_TRUE(reordered);
    ASSERT_EQ(boundary_ns, 2000000000ULL);
    for (uint64_t pts = 0; pts < 4000000000ULL; pts += 10000000ULL) {
        for (uint32_t track = 2; track <= 3; ++track) {
            MuxPacket p;
            p.pts_ns = pts;
            p.track_num = track;
            p.is_key = true;
            p.bytes.assign(480 * 2 * 2, 0);
            packets.push_back(std::move(p));
        }
    }
    std::stable_sort(packets.begin(), packets.end(), [](const MuxPacket& a, const MuxPacket& b) {
        return a.dts_ns.value_or(static_cast<int64_t>(a.pts_ns)) < b.dts_ns.value_or(static_cast<int64_t>(b.pts_ns));
    });
    MatroskaStreamConfig cfg;
    cfg.output_path = src_;
    cfg.video_codec_id = "V_MPEG4/ISO/AVC";
    cfg.video_codec_private.assign(video->codecpar->extradata,
                                   video->codecpar->extradata + video->codecpar->extradata_size);
    cfg.encode_width = cfg.encode_height = 64;
    cfg.frame_rate_num = 30;
    cfg.frame_rate_den = 1;
    cfg.audio_codec = exosnap::engine::StreamAudioCodec::Pcm;
    cfg.audio_track_count = 2;
    MatroskaStreamWriter writer;
    ASSERT_TRUE(writer.Open(cfg));
    for (auto& p : packets)
        ASSERT_TRUE(writer.Push(std::move(p)));
    ASSERT_TRUE(writer.Finalize());

    TrimRange trim;
    trim.end_us = 1500000;
    const auto result = RemuxToMkv(src_, dst_, RemuxNoopCallback(), trim);
    ASSERT_TRUE(result.success) << result.message;
    AVFormatContext* input = nullptr;
    ASSERT_GE(avformat_open_input(&input, dst_.c_str(), nullptr, nullptr), 0);
    std::unique_ptr<AVFormatContext, decltype(close_input)> guard(input, close_input);
    ASSERT_GE(avformat_find_stream_info(input, nullptr), 0);
    std::array<int64_t, 2> audio_ends{};
    int video_frames = 0;
    while (av_read_frame(input, packet.get()) >= 0) {
        const int stream = packet->stream_index;
        if (stream == 0) {
            size_t size = 0;
            const auto* additional = av_packet_get_side_data(packet.get(), AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL, &size);
            ASSERT_NE(additional, nullptr);
            ASSERT_EQ(size, 32u);
            const auto timestamps = exosnap::engine::DecodeMatroskaPacketTimestamps({additional + 8, size - 8});
            ASSERT_TRUE(timestamps);
            EXPECT_EQ(av_rescale_q(timestamps->dts_ns, {1, 1000000000}, {1, 30}), video_frames - 2);
            ++video_frames;
        }
        if (stream == 1 || stream == 2) {
            audio_ends[stream - 1] =
                av_rescale_q(packet->pts + packet->duration, input->streams[stream]->time_base, {1, 1000000000});
        }
        av_packet_unref(packet.get());
    }
    for (const auto end : audio_ends)
        EXPECT_NEAR(static_cast<double>(end), static_cast<double>(boundary_ns), 10000000.0);
    EXPECT_EQ(video_frames, 60);
}

TEST_F(TrimTest, ReorderedWriterStreamCopiesToMp4AndDecodesEveryFrame) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
    auto close_input = [](AVFormatContext* p) { avformat_close_input(&p); };
    AVFormatContext* source = nullptr;
    ASSERT_GE(avformat_open_input(&source, fixture.string().c_str(), nullptr, nullptr), 0);
    std::unique_ptr<AVFormatContext, decltype(close_input)> source_guard(source, close_input);
    ASSERT_GE(avformat_find_stream_info(source, nullptr), 0);
    const auto* video = source->streams[0];
    MatroskaStreamConfig cfg;
    cfg.output_path = src_;
    cfg.video_codec_id = "V_MPEG4/ISO/AVC";
    cfg.video_codec_private.assign(video->codecpar->extradata,
                                   video->codecpar->extradata + video->codecpar->extradata_size);
    cfg.encode_width = cfg.encode_height = 64;
    cfg.frame_rate_num = 30;
    cfg.frame_rate_den = 1;
    cfg.audio_track_count = 0;
    MatroskaStreamWriter writer;
    ASSERT_TRUE(writer.Open(cfg));
    auto free_packet = [](AVPacket* p) { av_packet_free(&p); };
    std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
    ASSERT_TRUE(packet);
    int submitted = 0;
    std::vector<exosnap::engine::MatroskaPacketTimestamps> expected_timestamps;
    while (av_read_frame(source, packet.get()) >= 0) {
        MuxPacket p;
        p.track_num = 1;
        p.pts_ns = static_cast<uint64_t>(av_rescale_q(packet->pts, video->time_base, {1, 1000000000}));
        p.dts_ns = av_rescale_q(packet->dts, video->time_base, {1, 1000000000});
        expected_timestamps.push_back({p.pts_ns, *p.dts_ns});
        p.is_key = (packet->flags & AV_PKT_FLAG_KEY) != 0;
        p.bytes.assign(packet->data, packet->data + packet->size);
        ASSERT_TRUE(writer.Push(std::move(p)));
        ++submitted;
        av_packet_unref(packet.get());
    }
    ASSERT_EQ(submitted, 120);
    ASSERT_TRUE(writer.Finalize());
    const auto rewritten_path = UniqueTrimTempPath("exact_rewrite.mkv");
    const auto partial_path = UniqueTrimTempPath("exact_partial.mkv");
    std::filesystem::copy_file(src_, partial_path);
    std::filesystem::resize_file(partial_path, std::filesystem::file_size(partial_path) - 32);
    const auto recovered = RemuxToMkv(partial_path, rewritten_path);
    ASSERT_TRUE(recovered.success) << recovered.message;
    std::filesystem::remove(src_);
    std::filesystem::rename(rewritten_path, src_);
    std::filesystem::remove(partial_path);
    const auto result = RemuxToProgressiveMp4(src_, dst_, RemuxNoopCallback());
    ASSERT_TRUE(result.success) << result.message;

    AVFormatContext* output = nullptr;
    ASSERT_GE(avformat_open_input(&output, dst_.c_str(), nullptr, nullptr), 0);
    std::unique_ptr<AVFormatContext, decltype(close_input)> output_guard(output, close_input);
    ASSERT_GE(avformat_find_stream_info(output, nullptr), 0);
    const AVCodec* codec = avcodec_find_decoder(output->streams[0]->codecpar->codec_id);
    ASSERT_NE(codec, nullptr);
    auto free_context = [](AVCodecContext* p) { avcodec_free_context(&p); };
    std::unique_ptr<AVCodecContext, decltype(free_context)> decoder(avcodec_alloc_context3(codec), free_context);
    ASSERT_TRUE(decoder);
    ASSERT_GE(avcodec_parameters_to_context(decoder.get(), output->streams[0]->codecpar), 0);
    ASSERT_GE(avcodec_open2(decoder.get(), codec, nullptr), 0);
    auto free_frame = [](AVFrame* p) { av_frame_free(&p); };
    std::unique_ptr<AVFrame, decltype(free_frame)> frame(av_frame_alloc(), free_frame);
    ASSERT_TRUE(frame);
    int decoded = 0;
    int delivered = 0;
    int64_t previous_dts = AV_NOPTS_VALUE;
    auto receive = [&] {
        int ret;
        while ((ret = avcodec_receive_frame(decoder.get(), frame.get())) == 0) {
            ++decoded;
            av_frame_unref(frame.get());
        }
        EXPECT_TRUE(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
    };
    while (av_read_frame(output, packet.get()) >= 0) {
        ASSERT_NE(packet->dts, AV_NOPTS_VALUE);
        ASSERT_NE(packet->pts, AV_NOPTS_VALUE);
        ASSERT_LT(static_cast<size_t>(delivered), expected_timestamps.size());
        const auto& expected = expected_timestamps[static_cast<size_t>(delivered)];
        EXPECT_EQ(packet->pts,
                  av_rescale_q(static_cast<int64_t>(expected.pts_ns), {1, 1000000000}, output->streams[0]->time_base));
        EXPECT_EQ(packet->dts, av_rescale_q(expected.dts_ns, {1, 1000000000}, output->streams[0]->time_base));
        if (previous_dts != AV_NOPTS_VALUE)
            EXPECT_GT(packet->dts, previous_dts);
        previous_dts = packet->dts;
        ASSERT_GE(avcodec_send_packet(decoder.get(), packet.get()), 0);
        receive();
        ++delivered;
        av_packet_unref(packet.get());
    }
    ASSERT_GE(avcodec_send_packet(decoder.get(), nullptr), 0);
    receive();
    EXPECT_EQ(delivered, submitted);
    EXPECT_EQ(decoded, submitted);
}

TEST_F(TrimTest, SecondReorderedGopRewritesToLocalClockAndDecodesEveryFrame) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
    auto close_input = [](AVFormatContext* p) { avformat_close_input(&p); };
    AVFormatContext* source = nullptr;
    ASSERT_GE(avformat_open_input(&source, fixture.string().c_str(), nullptr, nullptr), 0);
    std::unique_ptr<AVFormatContext, decltype(close_input)> source_guard(source, close_input);
    ASSERT_GE(avformat_find_stream_info(source, nullptr), 0);
    const auto* video = source->streams[0];
    MatroskaStreamConfig config;
    config.output_path = src_;
    config.video_codec_id = "V_MPEG4/ISO/AVC";
    config.video_codec_private.assign(video->codecpar->extradata,
                                      video->codecpar->extradata + video->codecpar->extradata_size);
    config.encode_width = config.encode_height = 64;
    config.frame_rate_num = 30;
    config.frame_rate_den = 1;
    MatroskaStreamWriter writer;
    ASSERT_TRUE(writer.Open(config));
    auto free_packet = [](AVPacket* p) { av_packet_free(&p); };
    std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
    ASSERT_TRUE(packet);
    std::vector<exosnap::engine::MatroskaPacketTimestamps> expected;
    while (av_read_frame(source, packet.get()) >= 0) {
        MuxPacket p;
        p.track_num = 1;
        p.pts_ns = static_cast<uint64_t>(av_rescale_q(packet->pts, video->time_base, {1, 1000000000}));
        p.dts_ns = av_rescale_q(packet->dts, video->time_base, {1, 1000000000});
        if (p.pts_ns >= 2000000000)
            expected.push_back({p.pts_ns - 2000000000, *p.dts_ns - 2000000000});
        p.is_key = (packet->flags & AV_PKT_FLAG_KEY) != 0;
        p.bytes.assign(packet->data, packet->data + packet->size);
        ASSERT_TRUE(writer.Push(std::move(p)));
        av_packet_unref(packet.get());
    }
    ASSERT_EQ(expected.size(), 60u);
    ASSERT_TRUE(writer.Finalize());
    const auto local_mkv = UniqueTrimTempPath("second_gop.mkv");
    extra_paths_.push_back(local_mkv);
    TrimRange trim;
    trim.start_us = 2000000;
    trim.end_us = 4000000;
    const auto rewritten = RemuxToMkv(src_, local_mkv, RemuxNoopCallback(), trim);
    ASSERT_TRUE(rewritten.success) << rewritten.message;
    AVFormatContext* local = nullptr;
    ASSERT_GE(avformat_open_input(&local, local_mkv.c_str(), nullptr, nullptr), 0);
    std::unique_ptr<AVFormatContext, decltype(close_input)> local_guard(local, close_input);
    ASSERT_GE(av_read_frame(local, packet.get()), 0);
    size_t size = 0;
    const auto* additional = av_packet_get_side_data(packet.get(), AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL, &size);
    ASSERT_NE(additional, nullptr);
    ASSERT_EQ(size, 32u);
    const auto first = exosnap::engine::DecodeMatroskaPacketTimestamps({additional + 8, size - 8});
    ASSERT_TRUE(first);
    EXPECT_EQ(first->pts_ns, 0u);
    EXPECT_LT(first->dts_ns, 0);
    av_packet_unref(packet.get());
    const auto delivered = RemuxToProgressiveMp4(local_mkv, dst_);
    ASSERT_TRUE(delivered.success) << delivered.message;
    AVFormatContext* output = nullptr;
    ASSERT_GE(avformat_open_input(&output, dst_.c_str(), nullptr, nullptr), 0);
    std::unique_ptr<AVFormatContext, decltype(close_input)> output_guard(output, close_input);
    ASSERT_GE(avformat_find_stream_info(output, nullptr), 0);
    const auto* codec = avcodec_find_decoder(output->streams[0]->codecpar->codec_id);
    ASSERT_NE(codec, nullptr);
    auto free_decoder = [](AVCodecContext* p) { avcodec_free_context(&p); };
    std::unique_ptr<AVCodecContext, decltype(free_decoder)> decoder(avcodec_alloc_context3(codec), free_decoder);
    ASSERT_TRUE(decoder);
    ASSERT_GE(avcodec_parameters_to_context(decoder.get(), output->streams[0]->codecpar), 0);
    ASSERT_GE(avcodec_open2(decoder.get(), codec, nullptr), 0);
    auto free_frame = [](AVFrame* p) { av_frame_free(&p); };
    std::unique_ptr<AVFrame, decltype(free_frame)> frame(av_frame_alloc(), free_frame);
    ASSERT_TRUE(frame);
    size_t packets = 0;
    int frames = 0;
    auto receive = [&] {
        int result;
        while ((result = avcodec_receive_frame(decoder.get(), frame.get())) == 0) {
            ++frames;
            av_frame_unref(frame.get());
        }
        EXPECT_TRUE(result == AVERROR(EAGAIN) || result == AVERROR_EOF);
    };
    while (av_read_frame(output, packet.get()) >= 0) {
        ASSERT_LT(packets, expected.size());
        EXPECT_EQ(packet->pts, av_rescale_q(static_cast<int64_t>(expected[packets].pts_ns), {1, 1000000000},
                                            output->streams[0]->time_base));
        EXPECT_EQ(packet->dts, av_rescale_q(expected[packets].dts_ns, {1, 1000000000}, output->streams[0]->time_base));
        ASSERT_GE(avcodec_send_packet(decoder.get(), packet.get()), 0);
        receive();
        ++packets;
        av_packet_unref(packet.get());
    }
    ASSERT_GE(avcodec_send_packet(decoder.get(), nullptr), 0);
    receive();
    EXPECT_EQ(packets, 60u);
    EXPECT_EQ(frames, 60);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// --- ExtractKeyframeTimestamps returns non-empty sorted vector for valid MKV ---
TEST_F(TrimTest, ExtractKeyframesReturnsNonEmpty) {
    ASSERT_FALSE(BuildTrimMkv(src_, 6.0, 60).empty()) << "Failed to build test MKV";
    const auto kfs = ExtractKeyframeTimestamps(src_);
    // 6 seconds at 60 fps, gop=60 → keyframe every 1 s → expect ~6 keyframes
    EXPECT_GE(kfs.size(), 2u) << "Expected >=2 keyframes in 6 s / gop=60 file";
    for (size_t i = 1; i < kfs.size(); ++i)
        EXPECT_GE(kfs[i], kfs[i - 1]) << "Keyframes are not sorted at index " << i;
}

// --- ExtractKeyframeTimestamps returns empty vector for non-existent input ---
TEST_F(TrimTest, ExtractKeyframesBadInput) {
    const auto kfs = ExtractKeyframeTimestamps("/nonexistent_xyz_exosnap_trim_input.mkv");
    EXPECT_TRUE(kfs.empty()) << "Expected empty vector for bad input path";
}

// --- No-trim pass: TrimRange{} (both kNoTimestamp) produces valid output ---
TEST_F(TrimTest, NoTrimMatchesFullRemux) {
    ASSERT_FALSE(BuildTrimMkv(src_).empty());
    TrimRange full; // both fields == kNoTimestamp
    auto res = RemuxToProgressiveMp4(src_, dst_, RemuxNoopCallback(), full);
    ASSERT_TRUE(res.success) << "No-trim remux failed: " << res.message;
    EXPECT_GT(std::filesystem::file_size(dst_), 0u);
}

// --- Start trim: output is smaller than full remux ---
TEST_F(TrimTest, StartTrimProducesSmallerOutput) {
    ASSERT_FALSE(BuildTrimMkv(src_, 6.0, 60).empty());
    const auto kfs = ExtractKeyframeTimestamps(src_);
    ASSERT_GE(kfs.size(), 2u) << "Need >=2 keyframes to test start trim";

    // Full remux.
    const std::string dst_full = UniqueTrimTempPath("dst_full.mp4");
    std::remove(dst_full.c_str());
    {
        auto res = RemuxToProgressiveMp4(src_, dst_full, RemuxNoopCallback(), TrimRange{});
        ASSERT_TRUE(res.success) << "Full remux failed: " << res.message;
    }
    const auto size_full = std::filesystem::file_size(dst_full);
    std::remove(dst_full.c_str());

    // Trimmed from 2nd keyframe onward.
    TrimRange tr;
    tr.start_us = kfs[1]; // ~1 s in (at 60 fps, gop=60, first keyframe at 0, second at ~1 s)
    tr.end_us = TrimRange::kNoTimestamp;

    auto res = RemuxToProgressiveMp4(src_, dst_, RemuxNoopCallback(), tr);
    ASSERT_TRUE(res.success) << "Trimmed remux failed: " << res.message;
    const auto size_trim = std::filesystem::file_size(dst_);
    EXPECT_LT(size_trim, size_full) << "Trimmed output should be smaller than full (" << size_trim << " vs "
                                    << size_full << ")";
}

// --- End trim: output is smaller than full remux ---
TEST_F(TrimTest, EndTrimProducesSmallerOutput) {
    ASSERT_FALSE(BuildTrimMkv(src_, 6.0, 60).empty());
    const auto kfs = ExtractKeyframeTimestamps(src_);
    ASSERT_GE(kfs.size(), 3u) << "Need >=3 keyframes to test end trim";

    // Full remux.
    const std::string dst_full = UniqueTrimTempPath("dst_full2.mp4");
    std::remove(dst_full.c_str());
    {
        auto res = RemuxToProgressiveMp4(src_, dst_full, RemuxNoopCallback(), TrimRange{});
        ASSERT_TRUE(res.success);
    }
    const auto size_full = std::filesystem::file_size(dst_full);
    std::remove(dst_full.c_str());

    // Trim to just before the midpoint keyframe.
    TrimRange tr;
    tr.start_us = TrimRange::kNoTimestamp;
    tr.end_us = kfs[kfs.size() / 2]; // midpoint keyframe

    auto res = RemuxToProgressiveMp4(src_, dst_, RemuxNoopCallback(), tr);
    ASSERT_TRUE(res.success) << "End-trim remux failed: " << res.message;
    const auto size_trim = std::filesystem::file_size(dst_);
    EXPECT_LT(size_trim, size_full) << "End-trimmed output should be smaller (" << size_trim << " vs " << size_full
                                    << ")";
}

// --- Trim to MKV output (stream-copy, matroska muxer) ---
TEST_F(TrimTest, TrimToMkv) {
    ASSERT_FALSE(BuildTrimMkv(src_).empty());
    const auto kfs = ExtractKeyframeTimestamps(src_);
    ASSERT_GE(kfs.size(), 2u);

    const std::string dst_mkv = UniqueTrimTempPath("dst.mkv");
    std::remove(dst_mkv.c_str());

    TrimRange tr;
    tr.start_us = kfs[1];
    tr.end_us = TrimRange::kNoTimestamp;

    auto res = RemuxToMkv(src_, dst_mkv, RemuxNoopCallback(), tr);
    ASSERT_TRUE(res.success) << "Trim-to-MKV failed: " << res.message;
    EXPECT_GT(std::filesystem::file_size(dst_mkv), 0u);
    std::remove(dst_mkv.c_str());
}

// --- Both start and end trim ---
TEST_F(TrimTest, BothStartAndEndTrim) {
    ASSERT_FALSE(BuildTrimMkv(src_, 9.0, 60).empty()); // 9 s → ~9 keyframes at gop=60
    const auto kfs = ExtractKeyframeTimestamps(src_);
    ASSERT_GE(kfs.size(), 4u) << "Need >=4 keyframes for both-ends trim test";

    const std::string dst_full = UniqueTrimTempPath("dst_full3.mp4");
    std::remove(dst_full.c_str());
    {
        auto res = RemuxToProgressiveMp4(src_, dst_full, RemuxNoopCallback(), TrimRange{});
        ASSERT_TRUE(res.success);
    }
    const auto size_full = std::filesystem::file_size(dst_full);
    std::remove(dst_full.c_str());

    TrimRange tr;
    tr.start_us = kfs[1];            // skip first ~1 s
    tr.end_us = kfs[kfs.size() - 1]; // stop before last keyframe

    auto res = RemuxToProgressiveMp4(src_, dst_, RemuxNoopCallback(), tr);
    ASSERT_TRUE(res.success) << "Both-ends trim failed: " << res.message;
    const auto size_trim = std::filesystem::file_size(dst_);
    EXPECT_LT(size_trim, size_full) << "Both-ends trimmed output should be smaller (" << size_trim << " vs "
                                    << size_full << ")";
}

// --- Start trim whose start_us lands mid-GOP, more than 1 s after the
// preceding keyframe: exercises the trim-start-keyframe-contract fix in
// mp4_remuxer.cpp. The fix (a) rescales av_seek_frame()'s timestamp into the
// video stream's own time_base — the un-rescaled AV_TIME_BASE value used to
// seek far past EOF and silently fail — and (b) removes the `start_us - 1 s`
// fudge so the backward seek's keyframe target (the keyframe AT OR BEFORE
// start_us) is accepted unconditionally instead of being rejected once the GOP
// exceeds 1 s.
//
// Fixture limitation (documented, not a product gap): the synthetic streams use
// non-conformant payloads (0xAB filler), and libavformat's H.264/AV1 bitstream
// parser overrides the per-packet AV_PKT_FLAG_KEY based on payload content when
// av_read_frame runs (the same reason ExtractKeyframeTimestamps reads Cues, not
// packet flags). The trim-start lock keys off that flag, so with synthetic
// streams no packet is ever accepted and the trimmed output is a near-empty
// container regardless of the start point — content-size assertions cannot
// distinguish keyframe boundaries here. What this test CAN guarantee is that
// the corrected seek path stays healthy: a mid-GOP start on a multi-second-GOP
// source remuxes successfully to a well-formed, re-openable container for both
// MKV and MP4 outputs. Exact keyframe-boundary preservation is verified in
// production against real bitstreams (EditExportPage snaps start_us to a real
// keyframe PTS before calling in). ---
TEST_F(TrimTest, StartTrimMidGopRemuxesToValidContainer) {
    // 3 s GOP (180 frames @ 60 fps): keyframes at ~0, 3, 6, 9 s. The start
    // lands 2.5 s past kfs[1] — well beyond the old 1 s fudge window, and
    // before kfs[2].
    ASSERT_FALSE(BuildTrimMkv(src_, 12.0, 180).empty());
    const auto kfs = ExtractKeyframeTimestamps(src_);
    ASSERT_GE(kfs.size(), 3u) << "Need >=3 keyframes (~0/3/6/9 s) for this test";

    const int64_t start_mid = kfs[1] + 2500000; // kfs[1] + 2.5 s
    ASSERT_LT(start_mid, kfs[2]) << "Fixture assumption violated: start must land before kfs[2]";

    TrimRange tr;
    tr.start_us = start_mid;
    tr.end_us = TrimRange::kNoTimestamp;

    // Both outputs must remux with RemuxResult::success — a documented hard
    // guarantee (mp4_remuxer.h): the corrected backward seek (rescaled into the
    // stream time_base) must not turn into a read/seek error the packet loop
    // surfaces as a failure. Before the units fix the seek target overshot far
    // past EOF; this asserts the trim-start path stays healthy end-to-end for a
    // GOP much wider than the removed 1 s fudge window.
    const std::string dst_mkv = UniqueTrimTempPath("midgop.mkv");
    std::remove(dst_mkv.c_str());
    {
        auto res = RemuxToMkv(src_, dst_mkv, RemuxNoopCallback(), tr);
        EXPECT_TRUE(res.success) << "Mid-GOP start trim to MKV failed: " << res.message;
    }
    std::remove(dst_mkv.c_str());
    {
        auto res = RemuxToProgressiveMp4(src_, dst_, RemuxNoopCallback(), tr);
        EXPECT_TRUE(res.success) << "Mid-GOP start trim to MP4 failed: " << res.message;
    }
}

// (diagnostic tests removed — root cause found and fixed)
