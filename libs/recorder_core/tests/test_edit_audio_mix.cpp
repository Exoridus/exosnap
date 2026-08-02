#include <gtest/gtest.h>

#include "edit_audio_mix.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using recorder_core::EditAudioMixer;

constexpr uint32_t kRate = EditAudioMixer::kSampleRate;
constexpr uint32_t kCh = EditAudioMixer::kChannels;

// A block of `frames` sample frames, every sample at `level`.
std::vector<float> ConstantBlock(size_t frames, float level) {
    return std::vector<float>(frames * kCh, level);
}

// A sine at `hz`, same signal in both channels.
std::vector<float> SineBlock(size_t frames, float amplitude, double hz) {
    std::vector<float> out(frames * kCh);
    for (size_t f = 0; f < frames; ++f) {
        const auto s = static_cast<float>(amplitude *
                                          std::sin(2.0 * 3.14159265358979323846 * hz * static_cast<double>(f) / kRate));
        for (uint32_t c = 0; c < kCh; ++c)
            out[f * kCh + c] = s;
    }
    return out;
}

int64_t UsForFrames(int64_t frames) {
    return frames * 1'000'000 / static_cast<int64_t>(kRate);
}

// Everything the mixer will hand over right now, concatenated. Take() emits
// one stretch per call; a test that wants "what did the mix produce" has to
// drain it.
std::vector<float> DrainReady(EditAudioMixer& mixer) {
    std::vector<float> out;
    while (auto block = mixer.Take()) {
        out.insert(out.end(), block->interleaved_stereo.begin(), block->interleaved_stereo.end());
    }
    return out;
}

std::vector<float> DrainRemainder(EditAudioMixer& mixer) {
    std::vector<float> out;
    while (auto block = mixer.TakeRemainder()) {
        out.insert(out.end(), block->interleaved_stereo.begin(), block->interleaved_stereo.end());
    }
    return out;
}

float PeakOf(const std::vector<float>& samples) {
    float peak = 0.0f;
    for (const float s : samples)
        peak = std::max(peak, std::fabs(s));
    return peak;
}

TEST(EditAudioMixer, SumsTwoTracksSampleForSample) {
    EditAudioMixer mixer;
    mixer.Reset(2, 0);
    const auto a = ConstantBlock(480, 0.2f);
    const auto b = ConstantBlock(480, 0.3f);
    mixer.Submit(0, 0, a.data(), 480);
    mixer.Submit(1, 0, b.data(), 480);

    const std::vector<float> out = DrainReady(mixer);
    ASSERT_EQ(out.size(), 480u * kCh);
    for (const float s : out)
        EXPECT_NEAR(s, 0.5f, 1e-5f);
}

// The reason the mix does not divide by the track count: a recording whose
// second source happened to be quiet would otherwise play back half as loud as
// the same recording with one track, which is not something a listener can
// reason about.
TEST(EditAudioMixer, ASilentTrackLeavesTheOtherTracksLevelUntouched) {
    EditAudioMixer mixer;
    mixer.Reset(2, 0);
    const auto loud = ConstantBlock(480, 0.6f);
    const auto silence = ConstantBlock(480, 0.0f);
    mixer.Submit(0, 0, loud.data(), 480);
    mixer.Submit(1, 0, silence.data(), 480);

    const std::vector<float> out = DrainReady(mixer);
    ASSERT_EQ(out.size(), 480u * kCh);
    for (const float s : out)
        EXPECT_NEAR(s, 0.6f, 1e-5f);
}

TEST(EditAudioMixer, ASingleTrackPassesThroughUnchanged) {
    // One track is the overwhelmingly common case and must not be coloured by
    // the mix path existing at all: below the ceiling the limiter's envelope
    // sits at unity and the samples come out as they went in.
    EditAudioMixer mixer;
    mixer.Reset(1, 0);
    const auto a = ConstantBlock(960, 0.75f);
    mixer.Submit(0, 0, a.data(), 960);

    const std::vector<float> out = DrainReady(mixer);
    ASSERT_EQ(out.size(), 960u * kCh);
    for (const float s : out)
        EXPECT_FLOAT_EQ(s, 0.75f);
}

