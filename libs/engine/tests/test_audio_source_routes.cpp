#include "audio_tee.h"
#include "logical_audio_source.h"
#include "track_audio_mixer.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exosnap/engine/audio_routing_model.h>
#include <exosnap/engine/audio_track_model.h>
#include <exosnap/engine/interfaces/IAudioCaptureSource.h>
#include <exosnap/engine/recording_media_types.h>
#include <functional>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace exosnap::engine;
namespace {
AudioCaptureDescriptor Descriptor(uint32_t rate = 48000) {
    AudioCaptureDescriptor d;
    d.sample_rate = rate;
    d.channels = 2;
    d.maximum_packet_frames = rate / 10;
    d.clock_corrected = true;
    return d;
}
AudioBlockView Block(AudioCaptureDescriptor d, std::vector<float>& samples, uint64_t sequence = 0) {
    AudioBlockView b;
    b.descriptor = d;
    b.samples = samples;
    b.metadata.sequence = sequence;
    b.metadata.frames = static_cast<uint32_t>(samples.size() / d.channels);
    b.metadata.capture_qpc_ns = b.metadata.output_qpc_ns = 1000000000ULL;
    return b;
}
ResolvedAudioSource Source(float gain = 1) {
    ResolvedAudioSource s;
    s.spec.id = NewId<AudioSourceId>();
    s.spec.binding.kind = AudioBindingKind::MicrophoneEndpoint;
    s.spec.channel_mode = MicChannelMode::PreserveStereo;
    s.spec.post_dsp_gain_linear = gain;
    return s;
}
} // namespace

TEST(AudioSourceRoutes, ThreeReadersReceiveOneProcessedBlockAndPreMuteMeter) {
    std::string error;
    auto source = LogicalAudioSource::Create(Source(2), Descriptor(), error);
    ASSERT_NE(source, nullptr) << error;
    EXPECT_FALSE(source->Meter().has_measurement);
    EXPECT_EQ(source->Meter().processed_frames, 0u);
    auto readers = source->AttachReaders(3, 0, error);
    ASSERT_EQ(readers.size(), 3u);
    std::vector<float> samples(960, .25f);
    ASSERT_TRUE(source->Process(Block(Descriptor(), samples), error)) << error;
    for (auto& reader : readers) {
        AudioBlockView block;
        ASSERT_TRUE(reader->Acquire(block, error));
        ASSERT_EQ(block.samples.size(), 960u);
        EXPECT_FLOAT_EQ(block.samples[0], .5f);
        EXPECT_TRUE(block.descriptor.clock_corrected);
        reader->Release();
    }
    EXPECT_EQ(source->Meter().processed_blocks, 1u);
    EXPECT_TRUE(source->Meter().has_measurement);
    EXPECT_EQ(source->Meter().processed_frames, 480u);
    source->SetMuted(true);
    ASSERT_TRUE(source->Process(Block(Descriptor(), samples, 1), error));
    EXPECT_FLOAT_EQ(source->Meter().peak, .5f);
    for (auto& reader : readers) {
        AudioBlockView block;
        ASSERT_TRUE(reader->Acquire(block, error));
        EXPECT_FLOAT_EQ(block.samples[0], 0);
        reader->Release();
    }
}

TEST(AudioSourceRoutes, TwoLogicalSourcesHaveIndependentGainAndChannelState) {
    std::string error;
    auto left = Source();
    left.spec.channel_mode = MicChannelMode::LeftToStereo;
    auto right = Source(2);
    right.spec.channel_mode = MicChannelMode::RightToStereo;
    auto a = LogicalAudioSource::Create(left, Descriptor(), error);
    auto b = LogicalAudioSource::Create(right, Descriptor(), error);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    auto ra = a->AttachReaders(1, 0, error);
    auto rb = b->AttachReaders(1, 0, error);
    std::vector<float> samples(960);
    for (size_t i = 0; i < samples.size(); i += 2) {
        samples[i] = .2f;
        samples[i + 1] = .3f;
    }
    ASSERT_TRUE(a->Process(Block(Descriptor(), samples), error));
    ASSERT_TRUE(b->Process(Block(Descriptor(), samples), error));
    AudioBlockView ba, bb;
    ASSERT_TRUE(ra[0]->Acquire(ba, error));
    ASSERT_TRUE(rb[0]->Acquire(bb, error));
    EXPECT_FLOAT_EQ(ba.samples[1], .2f);
    EXPECT_FLOAT_EQ(bb.samples[0], .6f);
    ra[0]->Release();
    rb[0]->Release();
}

