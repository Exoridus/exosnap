#include "committed_matroska_view.h"
#include "matroska_packet_timestamps.h"
#include "matroska_stream_writer.h"
#include "recording_media_index.h"
#include "test_unique_temp.h"
#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/recording_clip.h>
#include <exosnap/engine/recording_media_types.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <ios>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <windows.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
}

namespace exosnap::engine {
namespace {

struct NativeHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~NativeHandle() {
        if (value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};

class CommittedMediaReadTest : public testing::Test {
  protected:
    void SetUp() override {
        path = exosnap_test::UniqueTempPath("committed_media.mkv");
        const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
        AVFormatContext* input = nullptr;
        ASSERT_GE(avformat_open_input(&input, fixture.string().c_str(), nullptr, nullptr), 0);
        auto close = [](AVFormatContext* context) { avformat_close_input(&context); };
        std::unique_ptr<AVFormatContext, decltype(close)> guard(input, close);
        ASSERT_GE(avformat_find_stream_info(input, nullptr), 0);
        const auto* stream = input->streams[0];
        config.output_path = path.string();
        config.video_codec_id = "V_MPEG4/ISO/AVC";
        config.video_codec_private.assign(stream->codecpar->extradata,
                                          stream->codecpar->extradata + stream->codecpar->extradata_size);
        config.encode_width = config.encode_height = 64;
        config.frame_rate_num = 30;
        config.frame_rate_den = 1;
        config.audio_codec = StreamAudioCodec::Pcm;
        config.audio_track_count = 2;
        AVPacket* packet = av_packet_alloc();
        ASSERT_NE(packet, nullptr);
        while (av_read_frame(input, packet) >= 0) {
            MuxPacket item;
            item.track_num = 1;
            item.pts_ns = static_cast<uint64_t>(av_rescale_q(packet->pts, stream->time_base, {1, 1000000000}));
            item.dts_ns = av_rescale_q(packet->dts, stream->time_base, {1, 1000000000});
            item.is_key = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            item.bytes.assign(packet->data, packet->data + packet->size);
            video.push_back(std::move(item));
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
        config.publish_committed = [this](CommittedMediaReceipt receipt) {
            receipts.push_back(std::move(receipt));
            return ReceiptAcceptance::Accepted;
        };
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove(path, error);
    }

    bool Feed(MatroskaStreamWriter& writer, unsigned cycles = 3, unsigned first = 0) {
        for (unsigned cycle = first; cycle < first + cycles; ++cycle) {
            std::vector<MuxPacket> packets;
            for (auto packet : video) {
                packet.pts_ns += cycle * 4000000000ULL;
                *packet.dts_ns += static_cast<int64_t>(cycle * 4000000000ULL);
                packets.push_back(std::move(packet));
            }
            for (uint64_t pts = cycle * 4000000000ULL; pts < (cycle + 1) * 4000000000ULL; pts += 10000000ULL) {
                for (uint64_t track = 2; track <= 3; ++track) {
                    MuxPacket packet;
                    packet.track_num = track;
                    packet.pts_ns = pts;
                    packet.is_key = true;
                    packet.bytes.assign(480 * 2 * 2, 0);
                    packets.push_back(std::move(packet));
                }
            }
            std::stable_sort(packets.begin(), packets.end(), [](const auto& a, const auto& b) {
                return a.dts_ns.value_or(static_cast<int64_t>(a.pts_ns)) <
                       b.dts_ns.value_or(static_cast<int64_t>(b.pts_ns));
            });
            for (auto& packet : packets)
                if (!writer.Push(std::move(packet)))
                    return false;
        }
        return true;
    }

    std::filesystem::path path;
    MatroskaStreamConfig config;
    std::vector<MuxPacket> video;
    std::vector<CommittedMediaReceipt> receipts;
};

TEST_F(CommittedMediaReadTest, FreezesOwnedHeaderAndPublishesClosedClustersBeforeFinalize) {
    MatroskaStreamWriter writer;
    ASSERT_TRUE(writer.Open(config));
    auto descriptor = writer.media_descriptor();
    ASSERT_NE(descriptor, nullptr);
    ASSERT_FALSE(descriptor->immutable_header.empty());
    EXPECT_NE(descriptor->recording_id, RecordingId{});
    EXPECT_NE(descriptor->segment_id, SegmentId{});
    EXPECT_NE(descriptor->file_identity, FileIdentity{});
    const auto header = descriptor->immutable_header;
    ASSERT_TRUE(Feed(writer));
    ASSERT_FALSE(receipts.empty());
    ASSERT_FALSE(receipts.front().clusters.empty());
    for (const auto& cluster : receipts.front().clusters) {
        EXPECT_LT(cluster.file_begin, cluster.file_end);
        EXPECT_LE(cluster.file_end, receipts.front().readable_end_offset);
        EXPECT_EQ(cluster.coverage.size(), 3u);
    }
    ASSERT_TRUE(writer.Finalize());
    EXPECT_EQ(descriptor->immutable_header, header);
    const auto seal = writer.finalized_segment_seal();
    ASSERT_TRUE(seal.has_value());
    EXPECT_EQ(seal->recording_id, descriptor->recording_id);
    EXPECT_EQ(seal->segment_id, descriptor->segment_id);
    EXPECT_EQ(seal->final_commit_sequence, receipts.back().commit_sequence);
    EXPECT_EQ(seal->final_readable_end, receipts.back().readable_end_offset);
    EXPECT_GT(seal->measured_video_end_r_ns, descriptor->segment_origin_r_ns);
    bool closed_rap = false;
    for (const auto& receipt : receipts)
        for (const auto& cluster : receipt.clusters)
            for (const auto& rap : cluster.random_access_points)
                closed_rap |= rap.kind == RandomAccessKind::ClosedGop;
    EXPECT_TRUE(closed_rap);
}

TEST_F(CommittedMediaReadTest, CrtFlushFailureNeverPublishesReceipt) {
    config.fail_io = [](OutputIoOperation operation) { return operation == OutputIoOperation::CrtFlush; };
    MatroskaStreamWriter writer;
    ASSERT_TRUE(writer.Open(config));
    EXPECT_FALSE(Feed(writer));
    EXPECT_TRUE(receipts.empty());
    EXPECT_FALSE(writer.Finalize());
}

TEST_F(CommittedMediaReadTest, OsFlushFailurePublishesReadableWithoutDurablePrefix) {
    config.fail_io = [](OutputIoOperation operation) { return operation == OutputIoOperation::DurabilityFlush; };
    MatroskaStreamWriter writer;
    ASSERT_TRUE(writer.Open(config));
    ASSERT_TRUE(Feed(writer));
    ASSERT_FALSE(receipts.empty());
    EXPECT_GT(receipts.front().readable_end_offset, 0u);
    EXPECT_EQ(receipts.front().durable_end_offset, 0u);
    EXPECT_TRUE(writer.Finalize());
}

TEST_F(CommittedMediaReadTest, RejectedReceiptDegradesIndexWithoutStoppingWriter) {
    config.publish_committed = [](CommittedMediaReceipt) { return ReceiptAcceptance::IndexDegraded; };
    MatroskaStreamWriter writer;
    ASSERT_TRUE(writer.Open(config));
    ASSERT_TRUE(Feed(writer));
    EXPECT_TRUE(writer.media_index_degraded());
    EXPECT_TRUE(writer.Finalize());
}

TEST_F(CommittedMediaReadTest, DirectViewDecodesRealMediaWithFixedEofDuringGrowthAndFinalize) {
    MatroskaStreamWriter writer;
    ASSERT_TRUE(writer.Open(config));
    ASSERT_TRUE(Feed(writer, 2));
    ASSERT_FALSE(receipts.empty());
    const auto descriptor = writer.media_descriptor();
    const auto receipt = receipts.front();
    auto opened = CommittedMatroskaView::Open(descriptor, receipt.clusters, receipt.readable_end_offset);
    ASSERT_TRUE(opened.succeeded()) << opened.error()->context;
    auto view = std::move(*opened.value());
    const auto fixed_size = view->size();
    std::vector<uint8_t> before(static_cast<size_t>(fixed_size));
    ASSERT_EQ(*view->ReadAt(0, before).value(), before.size());
    std::atomic<bool> progressed{false};
    std::jthread producer([&] { progressed = Feed(writer, 1, 2) && writer.Finalize(); });

    AVFormatContext* input = avformat_alloc_context();
    input->pb = view->avio();
    input->flags |= AVFMT_FLAG_CUSTOM_IO;
    const int opened_input = avformat_open_input(&input, nullptr, nullptr, nullptr);
    if (opened_input < 0) {
        producer.join();
        FAIL() << "DIRECT custom AVIO open failed: " << opened_input;
    }
    auto close = [](AVFormatContext* context) { avformat_close_input(&context); };
    std::unique_ptr<AVFormatContext, decltype(close)> input_guard(input, close);
    const int discovered = avformat_find_stream_info(input, nullptr);
    if (discovered < 0) {
        producer.join();
        FAIL() << "DIRECT stream discovery failed: " << discovered;
    }
    ASSERT_EQ(input->nb_streams, 3u);
    std::array<AVCodecContext*, 3> decoders{};
    for (unsigned stream = 0; stream < 3; ++stream) {
        decoders[stream] = avcodec_alloc_context3(avcodec_find_decoder(input->streams[stream]->codecpar->codec_id));
        ASSERT_NE(decoders[stream], nullptr);
        ASSERT_GE(avcodec_parameters_to_context(decoders[stream], input->streams[stream]->codecpar), 0);
        ASSERT_GE(avcodec_open2(decoders[stream], decoders[stream]->codec, nullptr), 0);
    }
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::array<int64_t, 3> decoded{};
    int exact = 0;
    auto drain = [&](unsigned stream) {
        while (avcodec_receive_frame(decoders[stream], frame) == 0) {
            decoded[stream] += stream == 0 ? 1 : frame->nb_samples;
            av_frame_unref(frame);
        }
    };
    int read_result = 0;
    while ((read_result = av_read_frame(input, packet)) >= 0) {
        const unsigned stream = static_cast<unsigned>(packet->stream_index);
        if (stream == 0) {
            size_t size = 0;
            const auto* additional = av_packet_get_side_data(packet, AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL, &size);
            ASSERT_NE(additional, nullptr);
            ASSERT_EQ(size, 32u);
            const auto timestamps = DecodeMatroskaPacketTimestamps({additional + 8, size - 8});
            ASSERT_TRUE(timestamps);
            packet->pts =
                av_rescale_q(static_cast<int64_t>(timestamps->pts_ns), {1, 1000000000}, input->streams[0]->time_base);
            packet->dts = av_rescale_q(timestamps->dts_ns, {1, 1000000000}, input->streams[0]->time_base);
            ++exact;
        }
        ASSERT_GE(avcodec_send_packet(decoders[stream], packet), 0);
        drain(stream);
        av_packet_unref(packet);
    }
    for (unsigned stream = 0; stream < 3; ++stream) {
        EXPECT_GE(avcodec_send_packet(decoders[stream], nullptr), 0);
        drain(stream);
        avcodec_free_context(&decoders[stream]);
    }
    av_packet_free(&packet);
    av_frame_free(&frame);
    producer.join();
    EXPECT_TRUE(progressed);
    EXPECT_EQ(read_result, AVERROR_EOF);
    EXPECT_EQ(exact, 60);
    EXPECT_EQ(decoded[0], 60);
    EXPECT_EQ(decoded[1], 93120);
    EXPECT_EQ(decoded[1], decoded[2]);
    RecordProperty("clip_read_mode", "DIRECT");
    RecordProperty("decoded_video_frames", static_cast<int>(decoded[0]));
    RecordProperty("decoded_pcm_frames_per_track", static_cast<int>(decoded[1]));
    RecordProperty("finite_view_bytes", std::to_string(fixed_size));
    EXPECT_EQ(view->size(), fixed_size);
    std::array<uint8_t, 16> tail{};
    EXPECT_EQ(*view->ReadAt(fixed_size, tail).value(), 0u);
    std::vector<uint8_t> after(before.size());
    ASSERT_EQ(*view->ReadAt(0, after).value(), after.size());
    EXPECT_EQ(before, after);
    EXPECT_FALSE(view->last_error());
}

TEST_F(CommittedMediaReadTest, BoundsRejectUncommittedExtentsAndShortReadsAreIntegrityFailures) {
    MatroskaStreamWriter writer;
    ASSERT_TRUE(writer.Open(config));
    ASSERT_TRUE(Feed(writer));
    ASSERT_FALSE(receipts.empty());
    const auto descriptor = writer.media_descriptor();
    auto receipt = receipts.front();
    auto invalid = receipt.clusters;
    invalid.back().file_end = receipt.readable_end_offset + 1;
    EXPECT_FALSE(CommittedMatroskaView::Open(descriptor, invalid, receipt.readable_end_offset).succeeded());
    auto opened = CommittedMatroskaView::Open(descriptor, receipt.clusters, receipt.readable_end_offset);
    ASSERT_TRUE(opened.succeeded());
    auto view = std::move(*opened.value());
    ASSERT_TRUE(writer.Finalize());
    {
        std::ofstream truncate(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(truncate);
    }
    std::array<uint8_t, 16> buffer{};
    const auto read = view->ReadAt(descriptor->immutable_header.size(), buffer);
    ASSERT_FALSE(read.succeeded());
    EXPECT_EQ(read.error()->code, EngineErrorCode::SourceIntegrityFailure);
}

TEST_F(CommittedMediaReadTest, SourceLeaseSurvivesRenameAndRejectsReplacementAtOldPath) {
    MatroskaStreamWriter writer;
    ASSERT_TRUE(writer.Open(config));
    ASSERT_TRUE(Feed(writer));
    ASSERT_TRUE(writer.Finalize());
    const auto descriptor = writer.media_descriptor();
    const auto receipt = receipts.front();
    auto opened = CommittedMatroskaView::Open(descriptor, receipt.clusters, receipt.readable_end_offset);
    ASSERT_TRUE(opened.succeeded());
    auto view = std::move(*opened.value());
    const auto renamed = exosnap_test::UniqueTempPath("renamed.edit.mkv");
    std::filesystem::rename(path, renamed);
    {
        std::ofstream replacement(path, std::ios::binary);
        replacement << "different file";
    }
    const auto rejected = CommittedMatroskaView::Open(descriptor, receipt.clusters, receipt.readable_end_offset);
    EXPECT_FALSE(rejected.succeeded());
    std::array<uint8_t, 16> buffer{};
    EXPECT_TRUE(view->ReadAt(descriptor->immutable_header.size(), buffer).succeeded());
    view->Cancel();
    const auto cancelled = view->ReadAt(0, buffer);
    ASSERT_FALSE(cancelled.succeeded());
    EXPECT_EQ(cancelled.error()->code, EngineErrorCode::Cancelled);
    view.reset();
    EXPECT_TRUE(std::filesystem::remove(renamed));
}

TEST_F(CommittedMediaReadTest, PinnedHandleOpensAfterRenameWithoutTrustingReplacedPath) {
    MatroskaStreamWriter writer;
    ASSERT_TRUE(writer.Open(config));
    ASSERT_TRUE(Feed(writer));
    ASSERT_TRUE(writer.Finalize());
    const auto descriptor = writer.media_descriptor();
    const auto receipt = receipts.front();
    NativeHandle pinned{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    ASSERT_NE(pinned.value, INVALID_HANDLE_VALUE);
    const auto renamed = exosnap_test::UniqueTempPath("pinned_renamed.edit.mkv");
    ASSERT_TRUE(MoveFileExW(path.c_str(), renamed.c_str(), MOVEFILE_WRITE_THROUGH));
    {
        std::ofstream replacement(path, std::ios::binary);
        replacement << "different file";
    }
    EXPECT_FALSE(CommittedMatroskaView::Open(descriptor, receipt.clusters, receipt.readable_end_offset).succeeded());
    auto opened = CommittedMatroskaView::OpenFromPinnedHandle(descriptor, receipt.clusters, receipt.readable_end_offset,
                                                              pinned.value);
    ASSERT_TRUE(opened.succeeded()) << opened.error()->context;
    auto view = std::move(*opened.value());
    LARGE_INTEGER before{};
    ASSERT_TRUE(SetFilePointerEx(pinned.value, {}, &before, FILE_CURRENT));
    std::array<uint8_t, 16> buffer{};
    ASSERT_EQ(*view->ReadAt(descriptor->immutable_header.size(), buffer).value(), buffer.size());
    LARGE_INTEGER after{};
    ASSERT_TRUE(SetFilePointerEx(pinned.value, {}, &after, FILE_CURRENT));
    EXPECT_EQ(before.QuadPart, after.QuadPart);
    view.reset();
    EXPECT_TRUE(std::filesystem::remove(renamed));
}

bool WaitForCommit(const RecordingMediaIndex& index, uint64_t sequence) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (index.journal_committed_through() >= sequence)
            return true;
        if (index.needs_rebuild())
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

std::vector<uint8_t> ReadJournalBytes(const MediaIndexSnapshot& snapshot) {
    std::ifstream input(snapshot.journal_path, std::ios::binary);
    std::vector<uint8_t> result(static_cast<size_t>(snapshot.journal_end));
    input.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size()));
    return result;
}

void WriteJournalBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

uint32_t FixtureCrc(const uint8_t* begin, size_t count) {
    uint32_t crc = 0xffffffffu;
    for (size_t index = 0; index < count; ++index) {
        crc ^= begin[index];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

TEST_F(CommittedMediaReadTest, JournalSnapshotStaysFiniteWhileNewReceiptsArePublished) {
    const auto journal = exosnap_test::UniqueTempPath("recording.index");
    RecordingMediaIndex index(journal);
    config.publish_committed = [&](CommittedMediaReceipt receipt) {
        receipts.push_back(receipt);
        return index.PublishCommitted(std::move(receipt));
    };
    MatroskaStreamWriter writer;
    ASSERT_TRUE(writer.Open(config));
    ASSERT_EQ(index.RegisterDescriptor(writer.media_descriptor()), ReceiptAcceptance::Accepted);
    ASSERT_TRUE(Feed(writer, 2));
    ASSERT_FALSE(receipts.empty());
    ASSERT_TRUE(WaitForCommit(index, receipts.back().commit_sequence));
    const auto frozen = index.Freeze();
    ASSERT_TRUE(frozen.succeeded());
    const uint64_t frozen_sequence = frozen.value()->token.committed_through;
    ASSERT_TRUE(Feed(writer, 1, 2));
    ASSERT_TRUE(writer.Finalize());
    ASSERT_TRUE(WaitForCommit(index, receipts.back().commit_sequence));
    size_t seen = 0;
    const auto visited = RecordingMediaIndex::Visit(
        *frozen.value(),
        [&](const RecordingMediaDescriptor& descriptor) {
            EXPECT_EQ(descriptor.immutable_header, writer.media_descriptor()->immutable_header);
            EXPECT_EQ(descriptor.audio_tracks.size(), 2u);
            return true;
        },
        [&](const CommittedMediaReceipt& receipt) {
            EXPECT_LE(receipt.commit_sequence, frozen_sequence);
            ++seen;
            return true;
        });
    ASSERT_TRUE(visited.succeeded()) << visited.error()->context;
    EXPECT_EQ(seen, 1u);
    EXPECT_GT(index.journal_committed_through(), frozen_sequence);
    EXPECT_LE(index.queued_bytes(), 4u * 1024 * 1024);
    EXPECT_LE(index.cache_bytes(), 16u * 1024 * 1024);
    RecordProperty("index_cache_bytes", std::to_string(index.cache_bytes()));
    RecordProperty("index_journal_prefix_bytes", std::to_string(frozen.value()->journal_end));
    EXPECT_TRUE(std::filesystem::remove(journal));
}

TEST_F(CommittedMediaReadTest, ClipAvailabilityRequiresTwoCommittedVideoBoundaries) {
    const auto journal = exosnap_test::UniqueTempPath("clip_availability.index");
    RecordingMediaIndex index(journal);
    EXPECT_FALSE(index.has_closed_clip_interval());
    config.publish_committed = [&](CommittedMediaReceipt receipt) {
        receipts.push_back(receipt);
        return index.PublishCommitted(std::move(receipt));
    };
    MatroskaStreamWriter writer;
    ASSERT_TRUE(writer.Open(config));
    ASSERT_EQ(index.RegisterDescriptor(writer.media_descriptor()), ReceiptAcceptance::Accepted);
    ASSERT_TRUE(Feed(writer, 2));
    ASSERT_FALSE(receipts.empty());
    ASSERT_TRUE(WaitForCommit(index, receipts.back().commit_sequence));
    EXPECT_FALSE(index.has_closed_clip_interval());
    ASSERT_TRUE(Feed(writer, 1, 2));
    ASSERT_TRUE(WaitForCommit(index, receipts.back().commit_sequence));
    ASSERT_TRUE(writer.Finalize());
    ASSERT_TRUE(WaitForCommit(index, receipts.back().commit_sequence));
    EXPECT_TRUE(index.has_closed_clip_interval());
    const auto frozen = index.Freeze();
    ASSERT_TRUE(frozen.succeeded());
    size_t reads = 0;
    const auto rejected = RecordingMediaIndex::Visit(*frozen.value(), {}, {}, [&](uint64_t bytes) {
        EXPECT_GT(bytes, 0u);
        ++reads;
        return false;
    });
    EXPECT_FALSE(rejected.succeeded());
    EXPECT_EQ(reads, 1u);
    EXPECT_TRUE(std::filesystem::remove(journal));
}

TEST_F(CommittedMediaReadTest, JournalPreservesVerifiedAudioManifestProvenance) {
    const auto journal = exosnap_test::UniqueTempPath("manifest.index");
    RecordingMediaIndex index(journal);
    config.audio_track_count = 1;
    config.audio_tracks[0].name = "System";
    AudioManifest1 manifest;
    manifest.recording_id = NewId<RecordingId>();
    manifest.media_id = NewId<MediaId>();
    manifest.segment_id = NewId<SegmentId>();
    manifest.continuity_group = 7;
    AudioManifestSource source;
    source.id = NewId<AudioSourceId>();
    source.post_dsp_gain_linear = 0.75f;
    source.muted = true;
    manifest.sources.push_back(source);
    AudioManifestTrack track;
    track.container.track_id = NewId<AudioTrackId>();
    track.container.stream_index.value = 0;
    track.container.track_uid = 2;
    track.container.name = "System";
    track.container.role = "standard";
    track.container.codec.codec_id = "A_PCM/INT/LIT";
    track.container.codec.sample_rate = 48000;
    track.container.codec.channels = 2;
    track.container.codec.bit_depth = 16;
    track.container.codec.channel_layout = "stereo";
    track.container.source_ids.push_back(source.id);
    AudioManifestRoute route;
    route.id = NewId<AudioRouteId>();
    route.source_id = source.id;
    route.gain_linear = 0.5f;
    track.routes.push_back(route);
    manifest.default_monitor_track_ids.push_back(track.container.track_id);
    manifest.tracks.push_back(track);
    ASSERT_TRUE(ValidateAudioManifest1(manifest).succeeded());
    config.audio_manifest = manifest;
    uint64_t final_commit_sequence = 0;
    config.publish_committed = [&](CommittedMediaReceipt receipt) {
        final_commit_sequence = receipt.commit_sequence;
        return index.PublishCommitted(std::move(receipt));
    };
    MatroskaStreamWriter writer;
    ASSERT_TRUE(writer.Open(config)) << writer.error();
    ASSERT_TRUE(writer.media_descriptor()->audio_manifest);
    ASSERT_EQ(index.RegisterDescriptor(writer.media_descriptor()), ReceiptAcceptance::Accepted);
    for (auto packet : video)
        ASSERT_TRUE(writer.Push(std::move(packet))) << writer.error();
    ASSERT_TRUE(writer.Finalize()) << writer.error();
    ASSERT_GT(final_commit_sequence, 0u);
    ASSERT_TRUE(WaitForCommit(index, final_commit_sequence));
    const auto snapshot = index.Freeze();
    ASSERT_TRUE(snapshot.succeeded());
    const auto expected = SerializeAudioManifest1(manifest);
    ASSERT_TRUE(expected.succeeded());
    size_t seen = 0;
    const auto visited =
        VisitRecordingClipDescriptors(*snapshot.value(), [&](const RecordingMediaDescriptor& descriptor) {
            EXPECT_TRUE(descriptor.audio_manifest);
            if (descriptor.audio_manifest) {
                const auto actual = SerializeAudioManifest1(*descriptor.audio_manifest);
                EXPECT_TRUE(actual.succeeded());
                if (actual.succeeded())
                    EXPECT_EQ(*actual.value(), *expected.value());
            }
            ++seen;
            return true;
        });
    ASSERT_TRUE(visited.succeeded()) << visited.error()->context;
    EXPECT_EQ(seen, 1u);
    EXPECT_TRUE(std::filesystem::remove(journal));
}

TEST_F(CommittedMediaReadTest, JournalRecoveryIgnoresIncompleteTailButRejectsBadCrcAndNegativeSequence) {
    const auto journal = exosnap_test::UniqueTempPath("recovery.index");
    const auto copy = exosnap_test::UniqueTempPath("recovery-copy.index");
    RecordingMediaIndex index(journal);
    config.publish_committed = [&](CommittedMediaReceipt receipt) {
        receipts.push_back(receipt);
        return index.PublishCommitted(std::move(receipt));
    };
    MatroskaStreamWriter writer;
    ASSERT_TRUE(writer.Open(config));
    ASSERT_EQ(index.RegisterDescriptor(writer.media_descriptor()), ReceiptAcceptance::Accepted);
    ASSERT_TRUE(Feed(writer, 2));
    ASSERT_TRUE(WaitForCommit(index, 1));
    const auto snapshot = index.Freeze();
    ASSERT_TRUE(snapshot.succeeded());
    const auto bytes = ReadJournalBytes(*snapshot.value());
    auto partial = bytes;
    partial.pop_back();
    WriteJournalBytes(copy, partial);
    const auto recovered = RecordingMediaIndex::InspectJournal(copy);
    ASSERT_TRUE(recovered.succeeded());
    EXPECT_EQ(*recovered.value(), 1u);
    auto corrupt = bytes;
    corrupt.back() ^= 1;
    WriteJournalBytes(copy, corrupt);
    EXPECT_FALSE(RecordingMediaIndex::InspectJournal(copy).succeeded());
    auto negative = bytes;
    uint32_t descriptor_length = 0;
    for (size_t byte = 0; byte < 4; ++byte)
        descriptor_length |= static_cast<uint32_t>(negative[12 + byte]) << (8 * byte);
    const size_t record = 28 + descriptor_length;
    const size_t payload = record + 28;
    ASSERT_EQ(negative[payload], 0x88);
    ASSERT_EQ(negative[payload + 78], 0x01);
    negative[payload + 78] = 0x20; // CBOR negative one in CommitSequence.
    const auto crc = FixtureCrc(negative.data() + payload, negative.size() - payload);
    for (size_t byte = 0; byte < 4; ++byte)
        negative[record + 24 + byte] = static_cast<uint8_t>(crc >> (8 * byte));
    WriteJournalBytes(copy, negative);
    EXPECT_FALSE(RecordingMediaIndex::InspectJournal(copy).succeeded());
    EXPECT_TRUE(writer.Finalize());
    EXPECT_TRUE(std::filesystem::remove(copy));
    EXPECT_TRUE(std::filesystem::remove(journal));
}

TEST_F(CommittedMediaReadTest, IndexBudgetFailureRetainsConfirmedSnapshotAndWriterProgress) {
    const auto journal = exosnap_test::UniqueTempPath("budget.index");
    RecordingMediaIndex index(journal);
    config.publish_committed = [&](CommittedMediaReceipt receipt) {
        return index.PublishCommitted(std::move(receipt));
    };
    MatroskaStreamWriter writer;
    ASSERT_TRUE(writer.Open(config));
    ASSERT_EQ(index.RegisterDescriptor(writer.media_descriptor()), ReceiptAcceptance::Accepted);
    ASSERT_TRUE(Feed(writer, 2));
    ASSERT_TRUE(WaitForCommit(index, 1));
    auto snapshot = index.Freeze();
    ASSERT_TRUE(snapshot.succeeded());
    CommittedMediaReceipt oversized;
    oversized.clusters.resize(65537);
    EXPECT_EQ(index.PublishCommitted(std::move(oversized)), ReceiptAcceptance::IndexDegraded);
    EXPECT_TRUE(index.needs_rebuild());
    ASSERT_TRUE(Feed(writer, 1, 2));
    EXPECT_TRUE(writer.Finalize());
    EXPECT_TRUE(writer.media_index_degraded());
    EXPECT_TRUE(RecordingMediaIndex::Visit(*snapshot.value(), {}, {}).succeeded());
    EXPECT_TRUE(std::filesystem::remove(journal));
}

TEST_F(CommittedMediaReadTest, JournalRejectsMalformedContainerLengthsAndExcessiveNesting) {
    const auto journal = exosnap_test::UniqueTempPath("malformed.index");
    auto write_payload = [&](const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> bytes(28, 0);
        const uint32_t fields[] = {0x58494d45u, 1, 1, static_cast<uint32_t>(payload.size())};
        for (size_t field = 0; field < 4; ++field)
            for (size_t byte = 0; byte < 4; ++byte)
                bytes[field * 4 + byte] = static_cast<uint8_t>(fields[field] >> (byte * 8));
        bytes[16] = 1;
        const auto crc = FixtureCrc(payload.data(), payload.size());
        for (size_t byte = 0; byte < 4; ++byte)
            bytes[24 + byte] = static_cast<uint8_t>(crc >> (byte * 8));
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        WriteJournalBytes(journal, bytes);
    };
    write_payload({0x9b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff});
    EXPECT_FALSE(RecordingMediaIndex::InspectJournal(journal).succeeded());
    std::vector<uint8_t> nested(1000, 0x81);
    nested.push_back(0);
    write_payload(nested);
    EXPECT_FALSE(RecordingMediaIndex::InspectJournal(journal).succeeded());
    EXPECT_TRUE(std::filesystem::remove(journal));
}

TEST_F(CommittedMediaReadTest, SequenceExhaustionDegradesClipsWithoutInvalidatingRecording) {
    config.initial_commit_sequence = UINT64_MAX;
    MatroskaStreamWriter writer;
    ASSERT_TRUE(writer.Open(config));
    ASSERT_TRUE(Feed(writer));
    EXPECT_TRUE(receipts.empty());
    EXPECT_TRUE(writer.media_index_degraded());
    EXPECT_TRUE(writer.Finalize());
}

TEST_F(CommittedMediaReadTest, SharedJournalRetainsSegmentsWithContinuousCommitSequence) {
    const auto journal = exosnap_test::UniqueTempPath("segments.index");
    const auto second_path = exosnap_test::UniqueTempPath("second_segment.mkv");
    RecordingMediaIndex index(journal);
    uint64_t sequence = 0;
    RecordingMediaDescriptor descriptor;
    descriptor.recording_id = NewId<RecordingId>();
    config.media_descriptor = descriptor;
    config.publish_committed = [&](CommittedMediaReceipt receipt) {
        sequence = receipt.commit_sequence;
        return index.PublishCommitted(std::move(receipt));
    };
    MatroskaStreamWriter first;
    ASSERT_TRUE(first.Open(config));
    ASSERT_EQ(index.RegisterDescriptor(first.media_descriptor()), ReceiptAcceptance::Accepted);
    ASSERT_TRUE(Feed(first, 1));
    ASSERT_TRUE(first.Finalize());
    const auto first_sequence = sequence;
    ASSERT_GT(first_sequence, 0u);
    config.output_path = second_path.string();
    config.initial_commit_sequence = first_sequence;
    config.media_descriptor->descriptor_revision = 2;
    config.media_descriptor->segment_origin_r_ns = 4'000'000'000;
    MatroskaStreamWriter second;
    ASSERT_TRUE(second.Open(config));
    ASSERT_EQ(index.RegisterDescriptor(second.media_descriptor()), ReceiptAcceptance::Accepted);
    ASSERT_TRUE(Feed(second, 1));
    ASSERT_TRUE(second.Finalize());
    ASSERT_GT(sequence, first_sequence);
    ASSERT_TRUE(WaitForCommit(index, sequence));
    auto snapshot = index.Freeze();
    ASSERT_TRUE(snapshot.succeeded());
    std::vector<SegmentId> segments;
    uint64_t previous_sequence = 0;
    ASSERT_TRUE(RecordingMediaIndex::Visit(
                    *snapshot.value(),
                    [&](const RecordingMediaDescriptor& descriptor) {
                        segments.push_back(descriptor.segment_id);
                        return true;
                    },
                    [&](const CommittedMediaReceipt& receipt) {
                        EXPECT_GT(receipt.commit_sequence, previous_sequence);
                        previous_sequence = receipt.commit_sequence;
                        return true;
                    })
                    .succeeded());
    ASSERT_EQ(segments.size(), 2u);
    EXPECT_NE(segments[0], segments[1]);
    std::vector<SegmentId> public_segments;
    const auto public_visit =
        VisitRecordingClipDescriptors(*snapshot.value(), [&](const RecordingMediaDescriptor& descriptor) {
            public_segments.push_back(descriptor.segment_id);
            return true;
        });
    ASSERT_TRUE(public_visit.succeeded()) << public_visit.error()->context;
    EXPECT_EQ(public_segments, segments);
    EXPECT_EQ(previous_sequence, sequence);
    EXPECT_FALSE(index.needs_rebuild());
    EXPECT_TRUE(std::filesystem::remove(second_path));
    EXPECT_TRUE(std::filesystem::remove(journal));
}

} // namespace
} // namespace exosnap::engine
