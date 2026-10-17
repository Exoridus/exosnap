// Audio-clocked playback against the real default audio endpoint (silent).
// Live-labelled: it needs an output device, which hosted runners lack.

#include <gtest/gtest.h>

#include "exosnap/engine/audio_media_manifest.h"
#include "exosnap/engine/edit_player_engine.h"
#include "exosnap/engine/edit_player_session.h"

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using exosnap::engine::EditPlayerSession;

constexpr int kAudioRate = 48000;
constexpr int64_t kAudioBlockMs = 10;
constexpr int64_t kFixtureDurationMs = 4000;

// The committed reordered H.264 fixture with a silent stereo PCM track added,
// so playback runs on the audio clock instead of the wall clock.
bool WriteVideoWithSilentAudio(const std::filesystem::path& source, const std::filesystem::path& target) {
    AVFormatContext* input = nullptr;
    if (avformat_open_input(&input, source.string().c_str(), nullptr, nullptr) < 0)
        return false;
    bool ok = avformat_find_stream_info(input, nullptr) >= 0;
    const int video_index = ok ? av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0) : -1;
    ok = ok && video_index >= 0;

    AVFormatContext* output = nullptr;
    ok = ok && avformat_alloc_output_context2(&output, nullptr, "matroska", nullptr) >= 0 && output != nullptr;
    AVStream* video = ok ? avformat_new_stream(output, nullptr) : nullptr;
    AVStream* audio = video ? avformat_new_stream(output, nullptr) : nullptr;
    ok = ok && audio != nullptr && avcodec_parameters_copy(video->codecpar, input->streams[video_index]->codecpar) >= 0;
    if (ok) {
        video->codecpar->codec_tag = 0;
        video->time_base = input->streams[video_index]->time_base;
        audio->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        audio->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
        audio->codecpar->format = AV_SAMPLE_FMT_S16;
        audio->codecpar->sample_rate = kAudioRate;
        audio->codecpar->bits_per_coded_sample = 16;
        av_channel_layout_default(&audio->codecpar->ch_layout, 2);
        audio->time_base = AVRational{1, 1000};
    }
    ok = ok && avio_open(&output->pb, target.string().c_str(), AVIO_FLAG_WRITE) >= 0;
    ok = ok && avformat_write_header(output, nullptr) >= 0;

    AVPacket* packet = av_packet_alloc();
    AVPacket* audio_packet = av_packet_alloc();
    ok = ok && packet != nullptr && audio_packet != nullptr;
    const int audio_bytes = static_cast<int>(kAudioRate * kAudioBlockMs / 1000) * 2 * 2;
    int64_t next_audio_ms = 0;
    const auto write_audio_until = [&](int64_t end_ms) {
        while (ok && next_audio_ms < end_ms) {
            ok = av_new_packet(audio_packet, audio_bytes) >= 0;
            if (!ok)
                return;
            std::memset(audio_packet->data, 0, static_cast<size_t>(audio_bytes));
            audio_packet->stream_index = audio->index;
            audio_packet->pts = audio_packet->dts = av_rescale_q(next_audio_ms, AVRational{1, 1000}, audio->time_base);
            audio_packet->duration = av_rescale_q(kAudioBlockMs, AVRational{1, 1000}, audio->time_base);
            audio_packet->flags |= AV_PKT_FLAG_KEY;
            ok = av_interleaved_write_frame(output, audio_packet) >= 0;
            next_audio_ms += kAudioBlockMs;
        }
    };
    while (ok && av_read_frame(input, packet) >= 0) {
        if (packet->stream_index != video_index) {
            av_packet_unref(packet);
            continue;
        }
        const auto input_base = input->streams[video_index]->time_base;
        write_audio_until(
            av_rescale_q(packet->dts == AV_NOPTS_VALUE ? packet->pts : packet->dts, input_base, AVRational{1, 1000}));
        packet->stream_index = video->index;
        av_packet_rescale_ts(packet, input_base, video->time_base);
        ok = ok && av_interleaved_write_frame(output, packet) >= 0;
        av_packet_unref(packet);
    }
    write_audio_until(kFixtureDurationMs);

    av_packet_free(&audio_packet);
    av_packet_free(&packet);
    if (ok)
        ok = av_write_trailer(output) >= 0;
    if (output != nullptr)
        avio_closep(&output->pb);
    avformat_free_context(output);
    avformat_close_input(&input);
    return ok;
}