TEST(EditAudioMixer, TwoLoudTracksAreLimitedNotClipped) {
    // 0.8 + 0.8 is an amplitude of 1.6, well past full scale. Both hard
    // clipping and limiting keep the result inside the ceiling; what separates
    // them is HOW. A clipper flattens every sample beyond full scale -- for
    // this signal well over half of them -- and each one lands exactly on the
    // ceiling. The limiter pulls the gain down instead and leaves the
    // waveform's shape alone.
    constexpr size_t kFrames = 48000; // 1 s; at 50 Hz the 1 ms attack tracks it easily
    const auto a = SineBlock(kFrames, 0.8f, 50.0);
    EditAudioMixer mixer;
    mixer.Reset(2, 0);
    mixer.Submit(0, 0, a.data(), kFrames);
    mixer.Submit(1, 0, a.data(), kFrames);

    const std::vector<float> out = DrainReady(mixer);
    ASSERT_EQ(out.size(), kFrames * kCh);
    EXPECT_LE(PeakOf(out), 1.0f); // the brickwall guarantee

    // Measured over the second half, so the envelope has long settled.
    const size_t offset = out.size() / 2;
    const std::vector<float> settled(out.begin() + static_cast<std::ptrdiff_t>(offset), out.end());
    EXPECT_GT(PeakOf(settled), 0.9f) << "the mix was ducked away rather than limited";

    size_t at_ceiling = 0;
    size_t a_clipper_would_flatten = 0;
    for (size_t i = 0; i < settled.size(); ++i) {
        if (std::fabs(settled[i]) >= 0.999f)
            ++at_ceiling;
        if (std::fabs(2.0f * a[offset + i]) > 1.0f)
            ++a_clipper_would_flatten;
    }
    ASSERT_GT(a_clipper_would_flatten, settled.size() / 2) << "the input does not actually overload";
    // A plain clamp would pin every one of those samples. Gain reduction pins
    // only what the envelope's attack lag lets through, which on this
    // deliberately punishing signal -- a sustained tone 4 dB over full scale --
    // is a fraction of it.
    EXPECT_LT(at_ceiling, a_clipper_would_flatten / 2) << "the waveform is being chopped flat, not limited";
}

TEST(EditAudioMixer, TheMixRecoversItsLevelAfterAnOverload) {
    // The other half of "not hard clipping": the gain reduction is temporary.
    // A passage that follows an overload comes back at its own level instead
    // of staying ducked for the rest of the clip.
    constexpr size_t kLoud = 48000; // 1 s of overload
    constexpr size_t kQuiet = 48000;
    const auto loud = ConstantBlock(kLoud, 0.9f);
    const auto quiet = ConstantBlock(kQuiet, 0.1f);

    EditAudioMixer mixer;
    mixer.Reset(2, 0);
    mixer.Submit(0, 0, loud.data(), kLoud);
    mixer.Submit(1, 0, loud.data(), kLoud);
    std::vector<float> out = DrainReady(mixer);
    ASSERT_EQ(out.size(), kLoud * kCh);
    EXPECT_NEAR(out.back(), 1.0f, 1e-3f); // held right at the ceiling while it lasts

    const int64_t quiet_pts = UsForFrames(static_cast<int64_t>(kLoud));
    mixer.Submit(0, quiet_pts, quiet.data(), kQuiet);
    mixer.Submit(1, quiet_pts, quiet.data(), kQuiet);
    out = DrainReady(mixer);
    ASSERT_EQ(out.size(), kQuiet * kCh);
    // 0.1 + 0.1 is far below the ceiling, so once the envelope has released
    // the sum passes through untouched.
    EXPECT_NEAR(out.back(), 0.2f, 1e-4f);
}

