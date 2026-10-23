#include "audio_media_manifest_adapter.h"
#include "bounded_metadata_json.h"
#include "ffmpeg_aac_encoder.h"
#include "matroska_stream_writer.h"
#include "media_identity_sidecar.h"
#include "opus_audio_encoder.h"
#include "test_unique_temp.h"
#include <array>
#include <cstddef>
#include <exosnap/engine/edit_types.h>

#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/audio_routing_model.h>
#include <exosnap/engine/edit_player_engine.h>
#include <exosnap/engine/edit_timeline_export.h>
#include <exosnap/engine/mp4_remuxer.h>
#include <exosnap/engine/packet_types.h>
#include <exosnap/engine/recording_media_types.h>
#include <exosnap/engine/waveform_cache.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <ios>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/codec_id.h>
#include <libavcodec/defs.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
}

namespace exosnap::engine {
namespace {

AudioManifest1 Manifest() {
    AudioManifest1 manifest;
    manifest.media_id = NewId<MediaId>();
    manifest.recording_id = NewId<RecordingId>();
    manifest.segment_id = NewId<SegmentId>();
    AudioManifestSource source;
    source.id = NewId<AudioSourceId>();
    manifest.sources.push_back(source);
    for (uint32_t index = 0; index < 2; ++index) {
        AudioManifestTrack track;
        track.container.track_id = NewId<AudioTrackId>();
        track.container.stream_index = {index};
        track.container.track_uid = 2 + index;
        track.container.name = index == 0 ? "Master" : "Stem";
        track.container.role = index == 0 ? "mix" : "stem";
        track.container.codec = {"A_PCM/INT/LIT", {}, 48000, 2, 16, 0, "stereo"};
        track.container.source_ids = {source.id};
        track.routes.push_back({NewId<AudioRouteId>(), source.id, index == 0 ? 1.0f : 2.0f});
        manifest.tracks.push_back(std::move(track));
    }
    manifest.default_monitor_track_ids = {manifest.tracks.front().container.track_id};
    return manifest;
}

class AudioMetadataRoundtrip : public testing::Test {
  protected:
    void SetUp() override {
        path = exosnap_test::UniqueTempPath("metadata.mkv");
        rewrite = exosnap_test::UniqueTempPath("rewrite.mkv");
        roundtrip = exosnap_test::UniqueTempPath("roundtrip.mkv");
        manifest = Manifest();
    }
    void TearDown() override {
        for (const auto& file : {path, rewrite, roundtrip, MediaIdentitySidecarPath(path),
                                 MediaIdentitySidecarPath(rewrite), MediaIdentitySidecarPath(roundtrip)}) {
            std::error_code error;
            std::filesystem::remove(file, error);
        }
    }
    void WriteFixture(bool metadata = true, bool compressed = false, const std::filesystem::path& destination = {}) {
        const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
        AVFormatContext* input = nullptr;
        ASSERT_GE(avformat_open_input(&input, fixture.string().c_str(), nullptr, nullptr), 0);
        auto close = [](AVFormatContext* context) { avformat_close_input(&context); };
        std::unique_ptr<AVFormatContext, decltype(close)> guard(input, close);
        ASSERT_GE(avformat_find_stream_info(input, nullptr), 0);
        const auto* stream = input->streams[0];
        MatroskaStreamConfig config;
        config.output_path = (destination.empty() ? path : destination).string();
        config.video_codec_id = "V_MPEG4/ISO/AVC";
        config.video_codec_private.assign(stream->codecpar->extradata,
                                          stream->codecpar->extradata + stream->codecpar->extradata_size);
        config.encode_width = config.encode_height = 64;
        config.frame_rate_num = 30;
        config.frame_rate_den = 1;
        config.audio_codec = compressed ? StreamAudioCodec::Aac : StreamAudioCodec::Pcm;
        config.audio_track_count = 2;
        for (size_t index = 0; index < 2; ++index)
            config.audio_tracks[index].name = manifest.tracks[index].container.name;
        std::vector<MuxPacket> packets;
        AVPacket* packet = av_packet_alloc();
        ASSERT_NE(packet, nullptr);
        while (av_read_frame(input, packet) >= 0) {
            MuxPacket item;
            item.track_num = 1;
            item.pts_ns = static_cast<uint64_t>(av_rescale_q(packet->pts, stream->time_base, {1, 1000000000}));
            item.dts_ns = av_rescale_q(packet->dts, stream->time_base, {1, 1000000000});
            item.is_key = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            item.bytes.assign(packet->data, packet->data + packet->size);
            packets.push_back(std::move(item));
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
        if (compressed) {
            for (uint64_t track = 2; track <= 3; ++track) {
                FfmpegAacEncoder encoder;
                std::string error;
                ASSERT_TRUE(encoder.Init(48000, 2, error)) << error;
                std::vector<float> pcm(96000 * 2);
                for (size_t sample = 0; sample < pcm.size() / 2; ++sample) {
                    const float value = static_cast<float>(std::sin(static_cast<double>(sample) * 0.0575958653158)) *
                                        (track == 2 ? 0.125f : 0.25f);
                    pcm[sample * 2] = pcm[sample * 2 + 1] = value;
                }
                uint64_t accumulated = 0;
                std::vector<EncodedAudioPacket> encoded;
                encoder.FeedFloat32(pcm.data(), pcm.size(), 0, accumulated, 48000, 2, encoded);
                encoder.Flush(encoded);
                ASSERT_FALSE(encoded.empty());
                config.audio_tracks[track - 2].codec_private = encoder.CodecPrivateBytes();
                config.audio_tracks[track - 2].codec_delay_samples = encoder.CodecDelaySamples();
                auto& codec = manifest.tracks[track - 2].container.codec;
                codec.codec_id = "A_AAC";
                codec.bit_depth = 0;
                codec.codec_private = encoder.CodecPrivateBytes();
                codec.codec_delay_samples = encoder.CodecDelaySamples();
                for (auto& audio : encoded) {
                    MuxPacket item;
                    item.track_num = track;
                    item.pts_ns = audio.pts_ns;
                    item.is_key = true;
                    item.bytes = std::move(audio.bytes);
                    packets.push_back(std::move(item));
                }
            }
        }
        for (uint64_t pts = 0; !compressed && pts < 2000000000ULL; pts += 10000000ULL) {
            for (uint64_t track = 2; track <= 3; ++track) {
                MuxPacket item;
                item.track_num = track;
                item.pts_ns = pts;
                item.is_key = true;
                item.bytes.resize(480 * 2 * 2);
                const uint16_t sample = manifest.tracks[track - 2].container.role == "mix" ? 4096 : 8192;
                for (size_t index = 0; index < item.bytes.size(); index += 2) {
                    item.bytes[index] = static_cast<uint8_t>(sample);
                    item.bytes[index + 1] = static_cast<uint8_t>(sample >> 8);
                }
                packets.push_back(std::move(item));
            }
        }
        std::stable_sort(packets.begin(), packets.end(), [](const auto& left, const auto& right) {
            return left.dts_ns.value_or(static_cast<int64_t>(left.pts_ns)) <
                   right.dts_ns.value_or(static_cast<int64_t>(right.pts_ns));
        });
        MatroskaStreamWriter writer;
        if (metadata)
            config.audio_manifest = manifest;
        ASSERT_TRUE(writer.Open(config));
        for (auto& item : packets)
            ASSERT_TRUE(writer.Push(std::move(item)));
        ASSERT_TRUE(writer.Finalize());
    }
    std::filesystem::path path, rewrite, roundtrip;
    AudioManifest1 manifest;
};

TEST(AudioManifest, CanonicalSerializationRoundtripsStableIdentitiesAndRoutes) {
    auto manifest = Manifest();
    ASSERT_TRUE(ValidateAudioManifest1(manifest).succeeded());
    auto json = SerializeAudioManifest1(manifest);
    ASSERT_TRUE(json.succeeded());
    auto parsed = ParseAudioManifest1(*json.value());
    ASSERT_TRUE(parsed.succeeded());
    EXPECT_EQ(parsed.value()->media_id, manifest.media_id);
    EXPECT_EQ(parsed.value()->tracks[1].container.track_id, manifest.tracks[1].container.track_id);
    EXPECT_EQ(parsed.value()->tracks[0].routes[0].source_id, manifest.sources[0].id);
    auto again = SerializeAudioManifest1(*parsed.value());
    ASSERT_TRUE(again.succeeded());
    EXPECT_EQ(*again.value(), *json.value());
    EXPECT_EQ(json.value()->find("endpoint"), std::string::npos);
    EXPECT_EQ(json.value()->find("process_id"), std::string::npos);
}

TEST_F(AudioMetadataRoundtrip, WriterPublishesContainerAuthorityWithStableTrackNamesAndIds) {
    ASSERT_NO_FATAL_FAILURE(WriteFixture());
    EditPlayerEngine player;
    std::string error;
    ASSERT_TRUE(player.Open(path, error)) << error;
    const auto metadata = player.AudioMetadata();
    ASSERT_EQ(metadata.status, AudioMetadataStatus::Verified) << testing::PrintToString(metadata.warnings);
    ASSERT_TRUE(metadata.manifest);
    EXPECT_EQ(metadata.manifest->media_id, manifest.media_id);
    const auto tracks = player.AudioTracks();
    ASSERT_EQ(tracks.size(), 2u);
    EXPECT_EQ(tracks[0].name, "Master");
    EXPECT_EQ(tracks[1].name, "Stem");
    ASSERT_TRUE(tracks[1].track_id);
    EXPECT_EQ(*tracks[1].track_id, manifest.tracks[1].container.track_id);
    EXPECT_EQ(tracks[0].role, "mix");
    EXPECT_FALSE(metadata.monitoring_confirmation_required);
}

TEST_F(AudioMetadataRoundtrip, ExactRewritePreservesTracksAndCreatesDerivedMediaIdentity) {
    ASSERT_NO_FATAL_FAILURE(WriteFixture());
    const auto result = RemuxToMkv(path, rewrite);
    ASSERT_TRUE(result.success) << result.message;
    EditPlayerEngine player;
    std::string error;
    ASSERT_TRUE(player.Open(rewrite, error)) << error;
    const auto metadata = player.AudioMetadata();
    ASSERT_EQ(metadata.status, AudioMetadataStatus::Verified) << testing::PrintToString(metadata.warnings);
    ASSERT_TRUE(metadata.manifest);
    EXPECT_NE(metadata.manifest->media_id, manifest.media_id);
    ASSERT_EQ(metadata.manifest->derived_from_media_ids.size(), 1u);
    EXPECT_EQ(metadata.manifest->derived_from_media_ids[0], manifest.media_id);
    ASSERT_EQ(metadata.manifest->tracks.size(), 2u);
    EXPECT_EQ(metadata.manifest->tracks[1].container.track_id, manifest.tracks[1].container.track_id);
    EXPECT_EQ(metadata.manifest->default_monitor_track_ids, manifest.default_monitor_track_ids);
}

TEST_F(AudioMetadataRoundtrip, FinalByteReferencesSurviveRewriteAndRejectUnsafeNames) {
    ASSERT_NO_FATAL_FAILURE(WriteFixture());
    MediaIdentitySidecarReferences references;
    references.marker_refs = {"recording.markers.json", "unrelated.markers.json"};
    references.source_ranges_json = "[{\"association\":\"original segment\"}]";
    ASSERT_TRUE(WriteMediaIdentitySidecar(path, manifest, {}, references).succeeded());
    auto metadata = ReadMediaIdentitySidecar(path);
    ASSERT_EQ(metadata.status, AudioMetadataStatus::Verified);
    EXPECT_EQ(metadata.references.marker_refs, references.marker_refs);
    EXPECT_EQ(metadata.references.source_ranges_json, references.source_ranges_json);
    ASSERT_TRUE(WriteMediaIdentitySidecar(path, manifest).succeeded());
    EXPECT_EQ(ReadMediaIdentitySidecar(path).references.marker_refs, references.marker_refs);
    ASSERT_TRUE(UpdateMediaIdentityMarkerReference(path, std::string{"export.markers.json"}, {},
                                                   std::string{"recording.markers.json"})
                    .succeeded());
    metadata = ReadMediaIdentitySidecar(path);
    const std::vector<std::string> expected_refs{"unrelated.markers.json", "export.markers.json"};
    EXPECT_EQ(metadata.references.marker_refs, expected_refs);
    EXPECT_EQ(metadata.references.source_ranges_json, references.source_ranges_json);
    ASSERT_TRUE(UpdateMediaIdentityMarkerReference(path, std::string{"export.markers.json"}).succeeded());
    EXPECT_EQ(ReadMediaIdentitySidecar(path).references.marker_refs, expected_refs);
    references.marker_refs = {"../foreign.markers.json"};
    EXPECT_FALSE(WriteMediaIdentitySidecar(path, manifest, {}, references).succeeded());
    metadata = ReadMediaIdentitySidecar(path);
    EXPECT_EQ(metadata.status, AudioMetadataStatus::Verified);
    EXPECT_EQ(metadata.references.marker_refs, expected_refs);
    ASSERT_TRUE(
        UpdateMediaIdentityMarkerReference(path, std::nullopt, {}, std::string{"export.markers.json"}).succeeded());
    EXPECT_EQ(ReadMediaIdentitySidecar(path).references.marker_refs,
              std::vector<std::string>{"unrelated.markers.json"});
}

TEST_F(AudioMetadataRoundtrip, SidecarAcceptsMaximumReferencesNearWireLimit) {
    ASSERT_NO_FATAL_FAILURE(WriteFixture(false));
    MediaIdentitySidecarReferences references;
    references.source_ranges_json = "[]";
    for (size_t index = 0; index < 4096; ++index)
        references.marker_refs.push_back("marker_" + std::to_string(index) + "_" + std::string(450, 'a') + ".json");
    const auto written = WriteMediaIdentitySidecar(path, manifest, {}, references);
    ASSERT_TRUE(written.succeeded()) << (written.error() ? written.error()->context : "");
    const auto size = std::filesystem::file_size(MediaIdentitySidecarPath(path));
    EXPECT_GT(size, 1800u * 1024u);
    EXPECT_LT(size, 2u * 1024u * 1024u);
    const auto read = ReadMediaIdentitySidecar(path);
    ASSERT_EQ(read.status, AudioMetadataStatus::Verified) << testing::PrintToString(read.warnings);
    EXPECT_EQ(read.references.marker_refs, references.marker_refs);
}

TEST_F(AudioMetadataRoundtrip, MarkerReferenceCancellationRejectsMissingIdentitySidecar) {
    ASSERT_NO_FATAL_FAILURE(WriteFixture(false));
    ASSERT_FALSE(std::filesystem::exists(MediaIdentitySidecarPath(path)));
    const auto result = UpdateMediaIdentityMarkerReference(path, std::nullopt, [] { return false; });
    ASSERT_FALSE(result.succeeded());
    ASSERT_NE(result.error(), nullptr);
    EXPECT_EQ(result.error()->code, EngineErrorCode::Cancelled);
    EXPECT_FALSE(std::filesystem::exists(MediaIdentitySidecarPath(path)));
}

TEST_F(AudioMetadataRoundtrip, ReferencedCompanionCopyRejectsConflictsWithoutReplacingThem) {
    const auto source_dir = exosnap_test::UniqueTempPath("reference-source");
    const auto target_dir = exosnap_test::UniqueTempPath("reference-target");
    ASSERT_TRUE(std::filesystem::create_directory(source_dir));
    ASSERT_TRUE(std::filesystem::create_directory(target_dir));
    const auto source = source_dir / "source.mp4";
    const auto output = target_dir / "output.mp4";
    const auto name = "recording.markers.json";
    {
        std::ofstream file(source_dir / name);
        file << "original marker bytes";
    }
    MediaIdentitySidecarReferences references;
    references.marker_refs = {name};
    EXPECT_TRUE(CopyMediaIdentityReferenceFiles(source, output, references).succeeded());
    EXPECT_TRUE(CopyMediaIdentityReferenceFiles(source, output, references).succeeded());
    {
        std::ofstream file(target_dir / name);
        file << "preserve conflicting markers";
    }
    EXPECT_FALSE(CopyMediaIdentityReferenceFiles(source, output, references).succeeded());
    std::ifstream file(target_dir / name);
    std::string bytes;
    std::getline(file, bytes);
    EXPECT_EQ(bytes, "preserve conflicting markers");
    file.close();
    std::filesystem::remove(source_dir / name);
    std::filesystem::remove(target_dir / name);
    std::filesystem::remove(source_dir);
    std::filesystem::remove(target_dir);
}

TEST_F(AudioMetadataRoundtrip, ReferencedCompanionRollbackRetainsPreexistingFilesAndTransfersCreatedOwnership) {
    const auto source_dir = exosnap_test::UniqueTempPath("reference-rollback-source");
    const auto target_dir = exosnap_test::UniqueTempPath("reference-rollback-target");
    ASSERT_TRUE(std::filesystem::create_directory(source_dir));
    ASSERT_TRUE(std::filesystem::create_directory(target_dir));
    const auto source = source_dir / "source.mp4";
    const auto output = target_dir / "output.mp4";
    for (const auto& file : {source_dir / "existing.markers.json", target_dir / "existing.markers.json",
                             source_dir / "created.markers.json", source_dir / "second.markers.json"}) {
        std::ofstream bytes(file);
        bytes << "unchanged marker bytes";
        ASSERT_TRUE(bytes);
    }
    MediaIdentitySidecarReferences references;
    references.marker_refs = {"existing.markers.json", "created.markers.json", "missing.markers.json"};
    {
        MediaIdentityReferenceCopyGuard guard;
        EXPECT_FALSE(CopyMediaIdentityReferenceFiles(source, output, references, &guard).succeeded());
        EXPECT_EQ(guard.created_paths, std::vector<std::filesystem::path>{target_dir / "created.markers.json"});
        EXPECT_TRUE(std::filesystem::exists(target_dir / "created.markers.json"));
    }
    EXPECT_FALSE(std::filesystem::exists(target_dir / "created.markers.json"));
    EXPECT_TRUE(std::filesystem::exists(target_dir / "existing.markers.json"));
    {
        std::ifstream existing(target_dir / "existing.markers.json");
        std::string bytes;
        std::getline(existing, bytes);
        EXPECT_EQ(bytes, "unchanged marker bytes");
    }
    references.marker_refs.pop_back();
    std::vector<std::filesystem::path> created;
    {
        MediaIdentityReferenceCopyGuard guard;
        EXPECT_TRUE(CopyMediaIdentityReferenceFiles(source, output, references, &guard).succeeded());
        EXPECT_TRUE(CopyMediaIdentityReferenceFiles(source, output, references, &guard).succeeded());
        references.marker_refs = {"second.markers.json"};
        EXPECT_TRUE(CopyMediaIdentityReferenceFiles(source, output, references, &guard).succeeded());
        created = guard.Commit();
    }
    const std::vector<std::filesystem::path> expected_created{target_dir / "created.markers.json",
                                                              target_dir / "second.markers.json"};
    EXPECT_EQ(created, expected_created);
    EXPECT_TRUE(std::filesystem::exists(target_dir / "created.markers.json"));
    for (const auto& directory : {source_dir, target_dir}) {
        std::filesystem::remove(directory / "existing.markers.json");
        std::filesystem::remove(directory / "created.markers.json");
        std::filesystem::remove(directory / "second.markers.json");
        std::filesystem::remove(directory);
    }
}

TEST_F(AudioMetadataRoundtrip, ExplicitSingleTrackAndEmptySelectionsControlRangeDecode) {
    ASSERT_NO_FATAL_FAILURE(WriteFixture());
    EditPlayerEngine player;
    std::string error;
    ASSERT_TRUE(player.Open(path, error)) << error;
    MonitorSelectionSnapshot selection{
        manifest.media_id, {1}, {{manifest.tracks[0].container.track_id, 1.0f, false}}, false};
    ASSERT_TRUE(player.SetMonitorSelection(selection, error)) << error;
    std::vector<float> samples;
    ASSERT_TRUE(player.DecodeAudioRange(4800, 480, samples, error)) << error;
    ASSERT_EQ(samples.size(), 960u);
    for (const auto sample : samples)
        EXPECT_NEAR(sample, 0.125f, 1e-6f);
    selection.revision = {2};
    selection.tracks.clear();
    ASSERT_TRUE(player.SetMonitorSelection(selection, error)) << error;
    ASSERT_TRUE(player.DecodeAudioRange(4800, 480, samples, error)) << error;
    for (const auto sample : samples)
        EXPECT_EQ(sample, 0.0f);
}

TEST_F(AudioMetadataRoundtrip, SidecarBindsFinalBytesAndDetectsStaleContent) {
    ASSERT_NO_FATAL_FAILURE(WriteFixture());
    ASSERT_TRUE(WriteMediaIdentitySidecar(path, manifest).succeeded());
    EXPECT_EQ(MediaIdentitySidecarPath(path).wstring(), path.wstring() + L".exosnap.json");
    auto metadata = ReadMediaIdentitySidecar(path);
    ASSERT_EQ(metadata.status, AudioMetadataStatus::Verified);
    ASSERT_TRUE(metadata.manifest);
    EXPECT_EQ(metadata.manifest->media_id, manifest.media_id);
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file);
        file.seekg(-1, std::ios::end);
        char byte = 0;
        file.read(&byte, 1);
        byte ^= 1;
        file.seekp(-1, std::ios::end);
        file.write(&byte, 1);
    }
    metadata = ReadMediaIdentitySidecar(path);
    EXPECT_EQ(metadata.status, AudioMetadataStatus::Stale);
    EXPECT_FALSE(metadata.manifest);
    EXPECT_TRUE(metadata.monitoring_confirmation_required);
}

TEST_F(AudioMetadataRoundtrip, UntaggedLegacyImportRequiresVisibleMonitoringConfirmation) {
    ASSERT_NO_FATAL_FAILURE(WriteFixture(false));
    EditPlayerEngine player;
    std::string error;
    ASSERT_TRUE(player.Open(path, error)) << error;
    const auto metadata = player.AudioMetadata();
    EXPECT_EQ(metadata.status, AudioMetadataStatus::Missing) << testing::PrintToString(metadata.warnings);
    EXPECT_TRUE(metadata.monitoring_confirmation_required);
    EXPECT_FALSE(metadata.manifest);
    ASSERT_EQ(player.AudioTracks().size(), 2u);
}

TEST(AudioManifest, RejectsDuplicateKeysMappingAndMalformedLengths) {
    auto manifest = Manifest();
    auto serialized = SerializeAudioManifest1(manifest);
    ASSERT_TRUE(serialized.succeeded());
    EXPECT_FALSE(ParseAudioManifest1("{\"version\":1," + serialized.value()->substr(1)).succeeded());
    auto json = nlohmann::json::parse(*serialized.value());
    json["version"] = -1;
    EXPECT_FALSE(ParseAudioManifest1(json.dump()).succeeded());
    json = nlohmann::json::parse(*serialized.value());
    json["tracks"][0]["stream_index"] = UINT64_MAX;
    EXPECT_FALSE(ParseAudioManifest1(json.dump()).succeeded());
    EXPECT_FALSE(ParseAudioManifest1(std::string(1024 * 1024 + 1, ' ')).succeeded());
    manifest.tracks[1].container.track_id = manifest.tracks[0].container.track_id;
    EXPECT_FALSE(ValidateAudioManifest1(manifest).succeeded());
    manifest = Manifest();
    manifest.tracks[0].container.name = std::string("bad\xC0\xAF");
    EXPECT_FALSE(ValidateAudioManifest1(manifest).succeeded());
}

TEST(AudioManifest, RejectsTinyJsonNodesAtTheLiveAllocationQuota) {
    const auto serialized = SerializeAudioManifest1(Manifest());
    ASSERT_TRUE(serialized.succeeded());
    std::string amplified = serialized.value()->substr(0, serialized.value()->size() - 1);
    amplified += ",\"amplification\":[";
    for (size_t index = 0; index < 500'000; ++index)
        amplified += "0,";
    amplified.back() = ']';
    amplified += '}';
    ASSERT_LT(amplified.size(), 1024u * 1024u);
    const auto result = ParseAudioManifest1(amplified);
    ASSERT_FALSE(result.succeeded());
    EXPECT_EQ(result.error()->code, EngineErrorCode::ResourceBudgetExceeded);
}

TEST(AudioManifest, JsonQuotaRefundsOriginalOwnerAfterScopeEnds) {
    MetadataJson escaped;
    std::shared_ptr<MetadataJsonBudget> original_budget;
    {
        MetadataJsonQuotaScope scope(64 * 1024);
        original_budget = scope.budget_handle();
        escaped = MetadataJson{{"payload", std::string(2048, 'x')}, {"items", MetadataJson::array()}};
        for (size_t index = 0; index < 100; ++index)
            escaped["items"].push_back(index);
        EXPECT_GT(original_budget->live_bytes(), 0u);
        {
            MetadataJsonQuotaScope nested(64);
            EXPECT_EQ(nested.budget_handle(), original_budget);
        }
    }
    {
        MetadataJsonQuotaScope separate(64);
        escaped = nullptr;
        EXPECT_EQ(original_budget->live_bytes(), 0u);
        EXPECT_EQ(separate.budget_handle()->live_bytes(), 0u);
    }
}

TEST(AudioManifest, OverlapIsConfirmedExplicitlyInEitherTrackOrder) {
    auto manifest = Manifest();
    AudioManifestSource source;
    source.id = NewId<AudioSourceId>();
    manifest.sources.push_back(source);
    manifest.tracks[1].container.source_ids.push_back(source.id);
    manifest.tracks[1].routes.push_back({NewId<AudioRouteId>(), source.id, 1.0f});
    for (bool reverse : {false, true}) {
        MonitorSelectionSnapshot selection{manifest.media_id, {1}, {}, false};
        for (const auto& track : manifest.tracks)
            selection.tracks.push_back({track.container.track_id, 1.0f, false});
        if (reverse)
            std::reverse(selection.tracks.begin(), selection.tracks.end());
        EXPECT_FALSE(ValidateMonitorSelection(selection, manifest).succeeded());
        selection.overlap_confirmed = true;
        EXPECT_TRUE(ValidateMonitorSelection(selection, manifest).succeeded());
        selection.overlap_confirmed = false;
        selection.tracks.front().muted = true;
        EXPECT_TRUE(ValidateMonitorSelection(selection, manifest).succeeded());
    }
    manifest.default_monitor_track_ids = {manifest.tracks[0].container.track_id, manifest.tracks[1].container.track_id};
    manifest.monitor_selection_origin = MonitorSelectionOrigin::User;
    EXPECT_TRUE(ValidateMonitorSelection({manifest.media_id,
                                          {1},
                                          {{manifest.tracks[0].container.track_id, 1, false},
                                           {manifest.tracks[1].container.track_id, 1, false}},
                                          false},
                                         manifest)
                    .succeeded());
}

TEST_F(AudioMetadataRoundtrip, DeclaredTrackUidMustMatchPhysicalHeader) {
    ASSERT_NO_FATAL_FAILURE(WriteFixture());
    std::ifstream input(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    input.close();
    const std::string original = "\"track_uid\":2";
    size_t matches = 0;
    for (size_t at = 0; (at = bytes.find(original, at)) != std::string::npos; at += original.size()) {
        bytes[at + original.size() - 1] = '4';
        ++matches;
    }
    ASSERT_EQ(matches, 2u);
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), bytes.size());
    }
    EditPlayerEngine player;
    std::string error;
    ASSERT_TRUE(player.Open(path, error)) << error;
    EXPECT_EQ(player.AudioMetadata().status, AudioMetadataStatus::Invalid);
    EXPECT_TRUE(player.AudioMetadata().monitoring_confirmation_required);
    EXPECT_FALSE(player.AudioMetadata().manifest);
}

