#include "encoded_opus_preroll.h"
#include "matroska_stream_writer.h"
#include "opus_audio_encoder.h"
#include "recording_media_index.h"
#include "test_unique_temp.h"
#include <exosnap/engine/packet_types.h>
#include <exosnap/engine/recording_media_types.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
}

namespace exosnap::engine {
namespace {

TEST(SegmentMediaMappingTest, UsesExactSignedNanosecondsAcrossSplitAndNegativeDecodeTime) {
    constexpr int64_t origin = 3'600'000'000'333LL;
    constexpr int64_t first_local = 16'666'667;
    constexpr int64_t second_local = -33'333'334;
    EXPECT_EQ(SegmentLocalToRecordingNs(origin, first_local), origin + first_local);
    EXPECT_EQ(SegmentLocalToRecordingNs(origin, second_local), origin + second_local);
    EXPECT_EQ(RecordingToSegmentLocalNs(origin, origin + first_local), first_local);
    EXPECT_EQ(RecordingToSegmentLocalNs(origin, origin + second_local), second_local);
    EXPECT_NE(SegmentLocalToRecordingNs(origin, first_local), origin + 16'000'000);
    EXPECT_FALSE(SegmentLocalToRecordingNs((std::numeric_limits<int64_t>::max)(), 1).has_value());
    EXPECT_FALSE(RecordingToSegmentLocalNs((std::numeric_limits<int64_t>::min)(), 1).has_value());
}

std::shared_ptr<RecordingMediaDescriptor> Descriptor(const std::filesystem::path& path) {
    auto descriptor = std::make_shared<RecordingMediaDescriptor>();
    descriptor->recording_id.bytes[0] = 1;
    descriptor->segment_id.bytes[0] = 2;
    descriptor->media_id.bytes[0] = 3;
    descriptor->writer_generation = 1;
    descriptor->descriptor_revision = 1;
    descriptor->file_identity.volume_serial = 1;
    descriptor->file_identity.file_id[0] = 1;
    descriptor->path = path.string();
    descriptor->video.codec_id = "V_AV1";
    descriptor->immutable_header = {0x1a, 0x45, 0xdf, 0xa3};
    return descriptor;
}

bool RejectsReceiptWithGap(uint64_t second_begin, uint64_t readable_end) {
    const auto path = exosnap_test::UniqueTempPath("segment_gap.index");
    RecordingMediaIndex index(path);
    const auto descriptor = Descriptor(path);
    if (index.RegisterDescriptor(descriptor) != ReceiptAcceptance::Accepted)
        return false;
    CommittedMediaReceipt receipt;
    receipt.recording_id = descriptor->recording_id;
    receipt.segment_id = descriptor->segment_id;
    receipt.writer_generation = 1;
    receipt.commit_sequence = 1;
    receipt.descriptor_revision = 1;
    receipt.readable_end_offset = readable_end;
    receipt.clusters = {{4, 8, {}, {}}, {second_begin, 12, {}, {}}};
    if (index.PublishCommitted(std::move(receipt)) != ReceiptAcceptance::Accepted)
        return false;
    for (unsigned attempt = 0; attempt < 100 && !index.needs_rebuild(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const bool rejected = index.needs_rebuild() && !index.Freeze().succeeded();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return rejected;
}

TEST(SegmentMediaMappingTest, JournalDoesNotPublishAReadablePrefixWithMissingBytes) {
    EXPECT_TRUE(RejectsReceiptWithGap(9, 12));
    EXPECT_TRUE(RejectsReceiptWithGap(8, 13));
}

TEST(SegmentMediaMappingTest, JournalRetainsOptionalEncoderAndVideoProvenance) {
    const auto path = exosnap_test::UniqueTempPath("segment_provenance.index");
    RecordingMediaIndex index(path);
    auto descriptor = Descriptor(path);
    descriptor->video_width = 1920;
    descriptor->video_height = 1080;
    descriptor->frame_rate_num = 60000;
    descriptor->frame_rate_den = 1001;
    ContainerAudioTrack track;
    track.track_id.bytes[0] = 4;
    track.track_uid = 2;
    track.codec.codec_id = "A_OPUS";
    track.codec.codec_delay_samples = 312;
    track.encoder_instance_id.bytes[0] = 5;
    track.encoder_reset = true;
    track.first_decoded_sample_r = -312;
    track.packet_duration_ns = 20'000'000;
    track.preroll_ns = 80'000'000;
    descriptor->audio_tracks.push_back(track);
    ASSERT_EQ(index.RegisterDescriptor(descriptor), ReceiptAcceptance::Accepted);
    CommittedMediaReceipt receipt;
    receipt.recording_id = descriptor->recording_id;
    receipt.segment_id = descriptor->segment_id;
    receipt.writer_generation = 1;
    receipt.commit_sequence = 1;
    receipt.descriptor_revision = 1;
    receipt.readable_end_offset = 8;
    receipt.clusters = {{4, 8, {}, {}}};
    ASSERT_EQ(index.PublishCommitted(std::move(receipt)), ReceiptAcceptance::Accepted);
    for (unsigned attempt = 0; attempt < 100 && index.journal_committed_through() < 1; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_EQ(index.journal_committed_through(), 1);
    const auto snapshot = index.Freeze();
    ASSERT_TRUE(snapshot.succeeded());
    RecordingMediaDescriptor restored;
    const auto visited = RecordingMediaIndex::Visit(*snapshot.value(),
                                                    [&](const RecordingMediaDescriptor& value) {
                                                        restored = value;
                                                        return true;
                                                    },
                                                    {});
    ASSERT_TRUE(visited.succeeded());
    EXPECT_EQ(restored.video_width, 1920u);
    EXPECT_EQ(restored.video_height, 1080u);
    EXPECT_EQ(restored.frame_rate_num, 60000u);
    EXPECT_EQ(restored.frame_rate_den, 1001u);
    ASSERT_EQ(restored.audio_tracks.size(), 1);
    EXPECT_EQ(restored.audio_tracks[0].encoder_instance_id, track.encoder_instance_id);
    EXPECT_TRUE(restored.audio_tracks[0].encoder_reset);
    EXPECT_EQ(restored.audio_tracks[0].first_decoded_sample_r, -312);
    EXPECT_EQ(restored.audio_tracks[0].packet_duration_ns, 20'000'000u);
    EXPECT_EQ(restored.audio_tracks[0].preroll_ns, 80'000'000u);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST(SegmentMediaMappingTest, OpusPrerollSelectsOnlyContiguousPacketsFromSameTrack) {
    std::vector<EncodedAudioPacket> history(4);
    for (size_t index = 0; index < history.size(); ++index) {
        history[index].track_id = 2;
        history[index].pts_ns = 920'000'000 + index * 20'000'000;
        history[index].bytes = {1};
    }
    const auto selected = SelectOpusPreroll(history, 2, 1'000'000'000, 960);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->first_index, 0u);
    EXPECT_EQ(selected->one_past_last_index, 4u);
    EXPECT_EQ(selected->origin_r_ns, 920'000'000u);
    EXPECT_EQ(selected->audible_start_r_ns, 1'000'000'000u);
    EXPECT_EQ(selected->decoder_discard_samples, 3840u);
    EXPECT_EQ(selected->container_delay_ns, 80'000'000u);
    history[2].pts_ns += 1;
    EXPECT_FALSE(SelectOpusPreroll(history, 2, 1'000'000'000, 960).has_value());
    history[2].pts_ns -= 1;
    history[3].track_id = 3;
    EXPECT_FALSE(SelectOpusPreroll(history, 2, 1'000'000'000, 960).has_value());
    EXPECT_FALSE(
        SelectOpusPreroll(std::span<const EncodedAudioPacket>(history).first(3), 2, 1'000'000'000, 960).has_value());
}

struct DecodedAudio {
    std::vector<float> samples;
    std::vector<int64_t> frame_pts_ns;
    std::vector<int> frame_sample_counts;
    int64_t container_duration_ns = AV_NOPTS_VALUE;
};

DecodedAudio DecodeAudio(const std::filesystem::path& path) {
    AVFormatContext* input = nullptr;
    if (avformat_open_input(&input, path.string().c_str(), nullptr, nullptr) < 0)
        return {};
    auto close_input = [](AVFormatContext* value) { avformat_close_input(&value); };
    std::unique_ptr<AVFormatContext, decltype(close_input)> file(input, close_input);
    if (avformat_find_stream_info(input, nullptr) < 0)
        return {};
    const int64_t container_duration_ns =
        input->duration == AV_NOPTS_VALUE
            ? AV_NOPTS_VALUE
            : av_rescale_q(input->duration, AVRational{1, AV_TIME_BASE}, AVRational{1, 1000000000});
    const int audio_stream = av_find_best_stream(input, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio_stream < 0)
        return {};
    const auto* codec = avcodec_find_decoder(input->streams[audio_stream]->codecpar->codec_id);
    if (!codec)
        return {};
    auto free_decoder = [](AVCodecContext* value) { avcodec_free_context(&value); };
    std::unique_ptr<AVCodecContext, decltype(free_decoder)> decoder(avcodec_alloc_context3(codec), free_decoder);
    if (!decoder || avcodec_parameters_to_context(decoder.get(), input->streams[audio_stream]->codecpar) < 0 ||
        avcodec_open2(decoder.get(), codec, nullptr) < 0)
        return {};
    auto free_packet = [](AVPacket* value) { av_packet_free(&value); };
    auto free_frame = [](AVFrame* value) { av_frame_free(&value); };
    std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
    std::unique_ptr<AVFrame, decltype(free_frame)> frame(av_frame_alloc(), free_frame);
    if (!packet || !frame)
        return {};
    DecodedAudio decoded;
    decoded.container_duration_ns = container_duration_ns;
    const auto drain = [&] {
        int result = 0;
        while ((result = avcodec_receive_frame(decoder.get(), frame.get())) >= 0) {
            const auto format = static_cast<AVSampleFormat>(frame->format);
            if (av_get_packed_sample_fmt(format) != AV_SAMPLE_FMT_FLT || frame->ch_layout.nb_channels != 2)
                return false;
            const auto* channel = reinterpret_cast<const float*>(frame->extended_data[0]);
            const int stride = av_sample_fmt_is_planar(format) ? 1 : 2;
            for (int sample = 0; sample < frame->nb_samples; ++sample)
                decoded.samples.push_back(channel[sample * stride]);
            if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                decoded.frame_pts_ns.push_back(av_rescale_q(
                    frame->best_effort_timestamp, input->streams[audio_stream]->time_base, AVRational{1, 1000000000}));
                decoded.frame_sample_counts.push_back(frame->nb_samples);
            }
            av_frame_unref(frame.get());
        }
        return result == AVERROR(EAGAIN) || result == AVERROR_EOF;
    };
    while (av_read_frame(input, packet.get()) >= 0) {
        if (packet->stream_index == audio_stream && (avcodec_send_packet(decoder.get(), packet.get()) < 0 || !drain()))
            return {};
        av_packet_unref(packet.get());
    }
    if (avcodec_send_packet(decoder.get(), nullptr) < 0 || !drain())
        return {};
    return decoded;
}

TEST(SegmentMediaMappingTest, ContinuousOpusSplitHasOneInitialDecoderPriming) {
    OpusAudioEncoder encoder;
    std::string error;
    ASSERT_TRUE(encoder.Init(48000, 2, error)) << error;
    constexpr size_t frames = 48000 * 2;
    std::vector<float> pcm(frames * 2);
    for (size_t frame = 0; frame < frames; ++frame) {
        const float sample = static_cast<float>(0.2 * std::sin(static_cast<double>(frame) * 0.061));
        pcm[frame * 2] = sample;
        pcm[frame * 2 + 1] = sample;
    }
    uint64_t accumulated = 0;
    std::vector<EncodedAudioPacket> packets;
    encoder.FeedFloat32(pcm.data(), pcm.size(), 0, accumulated, 48000, 2, packets);
    ASSERT_EQ(packets.size(), 100);
    ASSERT_GT(encoder.CodecDelaySamples(), 0u);
    const uint64_t delay_ns = static_cast<uint64_t>(encoder.CodecDelaySamples()) * 1000000000ULL / 48000;
    const auto unsplit = exosnap_test::UniqueTempPath("opus_unsplit.mkv");
    const auto first = exosnap_test::UniqueTempPath("opus_split_first.mkv");
    const auto second = exosnap_test::UniqueTempPath("opus_split_second.mkv");
    const auto initial_probe = exosnap_test::UniqueTempPath("opus_initial_packet.mkv");
    const std::vector<EncodedAudioPacket> history(packets.begin() + 46, packets.begin() + 50);
    const auto preroll = SelectOpusPreroll(history, 0, 1'000'000'000, 960);
    ASSERT_TRUE(preroll.has_value());
    const uint32_t preroll_samples = preroll->decoder_discard_samples;
    const auto write = [&](const std::filesystem::path& path, size_t begin, size_t end, uint64_t origin_ns,
                           bool continuing_encoder, std::vector<MediaTrackCoverage>* coverage = nullptr,
                           std::optional<uint64_t>* measured_end_ns = nullptr) {
        MatroskaStreamConfig config;
        config.output_path = path.string();
        config.video_codec_id = "V_AV1";
        config.encode_width = 16;
        config.encode_height = 16;
        config.frame_rate_num = 30;
        config.frame_rate_den = 1;
        config.audio_codec = StreamAudioCodec::Opus;
        config.audio_track_count = 1;
        config.audio_channels = 2;
        config.audio_sample_rate = 48000;
        config.audio_tracks[0].codec_private = encoder.CodecPrivateBytes();
        if (continuing_encoder) {
            config.audio_tracks[0].codec_private[10] = static_cast<uint8_t>(preroll_samples & 0xff);
            config.audio_tracks[0].codec_private[11] = static_cast<uint8_t>(preroll_samples >> 8);
        }
        config.audio_tracks[0].codec_delay_samples = continuing_encoder ? preroll_samples : encoder.CodecDelaySamples();
        if (coverage) {
            config.publish_committed = [coverage](CommittedMediaReceipt receipt) {
                for (const auto& cluster : receipt.clusters)
                    for (const auto& track : cluster.coverage)
                        if (track.track_number == 2)
                            coverage->push_back(track);
                return ReceiptAcceptance::Accepted;
            };
        }
        const uint64_t effective_delay_ns = continuing_encoder ? preroll->container_delay_ns : delay_ns;
        MatroskaStreamWriter writer;
        if (!writer.Open(config))
            return false;
        for (size_t index = begin; index < end; ++index) {
            MuxPacket packet;
            packet.track_num = 2;
            packet.pts_ns = packets[index].pts_ns - origin_ns + effective_delay_ns;
            packet.is_key = true;
            packet.bytes = packets[index].bytes;
            if (!writer.Push(std::move(packet)))
                return false;
        }
        const bool finalized = writer.Finalize();
        EXPECT_FALSE(writer.finalized_segment_seal().has_value());
        if (measured_end_ns)
            *measured_end_ns = writer.measured_content_end_ns();
        return finalized;
    };
    std::vector<MediaTrackCoverage> initial_coverage;
    std::vector<MediaTrackCoverage> continuing_coverage;
    std::optional<uint64_t> continuing_end_ns;
    ASSERT_TRUE(write(initial_probe, 0, 1, 0, false, &initial_coverage));
    ASSERT_EQ(initial_coverage.size(), 1u);
    EXPECT_EQ(initial_coverage.front().begin_pts_ns, 0);
    EXPECT_EQ(initial_coverage.front().end_pts_ns, 13'500'000);
    ASSERT_TRUE(write(unsplit, 0, 100, 0, false));
    ASSERT_TRUE(write(first, 0, 50, 0, false));
    ASSERT_TRUE(write(second, 46 + preroll->first_index, 100, preroll->origin_r_ns, true, &continuing_coverage,
                      &continuing_end_ns));
    ASSERT_FALSE(continuing_coverage.empty());
    ASSERT_TRUE(continuing_end_ns.has_value());
    EXPECT_EQ(continuing_coverage.front().begin_pts_ns, 80'000'000);
    const auto all = DecodeAudio(unsplit);
    const auto first_audio = DecodeAudio(first);
    const auto second_audio = DecodeAudio(second);
    ASSERT_FALSE(all.samples.empty());
    ASSERT_FALSE(first_audio.samples.empty());
    ASSERT_FALSE(second_audio.samples.empty());
    ASSERT_FALSE(second_audio.frame_pts_ns.empty());
    ASSERT_FALSE(all.frame_pts_ns.empty());
    ASSERT_FALSE(all.frame_sample_counts.empty());
    const int64_t first_frame_end_ns =
        all.frame_pts_ns.front() + static_cast<int64_t>(all.frame_sample_counts.front()) * 1000000000LL / 48000;
    EXPECT_EQ(initial_coverage.front().begin_pts_ns, all.frame_pts_ns.front());
    EXPECT_EQ(initial_coverage.front().end_pts_ns, first_frame_end_ns);
    EXPECT_EQ(second_audio.frame_pts_ns.front() + preroll->origin_r_ns, preroll->audible_start_r_ns);
    ASSERT_GT(all.frame_pts_ns.size(), 51u);
    EXPECT_EQ(all.frame_pts_ns[50], 1'000'000'000)
        << "adjacent PTS: " << all.frame_pts_ns[49] << ", " << all.frame_pts_ns[51];
    EXPECT_EQ(first_audio.samples.size() + second_audio.samples.size(), all.samples.size());
    ASSERT_NE(second_audio.container_duration_ns, AV_NOPTS_VALUE);
    ASSERT_FALSE(second_audio.frame_sample_counts.empty());
    const int64_t second_decoded_end_ns =
        second_audio.frame_pts_ns.back() +
        static_cast<int64_t>(second_audio.frame_sample_counts.back()) * 1000000000LL / 48000;
    EXPECT_LE(static_cast<int64_t>(*continuing_end_ns), second_decoded_end_ns);
    EXPECT_LE(second_decoded_end_ns - static_cast<int64_t>(*continuing_end_ns), 500'000);
    EXPECT_LE(second_audio.container_duration_ns, second_decoded_end_ns + 500'000);
    if (first_audio.samples.size() + second_audio.samples.size() == all.samples.size()) {
        double boundary_error = 0;
        const size_t comparison = (std::min)(second_audio.samples.size(), size_t{3840});
        for (size_t sample = 0; sample < comparison; ++sample) {
            const double difference = second_audio.samples[sample] - all.samples[first_audio.samples.size() + sample];
            boundary_error += difference * difference;
        }
        EXPECT_LT(std::sqrt(boundary_error / comparison), 0.02);
    }
    std::error_code ignored;
    std::filesystem::remove(unsplit, ignored);
    std::filesystem::remove(first, ignored);
    std::filesystem::remove(second, ignored);
    std::filesystem::remove(initial_probe, ignored);
}

} // namespace
} // namespace exosnap::engine