TEST(AudioSourceRoutes, SamePhysicalClockKeepsIndependentHpfHistoryPerSourceId) {
    std::string error;
    auto d = Descriptor();
    d.clock_id = NewId<PhysicalClockId>();
    auto sa = Source();
    auto sb = Source();
    sa.spec.dsp.hpf_enabled = sb.spec.dsp.hpf_enabled = true;
    ASSERT_NE(sa.spec.id, sb.spec.id);
    auto a = LogicalAudioSource::Create(sa, d, error);
    auto b = LogicalAudioSource::Create(sb, d, error);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    auto ra = a->AttachReaders(1, 0, error);
    auto rb = b->AttachReaders(1, 0, error);
    std::vector<float> samples(960, .2f);
    ASSERT_TRUE(a->Process(Block(d, samples), error));
    AudioBlockView warmup;
    ASSERT_TRUE(ra[0]->Acquire(warmup, error));
    ra[0]->Release();
    ASSERT_TRUE(a->Process(Block(d, samples, 1), error));
    ASSERT_TRUE(b->Process(Block(d, samples, 1), error));
    AudioBlockView ba, bb;
    ASSERT_TRUE(ra[0]->Acquire(ba, error));
    ASSERT_TRUE(rb[0]->Acquire(bb, error));
    EXPECT_EQ(ba.descriptor.clock_id, bb.descriptor.clock_id);
    EXPECT_LT(std::abs(ba.samples[0]), .02f);
    EXPECT_GT(bb.samples[0], .18f);
    ra[0]->Release();
    rb[0]->Release();
}

TEST(AudioSourceRoutes, Non48kRnnoisePreservesNativeRateAndDrainsFiniteTail) {
    std::string error;
    auto spec = Source();
    spec.spec.dsp.rnnoise_enabled = true;
    auto d = Descriptor(96000);
    auto source = LogicalAudioSource::Create(spec, d, error);
    ASSERT_NE(source, nullptr) << error;
    EXPECT_EQ(source->Descriptor().sample_rate, 96000u);
    auto readers = source->AttachReaders(1, 0, error);
    std::vector<float> samples(9600, .1f);
    ASSERT_TRUE(source->Process(Block(d, samples), error)) << error;
    ASSERT_TRUE(source->Drain(error)) << error;
    uint64_t frames = 0;
    bool latency = false;
    bool changed = false;
    while (readers[0]->PendingFrames()) {
        AudioBlockView block;
        ASSERT_TRUE(readers[0]->Acquire(block, error));
        frames += block.metadata.frames;
        latency |= block.metadata.filter_latency_ns >= 10000000;
        for (float x : block.samples)
            changed |= std::abs(x - .1f) > .001f;
        readers[0]->Release();
    }
    EXPECT_EQ(frames, 4800u);
    EXPECT_TRUE(latency);
    EXPECT_TRUE(changed);
}

TEST(AudioSourceRoutes, FixedDivisorIncludesSilentSlotsAndSoloLimiterRuns) {
    for (uint32_t n = 1; n <= 3; ++n) {
        std::string error;
        auto source = LogicalAudioSource::Create(Source(), Descriptor(), error);
        ASSERT_NE(source, nullptr);
        auto readers = source->AttachReaders(1, 0, error);
        ResolvedRoutingTrack track;
        track.fixed_average_divisor = n;
        track.routes.resize(n);
        track.limiter.enabled = true;
        std::vector<std::unique_ptr<IAudioCaptureSource>> inputs;
        inputs.push_back(MakeLogicalAudioReader(readers[0]));
        while (inputs.size() < n)
            inputs.push_back(nullptr);
        uint64_t now = 1000000000ULL;
        TrackAudioMixer mixer(track, std::move(inputs), 48000, 2, [&] { return now; });
        ASSERT_TRUE(mixer.Init(error)) << error;
        std::vector<float> samples(960, n == 1 ? 2.f : .6f);
        ASSERT_TRUE(source->Process(Block(Descriptor(), samples), error));
        now += 50000000;
        ASSERT_GT(mixer.PendingFrameCount(), 0u);
        RawAudioBuffer out;
        ASSERT_TRUE(mixer.AcquireBuffer(out, error)) << error;
        ASSERT_GT(out.num_frames, 0u);
        const auto* pcm = reinterpret_cast<const float*>(out.bytes);
        if (n == 1)
            EXPECT_LE(std::abs(pcm[0]), 1.f);
        else
            EXPECT_NEAR(pcm[0], .6f / n, 1e-6f);
        EXPECT_TRUE(mixer.ClockCorrected());
        EXPECT_EQ(mixer.SampleRate(), 48000u);
        mixer.ReleaseBuffer();
        mixer.Shutdown();
    }
}