TEST_F(AudioMetadataRoundtrip, AacMp4KeepsActualTitlesAndVerifiedFinalByteSidecar) {
    ASSERT_NO_FATAL_FAILURE(WriteFixture(true, true));
    const auto result = RemuxToProgressiveMp4(path, rewrite);
    ASSERT_TRUE(result.success) << result.message;
    EditPlayerEngine player;
    std::string error;
    ASSERT_TRUE(player.Open(rewrite, error)) << error;
    const auto metadata = player.AudioMetadata();
    ASSERT_EQ(metadata.status, AudioMetadataStatus::Verified) << testing::PrintToString(metadata.warnings);
    ASSERT_TRUE(metadata.manifest);
    EXPECT_NE(metadata.manifest->media_id, manifest.media_id);
    EXPECT_EQ(metadata.manifest->tracks[1].container.track_id, manifest.tracks[1].container.track_id);
    EXPECT_EQ(player.AudioTracks()[0].name, "Master");
    EXPECT_EQ(player.AudioTracks()[1].name, "Stem");
    EXPECT_EQ(metadata.manifest->tracks[0].container.codec.codec_delay_samples,
              manifest.tracks[0].container.codec.codec_delay_samples);
    EXPECT_TRUE(std::filesystem::exists(MediaIdentitySidecarPath(rewrite)));
    std::vector<float> samples;
    ASSERT_TRUE(player.DecodeAudioRange(4800, 480, samples, error)) << error;
    EXPECT_TRUE(std::any_of(samples.begin(), samples.end(), [](float sample) { return std::abs(sample) > 0.01f; }));
}

