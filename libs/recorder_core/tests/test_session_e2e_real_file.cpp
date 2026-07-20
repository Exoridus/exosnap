// test_session_e2e_real_file.cpp — end-to-end "a stranger can play it" proof.
//
// Every other recorder_core test either drives a single component in isolation
// or hands the Matroska writer packets that were never produced by the real
// encode/mux pipeline. This test closes that gap: it runs the ACTUAL session
// worker components — the real AudioThread (real libopus / FFmpeg AAC encoder),
// the real MuxThread (codec-private readiness handshake, pre-mux buffering, the
// bounded mux queue, A/V epoch alignment, the reorder window, per-keyframe Cues,
// SeekHead, back-patched Duration, Finalize) — and lets them write a real file
// to disk. The file is then validated WITHOUT any of ExoSnap's own reader code,
// using the vendored libavformat (the "foreign player"): open, read stream info,
// demux every packet to a clean EOF, and assert the container's contents.
//
// What is real here vs. what is a test seam:
//   * REAL: AudioThread + its encoder, MuxThread + MatroskaStreamWriter, the
//           whole SessionState handshake, Finalize, and (for the MP4 case) the
//           real libavformat remuxer.
//   * SEAM: the GPU video path (WGC capture + NVENC) cannot run on a GPU-less CI
//           runner, so recorder_core_testutil's SyntheticSession plays
//           VideoThread's SessionState role — the same feeder, now extracted so
//           the soak twin and the recovery drills reuse it. The muxer, writer,
//           and validator never decode a frame, so synthetic-but-well-formed
//           packets exercise the exact container path a real recording takes.
//
// No `live` label: everything here is deterministic and GPU-/device-free, so it
// runs on the headless CI runner alongside the rest of the suite.

extern "C" {
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include <gtest/gtest.h>

#include "recorder_core/mp4_remuxer.h"
#include "synthetic_session.h"
#include "test_unique_temp.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using recorder_core::AudioCodec;
using recorder_core::VideoCodec;

static inline const char* AvErr(int errnum) noexcept {
    static thread_local char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, buf, sizeof(buf));
    return buf;
}

// ---------------------------------------------------------------------------
// Facts read back from a file by libavformat alone (no ExoSnap reader code).
// ---------------------------------------------------------------------------
struct DemuxFacts {
    bool open_ok = false;
    bool stream_info_ok = false;
    bool clean_eof = false;
    bool pts_monotonic = true;
    unsigned nb_streams = 0;
    unsigned video_streams = 0;
    unsigned audio_streams = 0;
    int total_packets = 0;
    int video_packets = 0;
    int audio_packets = 0;
    int video_keyframes = 0;
    double max_end_seconds = 0.0;
    std::string message;
};

DemuxFacts DemuxAndInspect(const std::string& path) {
    DemuxFacts f;

    // The video packets are deliberately synthetic (the muxer/demuxer never
    // decode a frame), so libav's elementary-stream parsers log expected
    // complaints. Silence them: correctness here is judged by return codes and
    // demuxed packet metadata, not by decodability.
    av_log_set_level(AV_LOG_QUIET);

    AVFormatContext* ctx = nullptr;
    int ret = avformat_open_input(&ctx, path.c_str(), nullptr, nullptr);
    if (ret != 0) {
        f.message = std::string("avformat_open_input: ") + AvErr(ret);
        return f;
    }
    f.open_ok = true;

    ret = avformat_find_stream_info(ctx, nullptr);
    f.stream_info_ok = (ret >= 0);
    if (!f.stream_info_ok)
        f.message = std::string("avformat_find_stream_info: ") + AvErr(ret);

    f.nb_streams = ctx->nb_streams;
    std::vector<int64_t> last_dts(ctx->nb_streams, INT64_MIN);
    for (unsigned i = 0; i < ctx->nb_streams; ++i) {
        const auto type = ctx->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO)
            ++f.video_streams;
        else if (type == AVMEDIA_TYPE_AUDIO)
            ++f.audio_streams;
    }

    AVPacket* pkt = av_packet_alloc();
    for (;;) {
        ret = av_read_frame(ctx, pkt);
        if (ret < 0)
            break;

        const unsigned si = static_cast<unsigned>(pkt->stream_index);
        AVStream* st = ctx->streams[si];
        const bool is_video = st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
        const bool is_audio = st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO;

        ++f.total_packets;
        if (is_video)
            ++f.video_packets;
        if (is_audio)
            ++f.audio_packets;
        if (is_video && (pkt->flags & AV_PKT_FLAG_KEY))
            ++f.video_keyframes;

        // Monotonic decode timestamps per stream (fall back to PTS).
        const int64_t ts = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : pkt->pts;
        if (ts != AV_NOPTS_VALUE) {
            if (last_dts[si] != INT64_MIN && ts < last_dts[si])
                f.pts_monotonic = false;
            last_dts[si] = ts;

            const int64_t end = (pkt->pts != AV_NOPTS_VALUE ? pkt->pts : ts) + (pkt->duration > 0 ? pkt->duration : 0);
            const double end_s = static_cast<double>(end) * av_q2d(st->time_base);
            if (end_s > f.max_end_seconds)
                f.max_end_seconds = end_s;
        }
        av_packet_unref(pkt);
    }
    f.clean_eof = (ret == AVERROR_EOF);
    if (!f.clean_eof && f.message.empty())
        f.message = std::string("av_read_frame ended with: ") + AvErr(ret);

    av_packet_free(&pkt);
    avformat_close_input(&ctx);
    return f;
}