class ScopedFixture {
  public:
    ScopedFixture()
        : path_(std::filesystem::temp_directory_path() / "edit_player_av_sync_silent_audio.mkv"),
          previous_log_level_(av_log_get_level()) {
        av_log_set_level(AV_LOG_QUIET);
        const auto source = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
        written_ = WriteVideoWithSilentAudio(source, path_);
    }
    ~ScopedFixture() {
        av_log_set_level(previous_log_level_);
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    ScopedFixture(const ScopedFixture&) = delete;
    ScopedFixture& operator=(const ScopedFixture&) = delete;

    [[nodiscard]] bool written() const noexcept {
        return written_;
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
    int previous_log_level_;
    bool written_ = false;
};

struct DeliveryObservation {
    bool device_available = false;
    int delivered = 0;
    // Frames handed on although the clock sample had already passed them; the
    // consumer's present gate would discard those.
    int behind_clock = 0;
    int64_t worst_lead_us = std::numeric_limits<int64_t>::min();
    // Audible gaps: 48 kHz frames the output had to replace with silence.
    uint64_t shortfall_frames = 0;
};

// Plays the fixture for 1.5 s, sampling the clock every tick the way the
// editor's presentation tick does.
DeliveryObservation PlayAndObserve(const std::filesystem::path& path, std::chrono::milliseconds tick) {
    DeliveryObservation observation;
    EditPlayerSession session;
    std::string error;
    EXPECT_TRUE(session.Open(path, error)) << error;
    if (!session.HasAudioStream())
        return observation;
    observation.device_available = true;

    // A foreign file has no verified manifest, so monitoring needs the same
    // explicit confirmation the editor asks the user for.
    const auto metadata = session.AudioMetadata();
    exosnap::engine::MonitorSelectionSnapshot selection;
    selection.media_id = metadata.selection_media_id;
    selection.revision = {1};
    selection.overlap_confirmed = true;
    selection.unverified_asset_binding = metadata.unverified_asset_binding;
    for (const auto& track : metadata.actual_tracks)
        selection.tracks.push_back({track.track_id, 1.0f, false});
    EXPECT_TRUE(session.SetMonitorSelection(selection, error)) << error;
    EXPECT_FALSE(session.MonitoringConfirmationRequired());
    session.SetVolume(0.0f);

    using Clock = std::chrono::steady_clock;
    struct TimedValue {
        int64_t value_us;
        Clock::time_point at;
    };
    std::mutex deliveries_mutex;
    std::vector<TimedValue> deliveries;
    std::atomic<int> behind_clock{0};
    session.SetOnFrameReady([&](exosnap::engine::RawDecodedVideoFrame frame) {
        const auto at = Clock::now();
        const int64_t clock = session.ClockSnapshotUs();
        if (clock < 0)
            return;
        if (frame.pts_us < clock)
            behind_clock.fetch_add(1);
        std::lock_guard lock(deliveries_mutex);
        deliveries.push_back({frame.pts_us, at});
    });

    // Clock samples taken on this thread, which is the only one allowed to
    // read the audio clock. Each delivery is later compared with the clock
    // interpolated between the samples around it, the clock as it actually
    // was at that moment.
    std::vector<TimedValue> samples;
    session.Play(0);
    const auto end = Clock::now() + std::chrono::milliseconds(1500);
    while (Clock::now() < end) {
        samples.push_back({session.CurrentPositionUs(), Clock::now()});
        std::this_thread::sleep_for(tick);
    }
    observation.shortfall_frames = session.AudioShortfallFrames();
    session.Pause();
    session.Close();

    std::lock_guard lock(deliveries_mutex);
    observation.delivered = static_cast<int>(deliveries.size());
    observation.behind_clock = behind_clock.load();
    for (const auto& delivery : deliveries) {
        const auto after = std::find_if(samples.begin(), samples.end(),
                                        [&](const TimedValue& sample) { return sample.at >= delivery.at; });
        if (after == samples.begin() || after == samples.end())
            continue;
        const auto before = std::prev(after);
        const auto span = std::chrono::duration<double>(after->at - before->at).count();
        const auto offset = std::chrono::duration<double>(delivery.at - before->at).count();
        const auto clock_us = static_cast<int64_t>(
            static_cast<double>(before->value_us) +
            (span > 0.0 ? static_cast<double>(after->value_us - before->value_us) * offset / span : 0.0));
        observation.worst_lead_us = std::max(observation.worst_lead_us, delivery.value_us - clock_us);
    }
    return observation;
}

TEST(EditPlayerAvSync, AudioClockedPlaybackNeverDeliversVideoAheadOfTheClock) {
    ScopedFixture fixture;
    ASSERT_TRUE(fixture.written());
    const auto observed = PlayAndObserve(fixture.path(), std::chrono::milliseconds(2));
    if (!observed.device_available)
        GTEST_SKIP() << "no audio output endpoint could be opened";
    EXPECT_GE(observed.delivered, 20);
    // Holding video must never starve the audio output.
    EXPECT_EQ(observed.shortfall_frames, 0u);
    // The deliberate early release plus scheduling slack. Without the hold the
    // decode thread delivers as far ahead as the demuxer reads, up to a second.
    EXPECT_LE(observed.worst_lead_us, 22'000);
}

TEST(EditPlayerAvSync, FrameRateTickReleasesFramesBeforeTheClockPassesThem) {
    // The editor samples the clock once per clip frame (33 ms at 30 fps). A hold
    // that waited for the next sample would release every frame already behind
    // it, and the present gate would drop nearly all of them.
    ScopedFixture fixture;
    ASSERT_TRUE(fixture.written());
    const auto observed = PlayAndObserve(fixture.path(), std::chrono::milliseconds(33));
    if (!observed.device_available)
        GTEST_SKIP() << "no audio output endpoint could be opened";
    EXPECT_GE(observed.delivered, 30);
    EXPECT_LE(observed.behind_clock, 2);
    EXPECT_LE(observed.worst_lead_us, 22'000);
}

} // namespace