TEST(EditAudioMixer, WaitsForTheTrailingTrackBeforeHandingAnythingOver) {
    // The alignment guarantee: a stretch only one track has delivered is not
    // emitted yet, or the other track's contribution to it would be lost.
    EditAudioMixer mixer;
    mixer.Reset(2, 0);
    const auto a = ConstantBlock(480, 0.25f);
    mixer.Submit(0, 0, a.data(), 480);
    EXPECT_FALSE(mixer.Take().has_value());

    const auto b = ConstantBlock(480, 0.25f);
    mixer.Submit(1, 0, b.data(), 480);
    const std::vector<float> out = DrainReady(mixer);
    ASSERT_EQ(out.size(), 480u * kCh);
    for (const float s : out)
        EXPECT_NEAR(s, 0.5f, 1e-5f);
}

TEST(EditAudioMixer, ATrackThatEndsEarlyDoesNotStopTheOthers) {
    // The failure this guards against: a source that stopped before the video
    // did would otherwise pin the mix at its last timestamp for the rest of
    // the clip -- nothing handed over, the renderer's ring running dry, and
    // the audio clock advancing over silence.
    constexpr size_t kBlock = 480; // 10 ms
    EditAudioMixer mixer;
    mixer.Reset(2, 0);

    const auto live = ConstantBlock(kBlock, 0.4f);
    const auto ending = ConstantBlock(kBlock, 0.1f);

    // Both tracks run for 10 blocks; then track 1 stops and track 0 continues
    // for a full second -- far past the mixer's lookbehind.
    size_t frames_delivered = 0;
    std::vector<float> out;
    for (size_t i = 0; i < 110; ++i) {
        const int64_t pts = UsForFrames(static_cast<int64_t>(i * kBlock));
        mixer.Submit(0, pts, live.data(), kBlock);
        if (i < 10)
            mixer.Submit(1, pts, ending.data(), kBlock);
        while (auto block = mixer.Take()) {
            frames_delivered += block->frame_count;
            out.insert(out.end(), block->interleaved_stereo.begin(), block->interleaved_stereo.end());
        }
    }

    // The mix kept flowing well past where track 1 stopped: it may hold back
    // at most the lookbehind window, never the whole remaining clip.
    const size_t submitted = 110 * kBlock;
    EXPECT_GE(frames_delivered, submitted - EditAudioMixer::kMaxTrackLagFrames);

    // The overlapping stretch carries both tracks, the rest carries track 0's
    // level unattenuated -- the surviving track is not faded out along with
    // the one that ended.
    ASSERT_GT(out.size(), 20u * kBlock * kCh);
    EXPECT_NEAR(out[0], 0.5f, 1e-5f);
    EXPECT_NEAR(out[20 * kBlock * kCh], 0.4f, 1e-5f);
    EXPECT_EQ(mixer.LateFramesDropped(), 0u);
}

TEST(EditAudioMixer, AHoleInEveryTrackIsPlayedAsSilenceRatherThanClosedUp) {
    // The renderer's clock counts samples, so swallowing a gap would walk the
    // audio clock ahead of media time and leave video behind by the length of
    // the hole for the rest of the run.
    constexpr size_t kBlock = 480; // 10 ms
    EditAudioMixer mixer;
    mixer.Reset(1, 0);
    const auto a = ConstantBlock(kBlock, 0.5f);

    mixer.Submit(0, 0, a.data(), kBlock);
    ASSERT_TRUE(mixer.Take().has_value());

    // Nothing at all for 20 ms, then audio resumes.
    const int64_t resume = UsForFrames(3 * static_cast<int64_t>(kBlock));
    mixer.Submit(0, resume, a.data(), kBlock);
    const auto block = mixer.Take();
    ASSERT_TRUE(block.has_value());
    EXPECT_EQ(block->pts_us, UsForFrames(static_cast<int64_t>(kBlock)));
    ASSERT_EQ(block->frame_count, 3u * kBlock);
    EXPECT_FLOAT_EQ(block->interleaved_stereo[0], 0.0f);                // the hole
    EXPECT_FLOAT_EQ(block->interleaved_stereo[2 * kBlock * kCh], 0.5f); // audio again
}