TEST_F(AudioMetadataRoundtrip, SidecarAbortNeverPublishesMonitoringAuthority) {
    ASSERT_NO_FATAL_FAILURE(WriteFixture());
    EXPECT_FALSE(WriteMediaIdentitySidecar(path, manifest, [] { return false; }).succeeded());
    EXPECT_FALSE(std::filesystem::exists(MediaIdentitySidecarPath(path)));
    EXPECT_EQ(ReadMediaIdentitySidecar(path).status, AudioMetadataStatus::Missing);
}

TEST_F(AudioMetadataRoundtrip, OpusWebmFinalByteSidecarRoundtripsActualAudioSemantics) {
    path.replace_extension(".webm");
    OpusAudioEncoder encoder;
    std::string error;
    ASSERT_TRUE(encoder.Init(48000, 2, error)) << error;
    std::vector<float> pcm(9600 * 2);
    for (size_t frame = 0; frame < pcm.size() / 2; ++frame) {
        pcm[frame * 2] = static_cast<float>(0.2 * std::sin(static_cast<double>(frame) * 0.0575958653158));
        pcm[frame * 2 + 1] = static_cast<float>(0.15 * std::sin(static_cast<double>(frame) * 0.0863937979737));
    }
    uint64_t accumulated = 0;
    std::vector<EncodedAudioPacket> encoded;
    encoder.FeedFloat32(pcm.data(), pcm.size(), 0, accumulated, 48000, 2, encoded);
    encoder.Flush(encoded);
    ASSERT_FALSE(encoded.empty());
    const auto codec_private = encoder.CodecPrivateBytes();

    AVFormatContext* output = nullptr;
    ASSERT_GE(avformat_alloc_output_context2(&output, nullptr, "webm", path.string().c_str()), 0);
    auto close_output = [](AVFormatContext* context) {
        if (context->pb)
            avio_closep(&context->pb);
        avformat_free_context(context);
    };
    std::unique_ptr<AVFormatContext, decltype(close_output)> output_guard(output, close_output);
    auto* stream = avformat_new_stream(output, nullptr);
    ASSERT_NE(stream, nullptr);
    stream->time_base = {1, 48000};
    stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    stream->codecpar->codec_id = AV_CODEC_ID_OPUS;
    stream->codecpar->sample_rate = 48000;
    av_channel_layout_default(&stream->codecpar->ch_layout, 2);
    stream->codecpar->initial_padding = static_cast<int>(encoder.CodecDelaySamples());
    stream->codecpar->seek_preroll = 3840;
    stream->codecpar->extradata =
        static_cast<uint8_t*>(av_mallocz(codec_private.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    ASSERT_NE(stream->codecpar->extradata, nullptr);
    stream->codecpar->extradata_size = static_cast<int>(codec_private.size());
    std::memcpy(stream->codecpar->extradata, codec_private.data(), codec_private.size());
    ASSERT_GE(av_dict_set(&stream->metadata, "title", "WebM Mix", 0), 0);
    ASSERT_GE(avio_open(&output->pb, path.string().c_str(), AVIO_FLAG_WRITE), 0);
    ASSERT_GE(avformat_write_header(output, nullptr), 0);
    auto free_packet = [](AVPacket* packet) { av_packet_free(&packet); };
    std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
    ASSERT_NE(packet, nullptr);
    for (const auto& audio : encoded) {
        ASSERT_GE(av_new_packet(packet.get(), static_cast<int>(audio.bytes.size())), 0);
        std::memcpy(packet->data, audio.bytes.data(), audio.bytes.size());
        packet->stream_index = stream->index;
        packet->pts = av_rescale_q(static_cast<int64_t>(audio.pts_ns), {1, 1000000000}, stream->time_base) -
                      av_rescale_q(encoder.CodecDelaySamples(), {1, 48000}, stream->time_base);
        packet->dts = packet->pts;
        packet->duration = av_rescale_q(encoder.FrameSizeSamples(), {1, 48000}, stream->time_base);
        packet->flags = AV_PKT_FLAG_KEY;
        ASSERT_GE(av_interleaved_write_frame(output, packet.get()), 0);
    }
    ASSERT_GE(av_write_trailer(output), 0);
    ASSERT_GE(avio_closep(&output->pb), 0);
    output_guard.reset();

    auto close_input = [](AVFormatContext* context) { avformat_close_input(&context); };
    AVFormatContext* input = nullptr;
    ASSERT_GE(avformat_open_input(&input, path.string().c_str(), nullptr, nullptr), 0);
    std::unique_ptr<AVFormatContext, decltype(close_input)> input_guard(input, close_input);
    ASSERT_GE(avformat_find_stream_info(input, nullptr), 0);
    ASSERT_EQ(input->nb_streams, 1u);
    const auto actual = ReadAudioMediaMetadata(path, input);
    ASSERT_EQ(actual.status, AudioMetadataStatus::Missing) << testing::PrintToString(actual.warnings);
    EXPECT_TRUE(actual.monitoring_confirmation_required);
    ASSERT_EQ(actual.actual_tracks.size(), 1u);
    EXPECT_EQ(actual.actual_tracks[0].name, "WebM Mix");
    EXPECT_EQ(actual.actual_tracks[0].codec.codec_id, "A_OPUS");
    EXPECT_EQ(actual.actual_tracks[0].codec.codec_private, codec_private);
    EXPECT_EQ(actual.actual_tracks[0].codec.sample_rate, 48000u);
    EXPECT_EQ(actual.actual_tracks[0].codec.channels, 2u);
    EXPECT_EQ(actual.actual_tracks[0].codec.channel_layout, "stereo");
    EXPECT_EQ(actual.actual_tracks[0].codec.bit_depth, 0u);
    EXPECT_EQ(actual.actual_tracks[0].codec.codec_delay_samples, encoder.CodecDelaySamples());
    ASSERT_GT(actual.actual_tracks[0].track_uid, 1u);
    input_guard.reset();
    manifest.sources.clear();
    manifest.tracks.resize(1);
    manifest.tracks[0] = {};
    manifest.tracks[0].container = actual.actual_tracks[0];
    manifest.tracks[0].container.track_id = NewId<AudioTrackId>();
    manifest.tracks[0].container.role = "mix";
    manifest.default_monitor_track_ids = {manifest.tracks[0].container.track_id};
    const auto written = WriteMediaIdentitySidecar(path, manifest);
    ASSERT_TRUE(written.succeeded()) << (written.error() ? written.error()->context : "");

    input = nullptr;
    ASSERT_GE(avformat_open_input(&input, path.string().c_str(), nullptr, nullptr), 0);
    input_guard.reset(input);
    ASSERT_GE(avformat_find_stream_info(input, nullptr), 0);
    ASSERT_EQ(input->nb_streams, 1u);
    const auto verified = ReadAudioMediaMetadata(path, input);
    ASSERT_EQ(verified.status, AudioMetadataStatus::Verified) << testing::PrintToString(verified.warnings);
    ASSERT_TRUE(verified.manifest);
    EXPECT_EQ(verified.selection_media_id, manifest.media_id);
    EXPECT_EQ(verified.manifest->media_id, manifest.media_id);
    EXPECT_EQ(verified.manifest->recording_id, manifest.recording_id);
    EXPECT_EQ(verified.manifest->segment_id, manifest.segment_id);
    EXPECT_EQ(verified.manifest->default_monitor_track_ids, manifest.default_monitor_track_ids);
    EXPECT_FALSE(verified.monitoring_confirmation_required);
    ASSERT_EQ(verified.actual_tracks.size(), 1u);
    EXPECT_EQ(verified.actual_tracks[0].track_id, manifest.tracks[0].container.track_id);
    EXPECT_EQ(verified.actual_tracks[0].track_uid, actual.actual_tracks[0].track_uid);
    EXPECT_EQ(verified.actual_tracks[0].name, "WebM Mix");
    EXPECT_EQ(verified.actual_tracks[0].role, "mix");
    const auto expected_json = SerializeAudioManifest1(manifest);
    const auto verified_json = SerializeAudioManifest1(*verified.manifest);
    ASSERT_TRUE(expected_json.succeeded());
    ASSERT_TRUE(verified_json.succeeded());
    EXPECT_EQ(*verified_json.value(), *expected_json.value());
    const auto* title = av_dict_get(input->streams[0]->metadata, "title", nullptr, 0);
    ASSERT_NE(title, nullptr);
    EXPECT_STREQ(title->value, "WebM Mix");

    const auto* codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    ASSERT_NE(codec, nullptr);
    auto free_decoder = [](AVCodecContext* context) { avcodec_free_context(&context); };
    std::unique_ptr<AVCodecContext, decltype(free_decoder)> decoder(avcodec_alloc_context3(codec), free_decoder);
    ASSERT_NE(decoder, nullptr);
    ASSERT_GE(avcodec_parameters_to_context(decoder.get(), input->streams[0]->codecpar), 0);
    ASSERT_GE(avcodec_open2(decoder.get(), codec, nullptr), 0);
    auto free_frame = [](AVFrame* frame) { av_frame_free(&frame); };
    std::unique_ptr<AVFrame, decltype(free_frame)> frame(av_frame_alloc(), free_frame);
    ASSERT_NE(frame, nullptr);
    size_t decoded_samples = 0;
    double energy = 0;
    bool finite = true;
    int read_result = 0;
    while ((read_result = av_read_frame(input, packet.get())) >= 0) {
        ASSERT_GE(avcodec_send_packet(decoder.get(), packet.get()), 0);
        av_packet_unref(packet.get());
        int result = 0;
        while ((result = avcodec_receive_frame(decoder.get(), frame.get())) >= 0) {
            const auto format = static_cast<AVSampleFormat>(frame->format);
            ASSERT_EQ(av_get_packed_sample_fmt(format), AV_SAMPLE_FMT_FLT);
            ASSERT_EQ(frame->ch_layout.nb_channels, 2);
            const bool planar = av_sample_fmt_is_planar(format) != 0;
            for (int sample = 0; sample < frame->nb_samples; ++sample) {
                for (int channel = 0; channel < 2; ++channel) {
                    const auto* values = reinterpret_cast<const float*>(frame->extended_data[planar ? channel : 0]);
                    const float value = values[planar ? sample : sample * 2 + channel];
                    finite = finite && std::isfinite(value);
                    energy += static_cast<double>(value) * value;
                    ++decoded_samples;
                }
            }
            av_frame_unref(frame.get());
        }
        ASSERT_EQ(result, AVERROR(EAGAIN));
    }
    EXPECT_EQ(read_result, AVERROR_EOF);
    EXPECT_GT(decoded_samples, 0u);
    EXPECT_TRUE(finite);
    EXPECT_GT(energy, 1e-8);
}

TEST_F(AudioMetadataRoundtrip, VerifiedMp4ToMkvRetainsSemanticTracksAndPhysicalAuthority) {
    ASSERT_NO_FATAL_FAILURE(WriteFixture(true, true));
    auto result = RemuxToProgressiveMp4(path, rewrite);
    ASSERT_TRUE(result.success) << result.message;
    EditPlayerEngine player;
    std::string error;
    ASSERT_TRUE(player.Open(rewrite, error)) << error;
    const auto mp4 = player.AudioMetadata();
    ASSERT_EQ(mp4.status, AudioMetadataStatus::Verified) << testing::PrintToString(mp4.warnings);
    ASSERT_TRUE(mp4.manifest);
    player.Close();
    result = RemuxToMkv(rewrite, roundtrip);
    ASSERT_TRUE(result.success) << result.message;
    ASSERT_TRUE(player.Open(roundtrip, error)) << error;
    const auto mkv = player.AudioMetadata();
    ASSERT_EQ(mkv.status, AudioMetadataStatus::Verified) << testing::PrintToString(mkv.warnings);
    ASSERT_TRUE(mkv.manifest);
    EXPECT_NE(mkv.manifest->media_id, mp4.manifest->media_id);
    ASSERT_EQ(mkv.actual_tracks.size(), manifest.tracks.size());
    for (size_t index = 0; index < mkv.actual_tracks.size(); ++index) {
        EXPECT_EQ(mkv.actual_tracks[index].track_id, manifest.tracks[index].container.track_id);
        EXPECT_EQ(mkv.actual_tracks[index].name, manifest.tracks[index].container.name);
        EXPECT_EQ(mkv.actual_tracks[index].role, manifest.tracks[index].container.role);
        EXPECT_EQ(mkv.actual_tracks[index].track_uid, 2 + index);
        EXPECT_EQ(mkv.actual_tracks[index].codec.codec_delay_samples,
                  manifest.tracks[index].container.codec.codec_delay_samples);
        EXPECT_EQ(mkv.actual_tracks[index].codec.bit_depth, 0u);
        EXPECT_EQ(mkv.actual_tracks[index].codec.channel_layout, "stereo");
    }
}

TEST_F(AudioMetadataRoundtrip, ConcatenationMapsReorderedStemsBySemanticIdentity) {
    ASSERT_NO_FATAL_FAILURE(WriteFixture());
    const auto original = manifest;
    manifest.media_id = NewId<MediaId>();
    std::swap(manifest.tracks[0], manifest.tracks[1]);
    for (size_t index = 0; index < manifest.tracks.size(); ++index) {
        manifest.tracks[index].container.stream_index.value = static_cast<uint32_t>(index);
        manifest.tracks[index].container.track_uid = 2 + index;
    }
    ASSERT_NO_FATAL_FAILURE(WriteFixture(true, false, rewrite));
    const auto first = ProbeEditMedia(path);
    const auto second = ProbeEditMedia(rewrite);
    ASSERT_TRUE(first.error.empty()) << first.error;
    ASSERT_TRUE(second.error.empty()) << second.error;
    const std::vector<TimelineExportClip> clips{{path, 0, first.duration_us, 0},
                                                {rewrite, 0, second.duration_us, first.duration_us}};
    const auto assessment = AssessTimelineExport(clips, false);
    ASSERT_EQ(assessment.eligibility, TimelineExportEligibility::StreamCopy) << assessment.reason;
    const auto exported = ExportTimelineStreamCopy(clips, roundtrip, false);
    ASSERT_TRUE(exported.success) << exported.message;
    EditPlayerEngine player;
    std::string error;
    ASSERT_TRUE(player.Open(roundtrip, error)) << error;
    const auto metadata = player.AudioMetadata();
    ASSERT_EQ(metadata.status, AudioMetadataStatus::Verified) << testing::PrintToString(metadata.warnings);
    ASSERT_TRUE(metadata.manifest);
    EXPECT_EQ(metadata.manifest->tracks[0].container.track_id, original.tracks[0].container.track_id);
    EXPECT_EQ(metadata.manifest->tracks[1].container.track_id, original.tracks[1].container.track_id);
    EXPECT_EQ(metadata.manifest->default_monitor_track_ids, original.default_monitor_track_ids);
    std::vector<float> samples;
    const auto next_source_sample = av_rescale_q(first.duration_us, {1, 1000000}, {1, 48000}) + 4800;
    ASSERT_TRUE(player.DecodeAudioRange(next_source_sample, 480, samples, error)) << error;
    for (float sample : samples)
        EXPECT_NEAR(sample, 0.125f, 1e-6f);
}

TEST_F(AudioMetadataRoundtrip, LegacyConfirmationUsesPerOpenTrackIdsAndRejectsStaleSelection) {
    ASSERT_NO_FATAL_FAILURE(WriteFixture(false));
    EditPlayerEngine player;
    std::string error;
    ASSERT_TRUE(player.Open(path, error)) << error;
    const auto imported = player.AudioMetadata();
    MonitorSelectionSnapshot selection{
        imported.selection_media_id, {1}, {{imported.actual_tracks[0].track_id, 0.5f, false}}, false};
    EXPECT_FALSE(player.SetMonitorSelection(selection, error));
    selection.overlap_confirmed = true;
    ASSERT_TRUE(player.SetMonitorSelection(selection, error)) << error;
    std::vector<float> samples;
    ASSERT_TRUE(player.DecodeAudioRange(4800, 480, samples, error)) << error;
    for (float sample : samples)
        EXPECT_NEAR(sample, 0.0625f, 1e-6f);
    player.Close();
    ASSERT_TRUE(player.Open(path, error)) << error;
    EXPECT_FALSE(player.SetMonitorSelection(selection, error));
}

TEST_F(AudioMetadataRoundtrip, LegacyAssetBindingSafelyReusesConfirmedSelectionAfterReopen) {
    ASSERT_NO_FATAL_FAILURE(WriteFixture(false));
    EditPlayerEngine player;
    std::string error;
    ASSERT_TRUE(player.Open(path, error)) << error;
    const auto metadata = player.AudioMetadata();
    ASSERT_TRUE(metadata.unverified_asset_binding);
    MonitorSelectionSnapshot selection{metadata.selection_media_id,
                                       {1},
                                       {{metadata.actual_tracks[0].track_id, 0.5f, false}},
                                       true,
                                       metadata.unverified_asset_binding};
    player.Close();
    ASSERT_TRUE(player.Open(path, error)) << error;
    ASSERT_TRUE(player.SetMonitorSelection(selection, error)) << error;
    std::vector<float> samples;
    ASSERT_TRUE(player.DecodeAudioRange(4800, 480, samples, error)) << error;
    for (float sample : samples)
        EXPECT_NEAR(sample, 0.0625f, 1e-6f);
    player.Close();
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        file.seekg(-1, std::ios::end);
        char byte = 0;
        file.read(&byte, 1);
        byte ^= 1;
        file.seekp(-1, std::ios::end);
        file.write(&byte, 1);
    }
    ASSERT_TRUE(player.Open(path, error)) << error;
    EXPECT_FALSE(player.SetMonitorSelection(selection, error));
}

TEST_F(AudioMetadataRoundtrip, LiveCapabilitiesShareOneVerificationAcrossConcurrentConsumers) {
    WriteFixture(false);
    const auto first = AcquireMediaReadCapability(path);
    const auto second = AcquireMediaReadCapability(path);
    ASSERT_TRUE(first.succeeded());
    ASSERT_TRUE(second.succeeded());
    EXPECT_EQ(*first.value(), *second.value());
    std::vector<std::thread> consumers;
    std::atomic<int> successes{0};
    for (int index = 0; index < 8; ++index)
        consumers.emplace_back([&] {
            if (VerifyMediaIdentity(*first.value()).succeeded())
                ++successes;
        });
    for (auto& consumer : consumers)
        consumer.join();
    EXPECT_EQ(successes.load(), 8);
    const auto measurements = (*first.value())->Measurements();
    EXPECT_EQ(measurements.hash_completions, 1);
    EXPECT_EQ(measurements.hash_bytes, std::filesystem::file_size(path));
}

TEST_F(AudioMetadataRoundtrip, VerifiedCapabilityRejectsIndependentExpectedDigestMismatch) {
    WriteFixture(false);
    const auto access = AcquireMediaReadCapability(path);
    ASSERT_TRUE(access.succeeded());
    const auto observed = VerifyMediaIdentity(*access.value());
    ASSERT_TRUE(observed.succeeded());
    auto expected = (*observed.value())->Identity().sha256;
    expected[0] ^= 1;
    EXPECT_FALSE(VerifyMediaIdentity(*access.value(), expected).succeeded());
    EXPECT_TRUE(VerifyMediaIdentity(*access.value(), (*observed.value())->Identity().sha256).succeeded());
    EXPECT_EQ((*access.value())->Measurements().hash_completions, 1);
}

TEST_F(AudioMetadataRoundtrip, ExplicitOffsetReadersNeverShareASeekPosition) {
    WriteFixture(false);
    const auto access = AcquireMediaReadCapability(path);
    ASSERT_TRUE(access.succeeded());
    std::ifstream input(path, std::ios::binary);
    const std::vector<uint8_t> expected((std::istreambuf_iterator<char>(input)), {});
    ASSERT_GT(expected.size(), 64);
    std::atomic<int> mismatches{0};
    std::vector<std::thread> readers;
    for (int index = 0; index < 4; ++index)
        readers.emplace_back([&, index] {
            std::array<uint8_t, 32> bytes{};
            for (int read = 0; read < 32; ++read) {
                const auto offset = (static_cast<size_t>(read) * 113 + index * 31) % (expected.size() - bytes.size());
                const auto result = (*access.value())->ReadAt(offset, bytes);
                if (!result.succeeded() || *result.value() != bytes.size() ||
                    !std::equal(bytes.begin(), bytes.end(), expected.begin() + static_cast<ptrdiff_t>(offset)))
                    ++mismatches;
            }
        });
    for (auto& reader : readers)
        reader.join();
    EXPECT_EQ(mismatches.load(), 0);
}

TEST_F(AudioMetadataRoundtrip, CancelledVerificationKeepsCapabilityReadableAndRetryable) {
    WriteFixture(false);
    const auto access = AcquireMediaReadCapability(path);
    ASSERT_TRUE(access.succeeded());
    std::atomic<bool> cancelled{true};
    EXPECT_FALSE(VerifyMediaIdentity(*access.value(), std::nullopt, {}, {}, &cancelled).succeeded());
    std::array<uint8_t, 4> bytes{};
    EXPECT_TRUE((*access.value())->ReadAt(0, bytes).succeeded());
    cancelled.store(false);
    EXPECT_TRUE(VerifyMediaIdentity(*access.value(), std::nullopt, {}, {}, &cancelled).succeeded());
    EXPECT_EQ((*access.value())->Measurements().hash_completions, 1);
}

TEST_F(AudioMetadataRoundtrip, ThrowingAdmissionReleasesTheSharedVerificationJobForRetry) {
    WriteFixture(false);
    const auto maximum_checks = 1 + (std::filesystem::file_size(path) + 65535) / 65536;
    const auto access = AcquireMediaReadCapability(path);
    ASSERT_TRUE(access.succeeded());
    const BackgroundIoAdmission admission = [](uint64_t, const std::atomic<bool>*) -> bool {
        throw std::runtime_error("injected admission failure");
    };
    EXPECT_THROW((void)VerifyMediaIdentity(*access.value(), std::nullopt, {}, admission), std::runtime_error);
    EXPECT_EQ((*access.value())->Measurements().hash_completions, 0);
    std::array<uint8_t, 4> header{};
    EXPECT_TRUE((*access.value())->ReadAt(0, header).succeeded());
    size_t continuation_checks = 0;
    const auto retried =
        VerifyMediaIdentity(*access.value(), std::nullopt, [&] { return ++continuation_checks <= maximum_checks; });
    EXPECT_TRUE(retried.succeeded());
    const auto measurements = (*access.value())->Measurements();
    EXPECT_EQ(measurements.hash_starts, 2);
    EXPECT_EQ(measurements.hash_completions, 1);
    EXPECT_EQ(measurements.hash_bytes, std::filesystem::file_size(path));
}

TEST_F(AudioMetadataRoundtrip, PathConfirmedLegacySelectionVerifiesAFreshProvisionalCapabilityBeforeReuse) {
    WriteFixture(false);
    EditPlayerEngine conservative;
    std::string error;
    ASSERT_TRUE(conservative.Open(path, error)) << error;
    const auto metadata = conservative.AudioMetadata();
    ASSERT_TRUE(metadata.unverified_asset_binding);
    ASSERT_NE(metadata.unverified_asset_binding->sha256, (std::array<uint8_t, 32>{}));
    MonitorSelectionSnapshot selection{metadata.selection_media_id,
                                       {1},
                                       {{metadata.actual_tracks[0].track_id, 0.5f, false}},
                                       true,
                                       metadata.unverified_asset_binding};
    conservative.Close();
    const auto access = AcquireMediaReadCapability(path);
    ASSERT_TRUE(access.succeeded());
    const auto provisional = ProbeEditMedia(*access.value(), {}, {}, nullptr, false);
    ASSERT_TRUE(provisional.audio_metadata.unverified_asset_binding);
    ASSERT_EQ(provisional.audio_metadata.unverified_asset_binding->sha256, (std::array<uint8_t, 32>{}));
    EditPlayerEngine leased;
    ASSERT_TRUE(leased.Open(*access.value(), error)) << error;
    EXPECT_EQ((*access.value())->Measurements().hash_starts, 0);
    std::atomic<bool> cancelled{true};
    EXPECT_FALSE(leased.SetMonitorSelection(selection, error, &cancelled));
    EXPECT_EQ((*access.value())->Measurements().hash_starts, 0);
    ASSERT_TRUE(leased.SetMonitorSelection(selection, error)) << error;
    EXPECT_EQ((*access.value())->Measurements().hash_completions, 1);
    std::vector<float> samples;
    ASSERT_TRUE(leased.DecodeAudioRange(4800, 480, samples, error)) << error;
    for (const auto sample : samples)
        EXPECT_NEAR(sample, 0.0625f, 1e-6f);
    EXPECT_TRUE(leased.SetMonitorSelection(selection, error)) << error;
    EXPECT_EQ((*access.value())->Measurements().hash_completions, 1);
}

TEST_F(AudioMetadataRoundtrip, ReleasedLeaseDoesNotTrustRestoredSizeAndWriteTime) {
    WriteFixture(false);
    const auto original_time = std::filesystem::last_write_time(path);
    std::array<uint8_t, 32> expected{};
    uint64_t generation = 0;
    {
        const auto access = AcquireMediaReadCapability(path);
        ASSERT_TRUE(access.succeeded());
        generation = (*access.value())->Generation();
        const auto identity = VerifyMediaIdentity(*access.value());
        ASSERT_TRUE(identity.succeeded());
        expected = (*identity.value())->Identity().sha256;
    }
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.is_open());
        file.seekg(-1, std::ios::end);
        char byte = 0;
        file.read(&byte, 1);
        file.seekp(-1, std::ios::end);
        byte ^= 1;
        file.write(&byte, 1);
    }
    std::filesystem::last_write_time(path, original_time);
    const auto reopened = AcquireMediaReadCapability(path);
    ASSERT_TRUE(reopened.succeeded());
    EXPECT_NE((*reopened.value())->Generation(), generation);
    EXPECT_FALSE(VerifyMediaIdentity(*reopened.value(), expected).succeeded());
    EXPECT_EQ((*reopened.value())->Measurements().hash_completions, 1);
}

