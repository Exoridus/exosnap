#include "synthetic_session.h"

#include "audio_thread.h"
#include "mux_thread.h"
#include "session_internal.h"
#include "session_stats_collector.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

namespace recorder_core::testutil {

namespace {

// Deterministic mock capture source: a fixed number of non-silent 48 kHz stereo
// float packets. Sets stop_requested once drained. (Lifted verbatim from
// test_session_e2e_real_file.cpp so behaviour is identical.)
class MockAudioCaptureSource : public IAudioCaptureSource {
  public:
    MockAudioCaptureSource(std::atomic<bool>* stop_requested, size_t packet_count) : stop_requested_(stop_requested) {
        packets_.resize(packet_count);
        for (auto& p : packets_)
            p.assign(static_cast<size_t>(kFramesPerPacket) * kChannels, 0.1f);
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
    std::string endpoint_ = "SyntheticCapture";
};

std::vector<uint8_t> FakeH264AnnexbSpsPps() {
    return {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1F, 0xAC, 0xD9, 0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80};
}
std::vector<uint8_t> MakeH264AnnexbAu(bool keyframe, size_t payload_bytes) {
    std::vector<uint8_t> au = {0x00, 0x00, 0x00, 0x01, static_cast<uint8_t>(keyframe ? 0x65 : 0x41)};
    au.insert(au.end(), payload_bytes, 0xAB);
    return au;
}
std::vector<uint8_t> FakeAv1CodecPrivate() {
    return {0x81, 0x0C, 0x00, 0x00};
}

} // namespace

struct SyntheticSession::Impl {
    SyntheticSessionConfig cfg;
    std::shared_ptr<SessionState> state = std::make_shared<SessionState>();
    StatsCallback stats_cb;
    DiagnosticsCallback diag_cb;
};

SyntheticSession::SyntheticSession(SyntheticSessionConfig config) : impl_(std::make_unique<Impl>()) {
    impl_->cfg = std::move(config);
}

SyntheticSession::~SyntheticSession() = default;

void SyntheticSession::SetStatsCallback(StatsCallback cb) {
    impl_->stats_cb = std::move(cb);
}
void SyntheticSession::SetDiagnosticsCallback(DiagnosticsCallback cb) {
    impl_->diag_cb = std::move(cb);
}

void SyntheticSession::RequestStop() {
    if (impl_ && impl_->state)
        impl_->state->stop_requested.store(true);
}

SyntheticSessionResult SyntheticSession::Run() {
    SyntheticSessionResult result;
    const SyntheticSessionConfig& cfg = impl_->cfg;
    auto state_ptr = impl_->state;
    SessionState& state = *state_ptr;

    state.config.output_path = cfg.output_path;
    state.config.container = Container::Matroska;
    state.config.video_codec = cfg.video_codec;
    state.config.audio_codec = cfg.audio_codec;
    state.config.audio_channels = 2;
    state.config.audio_sample_rate = 48000;
    state.config.audio_bit_depth = 16;
    state.config.frame_rate_num = static_cast<int>(cfg.fps);
    state.config.frame_rate_den = 1;
    state.audio_track_count = 1;

    state.session_start_qpc_100ns = 1000;
    state.video_epoch_qpc_100ns.store(1000);
    {
        std::lock_guard lk(state.stats_mutex);
        state.encode_width = cfg.width;
        state.encode_height = cfg.height;
    }

    // Wire the public callbacks through the shared state so a self-owned
    // SessionStatsCollector (below) delivers them — the same lifecycle Record()
    // uses, but without a RecorderSession.
    if (cfg.drive_stats_collector) {
        state.stats_callback = impl_->stats_cb;
        state.diagnostics_callback = impl_->diag_cb;
    }

    const size_t audio_packets = static_cast<size_t>(cfg.target_seconds * 50.0); // 20 ms packets
    auto source = std::make_unique<MockAudioCaptureSource>(&state.stop_requested, audio_packets);
    auto audio_thread = std::make_shared<AudioThread>(state_ptr, std::move(source), /*track_id=*/0);
    auto mux_thread = std::make_shared<MuxThread>(state_ptr);

    const uint64_t frame_dur_ns = 1000000000ULL / cfg.fps;
    const uint32_t frame_count = static_cast<uint32_t>(cfg.target_seconds * cfg.fps);
    const bool is_h264 = (cfg.video_codec == VideoCodec::H264);
    const bool realtime = cfg.realtime_pace;

    std::thread video_feeder([&, state_ptr] {
        SessionState& st = *state_ptr;
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

        // A real 60 fps capture delivers frames slowly enough (~16 ms apart) that
        // the audio codec-private is ready long before the video pre-mux buffer
        // (120 packets) fills. The as-fast-as-possible feeder has no such pacing,
        // so for anything past ~2 s it would overrun the buffer before AudioThread
        // publishes its codec-private. Wait for audio readiness first (it is
        // independent of video and arrives within ms) so the pre-mux never overflows.
        {
            std::unique_lock lk(st.premux_mutex);
            st.premux_cv.wait_for(lk, std::chrono::seconds(15), [&] {
                return st.codec_private.AudioAllReady(st.audio_track_count) || st.HasFailure() ||
                       st.stop_requested.load();
            });
        }

        auto route_video = [&](EncodedVideoPacket&& pkt) -> bool {
            std::unique_lock lk(st.premux_mutex);
            const bool both_ready = st.codec_private.VideoReady(st.config.video_codec) &&
                                    st.codec_private.AudioAllReady(st.audio_track_count);
            if (!both_ready) {
                if (st.video_premux.size() >= SessionState::kVideoPremuxLimit) {
                    lk.unlock();
                    st.RecordFailure(E_OUTOFMEMORY, ErrorPhase::Mux, "video pre-mux overflow (synthetic)");
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
                st.RecordFailure(E_OUTOFMEMORY, ErrorPhase::Mux, "mux queue overflow (synthetic)");
                return false;
            }
            st.PushMuxItemLocked(std::move(mi));
            return true;
        };

        const auto wall_start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < frame_count; ++i) {
            if (st.HasFailure() || st.stop_requested.load())
                break;
            if (realtime) {
                const auto target = wall_start + std::chrono::nanoseconds(static_cast<int64_t>(i) * frame_dur_ns);
                std::this_thread::sleep_until(target);
            }
            EncodedVideoPacket vp;
            vp.pts_ns = static_cast<uint64_t>(i) * frame_dur_ns;
            vp.keyframe = (i % cfg.gop == 0);
            vp.bytes =
                is_h264 ? MakeH264AnnexbAu(vp.keyframe, 256) : std::vector<uint8_t>(256, static_cast<uint8_t>(0xAB));
            if (!route_video(std::move(vp)))
                break;
        }

        // Even on an early cooperative stop we still enqueue EOS so the mux thread
        // finalizes what it has (an ordered stop). A "killed mid-recording" partial
        // is modelled by truncating the finalized file, not by abandoning threads.
        MuxItem eos;
        eos.payload = VideoEosSentinel{};
        std::lock_guard lk(st.mux_mutex);
        st.PushMuxItemLocked(std::move(eos));
    });

    std::unique_ptr<SessionStatsCollector> collector;
    if (cfg.drive_stats_collector) {
        collector = std::make_unique<SessionStatsCollector>(state);
        collector->Start();
    }

    mux_thread->Start();
    audio_thread->Start();

    video_feeder.join();

    const bool audio_joined = audio_thread->Join(30000);
    const bool mux_joined = mux_thread->Join(60000);
    if (collector)
        collector->Stop();

    if (!audio_joined) {
        result.error = "audio thread did not join";
        return result;
    }
    if (!mux_joined) {
        result.error = "mux thread did not join (finalize hang)";
        return result;
    }
    if (state.HasFailure()) {
        std::lock_guard lk(state.failure_mutex);
        result.error = "session failure: " + state.failure.error_detail;
        return result;
    }
    std::error_code ec;
    if (!std::filesystem::exists(cfg.output_path, ec) || std::filesystem::file_size(cfg.output_path, ec) == 0) {
        result.error = "output file missing or empty";
        return result;
    }
    result.success = true;
    result.finalized = true;
    result.output_bytes = std::filesystem::file_size(cfg.output_path, ec);
    return result;
}

} // namespace recorder_core::testutil