TEST(AudioSourceRoutes, MultipleInputsWaitForArrivalWatermarkBeforeMixing) {
    std::string error;
    auto a = LogicalAudioSource::Create(Source(), Descriptor(), error);
    auto b = LogicalAudioSource::Create(Source(), Descriptor(), error);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    auto ra = a->AttachReaders(1, 0, error);
    auto rb = b->AttachReaders(1, 0, error);
    ResolvedRoutingTrack track;
    track.routes.resize(2);
    track.fixed_average_divisor = 2;
    std::vector<std::unique_ptr<IAudioCaptureSource>> inputs;
    inputs.push_back(MakeLogicalAudioReader(ra[0]));
    inputs.push_back(MakeLogicalAudioReader(rb[0]));
    uint64_t now = 1000000000ULL;
    TrackAudioMixer mixer(track, std::move(inputs), 48000, 2, [&] { return now; });
    ASSERT_TRUE(mixer.Init(error));
    std::vector<float> first(960, .4f), delayed(960, .6f);
    ASSERT_TRUE(a->Process(Block(Descriptor(), first), error));
    now += 20000000;
    EXPECT_EQ(mixer.PendingFrameCount(), 0u);
    ASSERT_TRUE(b->Process(Block(Descriptor(), delayed), error));
    now += 30000000;
    ASSERT_EQ(mixer.PendingFrameCount(), 480u);
    RawAudioBuffer out;
    ASSERT_TRUE(mixer.AcquireBuffer(out, error));
    EXPECT_NEAR(reinterpret_cast<const float*>(out.bytes)[0], .5f, 1e-6f);
    mixer.ReleaseBuffer();
}

TEST(AudioSourceRoutes, FormatGenerationChangesPreserveLeasedOldDescriptor) {
    std::string error;
    auto d = Descriptor();
    d.clock_id = NewId<PhysicalClockId>();
    auto source = LogicalAudioSource::Create(Source(), d, error);
    ASSERT_NE(source, nullptr);
    auto readers = source->AttachReaders(1, 0, error);
    std::vector<float> old_samples(960, .25f);
    ASSERT_TRUE(source->Process(Block(d, old_samples), error));
    AudioBlockView old;
    ASSERT_TRUE(readers[0]->Acquire(old, error));
    auto changed = d;
    changed.sample_rate = 24000;
    changed.channels = 1;
    changed.maximum_packet_frames = 2400;
    changed.source_generation = changed.format_generation = 2;
    std::vector<float> next_samples(240, .4f);
    auto next = Block(changed, next_samples, 1);
    next.metadata.output_qpc_ns += 10000000;
    ASSERT_TRUE(source->Process(next, error)) << error;
    EXPECT_EQ(old.descriptor.sample_rate, 48000u);
    EXPECT_EQ(old.descriptor.source_generation, 1u);
    EXPECT_FLOAT_EQ(old.samples[0], .25f);
    readers[0]->Release();
    AudioBlockView updated;
    ASSERT_TRUE(readers[0]->Acquire(updated, error));
    EXPECT_EQ(updated.descriptor.sample_rate, 24000u);
    EXPECT_EQ(updated.descriptor.source_generation, 2u);
    EXPECT_EQ(updated.descriptor.channels, 2u);
    EXPECT_TRUE(updated.descriptor.clock_corrected);
    readers[0]->Release();
}

