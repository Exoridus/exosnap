#pragma once

#include <recorder_core/interfaces/IAudioCaptureSource.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace recorder_core {

// Mixes multiple IAudioCaptureSource instances into a single Float32 stereo output.
// Output is always kOutputSampleRate Hz, kOutputChannels channels, Float32.
// Produces kMixFrameCount frames per AcquireBuffer call (silence-padded if sources have fewer).
// The last source is treated as MIC and receives mic_gain_linear scaling; all others get base_gain.
class MixedAudioSrc final : public IAudioCaptureSource {
  public:
    static constexpr uint32_t kMixFrameCount = 480;
    static constexpr uint32_t kOutputSampleRate = 48000;
    static constexpr uint32_t kOutputChannels = 2;

    // sources must be non-empty. MIC source should be last by convention.
    explicit MixedAudioSrc(std::vector<std::unique_ptr<IAudioCaptureSource>> sources, float mic_gain_linear = 1.0f);

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
    float mic_gain_linear_;

    std::vector<float> mix_buffer_;     // kMixFrameCount * kOutputChannels floats
    std::vector<float> scratch_buffer_; // per-source conversion scratch
    std::vector<bool> source_acquired_; // which sources were acquired in current buffer

    std::string endpoint_name_{"Mixed Audio"};
    bool initialized_ = false;
};

} // namespace recorder_core