// ---------------------------------------------------------------------------
// Run the real audio+mux pipeline to a real MKV on disk via the shared synthetic
// session seam. video_codec selects the video track codec; audio_codec selects
// the audio encoder actually instantiated inside AudioThread. Returns true on a
// clean finalize with no recorded session failure.
// ---------------------------------------------------------------------------
bool RunRealPipelineToMkv(VideoCodec video_codec, AudioCodec audio_codec, const std::string& mkv_path,
                          double target_seconds, std::string& out_error) {
    recorder_core::testutil::SyntheticSessionConfig cfg;
    cfg.video_codec = video_codec;
    cfg.audio_codec = audio_codec;
    cfg.output_path = mkv_path;
    cfg.target_seconds = target_seconds;
    const auto r = recorder_core::testutil::SyntheticSession(cfg).Run();
    if (!r.success)
        out_error = r.error;
    return r.success;
}

// ---------------------------------------------------------------------------
// Fixture: unique temp paths, cleaned up after each case.
// ---------------------------------------------------------------------------
class SessionE2ETest : public ::testing::Test {
  protected:
    void TearDown() override {
        for (const auto& p : cleanup_) {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
    }
    std::string TempPath(const char* suffix) {
        auto p = exosnap_test::UniqueTempPathStr(suffix);
        cleanup_.push_back(p);
        return p;
    }
    std::vector<std::string> cleanup_;
};

// ---------------------------------------------------------------------------
// The flagship E2E: real pipeline -> H.264 + AAC MKV, validated by libavformat,
// then remuxed to a progressive MP4 by the real remuxer and validated again.
// ---------------------------------------------------------------------------
TEST_F(SessionE2ETest, RealPipeline_H264Aac_ProducesPlayableMkvAndMp4) {
    const std::string mkv = TempPath("e2e_h264aac.mkv");
    constexpr double kSeconds = 2.0;

    std::string err;
    ASSERT_TRUE(RunRealPipelineToMkv(VideoCodec::H264Nvenc, AudioCodec::AacMf, mkv, kSeconds, err))
        << "pipeline failed: " << err;

    // --- Foreign-player validation of the MKV ---
    const DemuxFacts mkvf = DemuxAndInspect(mkv);
    ASSERT_TRUE(mkvf.open_ok) << mkvf.message;
    EXPECT_TRUE(mkvf.stream_info_ok) << mkvf.message;
    EXPECT_TRUE(mkvf.clean_eof) << "demux did not reach a clean EOF: " << mkvf.message;
    EXPECT_EQ(mkvf.nb_streams, 2u);
    EXPECT_EQ(mkvf.video_streams, 1u);
    EXPECT_EQ(mkvf.audio_streams, 1u);
    EXPECT_GT(mkvf.video_packets, 0);
    EXPECT_GT(mkvf.audio_packets, 0);
    EXPECT_GE(mkvf.video_keyframes, 1) << "no video keyframe -> no Cues index";
    EXPECT_TRUE(mkvf.pts_monotonic) << "packet timestamps went backwards";
    EXPECT_GE(mkvf.max_end_seconds, kSeconds - 0.5);
    EXPECT_LE(mkvf.max_end_seconds, kSeconds + 0.5);

    // --- Second stage: real MKV -> progressive MP4 remux, validated again ---
    const std::string mp4 = TempPath("e2e_h264aac.mp4");
    const auto rr = recorder_core::RemuxToProgressiveMp4(mkv, mp4);
    ASSERT_TRUE(rr.success) << "remux failed: " << rr.message << " (av_err=" << rr.av_error_code << ")";

    const DemuxFacts mp4f = DemuxAndInspect(mp4);
    ASSERT_TRUE(mp4f.open_ok) << mp4f.message;
    EXPECT_TRUE(mp4f.stream_info_ok) << mp4f.message;
    EXPECT_TRUE(mp4f.clean_eof) << mp4f.message;
    EXPECT_EQ(mp4f.video_streams, 1u);
    EXPECT_EQ(mp4f.audio_streams, 1u);
    EXPECT_GT(mp4f.total_packets, 0);
    EXPECT_TRUE(mp4f.pts_monotonic);
}

// ---------------------------------------------------------------------------
// The shipped default profile end-to-end: real pipeline -> AV1 + Opus MKV
// (MKV + AV1 + Opus is ExoSnap's default), validated by libavformat. Exercises
// the real libopus encoder and the no-conversion AV1 mux path.
// ---------------------------------------------------------------------------
TEST_F(SessionE2ETest, RealPipeline_Av1Opus_ProducesPlayableMkv) {
    const std::string mkv = TempPath("e2e_av1opus.mkv");
    constexpr double kSeconds = 2.0;

    std::string err;
    ASSERT_TRUE(RunRealPipelineToMkv(VideoCodec::Av1Nvenc, AudioCodec::Opus, mkv, kSeconds, err))
        << "pipeline failed: " << err;

    const DemuxFacts f = DemuxAndInspect(mkv);
    ASSERT_TRUE(f.open_ok) << f.message;
    EXPECT_TRUE(f.clean_eof) << "demux did not reach a clean EOF: " << f.message;
    EXPECT_EQ(f.nb_streams, 2u);
    EXPECT_EQ(f.video_streams, 1u);
    EXPECT_EQ(f.audio_streams, 1u);
    EXPECT_GT(f.video_packets, 0);
    EXPECT_GT(f.audio_packets, 0);
    EXPECT_GE(f.video_keyframes, 1);
    EXPECT_TRUE(f.pts_monotonic);
    EXPECT_GE(f.max_end_seconds, kSeconds - 0.5);
    EXPECT_LE(f.max_end_seconds, kSeconds + 0.5);
}

} // namespace