TEST(AudioSourceRoutes, FiniteTrackDrainIncludesNonIdentityConverterTail) {
    std::string error;
    auto source = LogicalAudioSource::Create(Source(), Descriptor(), error);
    ASSERT_NE(source, nullptr);
    auto readers = source->AttachReaders(1, 0, error);
    ResolvedRoutingTrack track;
    track.fixed_average_divisor = 1;
    track.routes.resize(1);
    std::vector<std::unique_ptr<IAudioCaptureSource>> inputs;
    inputs.push_back(MakeLogicalAudioReader(readers[0]));
    uint64_t now = 1000000000ULL;
    TrackAudioMixer mixer(track, std::move(inputs), 44100, 2, [&] { return now; });
    ASSERT_TRUE(mixer.Init(error));
    std::vector<float> samples(9600, .25f);
    ASSERT_TRUE(source->Process(Block(Descriptor(), samples), error));
    ASSERT_TRUE(source->Drain(error));
    now += 100000000;
    ASSERT_TRUE(mixer.BeginDrain(now, error)) << error;
    uint64_t frames = 0;
    for (int i = 0; i < 100 && !mixer.Ended(); ++i) {
        ASSERT_GT(mixer.PendingFrameCount(), 0u);
        RawAudioBuffer block;
        ASSERT_TRUE(mixer.AcquireBuffer(block, error));
        frames += block.num_frames;
        mixer.ReleaseBuffer();
    }
    EXPECT_EQ(frames, 4410u);
    EXPECT_TRUE(mixer.Ended());
    EXPECT_EQ(mixer.DrainOutcome().undrained_frames, std::optional<uint64_t>(0));
    EXPECT_FALSE(mixer.DrainOutcome().stalled);
    EXPECT_EQ(mixer.DrainOutcome().end_qpc_ns, 1100000000ULL);
}

TEST(AudioSourceRoutes, ReservationsPrecedeAllocationAndRejectInvalidAdmission) {
    auto d = Descriptor();
    auto spec = Source();
    const size_t simple = LogicalAudioSource::RequiredPayloadBytes(spec, d);
    spec.spec.dsp.rnnoise_enabled = true;
    const size_t neural = LogicalAudioSource::RequiredPayloadBytes(spec, d);
    EXPECT_GT(neural, simple);
    std::string error;
    auto source = LogicalAudioSource::Create(spec, d, error);
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->ReservedPayloadBytes(), neural);
    d.sample_rate = 0;
    EXPECT_EQ(LogicalAudioSource::RequiredPayloadBytes(d), (std::numeric_limits<size_t>::max)());
    EXPECT_EQ(TrackAudioMixer::RequiredPayloadBytes(48000, 2, 0), (std::numeric_limits<size_t>::max)());
}

