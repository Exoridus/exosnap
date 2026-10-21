#include <exosnap/engine/edit_timeline_audio.h>

#include "test_unique_temp.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <semaphore>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

using namespace exosnap::engine;

namespace {
class TimelineAudioFixture : public testing::Test {
  protected:
    std::filesystem::path path;
    std::shared_ptr<const TimelineRenderSnapshot> snapshot;
    virtual uint16_t SampleValue(uint32_t) const {
        return 8192;
    }

    void SetUp() override {
        path = exosnap_test::UniqueTempPath("timeline-source.mkv");
        AVFormatContext* context = nullptr;
        ASSERT_GE(avformat_alloc_output_context2(&context, nullptr, "matroska", path.string().c_str()), 0);
        const auto close = [](AVFormatContext* value) {
            avio_closep(&value->pb);
            avformat_free_context(value);
        };
        std::unique_ptr<AVFormatContext, decltype(close)> output(context, close);
        auto* stream = avformat_new_stream(context, nullptr);
        ASSERT_NE(stream, nullptr);
        stream->time_base = {1, 48000};
        stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        stream->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
        stream->codecpar->sample_rate = 48000;
        stream->codecpar->bits_per_coded_sample = 16;
        stream->codecpar->format = AV_SAMPLE_FMT_S16;
        av_channel_layout_default(&stream->codecpar->ch_layout, 2);
        ASSERT_GE(avio_open(&context->pb, path.string().c_str(), AVIO_FLAG_WRITE), 0);
        ASSERT_GE(avformat_write_header(context, nullptr), 0);
        const auto free_packet = [](AVPacket* value) { av_packet_free(&value); };
        std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
        ASSERT_NE(packet, nullptr);
        for (uint32_t first = 0; first < 96000; first += 480) {
            ASSERT_GE(av_new_packet(packet.get(), 480 * 4), 0);
            for (uint32_t frame = 0; frame < 480; ++frame) {
                const auto value = SampleValue(first + frame);
                for (uint32_t channel = 0; channel < 2; ++channel) {
                    packet->data[frame * 4 + channel * 2] = static_cast<uint8_t>(value);
                    packet->data[frame * 4 + channel * 2 + 1] = static_cast<uint8_t>(value >> 8);
                }
            }
            packet->stream_index = 0;
            packet->pts = packet->dts = av_rescale_q(first, AVRational{1, 48000}, stream->time_base);
            packet->duration = av_rescale_q(480, AVRational{1, 48000}, stream->time_base);
            packet->flags |= AV_PKT_FLAG_KEY;
            ASSERT_GE(av_interleaved_write_frame(context, packet.get()), 0);
            av_packet_unref(packet.get());
        }
        ASSERT_GE(av_write_trailer(context), 0);
        output.reset();
        const auto access = AcquireMediaReadCapability(path);
        ASSERT_TRUE(access.succeeded());
        auto value = std::make_shared<TimelineRenderSnapshot>();
        const auto metadata = ProbeEditMedia(*access.value(), {}, {}, nullptr, false);
        ASSERT_TRUE(metadata.error.empty()) << metadata.error;
        value->sources.push_back({1, path, metadata, {}, *access.value()});
        TimelineClip clip{1, 1, 1, true, 0, 0, 2'000'000};
        clip.track = 1;
        clip.stream =
            TimelineStreamRef{1, metadata.audio_metadata.actual_tracks[0].track_id, 0, TimelineStreamKind::Audio};
        value->timeline.clips = {clip};
        value->timeline.tracks = {{1, true}};
        value->timeline.duration_us = 2'000'000;
        snapshot = std::move(value);
    }
    void TearDown() override {
        snapshot.reset();
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

TEST_F(TimelineAudioFixture, SourceReservoirRetainsMutedAndZeroGainContributions) {
    auto muted = *snapshot;
    muted.timeline.clips[0].enabled = false;
    muted.timeline.clips[0].gain_db = -120;
    muted.timeline.tracks[0].enabled = false;
    muted.timeline.tracks[0].gain_db = -120;
    EditTimelineReader reader;
    EditRenderPlan plan;
    plan.path = EditExportPath::Render;
    std::string error;
    ASSERT_TRUE(reader.Open(muted, plan, error)) << error;
    std::vector<TimelineSourceAudio> source;
    ASSERT_TRUE(reader.SourceAudioAt(0, 480, source, error)) << error;
    ASSERT_EQ(source.size(), 1);
    ASSERT_EQ(source[0].interleaved_stereo.size(), 960);
    for (float sample : source[0].interleaved_stereo)
        EXPECT_FLOAT_EQ(sample, 0.25f);
}

TEST_F(TimelineAudioFixture, LoopStartAdmissionEvictsFutureLookaheadBeforeCurrentSources) {
    auto loop = *snapshot;
    const auto original = loop.timeline.clips.front();
    loop.timeline.clips.clear();
    for (uint64_t index = 0; index < 9; ++index) {
        auto clip = original;
        clip.id = index + 1;
        clip.start_us = static_cast<int64_t>(index) * 200'000;
        clip.source_out_us = 200'000;
        loop.timeline.clips.push_back(clip);
    }
    loop.timeline.duration_us = 1'800'000;
    EditRenderPlan plan;
    plan.path = EditExportPath::Render;
    EditTimelineReader reader;
    std::string error;
    ASSERT_TRUE(reader.Open(loop, plan, error)) << error;
    ASSERT_TRUE(reader.PrepareAudioLookahead(9600, 86400, error)) << error;
    std::vector<TimelineSourceAudio> source;
    ASSERT_TRUE(reader.SourceAudioAt(0, 480, source, error)) << error;
    ASSERT_EQ(source.size(), 1);
    EXPECT_FLOAT_EQ(source.front().interleaved_stereo.front(), 0.25f);
    EXPECT_LE(reader.PeakDecoders(), 8);
    uint64_t deferred = 0;
    ASSERT_TRUE(reader.PrepareAudioLookahead(0, 86400, error, &deferred)) << error;
    EXPECT_GT(deferred, 0);
}

TEST_F(TimelineAudioFixture, StalledProducerCannotAccumulateUnboundedRetiredControlTables) {
    TimelineAudioProducer producer;
    std::binary_semaphore stalled(0);
    producer.Prepare(snapshot, {1, 1, 1}, 0, [&](const std::atomic<bool>& cancelled) {
        stalled.release();
        cancelled.wait(false);
        return false;
    });
    ASSERT_TRUE(stalled.try_acquire_for(std::chrono::seconds(3)));
    std::vector<std::shared_ptr<const TimelineAudioControls>> retained;
    uint64_t rejected = 0;
    for (uint64_t revision = 1; revision <= 128; ++revision) {
        const auto controls = BuildTimelineAudioControls(snapshot->timeline, {1, revision, 1});
        ASSERT_TRUE(controls.succeeded());
        retained.push_back(*controls.value());
        if (!producer.SetControls(*controls.value()))
            ++rejected;
    }
    const auto state = producer.Measurements();
    EXPECT_LE(state.retired_control_tables, 4);
    EXPECT_LE(state.peak_retired_control_tables, 4);
    EXPECT_GT(rejected, 0);
    EXPECT_EQ(state.rejected_controls, rejected);
    retained.clear();
    const auto latest = BuildTimelineAudioControls(snapshot->timeline, {1, 129, 1});
    ASSERT_TRUE(latest.succeeded());
    EXPECT_TRUE(producer.SetControls(*latest.value()));
    EXPECT_LE(producer.Measurements().retired_control_tables, 1);
}

TEST_F(TimelineAudioFixture, PreparedAudioAppliesNewestControlsWhileProducerIsStalled) {
    TimelineAudioProducer producer;
    const TimelineAudioStamp stamp{1, 1, 1};
    const auto controls = BuildTimelineAudioControls(snapshot->timeline, stamp);
    ASSERT_TRUE(controls.succeeded());
    producer.SetControls(*controls.value());
    std::binary_semaphore stalled(0);
    std::atomic<int> reads{0};
    producer.Prepare(snapshot, stamp, 0, [&](const std::atomic<bool>& cancelled) {
        if (++reads == 5) {
            stalled.release();
            cancelled.wait(false);
            return false;
        }
        return !cancelled.load();
    });
    ASSERT_TRUE(stalled.try_acquire_for(std::chrono::seconds(3)));
    std::array<float, 960> samples{};
    producer.Mix(1, 0, samples);
    EXPECT_FLOAT_EQ(samples[0], 0.25f);
    auto muted = snapshot->timeline;
    muted.tracks[0].enabled = false;
    const auto silence = BuildTimelineAudioControls(muted, {1, 2, 1});
    ASSERT_TRUE(silence.succeeded());
    producer.SetControls(*silence.value());
    producer.Mix(1, 480, samples);
    EXPECT_TRUE(std::all_of(samples.begin(), samples.end(), [](float sample) { return sample == 0; }));
    auto gain = snapshot->timeline;
    gain.clips[0].gain_db = -6;
    const auto reduced = BuildTimelineAudioControls(gain, {1, 3, 1});
    ASSERT_TRUE(reduced.succeeded());
    producer.SetControls(*reduced.value());
    producer.Mix(1, 960, samples);
    EXPECT_NEAR(samples[0], 0.25 * std::pow(10.0, -6.0 / 20), 1e-6);
    EXPECT_EQ(producer.Measurements().applied_revision, 3);
    EXPECT_EQ(producer.Measurements().requested_revision, 3);
    EXPECT_EQ(producer.Measurements().underrun_frames, 0);
    producer.Cancel();
}

TEST_F(TimelineAudioFixture, SourceReservoirIsBoundedAndSupportsShortOutputTails) {
    TimelineAudioProducer producer;
    const TimelineAudioStamp stamp{1, 1, 1};
    const auto controls = BuildTimelineAudioControls(snapshot->timeline, stamp);
    ASSERT_TRUE(controls.succeeded());
    producer.SetControls(*controls.value());
    producer.Prepare(snapshot, stamp, 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (producer.Measurements().source_buffer_frames < 24000 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const auto prepared = producer.Measurements();
    ASSERT_GE(prepared.source_buffer_frames, 24000);
    EXPECT_GE(prepared.source_capacity_frames, 24000);
    EXPECT_LE(prepared.source_buffer_frames, prepared.source_capacity_frames);
    std::array<float, 254> tail{};
    producer.Mix(1, 0, tail);
    for (float sample : tail)
        EXPECT_FLOAT_EQ(sample, 0.25f);
    producer.Mix(2, 127, tail);
    EXPECT_TRUE(std::all_of(tail.begin(), tail.end(), [](float sample) { return sample == 0; }));
    EXPECT_GT(producer.Measurements().stale_blocks, 0);
    producer.Cancel();
}

TEST_F(TimelineAudioFixture, NewGenerationCancelsStalledPreparationBeforeItCanPublish) {
    TimelineAudioProducer producer;
    const auto old_controls = BuildTimelineAudioControls(snapshot->timeline, {1, 1, 1});
    ASSERT_TRUE(old_controls.succeeded());
    producer.SetControls(*old_controls.value());
    std::binary_semaphore stalled(0);
    producer.Prepare(snapshot, {1, 1, 1}, 0, [&](const std::atomic<bool>& cancelled) {
        stalled.release();
        cancelled.wait(false);
        return false;
    });
    ASSERT_TRUE(stalled.try_acquire_for(std::chrono::seconds(3)));
    const auto new_controls = BuildTimelineAudioControls(snapshot->timeline, {1, 2, 2});
    ASSERT_TRUE(new_controls.succeeded());
    producer.SetControls(*new_controls.value());
    producer.Prepare(snapshot, {1, 2, 2}, 4800);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (producer.Measurements().ready_generation != 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    ASSERT_EQ(producer.Measurements().ready_generation, 2);
    std::array<float, 960> samples;
    producer.Mix(1, 0, samples);
    EXPECT_TRUE(std::all_of(samples.begin(), samples.end(), [](float sample) { return sample == 0; }));
    producer.Mix(2, 4800, samples);
    EXPECT_FLOAT_EQ(samples[0], 0.25f);
    EXPECT_EQ(producer.Measurements().applied_revision, 2);
}

TEST_F(TimelineAudioFixture, UnresolvedLegacyMonitoringKeepsTransportReadyWithExplicitSilence) {
    auto legacy = std::make_shared<TimelineRenderSnapshot>(*snapshot);
    legacy->timeline.clips[0].stream.reset();
    ASSERT_TRUE(legacy->sources[0].metadata.audio_metadata.monitoring_confirmation_required);
    ASSERT_FALSE(legacy->sources[0].monitor_selection);
    TimelineAudioProducer producer;
    const auto controls = BuildTimelineAudioControls(legacy->timeline, {1, 1, 1});
    ASSERT_TRUE(controls.succeeded());
    producer.SetControls(*controls.value());
    producer.Prepare(legacy, {1, 1, 1}, 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!producer.Measurements().ready_generation && !producer.Measurements().failed &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    ASSERT_EQ(producer.Measurements().ready_generation, 1) << producer.Error();
    EXPECT_FALSE(producer.Measurements().failed) << producer.Error();
    std::array<float, 960> samples{};
    producer.Mix(1, 0, samples);
    EXPECT_TRUE(std::all_of(samples.begin(), samples.end(), [](float sample) { return sample == 0; }));
    producer.Cancel();
}
class TimelineAudioRampFixture : public TimelineAudioFixture {
  protected:
    uint16_t SampleValue(uint32_t frame) const override {
        return static_cast<uint16_t>(8192 + (frame % 64) * 128);
    }
};

TEST_F(TimelineAudioRampFixture, EndpointStallReanchorsAtClockInsteadOfReplayingOldSource) {
    TimelineAudioProducer producer;
    const auto initial = BuildTimelineAudioControls(snapshot->timeline, {1, 1, 1});
    ASSERT_TRUE(initial.succeeded());
    ASSERT_TRUE(producer.SetControls(*initial.value()));
    producer.Prepare(snapshot, {1, 1, 1}, 0);
    const auto waitReady = [&](uint64_t generation) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while ((producer.Measurements().ready_generation != generation ||
                producer.Measurements().source_buffer_frames < 4800) &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        return producer.Measurements().ready_generation == generation;
    };
    ASSERT_TRUE(waitReady(1));
    std::array<float, 960> output{};
    producer.Mix(1, 0, output);
    TimelineAudioClock clock;
    clock.Reset(48000, 0);
    EXPECT_EQ(clock.Observe(0, 0), 0);
    EXPECT_EQ(clock.Observe(480, 10'000'000), 480);
    const auto played = clock.Observe(std::nullopt, 141'000'000);
    const auto recovery = PlanTimelineAudioRecovery(480, played, 480, true);
    ASSERT_TRUE(recovery.has_value());
    ASSERT_EQ(recovery->next_frame, played);
    EXPECT_EQ(recovery->skipped_frames, played - 480);
    EXPECT_NE(SampleValue(static_cast<uint32_t>(played)), SampleValue(480));
    const auto target = static_cast<int64_t>(recovery->next_frame);
    const auto controls = BuildTimelineAudioControls(snapshot->timeline, {1, 2, 2});
    ASSERT_TRUE(controls.succeeded());
    producer.Prepare(snapshot, {1, 2, 2}, target);
    ASSERT_TRUE(producer.SetControls(*controls.value()));
    ASSERT_TRUE(waitReady(2));
    producer.Mix(1, 480, output);
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](float value) { return value == 0; }));
    producer.Mix(2, target, output);
    for (uint32_t frame = 0; frame < 480; ++frame)
        EXPECT_FLOAT_EQ(output[frame * 2], SampleValue(static_cast<uint32_t>(target) + frame) / 32768.0f);
    const auto before_reset = played;
    EXPECT_GE(clock.Observe(0, 151'000'000), before_reset);
}

TEST(TimelineAudioRecovery, StartupAndHealthyEndpointLeadDoNotDiscardSourceFrames) {
    EXPECT_FALSE(PlanTimelineAudioRecovery(0, 9600, 480, false).has_value());
    EXPECT_FALSE(PlanTimelineAudioRecovery(1920, 960, 480, true).has_value());
    EXPECT_FALSE(PlanTimelineAudioRecovery(1920, 2400, 480, true).has_value());
}

TEST_F(TimelineAudioRampFixture, TinyLoopWrapsInsideReservoirWithoutRepeatedOrMissingSamples) {
    TimelineAudioProducer producer;
    const auto controls = BuildTimelineAudioControls(snapshot->timeline, {1, 1, 1});
    ASSERT_TRUE(controls.succeeded());
    producer.SetControls(*controls.value());
    producer.Prepare(snapshot, {1, 1, 1}, 10, {}, TimelineAudioRegion{10, 13, true});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (producer.Measurements().source_buffer_frames < 24000 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    ASSERT_GE(producer.Measurements().source_buffer_frames, 24000);
    std::array<float, 2002> samples{};
    producer.Mix(1, 10, samples);
    for (size_t frame = 0; frame < samples.size() / 2; ++frame) {
        const auto expected = static_cast<float>(SampleValue(static_cast<uint32_t>(10 + frame % 3))) / 32768;
        EXPECT_FLOAT_EQ(samples[frame * 2], expected);
        EXPECT_FLOAT_EQ(samples[frame * 2 + 1], expected);
    }
    const auto position = producer.PositionAt(1, 1011);
    ASSERT_TRUE(position);
    EXPECT_EQ(position->project_sample, 12);
    EXPECT_EQ(position->loop_iteration, 333);
    EXPECT_FALSE(position->ended);
    EXPECT_EQ(producer.Measurements().underrun_frames, 0);
    producer.Cancel();
}

TEST_F(TimelineAudioRampFixture, RangeOnceEndsAtCopiedSampleBoundary) {
    TimelineAudioProducer producer;
    const auto controls = BuildTimelineAudioControls(snapshot->timeline, {1, 1, 1});
    ASSERT_TRUE(controls.succeeded());
    producer.SetControls(*controls.value());
    producer.Prepare(snapshot, {1, 1, 1}, 10, {}, TimelineAudioRegion{10, 13, false});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!producer.Measurements().ready_generation && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    ASSERT_EQ(producer.Measurements().ready_generation, 1);
    std::array<float, 20> samples{};
    producer.Mix(1, 10, samples);
    for (size_t frame = 0; frame < 10; ++frame) {
        const float expected =
            frame < 3 ? static_cast<float>(SampleValue(static_cast<uint32_t>(10 + frame))) / 32768 : 0;
        EXPECT_FLOAT_EQ(samples[frame * 2], expected);
        EXPECT_FLOAT_EQ(samples[frame * 2 + 1], expected);
    }
    const auto position = producer.PositionAt(1, 20);
    ASSERT_TRUE(position);
    EXPECT_EQ(position->project_sample, 13);
    EXPECT_TRUE(position->ended);
    EXPECT_EQ(producer.Measurements().underrun_frames, 0);
    producer.Cancel();
}

TEST_F(TimelineAudioFixture, LateEnvelopeMixMatchesSharedExportEvaluator) {
    auto authored = std::make_shared<TimelineRenderSnapshot>(*snapshot);
    authored->timeline.clips[0].fade_in = {100000, {CurveKind::Smooth, 0}};
    authored->timeline.clips[0].gain_db = -3;
    const auto controls = BuildTimelineAudioControls(authored->timeline, {1, 1, 1});
    ASSERT_TRUE(controls.succeeded());
    TimelineAudioProducer producer;
    producer.SetControls(*controls.value());
    producer.Prepare(authored, {1, 1, 1}, 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (producer.Measurements().source_buffer_frames < 24000 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    ASSERT_GE(producer.Measurements().source_buffer_frames, 24000);
    EditTimelineReader reference;
    EditRenderPlan plan;
    plan.path = EditExportPath::Render;
    std::string error;
    ASSERT_TRUE(reference.Open(*authored, plan, error)) << error;
    std::vector<float> expected;
    ASSERT_TRUE(reference.AudioAt(240, 480, expected, error)) << error;
    std::array<float, 960> actual{};
    producer.Mix(1, 240, actual);
    ASSERT_EQ(actual.size(), expected.size());
    for (size_t index = 0; index < actual.size(); ++index)
        EXPECT_NEAR(actual[index], expected[index], 1e-6);
    producer.Cancel();
}
TEST(TimelineAudioClock, MissingDeviceClockAdvancesMonotonicallyWithoutSubmittedFrameLead) {
    TimelineAudioClock clock;
    clock.Reset(48000, 0);
    EXPECT_EQ(clock.Observe(std::nullopt, 10'000'000), 480);
    EXPECT_EQ(clock.Observe(std::nullopt, 20'000'000), 960);
    EXPECT_EQ(clock.Observe(100000, 30'000'000), 1440);
    EXPECT_EQ(clock.Observe(100480, 40'000'000), 1920);
    EXPECT_EQ(clock.Reanchors(), 1);
}

TEST(TimelineAudioClock, DeviceResetReanchorsWithoutProjectTimeGoingBackward) {
    TimelineAudioClock clock;
    clock.Reset(48000, 0);
    EXPECT_EQ(clock.Observe(480, 10'000'000), 480);
    EXPECT_EQ(clock.Observe(960, 20'000'000), 960);
    EXPECT_EQ(clock.Observe(0, 30'000'000), 1440);
    EXPECT_EQ(clock.Observe(480, 40'000'000), 1920);
    EXPECT_EQ(clock.Reanchors(), 1);
}

TEST(TimelineAudioClock, DeviceStallFallsBackThenRecoversAtCurrentTimelinePosition) {
    TimelineAudioClock clock;
    clock.Reset(48000, 0);
    EXPECT_EQ(clock.Observe(480, 10'000'000), 480);
    EXPECT_GE(clock.Observe(480, 120'000'000), 5760);
    const auto fallback = clock.Observe(480, 130'000'000);
    EXPECT_EQ(fallback, 6240);
    EXPECT_EQ(clock.Observe(960, 140'000'000), 6720);
    EXPECT_EQ(clock.Observe(1440, 150'000'000), 7200);
}

} // namespace

TEST_F(TimelineAudioFixture, AdjacentSplitIdentitiesReuseOneSourceStreamCursor) {
    auto cuts = *snapshot;
    const auto original = cuts.timeline.clips.front();
    cuts.timeline.clips.clear();
    for (uint64_t index = 0; index < 10; ++index) {
        auto clip = original;
        clip.id = index + 1;
        clip.start_us = static_cast<int64_t>(index) * 200'000;
        clip.source_in_us = clip.start_us;
        clip.source_out_us = clip.source_in_us + 200'000;
        cuts.timeline.clips.push_back(clip);
    }
    EditRenderPlan plan;
    plan.path = EditExportPath::Render;
    EditTimelineReader reader;
    std::string error;
    ASSERT_TRUE(reader.Open(cuts, plan, error)) << error;
    std::vector<TimelineSourceAudio> source;
    for (int64_t first = 0; first < 96000; first += 480) {
        ASSERT_TRUE(reader.SourceAudioAt(first, 480, source, error)) << error;
        ASSERT_EQ(source.size(), 1);
        EXPECT_FLOAT_EQ(source.front().interleaved_stereo.front(), 0.25f);
        ASSERT_TRUE(reader.PrepareAudioLookahead(first + 480, 96000, error)) << error;
    }
    const auto measured = reader.AudioReadStats();
    EXPECT_EQ(measured.decoder_opens, 1);
    EXPECT_EQ(measured.cursor_reuses, 9);
    EXPECT_EQ(measured.seeks, 0);
    EXPECT_EQ(reader.PeakDecoders(), 1);
}

TEST_F(TimelineAudioRampFixture, FutureCursorPreparesExactSourceRangeAndReusesItsPcm) {
    auto future = *snapshot;
    auto& clip = future.timeline.clips.front();
    clip.start_us = 1'000'000;
    clip.source_in_us = 1'000'125;
    clip.source_out_us = 1'500'125;
    EditRenderPlan plan;
    plan.path = EditExportPath::Render;
    EditTimelineReader reader;
    std::string error;
    ASSERT_TRUE(reader.Open(future, plan, error)) << error;
    ASSERT_TRUE(reader.PrepareAudioLookahead(0, 96000, error)) << error;
    const auto prepared = reader.AudioReadStats();
    EXPECT_EQ(prepared.decoder_opens, 1);
    EXPECT_EQ(prepared.prepared_source_ranges, 1);
    EXPECT_GT(prepared.decoded_frames, 0);
    std::vector<TimelineSourceAudio> source;
    ASSERT_TRUE(reader.SourceAudioAt(48000, 480, source, error)) << error;
    ASSERT_EQ(source.size(), 1);
    for (uint32_t frame = 0; frame < 480; ++frame)
        EXPECT_FLOAT_EQ(source[0].interleaved_stereo[frame * 2], SampleValue(48006 + frame) / 32768.0f);
    EXPECT_EQ(reader.AudioReadStats().prepared_cache_hits, 1);
    EXPECT_EQ(reader.AudioReadStats().seeks, prepared.seeks);
}

TEST_F(TimelineAudioRampFixture, SameSourceOverlapsKeepIndependentPositionedCursors) {
    auto overlap = *snapshot;
    overlap.timeline.clips[0].source_out_us = 500'000;
    auto second = overlap.timeline.clips[0];
    second.id = 2;
    second.track = 2;
    second.source_in_us = 1'000'125;
    second.source_out_us = 1'500'125;
    overlap.timeline.clips.push_back(second);
    overlap.timeline.tracks.push_back({2, true});
    EditRenderPlan plan;
    plan.path = EditExportPath::Render;
    EditTimelineReader reader;
    std::string error;
    ASSERT_TRUE(reader.Open(overlap, plan, error)) << error;
    std::vector<TimelineSourceAudio> source;
    ASSERT_TRUE(reader.SourceAudioAt(0, 480, source, error)) << error;
    ASSERT_EQ(source.size(), 2);
    for (uint32_t frame = 0; frame < 480; ++frame) {
        EXPECT_FLOAT_EQ(source[0].interleaved_stereo[frame * 2], SampleValue(frame) / 32768.0f);
        EXPECT_FLOAT_EQ(source[1].interleaved_stereo[frame * 2], SampleValue(48006 + frame) / 32768.0f);
    }
    EXPECT_EQ(reader.AudioReadStats().decoder_opens, 2);
    EXPECT_EQ(reader.AudioReadStats().cursor_reuses, 0);
    EXPECT_EQ(reader.PeakDecoders(), 2);
}

TEST_F(TimelineAudioRampFixture, GatedReplacementAdoptsCurrentSamplesWhileOldProducerKeepsPlaying) {
    TimelineAudioProducer audible;
    TimelineAudioProducer replacement;
    auto next = std::make_shared<TimelineRenderSnapshot>(*snapshot);
    next->timeline.clips[0].source_in_us = 125;
    next->timeline.duration_us -= 125;
    std::atomic<bool> admitted{false};
    std::atomic<bool> release{false};
    audible.Prepare(snapshot, {1, 1, 1}, 0);
    replacement.Prepare(next, {1, 2, 2}, 0, [&](const std::atomic<bool>& cancelled) {
        admitted.store(true);
        while (!release.load() && !cancelled.load())
            std::this_thread::yield();
        return !cancelled.load();
    });
    const auto old_controls = BuildTimelineAudioControls(snapshot->timeline, {1, 1, 1});
    const auto next_controls = BuildTimelineAudioControls(next->timeline, {1, 2, 2});
    ASSERT_TRUE(old_controls.succeeded());
    ASSERT_TRUE(next_controls.succeeded());
    ASSERT_TRUE(audible.SetControls(*old_controls.value()));
    ASSERT_TRUE(replacement.SetControls(*next_controls.value()));
    const auto wait_for = [](auto done) {
        const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!done() && std::chrono::steady_clock::now() < limit)
            std::this_thread::yield();
        return done();
    };
    ASSERT_TRUE(wait_for([&] { return admitted.load(); }));
    std::array<float, 960> output{};
    for (int64_t quantum = 0; quantum < 100; ++quantum) {
        ASSERT_TRUE(wait_for([&] { return audible.Measurements().source_buffer_frames >= 480; }));
        if (quantum == 50) {
            const auto reduced = BuildTimelineAudioControls(snapshot->timeline, {1, 1, 1}, 0.5f, {}, 2);
            ASSERT_TRUE(reduced.succeeded());
            ASSERT_TRUE(audible.SetControls(*reduced.value()));
        }
        audible.Mix(1, quantum * 480, output);
        const auto gain = quantum >= 50 ? 0.5f : 1.0f;
        for (uint32_t frame = 0; frame < 480; ++frame)
            EXPECT_FLOAT_EQ(output[frame * 2],
                            gain * SampleValue(static_cast<uint32_t>(quantum * 480) + frame) / 32768);
    }
    EXPECT_EQ(audible.Measurements().underrun_frames, 0);
    EXPECT_EQ(audible.Measurements().applied_control_revision, 2);
    EXPECT_EQ(replacement.Measurements().ready_generation, 0);
    EXPECT_EQ(replacement.Measurements().applied_revision, 0);
    release.store(true);
    ASSERT_TRUE(wait_for([&] { return replacement.Measurements().ready_generation == 2; }));
    ASSERT_TRUE(wait_for([&] { return replacement.AdvancePreparation(2, 48000); }));
    EXPECT_EQ(replacement.Measurements().applied_revision, 0);
    EXPECT_EQ(replacement.Measurements().underrun_frames, 0);
    replacement.Mix(2, 48000, output);
    for (uint32_t frame = 0; frame < 480; ++frame)
        EXPECT_FLOAT_EQ(output[frame * 2], SampleValue(48006 + frame) / 32768.0f);
    EXPECT_EQ(replacement.Measurements().applied_revision, 2);
    EXPECT_FALSE(replacement.AdvancePreparation(2, 48480));
    replacement.Cancel();
    EXPECT_FALSE(replacement.AdvancePreparation(2, 48480));
    audible.Cancel();
}

TEST_F(TimelineAudioFixture, AliasCursorReuseStillChecksItsIndependentExpectedDigest) {
    auto aliases = *snapshot;
    aliases.sources[0].metadata = ProbeEditMedia(aliases.sources[0].access);
    ASSERT_TRUE(aliases.sources[0].metadata.content_identity_valid);
    auto alias = aliases.sources[0];
    alias.id = 2;
    alias.metadata.content_sha256[0] ^= 0xff;
    alias.identity_proof.reset();
    aliases.sources.push_back(alias);
    aliases.timeline.clips[0].source_out_us = 1'000'000;
    auto second = aliases.timeline.clips[0];
    second.id = 2;
    second.asset = 2;
    second.stream->asset_id = 2;
    second.start_us = 1'000'000;
    second.source_in_us = 1'000'000;
    second.source_out_us = 2'000'000;
    aliases.timeline.clips.push_back(second);
    EditTimelineReader reader;
    EditRenderPlan plan;
    plan.path = EditExportPath::Render;
    std::string error;
    ASSERT_TRUE(reader.Open(aliases, plan, error)) << error;
    std::vector<TimelineSourceAudio> source;
    ASSERT_TRUE(reader.SourceAudioAt(0, 480, source, error)) << error;
    EXPECT_FALSE(reader.SourceAudioAt(48000, 480, source, error));
    EXPECT_NE(error.find("expected identity"), std::string::npos) << error;
    EXPECT_EQ(reader.AudioReadStats().decoder_opens, 1);
    EXPECT_EQ(reader.AudioReadStats().cursor_reuses, 0);
}

TEST(TimelineAudioPublication, EndpointUsesLockFreePublicationPrimitives) {
    TimelineAudioProducer producer;
    EXPECT_TRUE(producer.Measurements().endpoint_publication_lock_free);
}
TEST_F(TimelineAudioFixture, ConcurrentControlPublicationKeepsDestructionOffTheEndpoint) {
    std::atomic<uint64_t> endpoint_destructions{0};
    std::atomic<uint64_t> invalid_blocks{0};
    std::atomic<bool> finished{false};
    std::thread::id endpoint_id;
    TimelineAudioProducer producer;
    const auto initial = BuildTimelineAudioControls(snapshot->timeline, {1, 1, 1});
    ASSERT_TRUE(initial.succeeded());
    ASSERT_TRUE(producer.SetControls(*initial.value()));
    producer.Prepare(snapshot, {1, 1, 1}, 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (producer.Measurements().source_buffer_frames < 480 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    ASSERT_GE(producer.Measurements().source_buffer_frames, 480);
    std::binary_semaphore begin(0);
    std::thread endpoint([&] {
        begin.acquire();
        std::array<float, 960> samples{};
        while (!finished.load()) {
            producer.Mix(1, 0, samples);
            const float first = samples.front();
            if ((first != 0 && first != 0.25f) ||
                !std::all_of(samples.begin(), samples.end(), [&](float value) { return value == first; }))
                ++invalid_blocks;
        }
    });
    endpoint_id = endpoint.get_id();
    begin.release();
    for (uint64_t revision = 2; revision <= 1000; ++revision) {
        const auto next = BuildTimelineAudioControls(snapshot->timeline, {1, revision, 1}, revision % 2 ? 0.0f : 1.0f);
        if (!next.succeeded()) {
            ADD_FAILURE() << next.error()->context;
            break;
        }
        auto published = std::shared_ptr<const TimelineAudioControls>(new TimelineAudioControls(**next.value()),
                                                                      [&](const TimelineAudioControls* value) {
                                                                          if (std::this_thread::get_id() == endpoint_id)
                                                                              ++endpoint_destructions;
                                                                          delete value;
                                                                      });
        producer.SetControls(std::move(published));
    }
    finished.store(true);
    endpoint.join();
    EXPECT_EQ(endpoint_destructions.load(), 0);
    EXPECT_EQ(invalid_blocks.load(), 0);
    EXPECT_LE(producer.Measurements().retired_control_tables, 4);
    EXPECT_TRUE(producer.Measurements().endpoint_publication_lock_free);
}
TEST_F(TimelineAudioRampFixture, LongLoopPrimesOnlyItsExactSourceMappedHeadWithinTheSourceBudget) {
    auto offset = std::make_shared<TimelineRenderSnapshot>(*snapshot);
    offset->timeline.clips[0].source_in_us = 250000;
    offset->timeline.clips[0].source_out_us = 1750000;
    offset->timeline.duration_us = 1500000;
    const auto before_hashes = GlobalMediaHashMeasurements();
    TimelineAudioProducer producer;
    const auto controls = BuildTimelineAudioControls(offset->timeline, {1, 1, 1});
    ASSERT_TRUE(controls.succeeded());
    ASSERT_TRUE(producer.SetControls(*controls.value()));
    constexpr int64_t first = 48000 - 1000;
    producer.Prepare(offset, {1, 1, 1}, first, {}, TimelineAudioRegion{123, 48000, true});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (producer.Measurements().source_buffer_frames < 24000 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const auto ready = producer.Measurements();
    ASSERT_GE(ready.source_buffer_frames, 24000) << producer.Error();
    EXPECT_EQ(ready.loop_head_frames, 480);
    EXPECT_LE(ready.loop_head_bytes, 8 * 480 * 2 * sizeof(float));
    EXPECT_GT(ready.loop_head_bytes, 0);
    EXPECT_GE(ready.loop_head_reuses, 1);
    EXPECT_LE(ready.peak_decoders, 8);
    std::array<float, 4800> output{};
    producer.Mix(1, first, output);
    for (size_t frame = 0; frame < output.size() / 2; ++frame) {
        const auto project =
            frame < 1000 ? first + static_cast<int64_t>(frame) : 123 + static_cast<int64_t>(frame) - 1000;
        const auto source = static_cast<uint32_t>(12000 + project);
        const auto expected = static_cast<float>(SampleValue(source)) / 32768.0f;
        EXPECT_FLOAT_EQ(output[frame * 2], expected) << frame;
        EXPECT_FLOAT_EQ(output[frame * 2 + 1], expected) << frame;
    }
    EXPECT_EQ(producer.Measurements().underrun_frames, 0);
    EXPECT_EQ(GlobalMediaHashMeasurements().total.starts, before_hashes.total.starts);
}