TEST_F(AudioMetadataRoundtrip, LiveCapabilityRejectsWritersAndReplacement) {
    WriteFixture(false);
    const auto access = AcquireMediaReadCapability(path);
    ASSERT_TRUE(access.succeeded());
    std::fstream writer(path, std::ios::binary | std::ios::in | std::ios::out);
    EXPECT_FALSE(writer.is_open());
    std::error_code error;
    std::filesystem::rename(path, rewrite, error);
    EXPECT_TRUE(error);
    error.clear();
    EXPECT_FALSE(std::filesystem::remove(path, error));
    EXPECT_TRUE(error);
}

TEST_F(AudioMetadataRoundtrip, ProvisionalProbeDoesNotHashOrPublishContentIdentity) {
    WriteFixture(false);
    const auto access = AcquireMediaReadCapability(path);
    ASSERT_TRUE(access.succeeded());
    const auto provisional = ProbeEditMedia(*access.value(), {}, {}, nullptr, false);
    EXPECT_TRUE(provisional.error.empty()) << provisional.error;
    EXPECT_FALSE(provisional.content_identity_valid);
    EXPECT_EQ((*access.value())->Measurements().hash_starts, 0);
    const auto verified = ProbeEditMedia(*access.value());
    EXPECT_TRUE(verified.error.empty()) << verified.error;
    EXPECT_TRUE(verified.content_identity_valid);
    EXPECT_EQ((*access.value())->Measurements().hash_completions, 1);
}