TEST(AudioSourceRoutes, OverlappingPacketsKeepTheSameSamplesAcrossConsumerSchedules) {
    std::string error;
    const auto descriptor = Descriptor();
    auto source = LogicalAudioSource::Create(Source(), descriptor, error);
    ASSERT_NE(source, nullptr) << error;
    auto readers = source->AttachReaders(2, 1000000000ULL, error);
    ASSERT_EQ(readers.size(), 2u);
    uint64_t completed = 1000000000ULL;
    std::vector<std::unique_ptr<TrackAudioMixer>> mixers;
    for (size_t i = 0; i < 2; ++i) {
        ResolvedRoutingTrack track;
        track.fixed_average_divisor = 1;
        track.routes.resize(1);
        std::vector<std::unique_ptr<IAudioCaptureSource>> inputs;
        inputs.push_back(MakeLogicalAudioReader(readers[i]));
        mixers.push_back(std::make_unique<TrackAudioMixer>(
            track, std::move(inputs), 48000, 2, std::function<uint64_t()>{}, 1000000000ULL, [&] { return completed; }));
        ASSERT_TRUE(mixers.back()->Init(error)) << error;
    }
    std::vector<float> pcm[2];
    auto consume = [&](size_t index) {
        RawAudioBuffer buffer;
        if (!mixers[index]->AcquireBuffer(buffer, error))
            return false;
        const auto* samples = reinterpret_cast<const float*>(buffer.bytes);
        pcm[index].insert(pcm[index].end(), samples, samples + static_cast<size_t>(buffer.num_frames) * 2);
        mixers[index]->ReleaseBuffer();
        return true;
    };
    std::vector<float> first(960, 0.25f);
    ASSERT_TRUE(source->Process(Block(descriptor, first), error)) << error;
    completed = 1010000000ULL;
    ASSERT_TRUE(consume(0)) << error;
    std::vector<float> second(960, 0.75f);
    auto overlapping = Block(descriptor, second, 1);
    overlapping.metadata.output_qpc_ns = overlapping.metadata.capture_qpc_ns = 1009000000ULL;
    ASSERT_TRUE(source->Process(overlapping, error)) << error;
    completed = 1019000000ULL;
    for (size_t i = 0; i < 2; ++i)
        while (mixers[i]->PendingFrameCount())
            ASSERT_TRUE(consume(i)) << error;
    EXPECT_EQ(pcm[0], pcm[1]);
    ASSERT_EQ(pcm[0].size(), 1824u);
    EXPECT_TRUE(std::all_of(pcm[0].begin(), pcm[0].begin() + 960, [](float value) { return value == 0.25f; }));
    EXPECT_TRUE(std::all_of(pcm[0].begin() + 960, pcm[0].end(), [](float value) { return value == 0.75f; }));
}
TEST(AudioSourceRoutes, DelayedTrackConsumersPreserveBacklogWithinTeeHorizon) {
    for (uint32_t source_count : {1u, 2u}) {
        for (uint32_t track_count : {1u, 3u}) {
            for (uint32_t packet_frames : {480u, 48000u}) {
                SCOPED_TRACE(::testing::Message()
                             << source_count << " sources, " << track_count << " tracks, packet " << packet_frames);
                std::string error;
                auto descriptor = Descriptor();
                descriptor.maximum_packet_frames = 48000;
                uint64_t now = 1000000000ULL;
                std::vector<std::unique_ptr<LogicalAudioSource>> sources;
                std::vector<std::vector<std::shared_ptr<AudioTeeReader>>> readers;
                for (uint32_t i = 0; i < source_count; ++i) {
                    sources.push_back(LogicalAudioSource::Create(Source(), descriptor, error));
                    ASSERT_NE(sources.back(), nullptr) << error;
                    readers.push_back(sources.back()->AttachReaders(track_count, now, error));
                    ASSERT_EQ(readers.back().size(), track_count);
                }
                std::vector<std::unique_ptr<TrackAudioMixer>> mixers;
                for (uint32_t t = 0; t < track_count; ++t) {
                    ResolvedRoutingTrack track;
                    track.routes.resize(source_count);
                    track.fixed_average_divisor = source_count;
                    std::vector<std::unique_ptr<IAudioCaptureSource>> inputs;
                    for (uint32_t i = 0; i < source_count; ++i)
                        inputs.push_back(MakeLogicalAudioReader(readers[i][t]));
                    mixers.push_back(std::make_unique<TrackAudioMixer>(
                        track, std::move(inputs), 48000, 2, [&] { return now; }, now));
                    ASSERT_TRUE(mixers.back()->Init(error)) << error;
                }
                auto publish = [&](uint32_t first, uint32_t frames) {
                    std::vector<float> samples(static_cast<size_t>(frames) * 2);
                    for (uint32_t f = 0; f < frames; ++f)
                        samples[2 * f] = samples[2 * f + 1] = .1f + static_cast<float>((first + f) % 97) / 1000;
                    for (auto& source : sources) {
                        auto block = Block(descriptor, samples, first / 480);
                        block.metadata.output_qpc_ns = block.metadata.capture_qpc_ns =
                            1000000000ULL + DeviceFramesToNs(first, 48000);
                        ASSERT_TRUE(source->Process(block, error)) << error;
                    }
                };
                publish(0, 480);
                now += 40000000ULL;
                for (auto& mixer : mixers) {
                    RawAudioBuffer out;
                    ASSERT_TRUE(mixer->AcquireBuffer(out, error)) << error;
                    ASSERT_EQ(out.num_frames, 480u);
                    mixer->ReleaseBuffer();
                }
                for (uint32_t first = 480; first < 48480; first += packet_frames)
                    publish(first, packet_frames);
                now = 2010000000ULL;
                for (auto& source : sources)
                    ASSERT_TRUE(source->Drain(error)) << error;
                for (auto& mixer : mixers) {
                    ASSERT_TRUE(mixer->BeginDrain(now, error)) << error;
                    uint32_t emitted = 0;
                    bool matching = true;
                    while (!mixer->Ended() && emitted < 48000) {
                        ASSERT_GT(mixer->PendingFrameCount(), 0u) << mixer->Failure();
                        RawAudioBuffer out;
                        ASSERT_TRUE(mixer->AcquireBuffer(out, error)) << error;
                        const auto* pcm = reinterpret_cast<const float*>(out.bytes);
                        for (uint32_t f = 0; f < out.num_frames; ++f) {
                            const float expected = .1f + static_cast<float>((480 + emitted + f) % 97) / 1000;
                            matching &=
                                std::abs(pcm[2 * f] - expected) < 1e-6f && std::abs(pcm[2 * f + 1] - expected) < 1e-6f;
                        }
                        emitted += out.num_frames;
                        mixer->ReleaseBuffer();
                    }
                    EXPECT_EQ(emitted, 48000u);
                    EXPECT_TRUE(matching);
                    EXPECT_TRUE(mixer->Ended());
                }
            }
        }
    }
}