TEST(EditAudioMixer, EmittedBlocksCarryTheirOwnMediaTimestamp) {
    constexpr size_t kBlock = 480;
    EditAudioMixer mixer;
    mixer.Reset(2, 0);
    const auto a = ConstantBlock(kBlock, 0.1f);

    int64_t expected_pts = 0;
    for (size_t i = 0; i < 5; ++i) {
        const int64_t pts = UsForFrames(static_cast<int64_t>(i * kBlock));
        mixer.Submit(0, pts, a.data(), kBlock);
        mixer.Submit(1, pts, a.data(), kBlock);
        while (auto block = mixer.Take()) {
            EXPECT_EQ(block->pts_us, expected_pts);
            expected_pts += UsForFrames(static_cast<int64_t>(block->frame_count));
        }
    }
    EXPECT_EQ(expected_pts, UsForFrames(5 * static_cast<int64_t>(kBlock)));
}

TEST(EditAudioMixer, StartOffsetPlacesTheFirstSampleAtTheRequestedTime) {
    // Playback from mid-clip seeds the mix at start_us; the first block handed
    // over must be stamped there and not at zero, or video would lead audio by
    // the offset for the entire run.
    constexpr int64_t kStartUs = 5'000'000;
    EditAudioMixer mixer;
    mixer.Reset(1, kStartUs);
    const auto a = ConstantBlock(480, 0.2f);
    mixer.Submit(0, kStartUs, a.data(), 480);

    const auto block = mixer.Take();
    ASSERT_TRUE(block.has_value());
    EXPECT_EQ(block->pts_us, kStartUs);
    EXPECT_EQ(block->frame_count, 480u);
}

TEST(EditAudioMixer, SamplesArrivingBehindTheMixAreCountedNotSilentlyLost) {
    constexpr size_t kBlock = 480;
    EditAudioMixer mixer;
    mixer.Reset(1, 0);
    const auto a = ConstantBlock(kBlock, 0.2f);
    mixer.Submit(0, UsForFrames(kBlock), a.data(), kBlock);
    ASSERT_TRUE(mixer.Take().has_value()); // the mix has moved past 20 ms
    EXPECT_EQ(mixer.LateFramesDropped(), 0u);

    // A block for a stretch already handed over has nowhere to go.
    mixer.Submit(0, 0, a.data(), kBlock);
    EXPECT_EQ(mixer.LateFramesDropped(), kBlock);
}

TEST(EditAudioMixer, RemainderHandsOverWhatTheBarrierStillHeldBack) {
    // End of stream: nothing more can arrive, so a stretch only one track
    // reached must still be delivered rather than dropped on teardown.
    EditAudioMixer mixer;
    mixer.Reset(2, 0);
    const auto a = ConstantBlock(480, 0.3f);
    mixer.Submit(0, 0, a.data(), 480);
    EXPECT_FALSE(mixer.Take().has_value());

    const std::vector<float> out = DrainRemainder(mixer);
    ASSERT_EQ(out.size(), 480u * kCh);
    for (const float s : out)
        EXPECT_NEAR(s, 0.3f, 1e-5f);
}

TEST(EditAudioMixer, ResetClearsTheBuffersOfThePreviousRun) {
    EditAudioMixer mixer;
    mixer.Reset(2, 0);
    const auto a = ConstantBlock(480, 0.3f);
    mixer.Submit(0, 0, a.data(), 480);

    mixer.Reset(2, 0);
    EXPECT_FALSE(mixer.Take().has_value());
    EXPECT_FALSE(mixer.TakeRemainder().has_value());
    EXPECT_EQ(mixer.LateFramesDropped(), 0u);
}

TEST(EditAudioMixer, SubmitsOutsideTheTrackRangeAreIgnored) {
    EditAudioMixer mixer;
    mixer.Reset(1, 0);
    const auto a = ConstantBlock(480, 0.3f);
    mixer.Submit(1, 0, a.data(), 480); // no such track
    mixer.Submit(0, 0, nullptr, 480);
    mixer.Submit(0, 0, a.data(), 0);
    EXPECT_FALSE(mixer.TakeRemainder().has_value());
}

TEST(EditAudioMixer, NoTracksProducesNothing) {
    EditAudioMixer mixer;
    mixer.Reset(0, 0);
    EXPECT_FALSE(mixer.Take().has_value());
    EXPECT_FALSE(mixer.TakeRemainder().has_value());
}

} // namespace