TEST_F(AudioMetadataRoundtrip, ConcurrentProbesPublishOneFrozenAudioMetadataSnapshot) {
    WriteFixture(false);
    const auto access = AcquireMediaReadCapability(path);
    ASSERT_TRUE(access.succeeded());
    constexpr size_t count = 8;
    std::barrier header_gate(static_cast<ptrdiff_t>(count));
    std::array<EditMediaMetadata, count> results;
    std::vector<std::thread> workers;
    for (size_t index = 0; index < count; ++index)
        workers.emplace_back([&, index] {
            bool joined = false;
            results[index] = ProbeEditMedia(
                *access.value(), {},
                [&](uint64_t bytes, const std::atomic<bool>*) {
                    if (bytes == 12 && !joined) {
                        joined = true;
                        header_gate.arrive_and_wait();
                    }
                    return true;
                },
                nullptr, false);
        });
    for (auto& worker : workers)
        worker.join();
    ASSERT_EQ(results[0].audio_metadata.actual_tracks.size(), 2);
    for (const auto& result : results) {
        ASSERT_TRUE(result.error.empty()) << result.error;
        ASSERT_EQ(result.audio_metadata.actual_tracks.size(), 2);
        EXPECT_EQ(result.audio_metadata.actual_tracks[0].track_id, results[0].audio_metadata.actual_tracks[0].track_id);
        EXPECT_EQ(result.audio_metadata.actual_tracks[1].track_id, results[0].audio_metadata.actual_tracks[1].track_id);
    }
}