TEST(AudioSourceRoutes, TrackReportsExactSourceLossBeyondTeeHorizon) {
    std::string error;
    const auto descriptor = Descriptor();
    auto source = LogicalAudioSource::Create(Source(), descriptor, error);
    ASSERT_NE(source, nullptr) << error;
    auto readers = source->AttachReaders(1, 1000000000ULL, error);
    ASSERT_EQ(readers.size(), 1u);
    ResolvedRoutingTrack track;
    track.routes.resize(1);
    track.fixed_average_divisor = 1;
    std::vector<std::unique_ptr<IAudioCaptureSource>> inputs;
    inputs.push_back(MakeLogicalAudioReader(readers[0]));
    TrackAudioMixer mixer(track, std::move(inputs), 48000, 2, [] { return 4040000000ULL; }, 1000000000ULL);
    ASSERT_TRUE(mixer.Init(error)) << error;
    std::vector<float> samples(960, .25f);
    for (uint32_t i = 0; i < 300; ++i) {
        auto block = Block(descriptor, samples, i);
        block.metadata.output_qpc_ns = block.metadata.capture_qpc_ns = 1000000000ULL + i * 10000000ULL;
        ASSERT_TRUE(source->Process(block, error)) << error;
    }
    // Three seconds were published into a two-second Tee without consumption.
    // The missing second must remain on the output timeline and be reported.
    EXPECT_EQ(source->Meter().reader_overflow_frames, 48000u);
    ASSERT_TRUE(source->Drain(error)) << error;
    ASSERT_TRUE(mixer.BeginDrain(4000000000ULL, error)) << error;
    uint32_t emitted = 0;
    bool matching = true;
    bool discontinuity = false;
    while (!mixer.Ended() && emitted < 144000) {
        ASSERT_GT(mixer.PendingFrameCount(), 0u) << mixer.Failure();
        RawAudioBuffer out;
        ASSERT_TRUE(mixer.AcquireBuffer(out, error)) << error;
        discontinuity |= out.data_discontinuity;
        const auto* pcm = reinterpret_cast<const float*>(out.bytes);
        for (uint32_t f = 0; f < out.num_frames; ++f) {
            const float expected = emitted + f < 48000 ? 0.f : .25f;
            matching &= pcm[2 * f] == expected && pcm[2 * f + 1] == expected;
        }
        EXPECT_EQ(out.gap_frames, 0u);
        emitted += out.num_frames;
        mixer.ReleaseBuffer();
    }
    EXPECT_EQ(emitted, 144000u);
    EXPECT_TRUE(matching);
    EXPECT_TRUE(discontinuity);
    EXPECT_TRUE(mixer.Ended());
    EXPECT_TRUE(mixer.Failure().empty());
    EXPECT_EQ(source->Meter().reader_overflow_frames, 48000u);
}

TEST(AudioSourceRoutes, ShutdownReleasesPartiallyQueuedPacketWithoutStoppingOtherReaders) {
    std::string error;
    auto descriptor = Descriptor();
    descriptor.maximum_packet_frames = 48000;
    auto source = LogicalAudioSource::Create(Source(), descriptor, error);
    ASSERT_NE(source, nullptr) << error;
    auto readers = source->AttachReaders(2, 1000000000ULL, error);
    ASSERT_EQ(readers.size(), 2u);
    ResolvedRoutingTrack track;
    track.routes.resize(1);
    track.fixed_average_divisor = 1;
    std::vector<std::unique_ptr<IAudioCaptureSource>> inputs;
    inputs.push_back(MakeLogicalAudioReader(readers[0]));
    TrackAudioMixer mixer(track, std::move(inputs), 48000, 2, [] { return 2040000000ULL; }, 1000000000ULL);
    ASSERT_TRUE(mixer.Init(error)) << error;
    std::vector<float> samples(96000, .25f);
    ASSERT_TRUE(source->Process(Block(descriptor, samples), error)) << error;
    RawAudioBuffer out;
    ASSERT_TRUE(mixer.AcquireBuffer(out, error)) << error;
    mixer.ReleaseBuffer();
    EXPECT_EQ(readers[0]->PendingFrames(), 0u);
    AudioBlockView held;
    ASSERT_TRUE(readers[1]->Acquire(held, error)) << error;
    samples.assign(960, .75f);
    auto next = Block(descriptor, samples, 1);
    next.metadata.output_qpc_ns = next.metadata.capture_qpc_ns = 2000000000ULL;
    ASSERT_TRUE(source->Process(next, error)) << error;
    mixer.Shutdown();
    EXPECT_EQ(readers[0]->PendingFrames(), 480u);
    AudioBlockView remaining;
    ASSERT_TRUE(readers[0]->Acquire(remaining, error)) << error;
    EXPECT_FLOAT_EQ(remaining.samples[0], .75f);
    EXPECT_FLOAT_EQ(held.samples[0], .25f);
    EXPECT_FLOAT_EQ(held.samples.back(), .25f);
    readers[0]->Release();
    readers[1]->Release();
}

