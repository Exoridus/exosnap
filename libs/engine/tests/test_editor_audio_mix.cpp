#include "test_unique_temp.h"
#include <exosnap/engine/edit_timeline.h>
#include <exosnap/engine/edit_timeline_export.h>
#include <exosnap/engine/edit_timeline_render.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
}

using namespace exosnap::engine;

namespace {

constexpr int64_t Second = 1'000'000;

// Constant-level PCM in 10 ms packets. A constant source makes every gain,
// envelope and boundary error visible as a deviation from a known level.
bool WriteConstantPcm(const std::filesystem::path& path, int16_t left, int16_t right, int channels, int sample_rate,
                      int seconds) {
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
    stream->codecpar->sample_rate = sample_rate;
    stream->codecpar->bits_per_coded_sample = 16;
    av_channel_layout_default(&stream->codecpar->ch_layout, channels);
    stream->time_base = {1, sample_rate};
    if (avio_open(&context->pb, path.string().c_str(), AVIO_FLAG_WRITE) < 0 ||
        avformat_write_header(context, nullptr) < 0)
        return false;
    const int frames = sample_rate / 100;
    std::vector<uint8_t> payload(static_cast<size_t>(frames * channels) * 2);
    for (size_t index = 0; index < payload.size(); index += 2) {
        const auto value = static_cast<uint16_t>((index / 2) % channels == 0 ? left : right);
        payload[index] = static_cast<uint8_t>(value);
        payload[index + 1] = static_cast<uint8_t>(value >> 8);
    }
    for (int packet_index = 0; packet_index < seconds * 100; ++packet_index) {
        AVPacket packet{};
        packet.data = payload.data();
        packet.size = static_cast<int>(payload.size());
        packet.pts = av_rescale_q(static_cast<int64_t>(packet_index) * frames, {1, sample_rate}, stream->time_base);
        packet.dts = packet.pts;
        packet.duration = av_rescale_q(frames, {1, sample_rate}, stream->time_base);
        packet.flags = AV_PKT_FLAG_KEY;
        if (av_write_frame(context, &packet) < 0)
            return false;
    }
    return av_write_trailer(context) >= 0 && avio_closep(&context->pb) >= 0;
}

class EditorAudioMix : public ::testing::Test {
  protected:
    std::filesystem::path source = exosnap_test::UniqueTempPath("mix-source.mkv");
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove(source, error);
    }

    TimelineRenderSnapshot Snapshot(std::vector<TimelineClip> clips, std::vector<TimelineJunction> junctions = {},
                                    TimelineTrack track = {1, true}) const {
        TimelineRenderSnapshot snapshot;
        int64_t duration = 0;
        for (auto& clip : clips) {
            clip.audio = true;
            clip.track = track.id;
            duration = std::max(duration, clip.end_us());
        }
        snapshot.timeline.clips = std::move(clips);
        snapshot.timeline.junctions = std::move(junctions);
        snapshot.timeline.tracks = {track};
        snapshot.timeline.duration_us = duration;
        snapshot.sources = {{1, source, ProbeEditMedia(source), std::nullopt}};
        return snapshot;
    }

    static std::vector<float> Read(const TimelineRenderSnapshot& snapshot, int64_t first, int64_t count, uint32_t block,
                                   std::string& error) {
        EditTimelineReader reader;
        EditRenderPlan plan;
        plan.path = EditExportPath::Render;
        std::vector<float> result;
        if (!reader.Open(snapshot, plan, error))
            return result;
        std::vector<float> chunk;
        for (int64_t at = first; at < first + count; at += block) {
            const auto length = static_cast<uint32_t>(std::min<int64_t>(block, first + count - at));
            if (!reader.AudioAt(at, length, chunk, error))
                return {};
            result.insert(result.end(), chunk.begin(), chunk.end());
        }
        return result;
    }
};

TEST_F(EditorAudioMix, EqualGainCrossfadeOfIdenticalSourceKeepsItsLevel) {
    ASSERT_TRUE(WriteConstantPcm(source, 8192, 8192, 1, 48000, 2));
    const auto snapshot =
        Snapshot({{1, 1, 1, true, 0, 0, Second}, {2, 1, 2, true, Second / 2, Second / 2, 3 * Second / 2}},
                 {{7, 1, 1, 2, JunctionKind::Crossfade, Second / 2, Second}});
    std::string error;
    const auto samples = Read(snapshot, 0, 72000, 9600, error);
    ASSERT_EQ(samples.size(), 144000u) << error;
    for (size_t index = 0; index < samples.size(); ++index)
        ASSERT_NEAR(samples[index], 0.25f, 1e-4f) << "sample " << index / 2;
}