TEST_F(AudioMetadataRoundtrip, HardlinkAliasesKeepTheirOwnSidecarMetadataSnapshot) {
    WriteFixture(false);
    std::filesystem::create_hard_link(path, rewrite);
    ASSERT_TRUE(WriteMediaIdentitySidecar(path, manifest).succeeded());
    const auto original = AcquireMediaReadCapability(path);
    const auto alias = AcquireMediaReadCapability(rewrite);
    ASSERT_TRUE(original.succeeded());
    ASSERT_TRUE(alias.succeeded());
    const auto original_metadata = ProbeEditMedia(*original.value(), {}, {}, nullptr, false);
    const auto alias_metadata = ProbeEditMedia(*alias.value(), {}, {}, nullptr, false);
    ASSERT_TRUE(original_metadata.error.empty()) << original_metadata.error;
    ASSERT_TRUE(alias_metadata.error.empty()) << alias_metadata.error;
    EXPECT_TRUE(original_metadata.audio_metadata.manifest.has_value());
    EXPECT_FALSE(alias_metadata.audio_metadata.manifest.has_value());
    EXPECT_EQ((*alias.value())->Path(), rewrite);
}

TEST_F(AudioMetadataRoundtrip, WritableMappingPreventsImmutableCapabilityAdmission) {
    WriteFixture(false);
    HANDLE file =
        CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(file, INVALID_HANDLE_VALUE);
    std::unique_ptr<void, decltype(&CloseHandle)> writer(file, CloseHandle);
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, 0, nullptr);
    ASSERT_NE(mapping, nullptr);
    std::unique_ptr<void, decltype(&CloseHandle)> section(mapping, CloseHandle);
    void* view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 0);
    ASSERT_NE(view, nullptr);
    std::unique_ptr<void, decltype(&UnmapViewOfFile)> mapped(view, UnmapViewOfFile);
    writer.reset();
    EXPECT_FALSE(AcquireMediaReadCapability(path).succeeded());
    mapped.reset();
    section.reset();
    EXPECT_TRUE(AcquireMediaReadCapability(path).succeeded());
}