TEST(AudioSourceRoutes, NeuralRecoveryKeepsGapOnSourceQpcTimeline) {
    std::string error;
    auto spec = Source();
    spec.spec.dsp.rnnoise_enabled = true;
    auto d = Descriptor();
    auto source = LogicalAudioSource::Create(spec, d, error);
    ASSERT_NE(source, nullptr);
    auto readers = source->AttachReaders(1, 0, error);
    std::vector<float> samples(960, .1f);
    ASSERT_TRUE(source->Process(Block(d, samples), error));
    auto recovered = Block(d, samples, 1);
    recovered.metadata.output_qpc_ns = recovered.metadata.capture_qpc_ns = 1110000000ULL;
    recovered.metadata.device_gap_frames = 4800;
    recovered.metadata.discontinuity = true;
    ASSERT_TRUE(source->Process(recovered, error)) << error;
    ASSERT_TRUE(source->Drain(error)) << error;
    uint64_t final_end = 0;
    bool recovered_interval = false;
    bool missing_interval = false;
    while (readers[0]->PendingFrames()) {
        AudioBlockView block;
        ASSERT_TRUE(readers[0]->Acquire(block, error));
        final_end = (std::max)(final_end,
                               block.metadata.output_qpc_ns + DeviceFramesToNs(block.metadata.frames, d.sample_rate));
        recovered_interval |= block.metadata.output_qpc_ns >= 1110000000ULL;
        for (uint32_t frame = 0; frame < block.metadata.frames; ++frame) {
            const uint64_t qpc = block.metadata.output_qpc_ns + DeviceFramesToNs(frame, d.sample_rate);
            if (qpc >= 1010000000ULL && qpc < 1110000000ULL) {
                missing_interval = true;
                EXPECT_FLOAT_EQ(block.samples[static_cast<size_t>(frame) * block.descriptor.channels], 0.f);
            }
        }
        EXPECT_EQ(block.metadata.device_gap_frames, 0u);
        readers[0]->Release();
    }
    EXPECT_EQ(final_end, 1120000000ULL);
    EXPECT_TRUE(recovered_interval);
    EXPECT_TRUE(missing_interval);
    EXPECT_EQ(source->Meter().missing_frames, 4800u);
    EXPECT_EQ(source->Meter().history_reset_count, 0u);
}

TEST(AudioSourceRoutes, MissingIntervalsAdvanceHpfHistoryBeforeRecovery) {
    std::string error;
    auto spec = Source();
    spec.spec.dsp.hpf_enabled = true;
    auto d = Descriptor();
    auto source = LogicalAudioSource::Create(spec, d, error);
    ASSERT_NE(source, nullptr);
    auto readers = source->AttachReaders(1, 0, error);
    std::vector<float> dc(960, .2f), zeros(960, 0.f);
    ASSERT_TRUE(source->Process(Block(d, dc), error));
    AudioBlockView first;
    ASSERT_TRUE(readers[0]->Acquire(first, error));
    readers[0]->Release();
    auto recovered = Block(d, zeros, 1);
    recovered.metadata.output_qpc_ns = recovered.metadata.capture_qpc_ns = 1110000000ULL;
    recovered.metadata.device_gap_frames = 4800;
    recovered.metadata.silence = true;
    recovered.metadata.discontinuity = true;
    ASSERT_TRUE(source->Process(recovered, error)) << error;
    bool found = false;
    while (readers[0]->PendingFrames()) {
        AudioBlockView block;
        ASSERT_TRUE(readers[0]->Acquire(block, error));
        if (block.metadata.output_qpc_ns >= 1110000000ULL) {
            ASSERT_FALSE(block.samples.empty());
            EXPECT_LT(std::abs(block.samples[0]), 1e-4f);
            found = true;
        }
        readers[0]->Release();
    }
    EXPECT_TRUE(found);
}

