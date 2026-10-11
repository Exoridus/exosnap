#include "matroska_stream_writer.h"
#include "render_audio_manifest.h"
#include "test_unique_temp.h"

#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/audio_routing_model.h>
#include <exosnap/engine/edit_player_engine.h>
#include <exosnap/engine/recording_media_types.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <ios>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>
#include <windows.h>

extern "C" {
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

namespace exosnap::engine {
namespace {

TEST(RenderAudioManifest, MixCreatesNewIdentityWithDeduplicatedLineageAndActualOutputFormat) {
    const auto media = NewId<MediaId>();
    const auto first = NewId<AudioTrackId>();
    const auto second = NewId<AudioTrackId>();
    const std::vector<MonitorSelectionSnapshot> selections{
        {media, {1}, {{first, 0.5f, false}, {second, 1.0f, true}}, true}, {media, {2}, {{first, 1.0f, false}}, true}};
    const auto result = BuildRenderAudioManifest(selections);
    ASSERT_TRUE(result.succeeded());
    const auto& manifest = *result.value();
    ASSERT_EQ(manifest.tracks.size(), 1u);
    const auto& mix = manifest.tracks.front();
    EXPECT_NE(manifest.media_id, media);
    EXPECT_NE(mix.container.track_id, first);
    EXPECT_NE(mix.container.track_id, second);
    EXPECT_EQ(manifest.derived_from_media_ids, std::vector<MediaId>{media});
    EXPECT_EQ(mix.derived_from_track_ids, (std::vector<AudioTrackId>{first, second}));
    EXPECT_EQ(mix.container.stream_index.value, 0u);
    EXPECT_EQ(mix.container.track_uid, 2u);
    EXPECT_EQ(mix.container.role, "mix");
    EXPECT_FALSE(mix.container.name.empty());
    EXPECT_EQ(mix.container.codec.codec_id, "A_PCM/INT/LIT");
    EXPECT_EQ(mix.container.codec.sample_rate, 48000u);
    EXPECT_EQ(mix.container.codec.channels, 2u);
    EXPECT_EQ(mix.container.codec.bit_depth, 24u);
    EXPECT_EQ(mix.container.codec.codec_delay_samples, 0u);
    EXPECT_EQ(mix.container.codec.channel_layout, "stereo");
    EXPECT_TRUE(manifest.sources.empty());
    EXPECT_TRUE(mix.routes.empty());
    EXPECT_TRUE(mix.container.source_ids.empty());
    EXPECT_EQ(manifest.default_monitor_track_ids, std::vector<AudioTrackId>{mix.container.track_id});
    EXPECT_EQ(manifest.monitor_selection_origin, MonitorSelectionOrigin::User);
    EXPECT_TRUE(SerializeAudioManifest1(manifest).succeeded());
}

TEST(RenderAudioManifest, ExplicitEmptySelectionsProduceVideoOnlyManifest) {
    const auto media = NewId<MediaId>();
    const auto result = BuildRenderAudioManifest({{media, {1}, {}, true}});
    ASSERT_TRUE(result.succeeded());
    EXPECT_TRUE(result.value()->tracks.empty());
    EXPECT_TRUE(result.value()->default_monitor_track_ids.empty());
    EXPECT_EQ(result.value()->derived_from_media_ids, std::vector<MediaId>{media});
    EXPECT_EQ(result.value()->monitor_selection_origin, MonitorSelectionOrigin::User);
}

class RenderAudioPreflightTest : public testing::Test {
  protected:
    void SetUp() override {
        path = exosnap_test::UniqueTempPath("render-source.mkv");
        snapshot.sources = {{1, path, {}}};
        snapshot.timeline = {{{1, 1, 1, false, 0, 0, 1000000}, {2, 1, 1, true, 0, 0, 1000000}}, {}, 1000000};
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
    void WriteSource(bool verified = false) {
        const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
        AVFormatContext* context = nullptr;
        ASSERT_GE(avformat_open_input(&context, fixture.string().c_str(), nullptr, nullptr), 0);
        auto close = [](AVFormatContext* input) { avformat_close_input(&input); };
        std::unique_ptr<AVFormatContext, decltype(close)> input(context, close);
        ASSERT_GE(avformat_find_stream_info(context, nullptr), 0);
        const auto* stream = context->streams[0];
        MatroskaStreamConfig config;
        config.output_path = path.string();
        config.video_codec_id = "V_MPEG4/ISO/AVC";
        config.video_codec_private.assign(stream->codecpar->extradata,
                                          stream->codecpar->extradata + stream->codecpar->extradata_size);
        config.encode_width = config.encode_height = 64;
        config.frame_rate_num = 30;
        config.frame_rate_den = 1;
        config.audio_codec = StreamAudioCodec::Pcm;
        config.audio_track_count = 1;
        config.audio_tracks[0].name = "Legacy source";
        if (verified) {
            AudioManifest1 manifest;
            manifest.media_id = NewId<MediaId>();
            manifest.recording_id = NewId<RecordingId>();
            manifest.segment_id = NewId<SegmentId>();
            AudioManifestTrack track;
            track.container.track_id = NewId<AudioTrackId>();
            track.container.stream_index = {0};
            track.container.track_uid = 2;
            track.container.name = config.audio_tracks[0].name;
            track.container.role = "stem";
            track.container.codec = {"A_PCM/INT/LIT", {}, 48000, 2, 16, 0, "stereo"};
            manifest.default_monitor_track_ids = {track.container.track_id};
            manifest.tracks.push_back(std::move(track));
            config.audio_manifest = std::move(manifest);
        }
        MatroskaStreamWriter writer;
        ASSERT_TRUE(writer.Open(config));
        auto free_packet = [](AVPacket* packet) { av_packet_free(&packet); };
        std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
        ASSERT_TRUE(packet);
        while (av_read_frame(context, packet.get()) >= 0) {
            MuxPacket item;
            item.track_num = 1;
            item.pts_ns = static_cast<uint64_t>(av_rescale_q(packet->pts, stream->time_base, {1, 1000000000}));
            item.dts_ns = av_rescale_q(packet->dts, stream->time_base, {1, 1000000000});
            item.is_key = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            item.bytes.assign(packet->data, packet->data + packet->size);
            ASSERT_TRUE(writer.Push(std::move(item)));
            av_packet_unref(packet.get());
        }
        ASSERT_TRUE(writer.Push({0, 2, true, std::vector<uint8_t>(480 * 4), {}}));
        ASSERT_TRUE(writer.Finalize());
    }
    void FreezeSelection(bool empty = false) {
        EditPlayerEngine player;
        std::string error;
        ASSERT_TRUE(player.Open(path, error)) << error;
        const auto metadata = player.AudioMetadata();
        ASSERT_TRUE(metadata.manifest || metadata.unverified_asset_binding);
        MonitorSelectionSnapshot selection{
            metadata.selection_media_id, {1}, {}, true, metadata.unverified_asset_binding};
        if (!empty)
            selection.tracks.push_back({metadata.actual_tracks.front().track_id, 0.5f, false});
        snapshot.sources[0].monitor_selection = std::move(selection);
    }
    std::filesystem::path path;
    TimelineRenderSnapshot snapshot;
};

TEST_F(RenderAudioPreflightTest, MissingExplicitSelectionIsRejectedBeforeRendering) {
    ASSERT_NO_FATAL_FAILURE(WriteSource());
    const auto result = PrepareRenderAudioManifest(snapshot);
    ASSERT_FALSE(result.succeeded());
    EXPECT_NE(result.error()->context.find("selection"), std::string::npos);
}

TEST_F(RenderAudioPreflightTest, BoundConfirmedLegacySelectionCanReopenAndKeepsSourceImmutable) {
    ASSERT_NO_FATAL_FAILURE(WriteSource());
    ASSERT_NO_FATAL_FAILURE(FreezeSelection());
    const auto result = PrepareRenderAudioManifest(snapshot);
    ASSERT_TRUE(result.succeeded()) << result.error()->context;
    ASSERT_EQ(result.value()->manifest.tracks.size(), 1u);
    EXPECT_EQ(result.value()->manifest.tracks[0].derived_from_track_ids,
              std::vector<AudioTrackId>{snapshot.sources[0].monitor_selection->tracks[0].track_id});
    EXPECT_EQ(result.value()->source_leases.size(), 1u);
    HANDLE write = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_EQ(write, INVALID_HANDLE_VALUE);
    if (write != INVALID_HANDLE_VALUE)
        CloseHandle(write);
}

TEST_F(RenderAudioPreflightTest, ConfirmedLegacyEmptySelectionProducesVideoOnly) {
    ASSERT_NO_FATAL_FAILURE(WriteSource());
    ASSERT_NO_FATAL_FAILURE(FreezeSelection(true));
    const auto result = PrepareRenderAudioManifest(snapshot);
    ASSERT_TRUE(result.succeeded()) << result.error()->context;
    EXPECT_TRUE(result.value()->manifest.tracks.empty());
}

TEST_F(RenderAudioPreflightTest, ChangedLegacyBytesRejectTheFrozenSelection) {
    ASSERT_NO_FATAL_FAILURE(WriteSource());
    ASSERT_NO_FATAL_FAILURE(FreezeSelection());
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        file.seekg(-1, std::ios::end);
        char byte = 0;
        file.read(&byte, 1);
        byte ^= 1;
        file.seekp(-1, std::ios::end);
        file.write(&byte, 1);
    }
    EXPECT_FALSE(PrepareRenderAudioManifest(snapshot).succeeded());
}

TEST_F(RenderAudioPreflightTest, MissingTrackRejectsTheFrozenSelection) {
    ASSERT_NO_FATAL_FAILURE(WriteSource());
    ASSERT_NO_FATAL_FAILURE(FreezeSelection());
    snapshot.sources[0].monitor_selection->tracks[0].track_id = NewId<AudioTrackId>();
    EXPECT_FALSE(PrepareRenderAudioManifest(snapshot).succeeded());
}

TEST_F(RenderAudioPreflightTest, VerifiedFrozenSelectionKeepsOnlySelectedDerivationIdentity) {
    ASSERT_NO_FATAL_FAILURE(WriteSource(true));
    ASSERT_NO_FATAL_FAILURE(FreezeSelection());
    const auto selected = *snapshot.sources[0].monitor_selection;
    ASSERT_FALSE(selected.unverified_asset_binding);
    const auto result = PrepareRenderAudioManifest(snapshot);
    ASSERT_TRUE(result.succeeded()) << result.error()->context;
    ASSERT_EQ(result.value()->manifest.tracks.size(), 1u);
    EXPECT_EQ(result.value()->manifest.derived_from_media_ids, std::vector<MediaId>{selected.media_id});
    EXPECT_EQ(result.value()->manifest.tracks[0].derived_from_track_ids,
              std::vector<AudioTrackId>{selected.tracks[0].track_id});
    EXPECT_NE(result.value()->manifest.tracks[0].container.track_id, selected.tracks[0].track_id);
}

TEST_F(RenderAudioPreflightTest, VerifiedDefaultsMatchPreviewWithoutInventingAnUnconfirmedSelection) {
    ASSERT_NO_FATAL_FAILURE(WriteSource(true));
    const auto result = PrepareRenderAudioManifest(snapshot);
    ASSERT_TRUE(result.succeeded()) << result.error()->context;
    ASSERT_EQ(result.value()->manifest.tracks.size(), 1u);
    EditPlayerEngine player;
    std::string error;
    ASSERT_TRUE(player.Open(path, error)) << error;
    const auto metadata = player.AudioMetadata();
    ASSERT_TRUE(metadata.manifest);
    EXPECT_FALSE(metadata.monitoring_confirmation_required);
    EXPECT_EQ(result.value()->manifest.tracks[0].derived_from_track_ids, metadata.manifest->default_monitor_track_ids);
}

TEST_F(RenderAudioPreflightTest, ReferencedVideoAssetStillRejectsAnInvalidFrozenSelection) {
    ASSERT_NO_FATAL_FAILURE(WriteSource());
    ASSERT_NO_FATAL_FAILURE(FreezeSelection());
    snapshot.timeline.clips.pop_back();
    snapshot.sources[0].monitor_selection->tracks[0].track_id = NewId<AudioTrackId>();
    EXPECT_FALSE(PrepareRenderAudioManifest(snapshot).succeeded());
}

} // namespace
} // namespace exosnap::engine
