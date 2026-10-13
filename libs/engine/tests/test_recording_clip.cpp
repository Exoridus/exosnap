#include "codec_random_access.h"
#include "matroska_stream_writer.h"
#include "opus_audio_encoder.h"
#include "recording_media_index.h"
#include "test_unique_temp.h"
#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/packet_types.h>
#include <exosnap/engine/recorder_session.h>
#include <exosnap/engine/recording_clip.h>
#include <exosnap/engine/recording_media_types.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
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
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
}

namespace exosnap::engine {
namespace {

TEST(RecordingClipTest, RealH264IdrIsClosedWhileContainerFlagAndMalformedSampleAreInsufficient) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
    AVFormatContext* raw = nullptr;
    ASSERT_GE(avformat_open_input(&raw, fixture.string().c_str(), nullptr, nullptr), 0);
    const auto close = [](AVFormatContext* value) { avformat_close_input(&value); };
    std::unique_ptr<AVFormatContext, decltype(close)> input(raw, close);
    ASSERT_GE(avformat_find_stream_info(raw, nullptr), 0);
    ASSERT_GT(raw->nb_streams, 0u);
    const auto* codec = raw->streams[0]->codecpar;
    ASSERT_GT(codec->extradata_size, 4);
    const std::span<const uint8_t> private_data(codec->extradata, static_cast<size_t>(codec->extradata_size));
    const auto free_packet = [](AVPacket* value) { av_packet_free(&value); };
    std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
    ASSERT_NE(packet, nullptr);
    bool found = false;
    while (av_read_frame(raw, packet.get()) >= 0) {
        if (packet->stream_index == 0 && (packet->flags & AV_PKT_FLAG_KEY)) {
            const std::span<const uint8_t> sample(packet->data, static_cast<size_t>(packet->size));
            EXPECT_EQ(QualifyClosedRandomAccess("V_MPEG4/ISO/AVC", private_data, sample, true),
                      RandomAccessKind::ClosedGop);
            EXPECT_EQ(QualifyClosedRandomAccess("V_MPEG4/ISO/AVC", private_data, sample, false),
                      RandomAccessKind::Unknown);
            const std::vector<uint8_t> truncated{0, 0, 0, 1, 0x65};
            EXPECT_EQ(QualifyClosedRandomAccess("V_MPEG4/ISO/AVC", private_data, truncated, true),
                      RandomAccessKind::Unknown);
            found = true;
            break;
        }
        av_packet_unref(packet.get());
    }
    EXPECT_TRUE(found);
}

TEST(RecordingClipTest, CraAndNonIdrNalUnitsDoNotCreateClosedRap) {
    std::vector<uint8_t> hvcc(22);
    hvcc[0] = 1;
    hvcc[21] = 3;
    const std::vector<uint8_t> cra{0, 0, 0, 3, static_cast<uint8_t>(21 << 1), 1, 0x80};
    EXPECT_EQ(QualifyClosedRandomAccess("V_MPEGH/ISO/HEVC", hvcc, cra, true), RandomAccessKind::Unknown);
    const std::vector<uint8_t> short_idr{0, 0, 0, 2, static_cast<uint8_t>(19 << 1), 1};
    EXPECT_EQ(QualifyClosedRandomAccess("V_MPEGH/ISO/HEVC", hvcc, short_idr, true), RandomAccessKind::Unknown);
    const std::vector<uint8_t> av1_without_sequence{0x32, 2, 0x10, 0x00};
    const std::array<uint8_t, 4> av1_private{0x81, 0, 0, 0};
    EXPECT_EQ(QualifyClosedRandomAccess("V_AV1", av1_private, av1_without_sequence, true), RandomAccessKind::Unknown);
}

TEST(RecordingClipTest, StartupMetadataBudgetRejectsOversizedPinnedDescriptorBeforeOpeningSource) {
    auto descriptor = std::make_shared<RecordingMediaDescriptor>();
    descriptor->recording_id = NewId<RecordingId>();
    descriptor->segment_id = NewId<SegmentId>();
    descriptor->immutable_header.resize(2 * 1024 * 1024 + 1);
    const auto base_bytes = EstimateRecordingClipDescriptorBytes(*descriptor);
    descriptor->video.codec_private.reserve(64 * 1024);
    EXPECT_GE(EstimateRecordingClipDescriptorBytes(*descriptor), base_bytes + 64 * 1024);
    auto manifest = std::make_shared<AudioManifest1>();
    manifest->tracks.resize(1);
    manifest->tracks[0].routes.reserve(128);
    const auto before_manifest = EstimateRecordingClipDescriptorBytes(*descriptor);
    descriptor->audio_manifest = std::move(manifest);
    EXPECT_GE(EstimateRecordingClipDescriptorBytes(*descriptor), before_manifest + 128 * sizeof(AudioManifestRoute));

    MediaIndexSnapshot snapshot;
    snapshot.token.recording_id = descriptor->recording_id;
    ClipBoundaryPlan plan;
    plan.recording_id = descriptor->recording_id;
    plan.snapshot = snapshot.token;
    plan.segments.resize(1);
    const std::array<ClipPinnedSource, 1> sources{{{descriptor, INVALID_HANDLE_VALUE, std::nullopt}}};
    const auto result =
        RunRecordingClipJob(plan, snapshot, sources, exosnap_test::UniqueTempPath("clip_metadata_rejected.mkv"));
    ASSERT_FALSE(result.succeeded());
    EXPECT_EQ(result.error()->code, EngineErrorCode::ResourceBudgetExceeded);
}

TEST(RecordingClipTest, PlannerRejectsHighSegmentCountWithinFiniteJournal) {
    const auto journal = exosnap_test::UniqueTempPath("clip_many_segments.index");
    const RecordingId recording_id = NewId<RecordingId>();
    constexpr size_t segment_count = 2048;
    SegmentId final_segment_id;
    {
        RecordingMediaIndex index(journal);
        for (size_t segment = 0; segment < segment_count; ++segment) {
            if (segment != 0 && segment % 256 == 0) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (index.queued_bytes() != 0 && !index.needs_rebuild() &&
                       std::chrono::steady_clock::now() < deadline)
                    std::this_thread::yield();
                ASSERT_FALSE(index.needs_rebuild());
                ASSERT_EQ(index.queued_bytes(), 0u);
            }
            RecordingMediaDescriptor descriptor;
            descriptor.recording_id = recording_id;
            descriptor.media_id = NewId<MediaId>();
            descriptor.segment_id = NewId<SegmentId>();
            descriptor.writer_generation = 1;
            descriptor.descriptor_revision = 1;
            descriptor.file_identity.volume_serial = 1;
            descriptor.path = "unused.mkv";
            descriptor.video.codec_id = "V_MPEG4/ISO/AVC";
            descriptor.immutable_header = {0x1a};
            descriptor.segment_origin_r_ns = static_cast<int64_t>(segment) * 1'000'000'000;
            descriptor.continuity_group = 1;
            final_segment_id = descriptor.segment_id;
            ASSERT_EQ(index.RegisterDescriptor(std::make_shared<const RecordingMediaDescriptor>(std::move(descriptor))),
                      ReceiptAcceptance::Accepted);
        }
        CommittedMediaReceipt receipt;
        receipt.recording_id = recording_id;
        receipt.segment_id = final_segment_id;
        receipt.writer_generation = 1;
        receipt.descriptor_revision = 1;
        receipt.commit_sequence = 1;
        receipt.readable_end_offset = 2;
        receipt.durable_end_offset = 2;
        ClusterExtent extent;
        extent.file_begin = 1;
        extent.file_end = 2;
        extent.coverage.push_back({1, 0, 1'000'000'000, 0, 0});
        extent.random_access_points.push_back({1, 1, 1, 0, 0, RandomAccessKind::ClosedGop});
        receipt.clusters.push_back(std::move(extent));
        ASSERT_EQ(index.PublishCommitted(std::move(receipt)), ReceiptAcceptance::Accepted);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (index.journal_committed_through() < 1 && !index.needs_rebuild() &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        ASSERT_FALSE(index.needs_rebuild());
        ASSERT_EQ(index.journal_committed_through(), 1u);
        const auto frozen = index.Freeze();
        ASSERT_TRUE(frozen.succeeded()) << frozen.error()->context;
        EXPECT_EQ(frozen.value()->token.journal_revision, segment_count + 1);
        const auto plan = PlanClipBoundaries(*frozen.value(), recording_id, 0, 1'000'000'000);
        ASSERT_FALSE(plan.succeeded());
        EXPECT_EQ(plan.error()->code, EngineErrorCode::ResourceBudgetExceeded);
    }
    std::error_code ignored;
    std::filesystem::remove(journal, ignored);
}

TEST(RecordingClipTest, FiniteJournalClipScalesFromThirtySecondsToFullRecording) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
    const auto source_path = exosnap_test::UniqueTempPath("clip_duration_source.mkv");
    const auto journal = exosnap_test::UniqueTempPath("clip_duration.index");
    AVFormatContext* raw = nullptr;
    ASSERT_GE(avformat_open_input(&raw, fixture.string().c_str(), nullptr, nullptr), 0);
    const auto close_input = [](AVFormatContext* value) { avformat_close_input(&value); };
    std::unique_ptr<AVFormatContext, decltype(close_input)> input(raw, close_input);
    ASSERT_GE(avformat_find_stream_info(raw, nullptr), 0);
    const auto* codec = raw->streams[0]->codecpar;
    ASSERT_GT(codec->extradata_size, 0);
    const auto free_packet = [](AVPacket* value) { av_packet_free(&value); };
    std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
    ASSERT_NE(packet, nullptr);
    std::vector<uint8_t> key_sample;
    while (av_read_frame(raw, packet.get()) >= 0) {
        if (packet->stream_index == 0 && (packet->flags & AV_PKT_FLAG_KEY)) {
            key_sample.assign(packet->data, packet->data + packet->size);
            break;
        }
        av_packet_unref(packet.get());
    }
    ASSERT_FALSE(key_sample.empty());
    MatroskaStreamConfig config;
    config.output_path = source_path.string();
    config.video_codec_id = "V_MPEG4/ISO/AVC";
    config.video_codec_private.assign(codec->extradata, codec->extradata + codec->extradata_size);
    config.encode_width = static_cast<uint32_t>(codec->width);
    config.encode_height = static_cast<uint32_t>(codec->height);
    config.frame_rate_num = 1;
    config.frame_rate_den = 5;
    RecordingMediaIndex index(journal);
    config.publish_committed = [&](CommittedMediaReceipt receipt) {
        return index.PublishCommitted(std::move(receipt));
    };
    MatroskaStreamWriter writer;
    ASSERT_TRUE(writer.Open(config)) << writer.error();
    const auto descriptor = writer.media_descriptor();
    ASSERT_EQ(index.RegisterDescriptor(descriptor), ReceiptAcceptance::Accepted);
    constexpr uint64_t full_seconds = 45 * 60;
    for (uint64_t second = 0; second < full_seconds; second += 5) {
        MuxPacket item;
        item.track_num = 1;
        item.pts_ns = second * 1'000'000'000ULL;
        item.is_key = true;
        item.bytes = key_sample;
        ASSERT_TRUE(writer.Push(std::move(item))) << writer.error();
    }
    ASSERT_TRUE(writer.Finalize()) << writer.error();
    const auto seal = writer.finalized_segment_seal();
    ASSERT_TRUE(seal);
    ASSERT_EQ(seal->measured_video_end_r_ns, static_cast<int64_t>(full_seconds * 1'000'000'000ULL));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (index.journal_committed_through() < seal->final_commit_sequence && !index.needs_rebuild() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_FALSE(index.needs_rebuild());
    ASSERT_GE(index.journal_committed_through(), seal->final_commit_sequence);
    const auto frozen = index.Freeze();
    ASSERT_TRUE(frozen.succeeded()) << frozen.error()->context;
    ASSERT_LT(std::filesystem::file_size(source_path), 16ULL * 1024 * 1024);
    HANDLE pinned =
        CreateFileW(source_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    ASSERT_NE(pinned, INVALID_HANDLE_VALUE);
    const std::array<ClipPinnedSource, 1> sources{{{descriptor, pinned, std::nullopt}}};
    for (const uint64_t seconds : {30ULL, 5ULL * 60, 30ULL * 60, full_seconds}) {
        SCOPED_TRACE(seconds);
        const auto closed = seconds == full_seconds ? std::span<const RecordingSegmentSeal>(&*seal, 1)
                                                    : std::span<const RecordingSegmentSeal>{};
        const auto plan = PlanClipBoundaries(*frozen.value(), descriptor->recording_id, 0,
                                             static_cast<int64_t>(seconds * 1'000'000'000ULL), closed);
        ASSERT_TRUE(plan.succeeded()) << plan.error()->context;
        EXPECT_EQ(plan.value()->actual_begin_r_ns, 0);
        EXPECT_EQ(plan.value()->actual_end_r_ns, static_cast<int64_t>(seconds * 1'000'000'000ULL));
        EXPECT_EQ(plan.value()->end_at_sealed_eos, seconds == full_seconds);
        const auto output = exosnap_test::UniqueTempPath("clip_duration_output.mkv");
        const auto draft = RunRecordingClipJob(*plan.value(), *frozen.value(), sources, output);
        ASSERT_TRUE(draft.succeeded()) << draft.error()->context;
        EXPECT_EQ(draft.value()->video_packets, seconds / 5);
        ASSERT_EQ(draft.value()->source_ranges.size(), 1u);
        EXPECT_EQ(draft.value()->source_ranges[0].begin_r_ns, 0);
        EXPECT_EQ(draft.value()->source_ranges[0].end_r_ns, static_cast<int64_t>(seconds * 1'000'000'000ULL));
        EXPECT_LE(draft.value()->peak_packet_bytes, 4ULL * 1024 * 1024);
        EXPECT_LE(draft.value()->peak_writer_window_bytes, 8ULL * 1024 * 1024);
        EXPECT_LE(draft.value()->peak_cluster_payload_bytes, 8ULL * 1024 * 1024);
        std::error_code ignored;
        std::filesystem::remove(output, ignored);
    }
    CloseHandle(pinned);
    std::error_code ignored;
    std::filesystem::remove(source_path, ignored);
    std::filesystem::remove(journal, ignored);
}

void CheckCommittedPacketBudget(size_t packet_limit) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
    const auto source_path = exosnap_test::UniqueTempPath("clip_oversized_source.mkv");
    const auto journal = exosnap_test::UniqueTempPath("clip_oversized.index");
    AVFormatContext* raw = nullptr;
    ASSERT_GE(avformat_open_input(&raw, fixture.string().c_str(), nullptr, nullptr), 0);
    const auto close_input = [](AVFormatContext* value) { avformat_close_input(&value); };
    std::unique_ptr<AVFormatContext, decltype(close_input)> input(raw, close_input);
    ASSERT_GE(avformat_find_stream_info(raw, nullptr), 0);
    const auto* codec = raw->streams[0]->codecpar;
    ASSERT_GT(codec->extradata_size, 4);
    ASSERT_EQ(codec->extradata[4] & 3, 3);
    const auto free_packet = [](AVPacket* value) { av_packet_free(&value); };
    std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
    ASSERT_NE(packet, nullptr);
    std::vector<uint8_t> key_sample;
    while (av_read_frame(raw, packet.get()) >= 0) {
        if (packet->stream_index == 0 && (packet->flags & AV_PKT_FLAG_KEY)) {
            key_sample.assign(packet->data, packet->data + packet->size);
            break;
        }
        av_packet_unref(packet.get());
    }
    ASSERT_FALSE(key_sample.empty());

    MatroskaStreamConfig config;
    config.output_path = source_path.string();
    config.video_codec_id = "V_MPEG4/ISO/AVC";
    config.video_codec_private.assign(codec->extradata, codec->extradata + codec->extradata_size);
    config.encode_width = static_cast<uint32_t>(codec->width);
    config.encode_height = static_cast<uint32_t>(codec->height);
    config.frame_rate_num = 1;
    config.frame_rate_den = 2;
    RecordingMediaIndex index(journal);
    config.publish_committed = [&](CommittedMediaReceipt receipt) {
        return index.PublishCommitted(std::move(receipt));
    };
    MatroskaStreamWriter writer;
    ASSERT_TRUE(writer.Open(config)) << writer.error();
    const auto descriptor = writer.media_descriptor();
    ASSERT_NE(descriptor, nullptr);
    ASSERT_EQ(index.RegisterDescriptor(descriptor), ReceiptAcceptance::Accepted);

    const auto filler_length = static_cast<uint32_t>(packet_limit - key_sample.size() - 4);
    auto oversized = key_sample;
    oversized.push_back(static_cast<uint8_t>(filler_length >> 24));
    oversized.push_back(static_cast<uint8_t>(filler_length >> 16));
    oversized.push_back(static_cast<uint8_t>(filler_length >> 8));
    oversized.push_back(static_cast<uint8_t>(filler_length));
    oversized.push_back(0x0c);
    oversized.resize(packet_limit, 0xff);
    oversized.back() = 0x80;
    ASSERT_EQ(QualifyClosedRandomAccess(config.video_codec_id, config.video_codec_private, oversized, true),
              RandomAccessKind::ClosedGop);
    for (uint64_t second : {0ULL, 2ULL, 4ULL}) {
        MuxPacket item;
        item.track_num = 1;
        item.pts_ns = second * 1'000'000'000ULL;
        item.dts_ns = static_cast<int64_t>(item.pts_ns);
        item.is_key = true;
        item.bytes = second == 0 ? oversized : key_sample;
        ASSERT_TRUE(writer.Push(std::move(item))) << writer.error();
    }
    ASSERT_TRUE(writer.Finalize()) << writer.error();
    const auto seal = writer.finalized_segment_seal();
    ASSERT_TRUE(seal);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (index.journal_committed_through() < seal->final_commit_sequence && !index.needs_rebuild() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_FALSE(index.needs_rebuild());
    ASSERT_GE(index.journal_committed_through(), seal->final_commit_sequence);
    const auto frozen = index.Freeze();
    ASSERT_TRUE(frozen.succeeded()) << frozen.error()->context;
    const auto plan = PlanClipBoundaries(*frozen.value(), descriptor->recording_id, 0, 5'000'000'000);
    ASSERT_TRUE(plan.succeeded()) << plan.error()->context;
    HANDLE pinned =
        CreateFileW(source_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    ASSERT_NE(pinned, INVALID_HANDLE_VALUE);
    const ClipPinnedSource source{descriptor, pinned, std::nullopt};
    uint64_t admitted_read_bytes = 0;
    ClipJobIoControl control;
    control.before_io = [&](ClipJobIoPhase phase, uint64_t bytes) {
        if (phase != ClipJobIoPhase::Reading)
            return true;
        if (packet_limit > 4u * 1024 * 1024 && bytes > 256 * 1024 - admitted_read_bytes)
            return false;
        admitted_read_bytes += bytes;
        return true;
    };
    const auto output = exosnap_test::UniqueTempPath("clip_oversized_output.mkv");
    const auto result = RunRecordingClipJob(*plan.value(), *frozen.value(),
                                            std::span<const ClipPinnedSource>(&source, 1), output, nullptr, &control);
    if (packet_limit == 4u * 1024 * 1024) {
        ASSERT_TRUE(result.succeeded()) << result.error()->context;
        EXPECT_EQ(result.value()->peak_packet_bytes, 4u * 1024 * 1024);
        EXPECT_LE(result.value()->peak_cluster_payload_bytes, 6u * 1024 * 1024);
        EXPECT_GE(admitted_read_bytes, 4u * 1024 * 1024);
    } else {
        ASSERT_FALSE(result.succeeded());
        EXPECT_EQ(result.error()->code, EngineErrorCode::ResourceBudgetExceeded);
        EXPECT_LT(admitted_read_bytes, 256ULL * 1024);
    }
    CloseHandle(pinned);
    std::error_code ignored;
    std::filesystem::remove(output, ignored);
    std::filesystem::remove(source_path, ignored);
    std::filesystem::remove(journal, ignored);
}

TEST(RecordingClipTest, ExactPacketLimitIsCopied) {
    CheckCommittedPacketBudget(4u * 1024 * 1024);
}

TEST(RecordingClipTest, ExcessPacketIsRejectedBeforePayloadRead) {
    CheckCommittedPacketBudget(4u * 1024 * 1024 + 1);
}

TEST(RecordingClipTest, FrozenPlanUsesClosedRapAndRequiresMatchingSealForEos) {
    const auto journal = exosnap_test::UniqueTempPath("clip_plan.index");
    RecordingMediaDescriptor descriptor;
    descriptor.recording_id = NewId<RecordingId>();
    descriptor.media_id = NewId<MediaId>();
    descriptor.segment_id = NewId<SegmentId>();
    descriptor.writer_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.file_identity.volume_serial = 1;
    descriptor.path = "unused.mkv";
    descriptor.video.codec_id = "V_MPEG4/ISO/AVC";
    descriptor.immutable_header = {0x1a};
    descriptor.continuity_group = 1;
    constexpr uint64_t offset = 50ULL * 1024 * 1024 * 1024;
    CommittedMediaReceipt receipt;
    receipt.recording_id = descriptor.recording_id;
    receipt.segment_id = descriptor.segment_id;
    receipt.writer_generation = 1;
    receipt.descriptor_revision = 1;
    receipt.commit_sequence = 1;
    receipt.readable_end_offset = offset + 100;
    receipt.durable_end_offset = offset + 100;
    ClusterExtent extent;
    extent.file_begin = offset + 1;
    extent.file_end = offset + 100;
    extent.coverage.push_back({1, 2'000'000'000, 6'000'000'000, 1'995'000'000, 4'000'000'000});
    ClusterExtent prefix;
    prefix.file_begin = 1;
    prefix.file_end = offset + 1;
    prefix.coverage.push_back({1, 0, 2'000'000'000, -5'000'000, 1'995'000'000});
    prefix.random_access_points.push_back({1, 1, 2, 0, -5'000'000, RandomAccessKind::ClosedGop});
    receipt.clusters.push_back(std::move(prefix));
    extent.random_access_points.push_back(
        {1, offset + 1, offset + 3, 2'000'000'000, 1'995'000'000, RandomAccessKind::ClosedGop});
    extent.random_access_points.push_back(
        {1, offset + 1, offset + 4, 4'000'000'000, 3'995'000'000, RandomAccessKind::ClosedGop});
    receipt.clusters.push_back(std::move(extent));
    {
        RecordingMediaIndex index(journal);
        ASSERT_EQ(index.RegisterDescriptor(std::make_shared<const RecordingMediaDescriptor>(descriptor)),
                  ReceiptAcceptance::Accepted);
        ASSERT_EQ(index.PublishCommitted(receipt), ReceiptAcceptance::Accepted);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (index.journal_committed_through() < 1 && !index.needs_rebuild() &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ASSERT_FALSE(index.needs_rebuild());
        ASSERT_EQ(index.journal_committed_through(), 1u);
        const auto frozen = index.Freeze();
        ASSERT_TRUE(frozen.succeeded());
        const auto planned = PlanClipBoundaries(*frozen.value(), descriptor.recording_id, 2'500'000'000, 5'000'000'000);
        ASSERT_TRUE(planned.succeeded()) << planned.error()->context;
        EXPECT_EQ(planned.value()->actual_begin_r_ns, 2'000'000'000);
        EXPECT_EQ(planned.value()->actual_end_r_ns, 4'000'000'000);
        EXPECT_EQ(planned.value()->start_rap.dts_r_ns, 1'995'000'000);
        EXPECT_FALSE(planned.value()->end_at_sealed_eos);
        EXPECT_EQ(planned.value()->segments.size(), 1u);
        const auto storage = EstimateRecordingClipStorageBytes(*planned.value(), *frozen.value());
        ASSERT_TRUE(storage.succeeded()) << storage.error()->context;
        EXPECT_EQ(*storage.value(), 32ULL * 1024 * 1024 + 99 + 9 + 2 * 24);
        const auto cancelled_storage =
            EstimateRecordingClipStorageBytes(*planned.value(), *frozen.value(), [](uint64_t) { return false; });
        ASSERT_FALSE(cancelled_storage.succeeded());
        EXPECT_EQ(cancelled_storage.error()->code, EngineErrorCode::Cancelled);
        const RecordingSegmentSeal valid_seal{
            descriptor.recording_id, descriptor.segment_id, 1, 1, 1, offset + 100, 6'000'000'000};
        const auto sealed = PlanClipBoundaries(*frozen.value(), descriptor.recording_id, 2'500'000'000, 7'000'000'000,
                                               std::span<const RecordingSegmentSeal>(&valid_seal, 1));
        ASSERT_TRUE(sealed.succeeded()) << sealed.error()->context;
        EXPECT_TRUE(sealed.value()->end_at_sealed_eos);
        EXPECT_EQ(sealed.value()->actual_end_r_ns, 6'000'000'000);
        auto forged_seal = valid_seal;
        forged_seal.final_readable_end = offset + 99;
        const auto unsealed = PlanClipBoundaries(*frozen.value(), descriptor.recording_id, 2'500'000'000, 7'000'000'000,
                                                 std::span<const RecordingSegmentSeal>(&forged_seal, 1));
        ASSERT_TRUE(unsealed.succeeded()) << unsealed.error()->context;
        EXPECT_FALSE(unsealed.value()->end_at_sealed_eos);
        EXPECT_EQ(unsealed.value()->actual_end_r_ns, 4'000'000'000);
        EXPECT_FALSE(
            PlanClipBoundaries(*frozen.value(), descriptor.recording_id, 500'000'000, 1'500'000'000).succeeded());
    }
    std::error_code ignored;
    std::filesystem::remove(journal, ignored);
}

using DecodedVideoFrame = std::pair<int64_t, uint64_t>;

std::vector<DecodedVideoFrame> DecodeVideoReference(const std::filesystem::path& path, int64_t before_ns) {
    AVFormatContext* raw = nullptr;
    if (avformat_open_input(&raw, path.string().c_str(), nullptr, nullptr) < 0)
        return {};
    const auto close_input = [](AVFormatContext* input) { avformat_close_input(&input); };
    std::unique_ptr<AVFormatContext, decltype(close_input)> input(raw, close_input);
    if (avformat_find_stream_info(raw, nullptr) < 0)
        return {};
    const int video = av_find_best_stream(raw, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video < 0)
        return {};
    const auto* codec = avcodec_find_decoder(raw->streams[video]->codecpar->codec_id);
    if (!codec)
        return {};
    const auto free_decoder = [](AVCodecContext* value) { avcodec_free_context(&value); };
    std::unique_ptr<AVCodecContext, decltype(free_decoder)> decoder(avcodec_alloc_context3(codec), free_decoder);
    if (!decoder || avcodec_parameters_to_context(decoder.get(), raw->streams[video]->codecpar) < 0 ||
        avcodec_open2(decoder.get(), codec, nullptr) < 0)
        return {};
    const auto free_packet = [](AVPacket* value) { av_packet_free(&value); };
    const auto free_frame = [](AVFrame* value) { av_frame_free(&value); };
    std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
    std::unique_ptr<AVFrame, decltype(free_frame)> frame(av_frame_alloc(), free_frame);
    if (!packet || !frame)
        return {};
    std::vector<DecodedVideoFrame> decoded;
    const auto drain = [&] {
        int status = 0;
        while ((status = avcodec_receive_frame(decoder.get(), frame.get())) >= 0) {
            if (frame->best_effort_timestamp == AV_NOPTS_VALUE || !frame->data[0] || frame->width <= 0 ||
                frame->height <= 0 || frame->linesize[0] < frame->width)
                return false;
            const int64_t pts =
                av_rescale_q(frame->best_effort_timestamp, raw->streams[video]->time_base, {1, 1000000000});
            if (pts < before_ns) {
                uint64_t digest = 1469598103934665603ULL;
                for (int row = 0; row < frame->height; ++row)
                    for (int column = 0; column < frame->width; ++column) {
                        digest ^= frame->data[0][row * frame->linesize[0] + column];
                        digest *= 1099511628211ULL;
                    }
                decoded.emplace_back(pts, digest);
            }
            av_frame_unref(frame.get());
        }
        return status == AVERROR(EAGAIN) || status == AVERROR_EOF;
    };
    while (av_read_frame(raw, packet.get()) >= 0) {
        if (packet->stream_index == video && (avcodec_send_packet(decoder.get(), packet.get()) < 0 || !drain()))
            return {};
        av_packet_unref(packet.get());
    }
    if (avcodec_send_packet(decoder.get(), nullptr) < 0 || !drain())
        return {};
    return decoded;
}

TEST(RecordingClipTest, PinnedPcmClipCopiesACompleteReorderedH264GopAndVerifiedTrack) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
    const auto source_path = exosnap_test::UniqueTempPath("clip_pcm_source.mkv");
    const auto clip_path = exosnap_test::UniqueTempPath("clip_pcm_draft.mkv");
    const auto journal = exosnap_test::UniqueTempPath("clip_pcm.index");
    AVFormatContext* raw = nullptr;
    ASSERT_GE(avformat_open_input(&raw, fixture.string().c_str(), nullptr, nullptr), 0);
    const auto close_input = [](AVFormatContext* input) { avformat_close_input(&input); };
    std::unique_ptr<AVFormatContext, decltype(close_input)> input(raw, close_input);
    ASSERT_GE(avformat_find_stream_info(raw, nullptr), 0);
    const auto* video = raw->streams[0];
    MatroskaStreamConfig config;
    config.output_path = source_path.string();
    config.video_codec_id = "V_MPEG4/ISO/AVC";
    config.video_codec_private.assign(video->codecpar->extradata,
                                      video->codecpar->extradata + video->codecpar->extradata_size);
    config.encode_width = static_cast<uint32_t>(video->codecpar->width);
    config.encode_height = static_cast<uint32_t>(video->codecpar->height);
    config.frame_rate_num = 30;
    config.frame_rate_den = 1;
    config.audio_codec = StreamAudioCodec::Pcm;
    config.audio_track_count = 1;
    config.audio_sample_rate = 48000;
    config.audio_channels = 2;
    config.audio_bit_depth = 16;
    config.audio_tracks[0].name = "System";
    AudioManifest1 manifest;
    manifest.media_id = NewId<MediaId>();
    manifest.recording_id = NewId<RecordingId>();
    manifest.segment_id = NewId<SegmentId>();
    manifest.continuity_group = 1;
    AudioManifestTrack audio;
    audio.container.track_id = NewId<AudioTrackId>();
    audio.container.stream_index.value = 0;
    audio.container.track_uid = 2;
    audio.container.name = "System";
    audio.container.role = "standard";
    audio.container.codec.codec_id = "A_PCM/INT/LIT";
    audio.container.codec.sample_rate = 48000;
    audio.container.codec.channels = 2;
    audio.container.codec.bit_depth = 16;
    audio.container.codec.channel_layout = "stereo";
    audio.container.packet_duration_ns = 10'000'000;
    manifest.tracks.push_back(audio);
    manifest.default_monitor_track_ids.push_back(audio.container.track_id);
    ASSERT_TRUE(ValidateAudioManifest1(manifest).succeeded());
    config.audio_manifest = manifest;
    RecordingMediaIndex index(journal);
    config.publish_committed = [&](CommittedMediaReceipt receipt) {
        return index.PublishCommitted(std::move(receipt));
    };
    MatroskaStreamWriter writer;
    ASSERT_TRUE(writer.Open(config)) << writer.error();
    const auto descriptor = writer.media_descriptor();
    ASSERT_NE(descriptor, nullptr);
    ASSERT_EQ(index.RegisterDescriptor(descriptor), ReceiptAcceptance::Accepted);
    const auto free_packet = [](AVPacket* packet) { av_packet_free(&packet); };
    std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
    ASSERT_NE(packet, nullptr);
    std::vector<MuxPacket> video_packets;
    while (av_read_frame(raw, packet.get()) >= 0) {
        if (packet->stream_index == 0) {
            MuxPacket item;
            item.track_num = 1;
            item.pts_ns = static_cast<uint64_t>(av_rescale_q(packet->pts, video->time_base, {1, 1000000000}));
            item.dts_ns = av_rescale_q(packet->dts, video->time_base, {1, 1000000000});
            item.is_key = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            item.bytes.assign(packet->data, packet->data + packet->size);
            video_packets.push_back(std::move(item));
        }
        av_packet_unref(packet.get());
    }
    ASSERT_FALSE(video_packets.empty());
    for (uint64_t cycle = 0; cycle < 2; ++cycle) {
        std::vector<MuxPacket> packets;
        for (auto item : video_packets) {
            item.pts_ns += cycle * 4'000'000'000ULL;
            *item.dts_ns += static_cast<int64_t>(cycle * 4'000'000'000ULL);
            packets.push_back(std::move(item));
        }
        for (uint64_t pts = cycle * 4'000'000'000ULL; pts < (cycle + 1) * 4'000'000'000ULL; pts += 10'000'000ULL) {
            MuxPacket item;
            item.track_num = 2;
            item.pts_ns = pts;
            item.dts_ns = static_cast<int64_t>(pts);
            item.is_key = true;
            item.bytes.resize(480 * 2 * 2);
            for (size_t sample = 0; sample < 480; ++sample) {
                const auto value =
                    static_cast<uint16_t>(static_cast<int32_t>((pts / 10'000'000ULL * 480 + sample) % 32768) - 16384);
                for (size_t channel = 0; channel < 2; ++channel) {
                    const size_t offset = (sample * 2 + channel) * 2;
                    item.bytes[offset] = static_cast<uint8_t>(value);
                    item.bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
                }
            }
            packets.push_back(std::move(item));
        }
        std::stable_sort(packets.begin(), packets.end(), [](const auto& a, const auto& b) {
            return a.dts_ns.value_or(static_cast<int64_t>(a.pts_ns)) <
                   b.dts_ns.value_or(static_cast<int64_t>(b.pts_ns));
        });
        for (auto& item : packets)
            ASSERT_TRUE(writer.Push(std::move(item))) << writer.error();
    }
    ASSERT_TRUE(writer.Finalize()) << writer.error();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (index.journal_committed_through() < 2 && !index.needs_rebuild() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_FALSE(index.needs_rebuild());
    const auto frozen = index.Freeze();
    ASSERT_TRUE(frozen.succeeded()) << frozen.error()->context;
    const auto planned = PlanClipBoundaries(*frozen.value(), descriptor->recording_id, 500'000'000, 4'500'000'000);
    ASSERT_TRUE(planned.succeeded()) << planned.error()->context;
    EXPECT_EQ(planned.value()->actual_begin_r_ns, 0);
    EXPECT_EQ(planned.value()->actual_end_r_ns, 4'000'000'000);
    HANDLE pinned =
        CreateFileW(source_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    ASSERT_NE(pinned, INVALID_HANDLE_VALUE);
    const ClipPinnedSource source{descriptor, pinned, manifest};
    uint64_t measured_source_bytes = 0;
    uint64_t measured_staging_bytes = 0;
    bool saw_read = false;
    bool saw_remux = false;
    bool saw_finalize = false;
    ClipJobIoControl control;
    control.before_io = [](ClipJobIoPhase, uint64_t bytes) { return bytes != 0; };
    control.on_progress = [&](ClipJobIoPhase phase, uint64_t source_bytes, uint64_t staging_bytes) {
        EXPECT_GE(source_bytes, measured_source_bytes);
        EXPECT_GE(staging_bytes, measured_staging_bytes);
        measured_source_bytes = source_bytes;
        measured_staging_bytes = staging_bytes;
        saw_read |= phase == ClipJobIoPhase::Reading;
        saw_remux |= phase == ClipJobIoPhase::Remuxing;
        saw_finalize |= phase == ClipJobIoPhase::Finalizing;
    };
    const auto draft = RunRecordingClipJob(*planned.value(), *frozen.value(),
                                           std::span<const ClipPinnedSource>(&source, 1), clip_path, nullptr, &control);
    EXPECT_TRUE(saw_read);
    EXPECT_TRUE(saw_remux);
    EXPECT_TRUE(saw_finalize);
    EXPECT_GT(measured_source_bytes, 0u);
    EXPECT_GT(measured_staging_bytes, 0u);
    ClipJobIoControl refuse_read;
    refuse_read.before_io = [](ClipJobIoPhase phase, uint64_t) { return phase != ClipJobIoPhase::Reading; };
    const auto read_refused =
        RunRecordingClipJob(*planned.value(), *frozen.value(), std::span<const ClipPinnedSource>(&source, 1),
                            exosnap_test::UniqueTempPath("refused_read_clip.mkv"), nullptr, &refuse_read);
    ASSERT_FALSE(read_refused.succeeded());
    EXPECT_EQ(read_refused.error()->code, EngineErrorCode::Cancelled);
    std::atomic<bool> cancelled{true};
    const auto cancelled_job =
        RunRecordingClipJob(*planned.value(), *frozen.value(), std::span<const ClipPinnedSource>(&source, 1),
                            exosnap_test::UniqueTempPath("cancelled_clip.mkv"), &cancelled);
    ASSERT_FALSE(cancelled_job.succeeded());
    EXPECT_EQ(cancelled_job.error()->code, EngineErrorCode::Cancelled);
    CloseHandle(pinned);
    const ClipPinnedSource expired_source{descriptor, INVALID_HANDLE_VALUE, manifest};
    const auto expired_job =
        RunRecordingClipJob(*planned.value(), *frozen.value(), std::span<const ClipPinnedSource>(&expired_source, 1),
                            exosnap_test::UniqueTempPath("expired_clip.mkv"));
    ASSERT_FALSE(expired_job.succeeded());
    EXPECT_EQ(expired_job.error()->code, EngineErrorCode::SourceIntegrityFailure);
    ASSERT_TRUE(draft.succeeded()) << draft.error()->context;
    ASSERT_TRUE(draft.value()->output_manifest.has_value());
    EXPECT_EQ(draft.value()->output_manifest->tracks[0].container.track_id, audio.container.track_id);
    EXPECT_EQ(draft.value()->output_manifest->derived_from_media_ids, std::vector<MediaId>{descriptor->media_id});
    EXPECT_GT(draft.value()->video_packets, 0u);
    EXPECT_GT(draft.value()->peak_packet_bytes, 0u);
    EXPECT_LE(draft.value()->peak_packet_bytes, 4u * 1024u * 1024u);
    EXPECT_LE(draft.value()->peak_writer_window_bytes, 8u * 1024u * 1024u);
    EXPECT_LE(draft.value()->peak_cluster_payload_bytes, 8u * 1024u * 1024u);
    ASSERT_EQ(draft.value()->audio_packets.size(), 1u);
    EXPECT_EQ(draft.value()->audio_packets[0], 400u);
    ASSERT_EQ(draft.value()->audio_boundaries.size(), 1u);
    EXPECT_EQ(draft.value()->audio_boundaries[0].first_copied_packet_r_ns, 0);
    EXPECT_EQ(draft.value()->audio_boundaries[0].first_audible_r_ns, 0);
    EXPECT_EQ(draft.value()->audio_boundaries[0].last_audible_r_ns, 4'000'000'000);
    AVFormatContext* clipped = nullptr;
    ASSERT_GE(avformat_open_input(&clipped, clip_path.string().c_str(), nullptr, nullptr), 0);
    std::unique_ptr<AVFormatContext, decltype(close_input)> clipped_file(clipped, close_input);
    ASSERT_GE(avformat_find_stream_info(clipped, nullptr), 0);
    EXPECT_EQ(clipped->nb_streams, 2u);
    uint64_t copied_video = 0;
    uint64_t copied_audio = 0;
    bool nonzero_pcm = false;
    while (av_read_frame(clipped, packet.get()) >= 0) {
        copied_video += packet->stream_index == 0;
        copied_audio += packet->stream_index == 1;
        if (packet->stream_index == 1) {
            EXPECT_EQ(packet->size, 480 * 2 * 2);
            nonzero_pcm |=
                std::any_of(packet->data, packet->data + packet->size, [](uint8_t value) { return value != 0; });
        }
        av_packet_unref(packet.get());
    }
    EXPECT_EQ(copied_video, draft.value()->video_packets);
    EXPECT_EQ(copied_audio, draft.value()->audio_packets[0]);
    EXPECT_TRUE(nonzero_pcm);
    const auto original_video = DecodeVideoReference(source_path, 4'000'000'000);
    const auto clipped_video = DecodeVideoReference(clip_path, 4'000'000'000);
    ASSERT_FALSE(original_video.empty());
    EXPECT_EQ(clipped_video, original_video);
    EXPECT_EQ(clipped_video.front().first, 0);
    std::error_code ignored;
    std::filesystem::remove(source_path, ignored);
    std::filesystem::remove(clip_path, ignored);
    std::filesystem::remove(journal, ignored);
}

struct OpusDecodeReference {
    std::vector<float> samples;
    std::optional<int64_t> first_pts_ns;
    std::vector<std::pair<int64_t, size_t>> frame_offsets;
};

OpusDecodeReference DecodeOpusReference(const std::filesystem::path& path) {
    AVFormatContext* raw = nullptr;
    if (avformat_open_input(&raw, path.string().c_str(), nullptr, nullptr) < 0)
        return {};
    const auto close_input = [](AVFormatContext* input) { avformat_close_input(&input); };
    std::unique_ptr<AVFormatContext, decltype(close_input)> input(raw, close_input);
    if (avformat_find_stream_info(raw, nullptr) < 0)
        return {};
    const int audio = av_find_best_stream(raw, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio < 0)
        return {};
    const auto* codec = avcodec_find_decoder(raw->streams[audio]->codecpar->codec_id);
    if (!codec)
        return {};
    const auto free_decoder = [](AVCodecContext* value) { avcodec_free_context(&value); };
    std::unique_ptr<AVCodecContext, decltype(free_decoder)> decoder(avcodec_alloc_context3(codec), free_decoder);
    if (!decoder || avcodec_parameters_to_context(decoder.get(), raw->streams[audio]->codecpar) < 0 ||
        avcodec_open2(decoder.get(), codec, nullptr) < 0)
        return {};
    const auto free_packet = [](AVPacket* value) { av_packet_free(&value); };
    const auto free_frame = [](AVFrame* value) { av_frame_free(&value); };
    std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
    std::unique_ptr<AVFrame, decltype(free_frame)> frame(av_frame_alloc(), free_frame);
    if (!packet || !frame)
        return {};
    OpusDecodeReference result;
    const auto drain = [&] {
        int status = 0;
        while ((status = avcodec_receive_frame(decoder.get(), frame.get())) >= 0) {
            const auto format = static_cast<AVSampleFormat>(frame->format);
            if (av_get_packed_sample_fmt(format) != AV_SAMPLE_FMT_FLT || frame->ch_layout.nb_channels != 2)
                return false;
            if (!result.first_pts_ns && frame->best_effort_timestamp != AV_NOPTS_VALUE)
                result.first_pts_ns =
                    av_rescale_q(frame->best_effort_timestamp, raw->streams[audio]->time_base, {1, 1000000000});
            if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                result.frame_offsets.emplace_back(
                    av_rescale_q(frame->best_effort_timestamp, raw->streams[audio]->time_base, {1, 1000000000}),
                    result.samples.size());
            const auto* channel = reinterpret_cast<const float*>(frame->extended_data[0]);
            const int stride = av_sample_fmt_is_planar(format) ? 1 : 2;
            for (int sample = 0; sample < frame->nb_samples; ++sample)
                result.samples.push_back(channel[sample * stride]);
            av_frame_unref(frame.get());
        }
        return status == AVERROR(EAGAIN) || status == AVERROR_EOF;
    };
    while (av_read_frame(raw, packet.get()) >= 0) {
        if (packet->stream_index == audio && (avcodec_send_packet(decoder.get(), packet.get()) < 0 || !drain()))
            return {};
        av_packet_unref(packet.get());
    }
    if (avcodec_send_packet(decoder.get(), nullptr) < 0 || !drain())
        return {};
    return result;
}

void CheckOpusClipDuration(OpusFrameDuration frame_duration) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
    const auto source_path = exosnap_test::UniqueTempPath("clip_opus_source.mkv");
    const auto clip_path = exosnap_test::UniqueTempPath("clip_opus_draft.mkv");
    const auto journal = exosnap_test::UniqueTempPath("clip_opus.index");
    AVFormatContext* raw = nullptr;
    ASSERT_GE(avformat_open_input(&raw, fixture.string().c_str(), nullptr, nullptr), 0);
    const auto close_input = [](AVFormatContext* input) { avformat_close_input(&input); };
    std::unique_ptr<AVFormatContext, decltype(close_input)> input(raw, close_input);
    ASSERT_GE(avformat_find_stream_info(raw, nullptr), 0);
    const auto* video = raw->streams[0];
    std::vector<MuxPacket> video_packets;
    const auto free_packet = [](AVPacket* packet) { av_packet_free(&packet); };
    std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
    ASSERT_NE(packet, nullptr);
    while (av_read_frame(raw, packet.get()) >= 0) {
        if (packet->stream_index == 0) {
            MuxPacket item;
            item.track_num = 1;
            item.pts_ns = static_cast<uint64_t>(av_rescale_q(packet->pts, video->time_base, {1, 1000000000}));
            item.dts_ns = av_rescale_q(packet->dts, video->time_base, {1, 1000000000});
            item.is_key = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            item.bytes.assign(packet->data, packet->data + packet->size);
            video_packets.push_back(std::move(item));
        }
        av_packet_unref(packet.get());
    }
    ASSERT_FALSE(video_packets.empty());
    OpusAudioEncoder encoder;
    encoder.SetEncodingParams(0, frame_duration);
    std::string encoder_error;
    ASSERT_TRUE(encoder.Init(48000, 2, encoder_error)) << encoder_error;
    constexpr size_t frames = 48000 * 9;
    std::vector<float> pcm(frames * 2);
    for (size_t frame = 0; frame < frames; ++frame) {
        const float sample = static_cast<float>(0.2 * std::sin(static_cast<double>(frame) * 0.061));
        pcm[2 * frame] = sample;
        pcm[2 * frame + 1] = sample;
    }
    uint64_t accumulated = 0;
    std::vector<EncodedAudioPacket> encoded;
    encoder.FeedFloat32(pcm.data(), pcm.size(), 0, accumulated, 48000, 2, encoded);
    ASSERT_GE(encoded.size(), 401u);
    MatroskaStreamConfig config;
    config.output_path = source_path.string();
    config.video_codec_id = "V_MPEG4/ISO/AVC";
    config.video_codec_private.assign(video->codecpar->extradata,
                                      video->codecpar->extradata + video->codecpar->extradata_size);
    config.encode_width = static_cast<uint32_t>(video->codecpar->width);
    config.encode_height = static_cast<uint32_t>(video->codecpar->height);
    config.frame_rate_num = 30;
    config.frame_rate_den = 1;
    config.audio_codec = StreamAudioCodec::Opus;
    config.opus_frame_samples = static_cast<uint32_t>(encoder.FrameSizeSamples());
    const uint64_t packet_duration_ns = static_cast<uint64_t>(encoder.FrameSizeSamples()) * 1000000000ULL / 48000;
    config.audio_track_count = 1;
    config.audio_sample_rate = 48000;
    config.audio_channels = 2;
    config.audio_tracks[0].name = "System";
    config.audio_tracks[0].codec_private = encoder.CodecPrivateBytes();
    config.audio_tracks[0].codec_delay_samples = encoder.CodecDelaySamples();
    AudioManifest1 manifest;
    manifest.media_id = NewId<MediaId>();
    manifest.recording_id = NewId<RecordingId>();
    manifest.segment_id = NewId<SegmentId>();
    manifest.continuity_group = 1;
    AudioManifestTrack audio;
    audio.container.track_id = NewId<AudioTrackId>();
    audio.container.stream_index.value = 0;
    audio.container.track_uid = 2;
    audio.container.name = "System";
    audio.container.role = "standard";
    audio.container.codec.codec_id = "A_OPUS";
    audio.container.codec.codec_private = encoder.CodecPrivateBytes();
    audio.container.codec.codec_delay_samples = encoder.CodecDelaySamples();
    audio.container.codec.sample_rate = 48000;
    audio.container.codec.channels = 2;
    audio.container.codec.channel_layout = "stereo";
    audio.container.encoder_instance_id = NewId<AudioEncoderInstanceId>();
    audio.container.encoder_reset = true;
    audio.container.packet_duration_ns = packet_duration_ns;
    audio.container.preroll_ns = 80'000'000;
    manifest.tracks.push_back(audio);
    manifest.default_monitor_track_ids.push_back(audio.container.track_id);
    ASSERT_TRUE(ValidateAudioManifest1(manifest).succeeded());
    config.audio_manifest = manifest;
    RecordingMediaIndex index(journal);
    config.publish_committed = [&](CommittedMediaReceipt receipt) {
        return index.PublishCommitted(std::move(receipt));
    };
    MatroskaStreamWriter writer;
    ASSERT_TRUE(writer.Open(config)) << writer.error();
    const auto descriptor = writer.media_descriptor();
    ASSERT_NE(descriptor, nullptr);
    ASSERT_EQ(index.RegisterDescriptor(descriptor), ReceiptAcceptance::Accepted);
    std::vector<MuxPacket> packets;
    for (uint64_t cycle = 0; cycle < 3; ++cycle)
        for (auto item : video_packets) {
            item.pts_ns += cycle * 4'000'000'000ULL;
            *item.dts_ns += static_cast<int64_t>(cycle * 4'000'000'000ULL);
            packets.push_back(std::move(item));
        }
    const uint64_t delay_ns = static_cast<uint64_t>(encoder.CodecDelaySamples()) * 1000000000ULL / 48000;
    for (const auto& item : encoded) {
        MuxPacket output;
        output.track_num = 2;
        output.pts_ns = item.pts_ns + delay_ns;
        output.dts_ns = static_cast<int64_t>(output.pts_ns);
        output.is_key = true;
        output.bytes = item.bytes;
        packets.push_back(std::move(output));
    }
    std::stable_sort(packets.begin(), packets.end(), [](const auto& a, const auto& b) {
        return a.dts_ns.value_or(static_cast<int64_t>(a.pts_ns)) < b.dts_ns.value_or(static_cast<int64_t>(b.pts_ns));
    });
    for (auto& item : packets)
        ASSERT_TRUE(writer.Push(std::move(item))) << writer.error();
    ASSERT_TRUE(writer.Finalize()) << writer.error();
    const auto seal = writer.finalized_segment_seal();
    ASSERT_TRUE(seal.has_value());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (index.journal_committed_through() < seal->final_commit_sequence && !index.needs_rebuild() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_FALSE(index.needs_rebuild());
    const auto frozen = index.Freeze();
    ASSERT_TRUE(frozen.succeeded()) << frozen.error()->context;
    const auto planned = PlanClipBoundaries(*frozen.value(), descriptor->recording_id, 4'200'000'000, 8'100'000'000);
    ASSERT_TRUE(planned.succeeded()) << planned.error()->context;
    EXPECT_EQ(planned.value()->actual_begin_r_ns, 4'000'000'000);
    EXPECT_EQ(planned.value()->actual_end_r_ns, 8'000'000'000);
    EXPECT_EQ(planned.value()->read_begin_r_ns, 3'920'000'000);
    HANDLE pinned =
        CreateFileW(source_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    ASSERT_NE(pinned, INVALID_HANDLE_VALUE);
    const ClipPinnedSource source{descriptor, pinned, manifest};
    const auto draft = RunRecordingClipJob(*planned.value(), *frozen.value(),
                                           std::span<const ClipPinnedSource>(&source, 1), clip_path);
    CloseHandle(pinned);
    ASSERT_TRUE(draft.succeeded()) << draft.error()->context;
    EXPECT_EQ(draft.value()->audio_boundaries[0].first_audible_r_ns, draft.value()->actual_begin_r_ns);
    ASSERT_TRUE(draft.value()->output_manifest.has_value());
    EXPECT_EQ(draft.value()->output_manifest->tracks[0].container.codec.codec_delay_samples, 3840u);
    ASSERT_EQ(draft.value()->audio_packets.size(), 1u);
    EXPECT_EQ(draft.value()->audio_packets[0], 4'080'000'000ULL / packet_duration_ns);
    ASSERT_EQ(draft.value()->audio_boundaries.size(), 1u);
    EXPECT_EQ(draft.value()->audio_boundaries[0].first_copied_packet_r_ns, 3'920'000'000);
    EXPECT_EQ(draft.value()->audio_boundaries[0].first_audible_r_ns, 4'000'000'000);
    EXPECT_EQ(draft.value()->audio_boundaries[0].last_audible_r_ns, 8'000'000'000);
    EXPECT_EQ(draft.value()->audio_boundaries[0].decoder_discard_samples, 3840u);
    const auto source_audio = DecodeOpusReference(source_path);
    const auto clip_audio = DecodeOpusReference(clip_path);
    ASSERT_TRUE(source_audio.first_pts_ns.has_value());
    const int64_t expected_start_ns =
        frame_duration == OpusFrameDuration::Ms5 || frame_duration == OpusFrameDuration::Ms2_5 ? 5'000'000 : 0;
    EXPECT_EQ(*source_audio.first_pts_ns, expected_start_ns);
    ASSERT_TRUE(clip_audio.first_pts_ns.has_value());
    EXPECT_EQ(*clip_audio.first_pts_ns, 0);
    const auto reference = std::find_if(source_audio.frame_offsets.begin(), source_audio.frame_offsets.end(),
                                        [](const auto& frame) { return frame.first == 4'000'000'000; });
    ASSERT_NE(reference, source_audio.frame_offsets.end());
    ASSERT_GE(source_audio.samples.size(), reference->second + clip_audio.samples.size());
    EXPECT_EQ(clip_audio.samples.size(), 192000u);
    double error = 0;
    for (size_t sample = 0; sample < clip_audio.samples.size(); ++sample) {
        const double difference = clip_audio.samples[sample] - source_audio.samples[reference->second + sample];
        error += difference * difference;
    }
    EXPECT_LT(std::sqrt(error / clip_audio.samples.size()), 0.02);
    const auto first_path = exosnap_test::UniqueTempPath("clip_opus_split_first.mkv");
    const auto second_path = exosnap_test::UniqueTempPath("clip_opus_split_second.mkv");
    const auto joined_path = exosnap_test::UniqueTempPath("clip_opus_split_joined.mkv");
    const auto split_journal = exosnap_test::UniqueTempPath("clip_opus_split.index");
    RecordingMediaIndex split_index(split_journal);
    auto first_config = config;
    first_config.output_path = first_path.string();
    first_config.audio_manifest->media_id = NewId<MediaId>();
    first_config.audio_manifest->segment_id = NewId<SegmentId>();
    first_config.publish_committed = [&](CommittedMediaReceipt receipt) {
        return split_index.PublishCommitted(std::move(receipt));
    };
    MatroskaStreamWriter first_writer;
    ASSERT_TRUE(first_writer.Open(first_config)) << first_writer.error();
    const auto first_descriptor = first_writer.media_descriptor();
    ASSERT_EQ(split_index.RegisterDescriptor(first_descriptor), ReceiptAcceptance::Accepted);
    std::vector<MuxPacket> first_packets;
    for (auto item : video_packets)
        first_packets.push_back(std::move(item));
    for (size_t packet_index = 0; packet_index < 4'000'000'000ULL / packet_duration_ns; ++packet_index) {
        MuxPacket item;
        item.track_num = 2;
        item.pts_ns = encoded[packet_index].pts_ns + delay_ns;
        item.dts_ns = static_cast<int64_t>(item.pts_ns);
        item.is_key = true;
        item.bytes = encoded[packet_index].bytes;
        first_packets.push_back(std::move(item));
    }
    const auto by_decode_time = [](const MuxPacket& a, const MuxPacket& b) {
        return a.dts_ns.value_or(static_cast<int64_t>(a.pts_ns)) < b.dts_ns.value_or(static_cast<int64_t>(b.pts_ns));
    };
    std::stable_sort(first_packets.begin(), first_packets.end(), by_decode_time);
    for (auto& item : first_packets)
        ASSERT_TRUE(first_writer.Push(std::move(item))) << first_writer.error();
    ASSERT_TRUE(first_writer.Finalize()) << first_writer.error();
    const auto first_seal = first_writer.finalized_segment_seal();
    ASSERT_TRUE(first_seal.has_value());
    auto second_config = config;
    second_config.output_path = second_path.string();
    second_config.initial_commit_sequence = first_seal->final_commit_sequence;
    second_config.audio_tracks[0].codec_private[10] = 0;
    second_config.audio_tracks[0].codec_private[11] = 15;
    second_config.audio_tracks[0].codec_delay_samples = 3840;
    second_config.audio_manifest->media_id = NewId<MediaId>();
    second_config.audio_manifest->segment_id = NewId<SegmentId>();
    second_config.audio_manifest->segment_origin_r_ns = 3'920'000'000;
    second_config.audio_manifest->tracks[0].container.codec.codec_private = second_config.audio_tracks[0].codec_private;
    second_config.audio_manifest->tracks[0].container.codec.codec_delay_samples = 3840;
    second_config.audio_manifest->tracks[0].container.encoder_reset = false;
    second_config.audio_manifest->tracks[0].container.first_decoded_sample_r = 192000;
    second_config.publish_committed = [&](CommittedMediaReceipt receipt) {
        return split_index.PublishCommitted(std::move(receipt));
    };
    MatroskaStreamWriter second_writer;
    ASSERT_TRUE(second_writer.Open(second_config)) << second_writer.error();
    const auto second_descriptor = second_writer.media_descriptor();
    ASSERT_EQ(split_index.RegisterDescriptor(second_descriptor), ReceiptAcceptance::Accepted);
    std::vector<MuxPacket> second_packets;
    for (uint64_t cycle = 1; cycle < 3; ++cycle)
        for (auto item : video_packets) {
            item.pts_ns += cycle * 4'000'000'000ULL - 3'920'000'000ULL;
            *item.dts_ns += static_cast<int64_t>(cycle * 4'000'000'000ULL - 3'920'000'000ULL);
            second_packets.push_back(std::move(item));
        }
    for (size_t packet_index = 3'920'000'000ULL / packet_duration_ns; packet_index < encoded.size(); ++packet_index) {
        MuxPacket item;
        item.track_num = 2;
        item.pts_ns = encoded[packet_index].pts_ns - 3'920'000'000ULL + 80'000'000ULL;
        item.dts_ns = static_cast<int64_t>(item.pts_ns);
        item.is_key = true;
        item.bytes = encoded[packet_index].bytes;
        second_packets.push_back(std::move(item));
    }
    std::stable_sort(second_packets.begin(), second_packets.end(), by_decode_time);
    for (auto& item : second_packets)
        ASSERT_TRUE(second_writer.Push(std::move(item))) << second_writer.error();
    ASSERT_TRUE(second_writer.Finalize()) << second_writer.error();
    const auto second_seal = second_writer.finalized_segment_seal();
    ASSERT_TRUE(second_seal.has_value());
    const auto split_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (split_index.journal_committed_through() < second_seal->final_commit_sequence &&
           !split_index.needs_rebuild() && std::chrono::steady_clock::now() < split_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_FALSE(split_index.needs_rebuild());
    const auto split_snapshot = split_index.Freeze();
    ASSERT_TRUE(split_snapshot.succeeded()) << split_snapshot.error()->context;
    const auto split_plan =
        PlanClipBoundaries(*split_snapshot.value(), first_descriptor->recording_id, 500'000'000, 8'100'000'000);
    if (frame_duration == OpusFrameDuration::Ms2_5) {
        ASSERT_FALSE(split_plan.succeeded());
        EXPECT_EQ(split_plan.error()->code, EngineErrorCode::UnsupportedFormat);
        const auto later_plan =
            PlanClipBoundaries(*split_snapshot.value(), first_descriptor->recording_id, 4'200'000'000, 8'100'000'000);
        ASSERT_TRUE(later_plan.succeeded()) << later_plan.error()->context;
        ASSERT_EQ(later_plan.value()->segments.size(), 1u);
        HANDLE later_handle =
            CreateFileW(second_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        ASSERT_NE(later_handle, INVALID_HANDLE_VALUE);
        const std::array<ClipPinnedSource, 1> later_sources{
            {{second_descriptor, later_handle, *second_config.audio_manifest}}};
        const auto later =
            RunRecordingClipJob(*later_plan.value(), *split_snapshot.value(), later_sources, joined_path);
        CloseHandle(later_handle);
        ASSERT_TRUE(later.succeeded()) << later.error()->context;
        const auto later_audio = DecodeOpusReference(joined_path);
        EXPECT_EQ(later_audio.first_pts_ns, clip_audio.first_pts_ns);
        ASSERT_EQ(later_audio.samples.size(), clip_audio.samples.size());
        double later_error = 0;
        for (size_t sample = 0; sample < later_audio.samples.size(); ++sample) {
            const double difference = later_audio.samples[sample] - clip_audio.samples[sample];
            later_error += difference * difference;
        }
        EXPECT_LT(std::sqrt(later_error / later_audio.samples.size()), 0.02);
        EXPECT_EQ(DecodeVideoReference(joined_path, 4'000'000'000), DecodeVideoReference(clip_path, 4'000'000'000));
        std::error_code ignored;
        std::filesystem::remove(source_path, ignored);
        std::filesystem::remove(clip_path, ignored);
        std::filesystem::remove(journal, ignored);
        std::filesystem::remove(first_path, ignored);
        std::filesystem::remove(second_path, ignored);
        std::filesystem::remove(joined_path, ignored);
        std::filesystem::remove(split_journal, ignored);
        return;
    }
    ASSERT_TRUE(split_plan.succeeded()) << split_plan.error()->context;
    ASSERT_EQ(split_plan.value()->segments.size(), 2u);
    EXPECT_EQ(split_plan.value()->actual_begin_r_ns, 0);
    EXPECT_EQ(split_plan.value()->actual_end_r_ns, 8'000'000'000);
    HANDLE first_handle =
        CreateFileW(first_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    HANDLE second_handle =
        CreateFileW(second_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    ASSERT_NE(first_handle, INVALID_HANDLE_VALUE);
    ASSERT_NE(second_handle, INVALID_HANDLE_VALUE);
    const std::array<ClipPinnedSource, 2> split_sources{
        {{first_descriptor, first_handle, *first_config.audio_manifest},
         {second_descriptor, second_handle, *second_config.audio_manifest}}};
    const auto boundary_plan =
        PlanClipBoundaries(*split_snapshot.value(), first_descriptor->recording_id, 500'000'000, 4'100'000'000);
    ASSERT_TRUE(boundary_plan.succeeded()) << boundary_plan.error()->context;
    EXPECT_EQ(boundary_plan.value()->actual_end_r_ns, 4'000'000'000);
    const auto boundary_path = exosnap_test::UniqueTempPath("clip_opus_split_boundary.mkv");
    const auto boundary =
        RunRecordingClipJob(*boundary_plan.value(), *split_snapshot.value(),
                            std::span(split_sources).first(boundary_plan.value()->segments.size()), boundary_path);
    ASSERT_TRUE(boundary.succeeded()) << boundary.error()->context;
    ASSERT_EQ(boundary.value()->source_ranges.size(), 1u);
    ASSERT_TRUE(boundary.value()->output_manifest.has_value());
    EXPECT_EQ(boundary.value()->output_manifest->derived_from_media_ids.size(), 1u);
    EXPECT_EQ(boundary.value()->audio_packets[0], 4'000'000'000ULL / packet_duration_ns);
    EXPECT_EQ(boundary.value()->source_ranges[0].end_r_ns, 4'000'000'000);
    EXPECT_EQ(DecodeVideoReference(boundary_path, 5'000'000'000), DecodeVideoReference(source_path, 4'000'000'000));
    std::error_code boundary_remove_error;
    std::filesystem::remove(boundary_path, boundary_remove_error);
    const auto joined = RunRecordingClipJob(*split_plan.value(), *split_snapshot.value(), split_sources, joined_path);
    CloseHandle(first_handle);
    CloseHandle(second_handle);
    ASSERT_TRUE(joined.succeeded()) << joined.error()->context;
    ASSERT_EQ(joined.value()->source_ranges.size(), 2u);
    EXPECT_EQ(joined.value()->source_ranges[0].recording_id, first_descriptor->recording_id);
    EXPECT_EQ(joined.value()->source_ranges[0].segment_id, first_descriptor->segment_id);
    EXPECT_EQ(joined.value()->source_ranges[0].media_id, first_descriptor->media_id);
    EXPECT_EQ(joined.value()->source_ranges[0].begin_r_ns, 0);
    EXPECT_EQ(joined.value()->source_ranges[0].end_r_ns, 4'000'000'000);
    EXPECT_EQ(joined.value()->source_ranges[1].segment_id, second_descriptor->segment_id);
    EXPECT_EQ(joined.value()->source_ranges[1].media_id, second_descriptor->media_id);
    EXPECT_EQ(joined.value()->source_ranges[1].begin_r_ns, 4'000'000'000);
    EXPECT_EQ(joined.value()->source_ranges[1].end_r_ns, 8'000'000'000);
    ASSERT_EQ(joined.value()->audio_packets.size(), 1u);
    EXPECT_EQ(joined.value()->audio_packets[0], 8'000'000'000ULL / packet_duration_ns);
    ASSERT_TRUE(joined.value()->output_manifest.has_value());
    EXPECT_EQ(joined.value()->output_manifest->derived_from_media_ids.size(), 2u);
    const auto joined_audio = DecodeOpusReference(joined_path);
    ASSERT_TRUE(joined_audio.first_pts_ns.has_value());
    EXPECT_EQ(*joined_audio.first_pts_ns, expected_start_ns);
    ASSERT_GE(source_audio.samples.size(), joined_audio.samples.size());
    EXPECT_EQ(joined_audio.samples.size(), 383688u);
    double split_error = 0;
    for (size_t sample = 0; sample < joined_audio.samples.size(); ++sample) {
        const double difference = joined_audio.samples[sample] - source_audio.samples[sample];
        split_error += difference * difference;
    }
    EXPECT_LT(std::sqrt(split_error / joined_audio.samples.size()), 0.02);
    const auto original_video = DecodeVideoReference(source_path, 8'000'000'000);
    const auto joined_video = DecodeVideoReference(joined_path, 8'000'000'000);
    ASSERT_FALSE(original_video.empty());
    EXPECT_EQ(joined_video, original_video);
    std::error_code ignored;
    std::filesystem::remove(source_path, ignored);
    std::filesystem::remove(clip_path, ignored);
    std::filesystem::remove(journal, ignored);
    std::filesystem::remove(first_path, ignored);
    std::filesystem::remove(second_path, ignored);
    std::filesystem::remove(joined_path, ignored);
    std::filesystem::remove(split_journal, ignored);
}

TEST(RecordingClipTest, MidRecordingOpusClipConsumesDecoderPrerollOnce) {
    CheckOpusClipDuration(OpusFrameDuration::Ms20);
}
TEST(RecordingClipTest, TenMillisecondOpusClipPreservesAudibleBoundary) {
    CheckOpusClipDuration(OpusFrameDuration::Ms10);
}
TEST(RecordingClipTest, FiveMillisecondOpusClipPreservesAudibleBoundary) {
    CheckOpusClipDuration(OpusFrameDuration::Ms5);
}
TEST(RecordingClipTest, TwoPointFiveMillisecondOpusClipPreservesAudibleBoundary) {
    CheckOpusClipDuration(OpusFrameDuration::Ms2_5);
}

void CheckPcmTrackCount(unsigned track_count) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
    const auto source_path = exosnap_test::UniqueTempPath("clip_track_count_source.mkv");
    const auto clip_path = exosnap_test::UniqueTempPath("clip_track_count_draft.mkv");
    const auto journal = exosnap_test::UniqueTempPath("clip_track_count.index");
    AVFormatContext* raw = nullptr;
    ASSERT_GE(avformat_open_input(&raw, fixture.string().c_str(), nullptr, nullptr), 0);
    const auto close_input = [](AVFormatContext* input) { avformat_close_input(&input); };
    std::unique_ptr<AVFormatContext, decltype(close_input)> input(raw, close_input);
    ASSERT_GE(avformat_find_stream_info(raw, nullptr), 0);
    const auto* video = raw->streams[0];
    std::vector<MuxPacket> video_packets;
    const auto free_packet = [](AVPacket* packet) { av_packet_free(&packet); };
    std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
    ASSERT_NE(packet, nullptr);
    while (av_read_frame(raw, packet.get()) >= 0) {
        if (packet->stream_index == 0) {
            MuxPacket item;
            item.track_num = 1;
            item.pts_ns = static_cast<uint64_t>(av_rescale_q(packet->pts, video->time_base, {1, 1000000000}));
            item.dts_ns = av_rescale_q(packet->dts, video->time_base, {1, 1000000000});
            item.is_key = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            item.bytes.assign(packet->data, packet->data + packet->size);
            video_packets.push_back(std::move(item));
        }
        av_packet_unref(packet.get());
    }
    MatroskaStreamConfig config;
    config.output_path = source_path.string();
    config.video_codec_id = "V_MPEG4/ISO/AVC";
    config.video_codec_private.assign(video->codecpar->extradata,
                                      video->codecpar->extradata + video->codecpar->extradata_size);
    config.encode_width = static_cast<uint32_t>(video->codecpar->width);
    config.encode_height = static_cast<uint32_t>(video->codecpar->height);
    config.frame_rate_num = 30;
    config.frame_rate_den = 1;
    config.audio_codec = StreamAudioCodec::Pcm;
    config.audio_track_count = track_count;
    config.audio_sample_rate = 48000;
    config.audio_channels = 2;
    config.audio_bit_depth = 16;
    AudioManifest1 manifest;
    manifest.media_id = NewId<MediaId>();
    manifest.recording_id = NewId<RecordingId>();
    manifest.segment_id = NewId<SegmentId>();
    manifest.continuity_group = 1;
    for (unsigned track = 0; track < track_count; ++track) {
        AudioManifestTrack entry;
        entry.container.track_id = NewId<AudioTrackId>();
        entry.container.stream_index.value = track;
        entry.container.track_uid = track + 2;
        entry.container.name = "Stem " + std::to_string(track);
        entry.container.role = "stem";
        entry.container.codec.codec_id = "A_PCM/INT/LIT";
        entry.container.codec.sample_rate = 48000;
        entry.container.codec.channels = 2;
        entry.container.codec.bit_depth = 16;
        entry.container.codec.channel_layout = "stereo";
        entry.container.packet_duration_ns = 10'000'000;
        config.audio_tracks[track].name = entry.container.name;
        manifest.tracks.push_back(std::move(entry));
    }
    if (track_count) {
        manifest.default_monitor_track_ids.push_back(manifest.tracks.front().container.track_id);
        ASSERT_TRUE(ValidateAudioManifest1(manifest).succeeded());
        config.audio_manifest = manifest;
    }
    RecordingMediaIndex index(journal);
    config.publish_committed = [&](CommittedMediaReceipt receipt) {
        return index.PublishCommitted(std::move(receipt));
    };
    MatroskaStreamWriter writer;
    ASSERT_TRUE(writer.Open(config)) << writer.error();
    const auto descriptor = writer.media_descriptor();
    ASSERT_NE(descriptor, nullptr);
    ASSERT_EQ(index.RegisterDescriptor(descriptor), ReceiptAcceptance::Accepted);
    for (uint64_t cycle = 0; cycle < 2; ++cycle) {
        std::vector<MuxPacket> packets;
        for (auto item : video_packets) {
            item.pts_ns += cycle * 4'000'000'000ULL;
            *item.dts_ns += static_cast<int64_t>(cycle * 4'000'000'000ULL);
            packets.push_back(std::move(item));
        }
        for (uint64_t pts = cycle * 4'000'000'000ULL; pts < (cycle + 1) * 4'000'000'000ULL; pts += 10'000'000ULL)
            for (unsigned track = 0; track < track_count; ++track) {
                MuxPacket item;
                item.track_num = track + 2;
                item.pts_ns = pts;
                item.dts_ns = static_cast<int64_t>(pts);
                item.is_key = true;
                item.bytes.assign(480 * 2 * 2, static_cast<uint8_t>(track + 1));
                packets.push_back(std::move(item));
            }
        std::stable_sort(packets.begin(), packets.end(), [](const auto& a, const auto& b) {
            return a.dts_ns.value_or(static_cast<int64_t>(a.pts_ns)) <
                   b.dts_ns.value_or(static_cast<int64_t>(b.pts_ns));
        });
        for (auto& item : packets)
            ASSERT_TRUE(writer.Push(std::move(item))) << writer.error();
    }
    ASSERT_TRUE(writer.Finalize()) << writer.error();
    const auto seal = writer.finalized_segment_seal();
    ASSERT_TRUE(seal.has_value());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (index.journal_committed_through() < seal->final_commit_sequence && !index.needs_rebuild() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_FALSE(index.needs_rebuild());
    const auto frozen = index.Freeze();
    ASSERT_TRUE(frozen.succeeded()) << frozen.error()->context;
    const auto planned = PlanClipBoundaries(*frozen.value(), descriptor->recording_id, 500'000'000, 4'500'000'000);
    ASSERT_TRUE(planned.succeeded()) << planned.error()->context;
    EXPECT_EQ(planned.value()->actual_begin_r_ns, 0);
    EXPECT_EQ(planned.value()->actual_end_r_ns, 4'000'000'000);
    HANDLE pinned =
        CreateFileW(source_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    ASSERT_NE(pinned, INVALID_HANDLE_VALUE);
    const ClipPinnedSource source{descriptor, pinned, track_count ? std::optional(manifest) : std::nullopt};
    const auto draft = RunRecordingClipJob(*planned.value(), *frozen.value(),
                                           std::span<const ClipPinnedSource>(&source, 1), clip_path);
    CloseHandle(pinned);
    ASSERT_TRUE(draft.succeeded()) << draft.error()->context;
    EXPECT_EQ(draft.value()->audio_packets.size(), track_count);
    EXPECT_EQ(draft.value()->audio_boundaries.size(), track_count);
    EXPECT_EQ(draft.value()->output_manifest.has_value(), track_count != 0);
    for (unsigned track = 0; track < track_count; ++track) {
        EXPECT_EQ(draft.value()->audio_packets[track], 400u);
        EXPECT_EQ(draft.value()->audio_boundaries[track].track_id, manifest.tracks[track].container.track_id);
    }
    AVFormatContext* clipped = nullptr;
    ASSERT_GE(avformat_open_input(&clipped, clip_path.string().c_str(), nullptr, nullptr), 0);
    std::unique_ptr<AVFormatContext, decltype(close_input)> clipped_file(clipped, close_input);
    ASSERT_GE(avformat_find_stream_info(clipped, nullptr), 0);
    EXPECT_EQ(clipped->nb_streams, track_count + 1);
    std::error_code ignored;
    std::filesystem::remove(source_path, ignored);
    std::filesystem::remove(clip_path, ignored);
    std::filesystem::remove(journal, ignored);
}

TEST(RecordingClipTest, CopiesZeroThreeAndEightPcmTracksWithoutDroppingTopology) {
    CheckPcmTrackCount(0);
    CheckPcmTrackCount(3);
    CheckPcmTrackCount(8);
}

} // namespace
} // namespace exosnap::engine
