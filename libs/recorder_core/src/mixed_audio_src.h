#pragma once

#include <recorder_core/interfaces/IAudioCaptureSource.h>

#include "brickwall_limiter.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace recorder_core {

// Mixes multiple IAudioCaptureSource instances into a single Float32 stereo output.
// Output is always kOutputSampleRate Hz, kOutputChannels channels, Float32.
// Produces kMixFrameCount frames per AcquireBuffer call (silence-padded if sources have fewer).
// Each source uses gain: (1.0f / source_count) * source_gain_multipliers[i].
class MixedAudioSrc final : public IAudioCaptureSource {
  public:
    static constexpr uint32_t kMixFrameCount = 480;
    static constexpr uint32_t kOutputSampleRate = 48000;
    static constexpr uint32_t kOutputChannels = 2;

    // sources must be non-empty and source_gain_multipliers.size() must match sources.size().
    //
    // When limiter_enabled is true, the mixed output is peak-limited to
    // limiter_ceiling_linear (a BrickwallLimiter) instead of hard-clipped — this
    // is where per-track gain / summing can exceed full scale. Default false
    // preserves the legacy hard-clamp-at-1.0 behavior for existing callers/tests.
    explicit MixedAudioSrc(std::vector<std::unique_ptr<IAudioCaptureSource>> sources,
                           std::vector<float> source_gain_multipliers, bool limiter_enabled = false,
                           float limiter_ceiling_linear = 1.0f);

    bool Init(std::string& out_error) override;
    uint32_t PendingFrameCount() override;
    bool AcquireBuffer(RawAudioBuffer& out_buf, std::string& out_error) override;
    void ReleaseBuffer() override;

    uint32_t SampleRate() const override;
    uint32_t Channels() const override;
    AudioSampleFormat SampleFormat() const override;
    const std::string& EndpointName() const override;

    void Shutdown() override;

  private:
    std::vector<std::unique_ptr<IAudioCaptureSource>> sources_;
    std::vector<float> source_gain_multipliers_;

    std::vector<float> mix_buffer_;     // kMixFrameCount * kOutputChannels floats
    std::vector<float> scratch_buffer_; // per-source conversion scratch
    std::vector<bool> source_acquired_; // which sources were acquired in current buffer

    bool limiter_enabled_ = false;
    float limiter_ceiling_linear_ = 1.0f;
    BrickwallLimiter limiter_;

    std::string endpoint_name_{"Mixed Audio"};
    bool initialized_ = false;
};

} // namespace recorder_core