TEST_F(AudioMetadataRoundtrip, VerificationUsesTheFrozenSidecarSnapshot) {
    WriteFixture(false);
    ASSERT_TRUE(WriteMediaIdentitySidecar(path, manifest).succeeded());
    const auto access = AcquireMediaReadCapability(path);
    ASSERT_TRUE(access.succeeded());
    const auto provisional = ProbeEditMedia(*access.value(), {}, {}, nullptr, false);
    ASSERT_TRUE(provisional.audio_metadata.manifest.has_value());
    auto replacement = manifest;
    replacement.media_id = NewId<MediaId>();
    ASSERT_TRUE(WriteMediaIdentitySidecar(path, replacement, {}, MediaIdentitySidecarReferences{}).succeeded());
    const auto verified = ProbeEditMedia(*access.value());
    ASSERT_TRUE(verified.error.empty()) << verified.error;
    ASSERT_TRUE(verified.audio_metadata.manifest.has_value());
    EXPECT_EQ(verified.audio_metadata.manifest->media_id, manifest.media_id);
    EXPECT_NE(verified.audio_metadata.manifest->media_id, replacement.media_id);
}

TEST_F(AudioMetadataRoundtrip, PlaybackAndWaveformReuseTheAdmittedVerification) {
    WriteFixture(false);
    const auto access = AcquireMediaReadCapability(path);
    ASSERT_TRUE(access.succeeded());
    const auto metadata = ProbeEditMedia(*access.value());
    ASSERT_TRUE(metadata.error.empty()) << metadata.error;
    EditPlayerEngine player;
    std::string error;
    ASSERT_TRUE(player.Open(*access.value(), error)) << error;
    ASSERT_TRUE(player.ValidateContentIdentity(metadata.content_sha256, error)) << error;
    const auto stream = std::find_if(metadata.streams.begin(), metadata.streams.end(),
                                     [](const auto& value) { return value.kind == TimelineStreamKind::Audio; });
    ASSERT_NE(stream, metadata.streams.end());
    auto waveform =
        OpenEditWaveformDecoder(*access.value(), stream->container_index, error, nullptr, &metadata.content_sha256);
    ASSERT_NE(waveform, nullptr) << error;
    std::vector<float> samples;
    EXPECT_TRUE(waveform->Decode(0, 480, samples, error)) << error;
    EXPECT_EQ((*access.value())->Measurements().hash_completions, 1);
    EXPECT_EQ((*access.value())->Measurements().hash_bytes, std::filesystem::file_size(path));
}

TEST_F(AudioMetadataRoundtrip, CanonicalProvisionalMonitoringChoiceSurvivesSameLeaseVerification) {
    WriteFixture(false);
    const auto access = AcquireMediaReadCapability(path);
    ASSERT_TRUE(access.succeeded());
    const auto provisional = ProbeEditMedia(*access.value(), {}, {}, nullptr, false);
    ASSERT_TRUE(provisional.error.empty()) << provisional.error;
    ASSERT_NE(provisional.audio_metadata.unverified_asset_binding, nullptr);
    EditPlayerEngine player;
    std::string error;
    ASSERT_TRUE(player.Open(*access.value(), error)) << error;
    MonitorSelectionSnapshot selection{provisional.audio_metadata.selection_media_id,
                                       {1},
                                       {{provisional.audio_metadata.actual_tracks[0].track_id, 1, false}},
                                       true,
                                       provisional.audio_metadata.unverified_asset_binding};
    ASSERT_TRUE(player.SetMonitorSelection(selection, error)) << error;
    const auto verified = ProbeEditMedia(*access.value());
    ASSERT_TRUE(verified.error.empty()) << verified.error;
    player.Close();
    ASSERT_TRUE(player.Open(*access.value(), error)) << error;
    ASSERT_TRUE(player.SetMonitorSelection(selection, error)) << error;
    std::vector<float> samples;
    ASSERT_TRUE(player.DecodeAudioRange(4800, 480, samples, error)) << error;
    for (float sample : samples)
        EXPECT_FLOAT_EQ(sample, 0.125f);
    EXPECT_EQ((*access.value())->Measurements().hash_completions, 1);
}

TEST_F(AudioMetadataRoundtrip, StampOnlyProvisionalMonitoringBindingCannotImpersonateCanonicalSource) {
    WriteFixture(false);
    const auto access = AcquireMediaReadCapability(path);
    ASSERT_TRUE(access.succeeded());
    const auto metadata = ProbeEditMedia(*access.value(), {}, {}, nullptr, false);
    ASSERT_NE(metadata.audio_metadata.unverified_asset_binding, nullptr);
    auto forged = std::make_shared<UnverifiedAudioAssetBinding>(*metadata.audio_metadata.unverified_asset_binding);
    MonitorSelectionSnapshot selection{metadata.audio_metadata.selection_media_id,
                                       {1},
                                       {{metadata.audio_metadata.actual_tracks[0].track_id, 1, false}},
                                       true,
                                       forged};
    EditPlayerEngine player;
    std::string error;
    ASSERT_TRUE(player.Open(*access.value(), error)) << error;
    EXPECT_FALSE(player.SetMonitorSelection(selection, error));
}

TEST_F(AudioMetadataRoundtrip, GlobalHashMeasurementsIncludeConservativeAndCoalescedLeaseWork) {
    WriteFixture(false);
    const auto before = GlobalMediaHashMeasurements();
    const auto conservative = HashFinalMedia(path);
    ASSERT_TRUE(conservative.succeeded());
    const auto access = AcquireMediaReadCapability(path);
    ASSERT_TRUE(access.succeeded());
    ASSERT_TRUE(VerifyMediaIdentity(*access.value()).succeeded());
    ASSERT_TRUE(VerifyMediaIdentity(*access.value()).succeeded());
    const auto after = GlobalMediaHashMeasurements();
    EXPECT_EQ(after.total.starts - before.total.starts, 2);
    EXPECT_EQ(after.total.completions - before.total.completions, 2);
    EXPECT_EQ(after.total.bytes - before.total.bytes, 2 * std::filesystem::file_size(path));
    const auto legacy = static_cast<size_t>(MediaHashCause::Unspecified);
    const auto live = static_cast<size_t>(MediaHashCause::LiveLease);
    EXPECT_EQ(after.by_cause[legacy].completions - before.by_cause[legacy].completions, 1);
    EXPECT_EQ(after.by_cause[live].completions - before.by_cause[live].completions, 1);
}

TEST_F(AudioMetadataRoundtrip, GlobalHashMeasurementsRetainPartialCancelledReadWork) {
    WriteFixture(false);
    const auto before = GlobalMediaHashMeasurements();
    int reads = 0;
    const auto cancelled = HashFinalMedia(path, {}, [&](uint64_t, const std::atomic<bool>*) { return ++reads < 2; });
    ASSERT_FALSE(cancelled.succeeded());
    EXPECT_EQ(cancelled.error()->code, EngineErrorCode::Cancelled);
    const auto after = GlobalMediaHashMeasurements();
    EXPECT_EQ(after.total.starts - before.total.starts, 1);
    EXPECT_EQ(after.total.completions - before.total.completions, 0);
    EXPECT_EQ(after.total.cancellations - before.total.cancellations, 1);
    EXPECT_EQ(after.total.bytes - before.total.bytes, 64 * 1024);
}

TEST_F(AudioMetadataRoundtrip, ReapplyingIdenticalLiveMonitoringSelectionIsIdempotent) {
    WriteFixture(false);
    const auto access = AcquireMediaReadCapability(path);
    ASSERT_TRUE(access.succeeded());
    const auto metadata = ProbeEditMedia(*access.value(), {}, {}, nullptr, false);
    ASSERT_NE(metadata.audio_metadata.unverified_asset_binding, nullptr);
    MonitorSelectionSnapshot selection{metadata.audio_metadata.selection_media_id,
                                       {1},
                                       {{metadata.audio_metadata.actual_tracks[0].track_id, 1, false}},
                                       true,
                                       metadata.audio_metadata.unverified_asset_binding};
    EditPlayerEngine player;
    std::string error;
    ASSERT_TRUE(player.Open(*access.value(), error)) << error;
    ASSERT_TRUE(player.SetMonitorSelection(selection, error)) << error;
    ASSERT_TRUE(ProbeEditMedia(*access.value()).error.empty());
    EXPECT_TRUE(player.SetMonitorSelection(selection, error)) << error;
    auto conflict = selection;
    conflict.tracks[0].gain_linear = 0.5f;
    EXPECT_FALSE(player.SetMonitorSelection(conflict, error));
    auto stale = selection;
    stale.revision = {0};
    EXPECT_FALSE(player.SetMonitorSelection(stale, error));
}

TEST_F(AudioMetadataRoundtrip, ExportAssessmentAndExecutionReuseTheExactAdmittedCapability) {
    WriteFixture(false);
    const auto acquired = AcquireMediaReadCapability(path);
    ASSERT_TRUE(acquired.succeeded());
    const auto access = *acquired.value();
    const auto metadata = ProbeEditMedia(access);
    ASSERT_TRUE(metadata.error.empty()) << metadata.error;
    const auto proof = VerifyMediaIdentity(access, metadata.content_sha256);
    ASSERT_TRUE(proof.succeeded());
    TimelineExportClip clip{path, 0, metadata.duration_us, 0};
    clip.access = access;
    clip.identity_proof = *proof.value();
    const auto before = GlobalMediaHashMeasurements();
    const auto assessment = AssessTimelineExport({clip}, false);
    ASSERT_EQ(assessment.eligibility, TimelineExportEligibility::StreamCopy) << assessment.reason;
    const auto result = ExportTimelineStreamCopy({clip}, rewrite, false);
    ASSERT_TRUE(result.success) << result.message;
    const auto after = GlobalMediaHashMeasurements();
    for (size_t cause = 0; cause < after.by_cause.size(); ++cause)
        if (cause != static_cast<size_t>(MediaHashCause::RecordingFinalization))
            EXPECT_EQ(after.by_cause[cause].starts, before.by_cause[cause].starts) << "Hash cause " << cause;
    EXPECT_EQ(access->Measurements().hash_completions, 1);
}

