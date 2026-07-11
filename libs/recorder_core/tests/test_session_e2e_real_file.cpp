// test_session_e2e_real_file.cpp — end-to-end "a stranger can play it" proof.
//
// Every other recorder_core test either drives a single component in isolation
// or hands the Matroska writer packets that were never produced by the real
// encode/mux pipeline. This test closes that gap: it runs the ACTUAL session
// worker components — the real AudioThread (real libopus / libfdk-aac encoder),
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
//           runner, so a small in-test "video feeder" plays VideoThread's role
//           on the SessionState contract: it publishes the video codec-private +
//           ready flag, the encode dimensions, the video epoch, then feeds
//           deterministic encoded video packets (Annex-B for H.264) and the
//           VideoEos sentinel. The muxer, writer, and validator never decode a
//           frame, so synthetic-but-well-formed packets exercise the exact
//           container path a real recording takes.
//
// No `live` label: everything here is deterministic and GPU-/device-free, so it
// runs on the headless CI runner alongside the rest of the suite.

extern "C" {
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include <gtest/gtest.h>

#include "audio_thread.h"
#include "mux_thread.h"
#include "session_internal.h"

#include "recorder_core/mp4_remuxer.h"
#include "test_unique_temp.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

using recorder_core::AudioCodec;
using recorder_core::AudioSampleFormat;
using recorder_core::AudioThread;
using recorder_core::Container;
using recorder_core::EncodedVideoPacket;
using recorder_core::IAudioCaptureSource;
using recorder_core::MuxItem;
using recorder_core::MuxThread;
using recorder_core::RawAudioBuffer;
using recorder_core::SessionState;
using recorder_core::VideoCodec;
using recorder_core::VideoEosSentinel;

static inline const char* AvErr(int errnum) noexcept {
    static thread_local char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, buf, sizeof(buf));
    return buf;
}

// ---------------------------------------------------------------------------
// Deterministic mock capture source: a fixed number of non-silent 48 kHz stereo
// float packets. Sets stop_requested once drained (mirrors how a real source's
// end-of-stream ultimately stops the session). This is the ONLY audio input;
// everything downstream (encode, mux, finalize) is production code.
// ---------------------------------------------------------------------------
class MockAudioCaptureSource : public IAudioCaptureSource {
  public:
    MockAudioCaptureSource(std::atomic<bool>* stop_requested, size_t packet_count) : stop_requested_(stop_requested) {
        packets_.resize(packet_count);
        for (auto& p : packets_) {
            p.assign(static_cast<size_t>(kFramesPerPacket) * kChannels, 0.1f);
        }
    }

    bool Init(std::string& out_error) override {
        initialized_ = true;
        out_error.clear();
        return true;
    }

    uint32_t PendingFrameCount() override {
        if (!initialized_ || acquired_)
            return 0;
        if (next_ < packets_.size())
            return kFramesPerPacket;
        if (stop_requested_)
            stop_requested_->store(true);
        return 0;
    }

    bool AcquireBuffer(RawAudioBuffer& out_buf, std::string& out_error) override {
        out_buf = {};
        if (!initialized_ || acquired_ || next_ >= packets_.size()) {
            out_error.clear();
            return false;
        }
        acquired_ = true;
        out_buf.bytes = reinterpret_cast<const uint8_t*>(packets_[next_].data());
        out_buf.num_frames = kFramesPerPacket;
        out_buf.silent = false;
        out_error.clear();
        return true;
    }

    void ReleaseBuffer() override {
        if (!acquired_)
            return;
        acquired_ = false;
        ++next_;
        if (next_ >= packets_.size() && stop_requested_)
            stop_requested_->store(true);
    }

    uint32_t SampleRate() const override {
        return kSampleRate;
    }
    uint32_t Channels() const override {
        return kChannels;
    }
    AudioSampleFormat SampleFormat() const override {
        return AudioSampleFormat::Float32;
    }
    const std::string& EndpointName() const override {
        return endpoint_;
    }
    void Shutdown() override {
    }