TEST_F(EditorAudioMix, TrackMuteSilencesAndTrackGainMultipliesClipGain) {
    ASSERT_TRUE(WriteConstantPcm(source, 8192, 8192, 1, 48000, 1));
    TimelineClip clip{1, 1, 1, true, 0, 0, Second};
    clip.gain_db = 6.0206;
    std::string error;
    TimelineTrack quieter{1, true, true, -12.0412};
    auto samples = Read(Snapshot({clip}, {}, quieter), 0, 4800, 4800, error);
    ASSERT_EQ(samples.size(), 9600u) << error;
    for (const auto sample : samples)
        ASSERT_NEAR(sample, 0.125f, 1e-4f);
    TimelineTrack muted{1, true, false};
    samples = Read(Snapshot({clip}, {}, muted), 0, 4800, 4800, error);
    ASSERT_EQ(samples.size(), 9600u) << error;
    for (const auto sample : samples)
        ASSERT_EQ(sample, 0.0f);
}

TEST_F(EditorAudioMix, AddingASilentDisabledClipLeavesTheMixUnchanged) {
    ASSERT_TRUE(WriteConstantPcm(source, 8192, 8192, 1, 48000, 1));
    TimelineClip disabled{2, 1, 2, true, 0, 0, Second};
    disabled.enabled = false;
    auto snapshot = Snapshot({{1, 1, 1, true, 0, 0, Second}});
    std::string error;
    const auto alone = Read(snapshot, 0, 4800, 4800, error);
    ASSERT_FALSE(alone.empty()) << error;
    snapshot.timeline.tracks.push_back({2, true});
    disabled.track = 2;
    snapshot.timeline.clips.push_back(disabled);
    const auto with_disabled = Read(snapshot, 0, 4800, 4800, error);
    EXPECT_EQ(with_disabled, alone) << error;
}

TEST_F(EditorAudioMix, ResampledHardCutHasNoGapOrDuplicateAtTheBoundary) {
    ASSERT_TRUE(WriteConstantPcm(source, 8192, 8192, 1, 44100, 3));
    const auto snapshot = Snapshot({{1, 1, 1, true, 0, 0, Second}, {2, 1, 2, true, Second, Second, 2 * Second}});
    std::string error;
    const auto first = Read(snapshot, 40000, 16000, 4410, error);
    ASSERT_EQ(first.size(), 32000u) << error;
    for (size_t index = 0; index < first.size(); ++index)
        ASSERT_NEAR(first[index], 0.25f, 1e-3f) << "sample " << 40000 + index / 2;
    const auto second = Read(snapshot, 40000, 16000, 4410, error);
    EXPECT_EQ(first, second);
}

TEST_F(EditorAudioMix, StereoSourceKeepsChannelIdentity) {
    ASSERT_TRUE(WriteConstantPcm(source, 8192, -4096, 2, 48000, 1));
    std::string error;
    const auto samples = Read(Snapshot({{1, 1, 1, true, 0, 0, Second}}), 0, 4800, 4800, error);
    ASSERT_EQ(samples.size(), 9600u) << error;
    for (size_t index = 0; index < samples.size(); index += 2) {
        ASSERT_NEAR(samples[index], 0.25f, 1e-4f);
        ASSERT_NEAR(samples[index + 1], -0.125f, 1e-4f);
    }
}

TEST_F(EditorAudioMix, MonitorSelectionDoesNotChangeTheResultMix) {
    ASSERT_TRUE(WriteConstantPcm(source, 8192, 8192, 1, 48000, 1));
    auto snapshot = Snapshot({{1, 1, 1, true, 0, 0, Second}});
    const auto& audio = snapshot.sources.front().metadata.audio_metadata;
    ASSERT_EQ(audio.actual_tracks.size(), 1u);
    snapshot.sources.front().monitor_selection =
        MonitorSelectionSnapshot{audio.selection_media_id,
                                 {1},
                                 {{audio.actual_tracks.front().track_id, 0.0f, true}},
                                 true,
                                 audio.unverified_asset_binding};
    std::string error;
    const auto samples = Read(snapshot, 0, 4800, 4800, error);
    ASSERT_EQ(samples.size(), 9600u) << error;
    for (const auto sample : samples)
        ASSERT_NEAR(sample, 0.25f, 1e-4f);
}

TEST_F(EditorAudioMix, EnabledSourceFailureIsAnErrorNotSilence) {
    ASSERT_TRUE(WriteConstantPcm(source, 8192, 8192, 1, 48000, 1));
    TimelineClip wrong_kind{1, 1, 1, true, 0, 0, Second};
    wrong_kind.stream = TimelineStreamRef{1, {}, 0, TimelineStreamKind::Video};
    std::string error;
    EXPECT_TRUE(Read(Snapshot({wrong_kind}), 0, 4800, 4800, error).empty());
    EXPECT_FALSE(error.empty());
    auto missing = Snapshot({{1, 1, 1, true, 0, 0, Second}});
    missing.sources.front().path = source.parent_path() / "absent-mix-source.mkv";
    error.clear();
    EXPECT_TRUE(Read(missing, 0, 4800, 4800, error).empty());
    EXPECT_FALSE(error.empty());
}

} // namespace
