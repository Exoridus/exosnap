#include "fakes/fake_audio_capture_source.h"

#include <gtest/gtest.h>

#include <cstring>

namespace exosnap::engine::testing {
namespace {

// The fake is a test instrument, so it gets tested: an instrument that lies is
// worse than no instrument, because every result taken with it looks like
// evidence.

TEST(FakeAudioCaptureSource, DeliversQueuedBuffersInOrder) {
    FakeAudioCaptureSource source;
    source.PushBuffers(3, 480);

    std::string error;
    ASSERT_TRUE(source.Init(error)) << error;

    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(source.PendingFrameCount(), 480u) << i;
        RawAudioBuffer buffer;
        ASSERT_TRUE(source.AcquireBuffer(buffer, error)) << error;
        EXPECT_EQ(buffer.num_frames, 480u);
        EXPECT_FALSE(buffer.data_discontinuity);
        EXPECT_EQ(buffer.gap_frames, 0u);
        source.ReleaseBuffer();
    }

    EXPECT_EQ(source.PendingFrameCount(), 0u);
    EXPECT_EQ(source.frames_delivered(), 1440u);
}

// The whole point of the seam: a gap with a known length, which no real card
// produces on demand.
TEST(FakeAudioCaptureSource, ReportsAMeasuredGap) {
    FakeAudioCaptureSource source;
    source.PushBuffers(1, 480);
    source.PushGap(/*lost_frames=*/960, /*next_buffer_frames=*/480);

    std::string error;
    ASSERT_TRUE(source.Init(error)) << error;

    RawAudioBuffer first;
    ASSERT_TRUE(source.AcquireBuffer(first, error)) << error;
    EXPECT_EQ(first.gap_frames, 0u);
    source.ReleaseBuffer();

    RawAudioBuffer second;
    ASSERT_TRUE(source.AcquireBuffer(second, error)) << error;
    EXPECT_TRUE(second.data_discontinuity);
    EXPECT_EQ(second.gap_frames, 960u);
    source.ReleaseBuffer();
}

// A discontinuity the platform could not measure. gap_frames stays 0, and a
// consumer must not read that as "no gap" -- the flag is the signal.
TEST(FakeAudioCaptureSource, DistinguishesAnUnmeasuredDiscontinuity) {
    FakeAudioCaptureSource source;
    source.PushUnmeasuredDiscontinuity(480);

    std::string error;
    ASSERT_TRUE(source.Init(error)) << error;

    RawAudioBuffer buffer;
    ASSERT_TRUE(source.AcquireBuffer(buffer, error)) << error;
    EXPECT_TRUE(buffer.data_discontinuity);
    EXPECT_EQ(buffer.gap_frames, 0u);
}

TEST(FakeAudioCaptureSource, SilentBufferIsSilentButNotDegraded) {
    FakeAudioCaptureSource source;
    source.PushBuffer(FakeAudioBuffer{480, /*silent=*/true, false, 0});

    std::string error;
    ASSERT_TRUE(source.Init(error)) << error;

    RawAudioBuffer buffer;
    ASSERT_TRUE(source.AcquireBuffer(buffer, error)) << error;
    EXPECT_TRUE(buffer.silent);
    EXPECT_EQ(source.DegradedSourceCount(), 0u);

    // Digitally silent means the samples really are zero, not merely flagged.
    const size_t bytes = static_cast<size_t>(buffer.num_frames) * source.Channels() * sizeof(float);
    for (size_t i = 0; i < bytes; ++i)
        ASSERT_EQ(buffer.bytes[i], 0u) << i;
}

TEST(FakeAudioCaptureSource, NonSilentBufferCarriesRealSamples) {
    FakeAudioCaptureSource source;
    source.PushBuffers(1, 480);

    std::string error;
    ASSERT_TRUE(source.Init(error)) << error;

    RawAudioBuffer buffer;
    ASSERT_TRUE(source.AcquireBuffer(buffer, error)) << error;

    bool any_non_zero = false;
    const size_t samples = static_cast<size_t>(buffer.num_frames) * source.Channels();
    for (size_t i = 0; i < samples; ++i) {
        float sample = 0.0F;
        std::memcpy(&sample, buffer.bytes + i * sizeof(float), sizeof(sample));
        if (sample != 0.0F) {
            any_non_zero = true;
            break;
        }
    }
    EXPECT_TRUE(any_non_zero) << "a non-silent buffer must be distinguishable from a silence fill";
}

// Every rate and depth the product offers, without an endpoint that supports it.
TEST(FakeAudioCaptureSource, ReportsTheConfiguredFormat) {
    for (const uint32_t rate : {44100u, 48000u, 96000u}) {
        FakeAudioCaptureSource::Config config;
        config.sample_rate = rate;
        config.format = AudioSampleFormat::Int16;
        config.channels = 1;
        FakeAudioCaptureSource source(config);

        EXPECT_EQ(source.SampleRate(), rate);
        EXPECT_EQ(source.Channels(), 1u);
        EXPECT_EQ(source.SampleFormat(), AudioSampleFormat::Int16);
    }
}

TEST(FakeAudioCaptureSource, DeviceTimingIsAbsentUntilItIsSet) {
    FakeAudioCaptureSource source;
    AudioDeviceTiming timing{};
    EXPECT_FALSE(source.LastBufferDeviceTiming(timing));

    // A device clock running 3 ms behind QPC -- the input to the drift metric.
    source.SetDeviceTiming(/*device_position_ns=*/1'000'000'000ULL, /*qpc_position_ns=*/1'003'000'000ULL);
    ASSERT_TRUE(source.LastBufferDeviceTiming(timing));
    EXPECT_EQ(timing.qpc_position_ns - timing.device_position_ns, 3'000'000ULL);
}

TEST(FakeAudioCaptureSource, FailingInitReportsItsReasonAndStaysStopped) {
    FakeAudioCaptureSource::Config config;
    config.init_error = "endpoint is in use";
    FakeAudioCaptureSource source(config);
    source.PushBuffers(1, 480);

    std::string error;
    EXPECT_FALSE(source.Init(error));
    EXPECT_EQ(error, "endpoint is in use");
    EXPECT_EQ(source.PendingFrameCount(), 0u);
}

// Reinit()'s default tears down and re-opens with the same identity.
TEST(FakeAudioCaptureSource, ReinitReopensTheSameSource) {
    FakeAudioCaptureSource source;
    std::string error;
    ASSERT_TRUE(source.Init(error)) << error;
    ASSERT_TRUE(source.Reinit(error)) << error;

    EXPECT_EQ(source.init_count(), 2);
    EXPECT_EQ(source.shutdown_count(), 1);
}

// A double acquire would read freed memory on a real backend. The fake refuses
// it so that bug fails here instead of on a device.
TEST(FakeAudioCaptureSource, RefusesASecondAcquireBeforeRelease) {
    FakeAudioCaptureSource source;
    source.PushBuffers(2, 480);

    std::string error;
    ASSERT_TRUE(source.Init(error)) << error;

    RawAudioBuffer first;
    ASSERT_TRUE(source.AcquireBuffer(first, error)) << error;

    RawAudioBuffer second;
    EXPECT_FALSE(source.AcquireBuffer(second, error));
    EXPECT_FALSE(error.empty());
}

TEST(FakeAudioCaptureSource, PartlyDegradedIsDistinguishableFromFullySilent) {
    FakeAudioCaptureSource source;
    source.SetDegraded(/*sources=*/3, /*degraded=*/1);

    EXPECT_EQ(source.CaptureSourceCount(), 3u);
    EXPECT_EQ(source.DegradedSourceCount(), 1u);
}

TEST(FakeAudioCaptureSource, SurfacesTheLastCaptureHresult) {
    FakeAudioCaptureSource::Config config;
    config.last_capture_hresult = static_cast<int32_t>(0x88890004); // AUDCLNT_E_DEVICE_INVALIDATED
    FakeAudioCaptureSource source(config);

    EXPECT_EQ(source.LastCaptureHresult(), static_cast<int32_t>(0x88890004));
}

} // namespace
} // namespace exosnap::engine::testing