TEST_F(AudioMetadataRoundtrip, ExportRejectsProofFromAnotherCapabilityBeforeWriting) {
    WriteFixture();
    WriteFixture(true, false, roundtrip);
    const auto source = AcquireMediaReadCapability(path);
    const auto other = AcquireMediaReadCapability(roundtrip);
    ASSERT_TRUE(source.succeeded());
    ASSERT_TRUE(other.succeeded());
    const auto metadata = ProbeEditMedia(*source.value());
    const auto proof = VerifyMediaIdentity(*other.value());
    ASSERT_TRUE(proof.succeeded());
    TimelineExportClip clip{path, 0, metadata.duration_us, 0};
    clip.access = *source.value();
    clip.identity_proof = *proof.value();
    EXPECT_EQ(AssessTimelineExport({clip}, false).eligibility, TimelineExportEligibility::Invalid);
    EXPECT_FALSE(ExportTimelineStreamCopy({clip}, rewrite, false).success);
    EXPECT_FALSE(std::filesystem::exists(rewrite));
}

} // namespace
TEST_F(AudioMetadataRoundtrip, ExplicitAdoptionKeepsStaleExpectedAuthoritySeparateFromObservedBytes) {
    ASSERT_NO_FATAL_FAILURE(WriteFixture(false));
    ASSERT_TRUE(WriteMediaIdentitySidecar(path, manifest).succeeded());
    nlohmann::json sidecar;
    {
        std::ifstream input(MediaIdentitySidecarPath(path));
        input >> sidecar;
    }
    sidecar["sha256"] = std::string(64, '0');
    {
        std::ofstream output(MediaIdentitySidecarPath(path));
        output << sidecar;
    }
    const auto admitted = AcquireMediaReadCapability(path);
    ASSERT_TRUE(admitted.succeeded());
    const auto source = *admitted.value();
    const auto pending = ProbeEditMedia(source, {}, {}, nullptr, false);
    ASSERT_TRUE(pending.error.empty()) << pending.error;
    ASSERT_TRUE(pending.audio_metadata.expected_content_sha256);
    const auto expected = pending.audio_metadata.expected_content_sha256;
    const auto observed = VerifyMediaIdentity(source);
    ASSERT_TRUE(observed.succeeded());
    const auto adopted = ProbeEditMediaForAdoption(source, *observed.value());
    ASSERT_TRUE(adopted.error.empty()) << adopted.error;
    EXPECT_TRUE(adopted.content_identity_valid);
    EXPECT_EQ(adopted.content_sha256, (*observed.value())->Identity().sha256);
    EXPECT_EQ(adopted.audio_metadata.status, AudioMetadataStatus::Stale);
    EXPECT_FALSE(adopted.audio_metadata.manifest);
    EXPECT_TRUE(adopted.audio_metadata.monitoring_confirmation_required);
    ASSERT_TRUE(adopted.audio_metadata.unverified_asset_binding);
    EXPECT_EQ(adopted.audio_metadata.unverified_asset_binding->sha256, adopted.content_sha256);
    EXPECT_FALSE(ProbeEditMedia(source).error.empty());
    EXPECT_EQ(CachedMediaAudioMetadata(source, false)->expected_content_sha256, expected);
    EXPECT_EQ(source->Measurements().hash_starts, 1);
    EditPlayerEngine player;
    std::string error;
    ASSERT_TRUE(player.Open(source, error)) << error;
    const auto& audio = adopted.audio_metadata;
    ASSERT_EQ(audio.actual_tracks.size(), 2);
    MonitorSelectionSnapshot choice{audio.selection_media_id,
                                    {1},
                                    {{audio.actual_tracks[0].track_id, 0.5f, false}},
                                    true,
                                    audio.unverified_asset_binding};
    ASSERT_TRUE(player.SetMonitorSelection(choice, error)) << error;
    ASSERT_TRUE(player.SetMonitorSelection(choice, error)) << error;
    std::vector<float> samples;
    ASSERT_TRUE(player.DecodeAudioRange(4800, 480, samples, error)) << error;
    for (float sample : samples)
        EXPECT_FLOAT_EQ(sample, 0.0625f);
    EXPECT_FALSE(ProbeEditMedia(source).error.empty());
    EXPECT_EQ(source->Measurements().hash_starts, 1);
}

TEST_F(AudioMetadataRoundtrip, ExplicitAdoptionRejectsOtherCapabilityProofAndHonorsCancellation) {
    ASSERT_NO_FATAL_FAILURE(WriteFixture(false));
    ASSERT_NO_FATAL_FAILURE(WriteFixture(false, false, rewrite));
    const auto first = AcquireMediaReadCapability(path);
    const auto second = AcquireMediaReadCapability(rewrite);
    ASSERT_TRUE(first.succeeded());
    ASSERT_TRUE(second.succeeded());
    const auto proof = VerifyMediaIdentity(*first.value());
    ASSERT_TRUE(proof.succeeded());
    EXPECT_FALSE(ProbeEditMediaForAdoption(*second.value(), *proof.value()).error.empty());
    EXPECT_EQ((*second.value())->Measurements().hash_starts, 0);
    std::atomic<bool> cancelled{true};
    EXPECT_FALSE(ProbeEditMediaForAdoption(*first.value(), *proof.value(), {}, {}, &cancelled).error.empty());
    EXPECT_EQ((*first.value())->Measurements().hash_starts, 1);
}
TEST_F(AudioMetadataRoundtrip, ExplicitAdoptionUsesCurrentVerifiedSidecarWithoutReplacingFrozenAuthority) {
    ASSERT_NO_FATAL_FAILURE(WriteFixture(false));
    ASSERT_TRUE(WriteMediaIdentitySidecar(path, manifest).succeeded());
    const auto admitted = AcquireMediaReadCapability(path);
    ASSERT_TRUE(admitted.succeeded());
    const auto source = *admitted.value();
    const auto frozen = ProbeEditMedia(source, {}, {}, nullptr, false);
    ASSERT_TRUE(frozen.error.empty()) << frozen.error;
    ASSERT_TRUE(frozen.audio_metadata.manifest);
    auto replacement = manifest;
    replacement.media_id = NewId<MediaId>();
    ASSERT_TRUE(WriteMediaIdentitySidecar(path, replacement, {}, MediaIdentitySidecarReferences{}).succeeded());
    const auto proof = VerifyMediaIdentity(source);
    ASSERT_TRUE(proof.succeeded());
    const auto adopted = ProbeEditMediaForAdoption(source, *proof.value());
    ASSERT_TRUE(adopted.error.empty()) << adopted.error;
    ASSERT_TRUE(adopted.audio_metadata.manifest);
    EXPECT_EQ(adopted.audio_metadata.selection_media_id, replacement.media_id);
    EXPECT_EQ(ProbeEditMedia(source).audio_metadata.selection_media_id, manifest.media_id);
    EditPlayerEngine player;
    std::string error;
    ASSERT_TRUE(player.Open(source, error)) << error;
    MonitorSelectionSnapshot choice{
        replacement.media_id, {1}, {{replacement.tracks[0].container.track_id, 0.5f, false}}, true};
    ASSERT_TRUE(player.SetMonitorSelection(choice, error)) << error;
    std::vector<float> samples;
    ASSERT_TRUE(player.DecodeAudioRange(4800, 480, samples, error)) << error;
    for (float sample : samples)
        EXPECT_FLOAT_EQ(sample, 0.0625f);
    EXPECT_EQ(ProbeEditMedia(source).audio_metadata.selection_media_id, manifest.media_id);
}
} // namespace exosnap::engine

namespace exosnap::engine {
TEST_F(AudioMetadataRoundtrip, ProvisionalSidecarCannotInstallAnUnverifiedDefaultMonitorMix) {
    ASSERT_NO_FATAL_FAILURE(WriteFixture(false));
    ASSERT_TRUE(WriteMediaIdentitySidecar(path, manifest).succeeded());
    nlohmann::json sidecar;
    {
        std::ifstream input(MediaIdentitySidecarPath(path));
        input >> sidecar;
    }
    sidecar["sha256"] = std::string(64, '0');
    {
        std::ofstream output(MediaIdentitySidecarPath(path));
        output << sidecar;
    }
    const auto admitted = AcquireMediaReadCapability(path);
    ASSERT_TRUE(admitted.succeeded());
    const auto source = *admitted.value();
    const auto pending = ProbeEditMedia(source, {}, {}, nullptr, false);
    ASSERT_TRUE(pending.error.empty()) << pending.error;
    ASSERT_EQ(pending.audio_metadata.status, AudioMetadataStatus::Pending);
    EXPECT_TRUE(pending.audio_metadata.monitoring_confirmation_required);
    EditPlayerEngine player;
    std::string error;
    ASSERT_TRUE(player.Open(source, error)) << error;
    EXPECT_TRUE(player.AudioMetadata().monitoring_confirmation_required);
    std::vector<float> samples;
    EXPECT_FALSE(player.DecodeAudioRange(0, 480, samples, error));
    EXPECT_EQ(source->Measurements().hash_starts, 0);
    EXPECT_FALSE(ProbeEditMedia(source).error.empty());
}
TEST_F(AudioMetadataRoundtrip, VerifiedSidecarRestoresItsAdmittedDefaultMonitorMix) {
    ASSERT_NO_FATAL_FAILURE(WriteFixture(false));
    ASSERT_TRUE(WriteMediaIdentitySidecar(path, manifest).succeeded());
    const auto admitted = AcquireMediaReadCapability(path);
    ASSERT_TRUE(admitted.succeeded());
    const auto source = *admitted.value();
    EXPECT_TRUE(ProbeEditMedia(source, {}, {}, nullptr, false).audio_metadata.monitoring_confirmation_required);
    const auto verified = ProbeEditMedia(source);
    ASSERT_TRUE(verified.error.empty()) << verified.error;
    EXPECT_EQ(verified.audio_metadata.status, AudioMetadataStatus::Verified);
    EXPECT_FALSE(verified.audio_metadata.monitoring_confirmation_required);
    EditPlayerEngine player;
    std::string error;
    ASSERT_TRUE(player.Open(source, error)) << error;
    std::vector<float> samples;
    ASSERT_TRUE(player.DecodeAudioRange(0, 480, samples, error)) << error;
    EXPECT_TRUE(std::any_of(samples.begin(), samples.end(), [](float sample) { return sample != 0; }));
    EXPECT_EQ(source->Measurements().hash_starts, 1);
}
} // namespace exosnap::engine
