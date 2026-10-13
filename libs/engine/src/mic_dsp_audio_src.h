#pragma once

#include <exosnap/engine/mic_dsp_config.h>

#include <exosnap/engine/interfaces/IAudioCaptureSource.h>

#include "automatic_gain_control.h"
#include "high_pass_filter.h"
#include "noise_gate.h"
#include "rnnoise_denoiser.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace exosnap::engine {

// ---------------------------------------------------------------------------
// MicDspAudioSrc (Audio v2 — 0.6.0)
// ---------------------------------------------------------------------------
//
// IAudioCaptureSource decorator that runs the enabled mic-DSP stages on the
// inner source's audio before handing it to the rest of the pipeline. The
// decorator always emits interleaved Float32 (Int16 inner sources are converted
// up), preserving the inner channel count and sample rate.
//
// Stages run in a fixed order (high-pass filter, then noise gate, then AGC, then
// RNNoise neural noise suppression). The class is not thread-safe; one source is
// processed on one thread (the audio thread).
class MicDspAudioSrc final : public IAudioCaptureSource {
  public:
    MicDspAudioSrc(std::unique_ptr<IAudioCaptureSource> inner, const MicDspConfig& cfg);

    bool Init(std::string& out_error) override;
    bool Reinit(std::string& out_error) override;
    uint32_t PendingFrameCount() override;
    bool AcquireBuffer(RawAudioBuffer& out_buf, std::string& out_error) override;
    void ReleaseBuffer() override;

    uint32_t SampleRate() const override;
    uint32_t Channels() const override;
    AudioSampleFormat SampleFormat() const override;
    const std::string& EndpointName() const override;
    int32_t LastCaptureHresult() const override;
    int32_t LastInitHresult() const override;
    bool LastBufferDeviceTiming(AudioDeviceTiming& out_timing) const override;
    void* BufferReadyEvent() const override;
    uint32_t CaptureSourceCount() const override;
    uint32_t DegradedSourceCount() const override;
    uint32_t DegradedSourceIndexMask() const override;

    void Shutdown() override;

  private:
    std::unique_ptr<IAudioCaptureSource> inner_;
    MicDspConfig cfg_;

    HighPassFilter hpf_;
    NoiseGate gate_;
    AutomaticGainControl agc_;
    RnnoiseDenoiser rnnoise_;

    std::vector<float> scratch_buffer_; // interleaved Float32, valid until ReleaseBuffer
    uint32_t channels_ = 2;
    uint32_t sample_rate_ = 48000;
    bool initialized_ = false;
};

} // namespace exosnap::engine
