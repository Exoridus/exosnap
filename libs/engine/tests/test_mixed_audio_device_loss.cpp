// MixedAudioSrc per-source degradation (ADR 0046). The central hole this closes:
// a merged track used to swallow a dead inner source silently (continue without
// trace) and never bring it back. Now a lost inner is marked degraded (visible
// via DegradedSourceCount), the survivors keep mixing, and Reinit reacquires it.

#include "mixed_audio_src.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace exosnap::engine {
namespace {

// Inner mock that can simulate an endpoint loss (AcquireBuffer -> false with a
// non-empty message, like a real WASAPI source on device-invalidated) and a
// Reinit that either recovers or refuses (the PID-guard "stay silent" case).
class LossMock final : public IAudioCaptureSource {
  public:
    void Deliver(uint32_t frames) {
        pending_ = frames;
    }
    void SetLost(bool lost) {
        lost_ = lost;
    }
    void SetReinitRefuses(bool refuse) {
        reinit_refuses_ = refuse;
    }
    int reinit_calls = 0;

    bool Init(std::string&) override {
        return true;
    }
    bool Reinit(std::string& e) override {
        ++reinit_calls;
        if (reinit_refuses_) {
            e = "reinit refused (target gone)";
            return false;
        }
        lost_ = false;
        return true;
    }
    uint32_t PendingFrameCount() override {
        return lost_ ? 1u : pending_;
    }
    bool AcquireBuffer(RawAudioBuffer& out, std::string& e) override {
        out = {};
        if (lost_) {
            e = "endpoint lost (AUDCLNT_E_DEVICE_INVALIDATED)";
            return false;
        }
        if (pending_ == 0) {
            e.clear();
            return false;
        }
        acquired_ = true;
        out.bytes = reinterpret_cast<const uint8_t*>(data_.data());
        out.num_frames = pending_;
        out.silent = false;
        return true;
    }
    void ReleaseBuffer() override {
        if (acquired_) {
            acquired_ = false;
            pending_ = 0;
        }
    }
    uint32_t SampleRate() const override {
        return 48000;
    }
    uint32_t Channels() const override {
        return 2;
    }
    AudioSampleFormat SampleFormat() const override {
        return AudioSampleFormat::Float32;
    }
    const std::string& EndpointName() const override {
        return name_;
    }
    void Shutdown() override {
    }

  private:
    uint32_t pending_ = 0;
    bool lost_ = false;
    bool reinit_refuses_ = false;
    bool acquired_ = false;
    std::vector<float> data_ = std::vector<float>(2048, 0.25f);
    std::string name_ = "loss-mock";
};

TEST(MixedAudioDeviceLoss, InnerLossDegradesOnlyThatSourceAndSurvivorKeepsMixing) {
    std::vector<std::unique_ptr<IAudioCaptureSource>> inners;
    auto a = std::make_unique<LossMock>();
    auto b = std::make_unique<LossMock>();
    LossMock* pa = a.get();
    LossMock* pb = b.get();
    inners.push_back(std::move(a));
    inners.push_back(std::move(b));

    MixedAudioSrc mix(std::move(inners), {1.0f, 1.0f});
    std::string err;
    ASSERT_TRUE(mix.Init(err));
    EXPECT_EQ(mix.CaptureSourceCount(), 2u);
    EXPECT_EQ(mix.DegradedSourceCount(), 0u);

    // Both healthy: the mixer emits frames.
    pa->Deliver(480);
    pb->Deliver(480);
    RawAudioBuffer out{};
    ASSERT_TRUE(mix.AcquireBuffer(out, err));
    EXPECT_GT(out.num_frames, 0u);

    // Inner b loses its endpoint. The merged track NEVER fails as a whole; only
    // b's contribution degrades, and the survivor a still mixes.
    pa->Deliver(480);
    pb->SetLost(true);
    ASSERT_TRUE(mix.AcquireBuffer(out, err));
    EXPECT_EQ(mix.DegradedSourceCount(), 1u);
    EXPECT_GT(out.num_frames, 0u);

    // The degraded inner is not polled again (it would only re-fail); pumping
    // more keeps exactly one source degraded.
    pa->Deliver(480);
    ASSERT_TRUE(mix.AcquireBuffer(out, err));
    EXPECT_EQ(mix.DegradedSourceCount(), 1u);

    // Reinit reacquires the degraded inner; the survivor is untouched.
    EXPECT_TRUE(mix.Reinit(err));
    EXPECT_EQ(mix.DegradedSourceCount(), 0u);
    EXPECT_GE(pb->reinit_calls, 1);
}

TEST(MixedAudioDeviceLoss, RefusedReinitStaysSilentWithoutKillingTheTrack) {
    std::vector<std::unique_ptr<IAudioCaptureSource>> inners;
    auto a = std::make_unique<LossMock>();
    auto b = std::make_unique<LossMock>();
    LossMock* pa = a.get();
    LossMock* pb = b.get();
    inners.push_back(std::move(a));
    inners.push_back(std::move(b));

    MixedAudioSrc mix(std::move(inners), {1.0f, 1.0f});
    std::string err;
    ASSERT_TRUE(mix.Init(err));

    pa->Deliver(480);
    pb->SetLost(true);
    RawAudioBuffer out{};
    ASSERT_TRUE(mix.AcquireBuffer(out, err));
    EXPECT_EQ(mix.DegradedSourceCount(), 1u);

    // The target is gone: Reinit refuses (PID-guard). The inner stays silent,
    // the track and its survivor keep running — never a fatal.
    pb->SetReinitRefuses(true);
    EXPECT_FALSE(mix.Reinit(err));
    EXPECT_EQ(mix.DegradedSourceCount(), 1u);

    pa->Deliver(480);
    ASSERT_TRUE(mix.AcquireBuffer(out, err));
    EXPECT_GT(out.num_frames, 0u);
}

} // namespace
} // namespace exosnap::engine