  private:
    static constexpr uint32_t kSampleRate = 48000;
    static constexpr uint32_t kChannels = 2;
    static constexpr uint32_t kFramesPerPacket = 960; // 20 ms

    std::atomic<bool>* stop_requested_ = nullptr;
    bool initialized_ = false;
    bool acquired_ = false;
    size_t next_ = 0;
    std::vector<std::vector<float>> packets_;
    std::string endpoint_ = "MockCapture";
};

// A minimal, structurally valid Annex-B SPS+PPS pair (Baseline profile 66 so the
// avcC carries no chroma extension). BuildAvccFromAnnexBSpsAndPps (the real mux
// path) turns this into the avcC written to the MKV video track.
std::vector<uint8_t> FakeH264AnnexbSpsPps() {
    return {
        0x00, 0x00, 0x00, 0x01,             // start code
        0x67, 0x42, 0x00, 0x1F, 0xAC, 0xD9, // SPS NAL (type 7, profile 66, level 31)
        0x00, 0x00, 0x00, 0x01,             // start code
        0x68, 0xCE, 0x3C, 0x80,             // PPS NAL (type 8)
    };
}

// One synthetic Annex-B access unit. keyframe -> IDR slice (0x65); otherwise a
// non-IDR slice (0x41). MuxThread's ConvertAnnexBToAvcc length-prefixes it.
std::vector<uint8_t> MakeH264AnnexbAu(bool keyframe, size_t payload_bytes) {
    std::vector<uint8_t> au = {0x00, 0x00, 0x00, 0x01, static_cast<uint8_t>(keyframe ? 0x65 : 0x41)};
    au.insert(au.end(), payload_bytes, 0xAB);
    return au;
}

