#pragma once

#include <recorder_core/interfaces/IAudioCaptureSource.h>

#include "high_pass_filter.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace recorder_core {

// ---------------------------------------------------------------------------
// MicDspConfig (Audio v2 — 0.6.0)
// ---------------------------------------------------------------------------
//
// The single aggregate of microphone-DSP settings. Today it carries only the
// high-pass filter; later Audio v2 slices (noise gate, AGC, RNNoise) add their
// own enable flags and parameters here so MicDspAudioSrc stays the one chain
// that owns all mic processing.
struct MicDspConfig {
    bool hpf_enabled = false;
    float hpf_cutoff_hz = 80.0f;

    // True when at least one DSP stage is enabled. When false the decorator is a
    // no-op and the caller should not bother wrapping the mic source.
    [[nodiscard]] bool AnyEnabled() const {
        return hpf_enabled;
    }
};

// ---------------------------------------------------------------------------
// MicDspAudioSrc (Audio v2 — 0.6.0)
// ---------------------------------------------------------------------------
//
// IAudioCaptureSource decorator that runs the enabled mic-DSP stages on the
// inner source's audio before handing it to the rest of the pipeline. The
// decorator always emits interleaved Float32 (Int16 inner sources are converted
// up), preserving the inner channel count and sample rate.
//
// Stages run in a fixed order (currently only the high-pass filter). The class
// is not thread-safe; one source is processed on one thread (the audio thread).
class MicDspAudioSrc final : public IAudioCaptureSource {
  public:
    MicDspAudioSrc(std::unique_ptr<IAudioCaptureSource> inner, const MicDspConfig& cfg);

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
    std::unique_ptr<IAudioCaptureSource> inner_;
    MicDspConfig cfg_;

    HighPassFilter hpf_;

    std::vector<float> scratch_buffer_; // interleaved Float32, valid until ReleaseBuffer
    uint32_t channels_ = 2;
    uint32_t sample_rate_ = 48000;
    bool initialized_ = false;
};

} // namespace recorder_core