TEST(AudioSourceRoutes, ProcessedReaderKeepsExactOverflowAndCapsCompatibilityGap) {
    std::string error;
    auto d = Descriptor();
    auto tee = AudioTee::Create(d, error);
    ASSERT_NE(tee, nullptr);
    auto readers = tee->AttachReaders(1, 0, error);
    auto adapter = MakeLogicalAudioReader(readers[0]);
    ASSERT_TRUE(adapter->Init(error));
    std::vector<float> samples(960, .25f);
    AudioBlockMetadata metadata;
    metadata.frames = 480;
    metadata.output_qpc_ns = 1000000000ULL;
    ASSERT_TRUE(tee->Publish(samples, metadata, error));
    metadata.output_qpc_ns += 10000000;
    ASSERT_TRUE(tee->Publish(samples, metadata, error));
    metadata.output_qpc_ns += 10000000;
    metadata.device_gap_frames = (std::numeric_limits<uint32_t>::max)() - 480;
    ASSERT_TRUE(tee->Publish(samples, metadata, error));
    RawAudioBuffer raw;
    ASSERT_TRUE(adapter->AcquireBuffer(raw, error));
    AudioBlockView snapshot;
    ASSERT_TRUE(LastLogicalAudioBlock(*adapter, snapshot));
    EXPECT_EQ(snapshot.reader_overflow_frames, 960ULL);
    EXPECT_EQ(snapshot.metadata.device_gap_frames, (std::numeric_limits<uint32_t>::max)() - 480);
    EXPECT_EQ(raw.gap_frames, (std::numeric_limits<uint32_t>::max)());
    EXPECT_TRUE(raw.data_discontinuity);
    adapter->ReleaseBuffer();
}

TEST(AudioSourceRoutes, LongNeuralOutageSkipsTimeWithoutUnboundedDspReplay) {
    std::string error;
    auto spec = Source();
    spec.spec.dsp.rnnoise_enabled = true;
    auto d = Descriptor();
    auto source = LogicalAudioSource::Create(spec, d, error);
    ASSERT_NE(source, nullptr);
    auto readers = source->AttachReaders(1, 0, error);
    std::vector<float> samples(960, .1f);
    ASSERT_TRUE(source->Process(Block(d, samples), error));
    auto recovered = Block(d, samples, 1);
    recovered.metadata.output_qpc_ns = recovered.metadata.capture_qpc_ns = 3601010000000ULL;
    recovered.metadata.device_gap_frames = 172800000;
    recovered.metadata.discontinuity = true;
    ASSERT_TRUE(source->Process(recovered, error)) << error;
    ASSERT_TRUE(source->Drain(error)) << error;
    const auto meter = source->Meter();
    EXPECT_EQ(meter.missing_frames, 172800000ULL);
    EXPECT_EQ(meter.history_reset_count, 1u);
    EXPECT_LE(meter.processed_frames, 960ULL + 2ULL * d.sample_rate);
    uint64_t final_end = 0;
    while (readers[0]->PendingFrames()) {
        AudioBlockView block;
        ASSERT_TRUE(readers[0]->Acquire(block, error));
        final_end = (std::max)(final_end,
                               block.metadata.output_qpc_ns + DeviceFramesToNs(block.metadata.frames, d.sample_rate));
        readers[0]->Release();
    }
    EXPECT_EQ(final_end, 3601020000000ULL);
}

TEST(AudioSourceRoutes, MissingTimingRemainsUnavailableAcrossSyntheticGapChunks) {
    std::string error;
    auto descriptor = Descriptor();
    descriptor.maximum_packet_frames = 480;
    auto source = LogicalAudioSource::Create(Source(), descriptor, error);
    ASSERT_NE(source, nullptr);
    auto readers = source->AttachReaders(1, 0, error);
    std::vector<float> samples(960, .2f);
    auto input = Block(descriptor, samples);
    input.metadata.capture_qpc_ns = input.metadata.output_qpc_ns = 0;
    ASSERT_TRUE(source->Process(input, error));
    input.metadata.device_gap_frames = 960;
    input.metadata.sequence = 1;
    input.metadata.discontinuity = true;
    ASSERT_TRUE(source->Process(input, error));
    uint64_t frames = 0;
    while (readers.front()->PendingFrames()) {
        AudioBlockView block;
        ASSERT_TRUE(readers.front()->Acquire(block, error));
        EXPECT_EQ(block.metadata.output_qpc_ns, 0u);
        frames += block.metadata.frames;
        readers.front()->Release();
    }
    EXPECT_EQ(frames, 1920u);
}