// A 4-byte AV1CodecConfigurationRecord stub (marker+version, seq_profile/level,
// flags). MuxThread copies it verbatim into the V_AV1 track's CodecPrivate.
std::vector<uint8_t> FakeAv1CodecPrivate() {
    return {0x81, 0x0C, 0x00, 0x00};
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
// Run the real audio+mux pipeline to a real MKV on disk.
//
// video_codec selects the video track codec; audio_codec selects the audio
// encoder actually instantiated inside AudioThread. Returns true on a clean
// finalize with no recorded session failure.
// ---------------------------------------------------------------------------
bool RunRealPipelineToMkv(VideoCodec video_codec, AudioCodec audio_codec, const std::string& mkv_path,
                          double target_seconds, std::string& out_error) {
    auto state_ptr = std::make_shared<SessionState>();
    SessionState& state = *state_ptr;

    // --- Session config the workers read (as RecorderSession::Record would set) ---
    state.config.output_path = mkv_path;
    state.config.container = Container::Matroska;
    state.config.video_codec = video_codec;
    state.config.audio_codec = audio_codec;
    state.config.audio_channels = 2;
    state.config.audio_sample_rate = 48000;
    state.config.audio_bit_depth = 16;
    state.config.frame_rate_num = 60;
    state.config.frame_rate_den = 1;
    state.audio_track_count = 1;

    // Epoch: make the video epoch equal to the session start so head_start_ns==0
    // (audio is rebased to itself, nothing dropped) — the exact alignment a real
    // session resolves once the first captured frame arrives.
    state.session_start_qpc_100ns = 1000;
    state.video_epoch_qpc_100ns.store(1000);

    const uint32_t kWidth = 1280;
    const uint32_t kHeight = 720;
    {
        std::lock_guard lk(state.stats_mutex);
        state.encode_width = kWidth;
        state.encode_height = kHeight;
    }

    // --- Audio: real AudioThread with a deterministic mock capture source ---
    const size_t audio_packets = static_cast<size_t>(target_seconds * 50.0); // 20 ms packets
    auto source = std::make_unique<MockAudioCaptureSource>(&state.stop_requested, audio_packets);
    auto audio_thread = std::make_shared<AudioThread>(state_ptr, std::move(source), /*track_id=*/0);

    // --- Mux: the real MuxThread + MatroskaStreamWriter + Finalize ---
    auto mux_thread = std::make_shared<MuxThread>(state_ptr);

    // --- Video feeder: plays VideoThread's role on the SessionState contract ---
    const uint32_t kFps = 60;
    const uint32_t kGop = 30;
    const uint64_t frame_dur_ns = 1000000000ULL / kFps;
    const uint32_t frame_count = static_cast<uint32_t>(target_seconds * kFps);
    const bool is_h264 = (video_codec == VideoCodec::H264Nvenc);

    std::thread video_feeder([&, state_ptr] {
        SessionState& st = *state_ptr;

        // Publish video codec-private + ready flag (mirrors VideoThread's prepare).
        {
            std::lock_guard lk(st.premux_mutex);
            if (is_h264) {
                st.codec_private.h264_sps_pps = FakeH264AnnexbSpsPps();
                st.codec_private.h264_ready = true;
            } else {
                const auto cp = FakeAv1CodecPrivate();
                std::copy(cp.begin(), cp.end(), st.codec_private.av1_codec_private);
                st.codec_private.av1_ready = true;
            }
            st.premux_cv.notify_all();
        }

        auto route_video = [&](EncodedVideoPacket&& pkt) -> bool {
            std::unique_lock lk(st.premux_mutex);
            const bool both_ready = st.codec_private.VideoReady(st.config.video_codec) &&
                                    st.codec_private.AudioAllReady(st.audio_track_count);
            if (!both_ready) {
                if (st.video_premux.size() >= SessionState::kVideoPremuxLimit) {
                    lk.unlock();
                    st.RecordFailure(E_OUTOFMEMORY, recorder_core::ErrorPhase::Mux, "video pre-mux overflow (test)");
                    return false;
                }
                st.video_premux.push_back(std::move(pkt));
                return true;
            }
            lk.unlock();
            MuxItem mi;
            mi.payload = std::move(pkt);
            std::unique_lock mlk(st.mux_mutex);
            if (!st.WaitForMuxQueueSpace(mlk)) {
                mlk.unlock();
                st.RecordFailure(E_OUTOFMEMORY, recorder_core::ErrorPhase::Mux, "mux queue overflow (test)");
                return false;
            }
            st.PushMuxItemLocked(std::move(mi));
            return true;
        };

        for (uint32_t i = 0; i < frame_count; ++i) {
            if (st.HasFailure())
                break;
            EncodedVideoPacket vp;
            vp.pts_ns = static_cast<uint64_t>(i) * frame_dur_ns;
            vp.keyframe = (i % kGop == 0);
            vp.bytes = is_h264 ? MakeH264AnnexbAu(vp.keyframe, /*payload_bytes=*/256)
                               : std::vector<uint8_t>(256, static_cast<uint8_t>(0xAB));
            if (!route_video(std::move(vp)))
                break;
        }

        // Video EOS sentinel (bypasses the queue bound).
        MuxItem eos;
        eos.payload = VideoEosSentinel{};
        std::lock_guard lk(st.mux_mutex);
        st.PushMuxItemLocked(std::move(eos));
    });

    // Start the real workers.
    mux_thread->Start();
    audio_thread->Start();

    video_feeder.join();
    const bool audio_joined = audio_thread->Join(30000);
    const bool mux_joined = mux_thread->Join(60000);

    if (!audio_joined) {
        out_error = "audio thread did not join";
        return false;
    }
    if (!mux_joined) {
        out_error = "mux thread did not join (finalize hang)";
        return false;
    }
    if (state.HasFailure()) {
        std::lock_guard lk(state.failure_mutex);
        out_error = "session failure: " + state.failure.error_detail;
        return false;
    }
    if (!std::filesystem::exists(mkv_path) || std::filesystem::file_size(mkv_path) == 0) {
        out_error = "output file missing or empty";
        return false;
    }
    return true;
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
