#pragma once

// OutputFormatAudioSrc (ADR 0030 — 0.6.0)
//
// IAudioCaptureSource decorator that converts the 48 kHz/stereo/Float32 mix bus
// to a target {sample_rate, channels} / Float32 using FFmpeg libswresample.
//
// Design:
//   - Wraps an inner source (typically MixedAudioSrc at 48 kHz/stereo/F32).
//   - If target == inner format (same sample rate AND same channel count) a
//     `passthrough_` flag is set during Init; no SwrContext is created and
//     AcquireBuffer returns the inner buffer byte-identical. This preserves the
//     existing default (48 kHz/stereo) path with zero overhead.
//   - Otherwise one SwrContext is created using swr_alloc_set_opts2 (modern
//     channel-layout API; avutil-60 / swresample-6). The resampler converts
//     inner Float32 → output Float32 in one swr_convert call per AcquireBuffer.
//
// Thread-safety: single-threaded (lives on the audio worker thread, like all
// IAudioCaptureSource implementations in this codebase).
//
// PTS: The caller reads SampleRate() from this decorator (the target rate), so
// PTS accumulation in audio_thread.cpp is already sample-accurate at the target
// rate after wrapping.

#include <recorder_core/interfaces/IAudioCaptureSource.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Forward-declare the SwrContext opaque type so the swresample header is not
// dragged into every translation unit that includes this header.
struct SwrContext;

namespace recorder_core {

class OutputFormatAudioSrc final : public IAudioCaptureSource {
  public:
    // target_sample_rate: desired output rate (44100, 48000, or 96000 Hz).
    // target_channels:    desired output channel count (1 or 2).
    // The inner source must outlive this decorator (it is taken by unique_ptr).
    OutputFormatAudioSrc(std::unique_ptr<IAudioCaptureSource> inner, uint32_t target_sample_rate,
                         uint32_t target_channels);

    ~OutputFormatAudioSrc() override;

    OutputFormatAudioSrc(const OutputFormatAudioSrc&) = delete;
    OutputFormatAudioSrc& operator=(const OutputFormatAudioSrc&) = delete;

    // IAudioCaptureSource
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
    uint32_t target_sample_rate_;
    uint32_t target_channels_;

    bool passthrough_ = false;  // true when target == inner format
    SwrContext* swr_ = nullptr; // null in passthrough mode

    // The last buffer acquired from the inner source (valid until ReleaseBuffer).
    // In passthrough mode we hand out the inner bytes directly.
    // In resampling mode we convert into resample_buf_ and point out_buf at it.
    std::vector<float> resample_buf_;

    // Resampled buffer exposed to the caller (non-owning view into resample_buf_).
    RawAudioBuffer exposed_buf_{};

    bool initialized_ = false;
};

} // namespace recorder_core
