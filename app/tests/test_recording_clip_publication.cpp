#include "matroska_stream_writer.h"
#include "media_identity_sidecar.h"
#include "models/MarkerSidecar.h"
#include "recording_media_index.h"
#include "services/RecordingClipJobs.h"
#include "test_unique_temp.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

extern "C" {
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

namespace exosnap {
namespace {

class RecordingClipPublicationTest : public testing::Test {
  protected:
    void SetUp() override {
        source = exosnap_test::UniqueTempPath("clip_publication_source.mkv");
        journal = exosnap_test::UniqueTempPath("clip_publication.index");
        index = std::make_unique<engine::RecordingMediaIndex>(journal);
        const auto fixture = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
                             "libs/engine/tests/fixtures/reordered_h264.mp4";
        AVFormatContext* raw = nullptr;
        ASSERT_GE(avformat_open_input(&raw, fixture.string().c_str(), nullptr, nullptr), 0);
        const auto close_input = [](AVFormatContext* input) { avformat_close_input(&input); };
        std::unique_ptr<AVFormatContext, decltype(close_input)> input(raw, close_input);
        ASSERT_GE(avformat_find_stream_info(raw, nullptr), 0);
        const auto* video = raw->streams[0];
        engine::MatroskaStreamConfig config;
        config.output_path = source.string();
        config.video_codec_id = "V_MPEG4/ISO/AVC";
        config.video_codec_private.assign(video->codecpar->extradata,
                                          video->codecpar->extradata + video->codecpar->extradata_size);
        config.encode_width = static_cast<uint32_t>(video->codecpar->width);
        config.encode_height = static_cast<uint32_t>(video->codecpar->height);
        config.frame_rate_num = 30;
        config.frame_rate_den = 1;
        config.publish_committed = [&](engine::CommittedMediaReceipt receipt) {
            final_commit = receipt.commit_sequence;
            return index->PublishCommitted(std::move(receipt));
        };
        auto free_packet = [](AVPacket* packet) { av_packet_free(&packet); };
        std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
        ASSERT_NE(packet, nullptr);
        std::vector<engine::MuxPacket> encoded;
        while (av_read_frame(raw, packet.get()) >= 0) {
            if (packet->stream_index == 0) {
                engine::MuxPacket item;
                item.track_num = 1;
                item.pts_ns = static_cast<uint64_t>(av_rescale_q(packet->pts, video->time_base, {1, 1000000000}));
                item.dts_ns = av_rescale_q(packet->dts, video->time_base, {1, 1000000000});
                item.is_key = (packet->flags & AV_PKT_FLAG_KEY) != 0;
                item.bytes.assign(packet->data, packet->data + packet->size);
                encoded.push_back(std::move(item));
            }
            av_packet_unref(packet.get());
        }
        ASSERT_FALSE(encoded.empty());
        engine::MatroskaStreamWriter writer;
        ASSERT_TRUE(writer.Open(config)) << writer.error();
        descriptor = writer.media_descriptor();
        ASSERT_EQ(index->RegisterDescriptor(descriptor), engine::ReceiptAcceptance::Accepted);
        for (uint64_t cycle = 0; cycle < 2; ++cycle)
            for (auto item : encoded) {
                item.pts_ns += cycle * 4'000'000'000ULL;
                *item.dts_ns += static_cast<int64_t>(cycle * 4'000'000'000ULL);
                ASSERT_TRUE(writer.Push(std::move(item))) << writer.error();
            }
        ASSERT_TRUE(writer.Finalize()) << writer.error();
        ASSERT_GT(final_commit, 0u);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (index->journal_committed_through() < final_commit && !index->needs_rebuild() &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ASSERT_FALSE(index->needs_rebuild());
        ASSERT_GE(index->journal_committed_through(), final_commit);
        const auto frozen = index->Freeze();
        ASSERT_TRUE(frozen.succeeded()) << frozen.error()->context;
        snapshot = *frozen.value();
    }

    void TearDown() override {
        index.reset();
        std::error_code ignored;
        std::filesystem::remove(source, ignored);
        std::filesystem::remove(journal, ignored);
    }

    RecordingClipRequest Request(const std::filesystem::path& destination) const {
        RecordingClipRequest request;
        request.snapshot = snapshot;
        request.requested_begin_r_ns = 500'000'000;
        request.requested_end_r_ns = 4'100'000'000;
        request.destination = destination;
        request.markers = {{500, RecordingMarkerType::Highlight, "first"},
                           {4'500, RecordingMarkerType::Cut, "outside"}};
        return request;
    }

    std::optional<RecordingClipJobSnapshot> WaitFor(RecordingClipJobs& clips, RecordingClipPhase phase) const {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto jobs = clips.Snapshots();
            if (!jobs.empty() && jobs.front().phase == phase)
                return jobs.front();
            if (!jobs.empty() && (jobs.front().phase == RecordingClipPhase::Failed ||
                                  jobs.front().phase == RecordingClipPhase::Cancelled ||
                                  jobs.front().phase == RecordingClipPhase::Completed))
                return jobs.front();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return std::nullopt;
    }

    std::filesystem::path source;
    std::filesystem::path journal;
    std::unique_ptr<engine::RecordingMediaIndex> index;
    std::shared_ptr<const engine::RecordingMediaDescriptor> descriptor;
    engine::MediaIndexSnapshot snapshot;
    uint64_t final_commit = 0;
    RecordingArtifactRegistry registry;
};

TEST_F(RecordingClipPublicationTest, ReviewPrecedesPublicationAndConfirmedClipHasVerifiedReferences) {
    const auto destination = exosnap_test::UniqueTempPath("clip_publication_output.mkv");
    RecordingClipJobs clips(registry, {}, {}, {});
    auto request = Request(destination);
    request.cfr = false;
    const auto accepted = clips.RequestPlan(std::move(request));
    ASSERT_TRUE(accepted.succeeded()) << accepted.error()->context;
    const auto review = WaitFor(clips, RecordingClipPhase::Review);
    ASSERT_TRUE(review);
    ASSERT_EQ(review->phase, RecordingClipPhase::Review)
        << (review->error ? review->error->context : "No review result");
    EXPECT_FALSE(review->completed);
    EXPECT_FALSE(std::filesystem::exists(destination));
    ASSERT_TRUE(clips.Confirm(review->job_id, review->generation));
    const auto completed = WaitFor(clips, RecordingClipPhase::Completed);
    ASSERT_TRUE(completed);
    ASSERT_EQ(completed->phase, RecordingClipPhase::Completed)
        << (completed->error ? completed->error->context : "No completed clip");
    ASSERT_TRUE(completed->completed);
    EXPECT_TRUE(completed->completed->succeeded);
    EXPECT_FALSE(completed->completed->cfr);
    EXPECT_TRUE(std::filesystem::exists(destination));
    ASSERT_EQ(completed->completed->markers.size(), 1u);
    EXPECT_EQ(completed->completed->markers[0].time_ms, 500u);
    EXPECT_EQ(ReadMarkerSidecar(DeriveMarkerSidecarPath(destination)), completed->completed->markers);
    const auto metadata = engine::ReadMediaIdentitySidecar(destination);
    ASSERT_EQ(metadata.status, engine::AudioMetadataStatus::Verified);
    ASSERT_TRUE(metadata.manifest);
    ASSERT_EQ(metadata.manifest->derived_from_media_ids.size(), 1u);
    EXPECT_EQ(metadata.manifest->derived_from_media_ids[0], descriptor->media_id);
    ASSERT_EQ(metadata.references.marker_refs.size(), 1u);
    const auto ranges =
        QJsonDocument::fromJson(QByteArray::fromStdString(metadata.references.source_ranges_json)).array();
    ASSERT_EQ(ranges.size(), 1);
    const auto range = ranges[0].toObject();
    EXPECT_EQ(range.value(QStringLiteral("media_id")).toString().toStdString(), engine::ToUuid(descriptor->media_id));
    EXPECT_EQ(range.value(QStringLiteral("begin_r_ns")).toString(), QStringLiteral("0"));
    EXPECT_EQ(range.value(QStringLiteral("end_r_ns")).toString(), QStringLiteral("4000000000"));
    clips.Shutdown();
    std::error_code ignored;
    std::filesystem::remove(destination, ignored);
    std::filesystem::remove(engine::MediaIdentitySidecarPath(destination), ignored);
    std::filesystem::remove(DeriveMarkerSidecarPath(destination), ignored);
}

TEST_F(RecordingClipPublicationTest, CancelDuringFinalSpaceCheckCannotReturnToReview) {
    const auto destination = exosnap_test::UniqueTempPath("clip_publication_cancel_planning.mkv");
    RecordingClipJobs* owner = nullptr;
    std::atomic<bool> cancelled_once{false};
    RecordingClipJobs clips(registry, {}, {}, [&] {
        if (owner && !cancelled_once.exchange(true)) {
            const auto job = owner->Snapshots().front();
            EXPECT_TRUE(owner->Cancel(job.job_id, job.generation));
        }
        return uint64_t{0};
    });
    owner = &clips;
    ASSERT_TRUE(clips.RequestPlan(Request(destination)).succeeded());
    const auto cancelled = WaitFor(clips, RecordingClipPhase::Cancelled);
    ASSERT_TRUE(cancelled);
    EXPECT_EQ(cancelled->phase, RecordingClipPhase::Cancelled);
    EXPECT_FALSE(std::filesystem::exists(destination));
    clips.Shutdown();
}

TEST_F(RecordingClipPublicationTest, OccupiedDestinationNeverOverwritesOrCompletes) {
    const auto destination = exosnap_test::UniqueTempPath("clip_publication_occupied.mkv");
    {
        std::ofstream output(destination, std::ios::binary);
        output << "occupied destination";
    }
    RecordingClipJobs clips(registry, {}, {}, {});
    ASSERT_TRUE(clips.RequestPlan(Request(destination)).succeeded());
    const auto review = WaitFor(clips, RecordingClipPhase::Review);
    ASSERT_TRUE(review);
    ASSERT_EQ(review->phase, RecordingClipPhase::Review);
    ASSERT_TRUE(clips.Confirm(review->job_id, review->generation));
    const auto failed = WaitFor(clips, RecordingClipPhase::Failed);
    ASSERT_TRUE(failed);
    EXPECT_EQ(failed->phase, RecordingClipPhase::Failed);
    EXPECT_FALSE(failed->completed);
    EXPECT_FALSE(failed->asset_id);
    std::ifstream input(destination, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>{}),
              "occupied destination");
    clips.Shutdown();
    std::error_code ignored;
    std::filesystem::remove(destination, ignored);
}

TEST_F(RecordingClipPublicationTest, CancellationPreservesSourceAndPublishesNothing) {
    const auto destination = exosnap_test::UniqueTempPath("clip_publication_cancelled.mkv");
    const auto source_size = std::filesystem::file_size(source);
    RecordingClipJobs clips(registry, {}, {}, {});
    ASSERT_TRUE(clips.RequestPlan(Request(destination)).succeeded());
    const auto review = WaitFor(clips, RecordingClipPhase::Review);
    ASSERT_TRUE(review);
    ASSERT_EQ(review->phase, RecordingClipPhase::Review);
    ASSERT_TRUE(clips.Cancel(review->job_id, review->generation));
    const auto cancelled = WaitFor(clips, RecordingClipPhase::Cancelled);
    ASSERT_TRUE(cancelled);
    EXPECT_EQ(cancelled->phase, RecordingClipPhase::Cancelled);
    EXPECT_FALSE(cancelled->completed);
    EXPECT_FALSE(std::filesystem::exists(destination));
    EXPECT_EQ(std::filesystem::file_size(source), source_size);
    clips.Shutdown();
}

TEST_F(RecordingClipPublicationTest, RecordingReserveRefusesClipBeforeReviewOrStaging) {
    const auto destination = exosnap_test::UniqueTempPath("clip_publication_storage_reserve.mkv");
    const auto source_size = std::filesystem::file_size(source);
    RecordingClipJobs clips(registry, {}, {}, [] { return (std::numeric_limits<uint64_t>::max)(); });
    ASSERT_TRUE(clips.RequestPlan(Request(destination)).succeeded());
    const auto failed = WaitFor(clips, RecordingClipPhase::Failed);
    ASSERT_TRUE(failed);
    ASSERT_EQ(failed->phase, RecordingClipPhase::Failed);
    ASSERT_TRUE(failed->error);
    EXPECT_EQ(failed->error->code, engine::EngineErrorCode::StorageInsufficient);
    EXPECT_FALSE(failed->completed);
    EXPECT_EQ(failed->reserved_bytes, 0u);
    EXPECT_FALSE(std::filesystem::exists(destination));
    EXPECT_EQ(std::filesystem::file_size(source), source_size);
    clips.Shutdown();
}

TEST_F(RecordingClipPublicationTest, ShutdownCancelsReviewAndClosesLeasedSources) {
    const auto destination = exosnap_test::UniqueTempPath("clip_publication_shutdown_review.mkv");
    RecordingClipJobs clips(registry, {}, {}, {});
    ASSERT_TRUE(clips.RequestPlan(Request(destination)).succeeded());
    const auto review = WaitFor(clips, RecordingClipPhase::Review);
    ASSERT_TRUE(review);
    ASSERT_EQ(review->phase, RecordingClipPhase::Review);
    clips.Shutdown();
    const auto jobs = clips.Snapshots();
    ASSERT_EQ(jobs.size(), 1u);
    EXPECT_EQ(jobs.front().phase, RecordingClipPhase::Cancelled);
    EXPECT_EQ(jobs.front().reserved_bytes, 0u);
    EXPECT_FALSE(jobs.front().completed);
    EXPECT_FALSE(std::filesystem::exists(destination));
    HANDLE exclusive = CreateFileW(source.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    EXPECT_NE(exclusive, INVALID_HANDLE_VALUE);
    if (exclusive != INVALID_HANDLE_VALUE)
        CloseHandle(exclusive);
}
TEST_F(RecordingClipPublicationTest, PinnedSourceSurvivesPathReplacementDuringReview) {
    const auto destination = exosnap_test::UniqueTempPath("clip_publication_pinned.mkv");
    const auto moved = exosnap_test::UniqueTempPath("clip_publication_original.mkv");
    const auto source_size = std::filesystem::file_size(source);
    RecordingClipJobs clips(registry, {}, {}, {});
    ASSERT_TRUE(clips.RequestPlan(Request(destination)).succeeded());
    const auto review = WaitFor(clips, RecordingClipPhase::Review);
    ASSERT_TRUE(review);
    ASSERT_EQ(review->phase, RecordingClipPhase::Review);
    ASSERT_TRUE(MoveFileExW(source.c_str(), moved.c_str(), MOVEFILE_WRITE_THROUGH));
    {
        std::ofstream replacement(source, std::ios::binary);
        replacement << "replacement";
    }
    ASSERT_TRUE(clips.Confirm(review->job_id, review->generation));
    const auto completed = WaitFor(clips, RecordingClipPhase::Completed);
    ASSERT_TRUE(completed);
    EXPECT_EQ(completed->phase, RecordingClipPhase::Completed)
        << (completed->error ? completed->error->context : "No completed clip");
    EXPECT_EQ(std::filesystem::file_size(moved), source_size);
    std::ifstream replacement(source, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(replacement), std::istreambuf_iterator<char>{}),
              "replacement");
    clips.Shutdown();
    std::error_code ignored;
    std::filesystem::remove(moved, ignored);
    std::filesystem::remove(destination, ignored);
    std::filesystem::remove(engine::MediaIdentitySidecarPath(destination), ignored);
    std::filesystem::remove(DeriveMarkerSidecarPath(destination), ignored);
}

} // namespace
} // namespace exosnap
