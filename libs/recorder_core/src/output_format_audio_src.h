#pragma once

// OutputFormatAudioSrc (ADR 0030 — 0.6.0)
//
// IAudioCaptureSource decorator that converts an inner source to a target
// {sample_rate, channels} / Float32 using FFmpeg libswresample.
//
// The decorator always emits Float32 (its SampleFormat() contract). Inner
// sources deliver Float32 (mix bus, WASAPI loopback, mic DSP) or Int16 (process
// loopback, some mic endpoints); Int16 inputs are converted up to Float32.
//
// Design:
//   - Wraps an inner source (typically MixedAudioSrc at 48 kHz/stereo/F32).
//   - If target rate AND channel count equal the inner's, a `passthrough_` flag
//     is set during Init; no SwrContext is created. For a Float32 inner the
//     buffer is returned byte-identical (zero overhead); for an Int16 inner the
//     samples are converted to Float32 in AcquireBuffer.
//   - Otherwise one SwrContext is created using swr_alloc_set_opts2 (modern
//     channel-layout API; avutil-60 / swresample-6). The resampler converts the
//     inner format (S16 or FLT) → output Float32 in one swr_convert call per
//     AcquireBuffer.
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
    bool Reinit(std::string& out_error) override;
    uint32_t PendingFrameCount() override;
    bool AcquireBuffer(RawAudioBuffer& out_buf, std::string& out_error) override;
    void ReleaseBuffer() override;
    uint32_t SampleRate() const override;
    uint32_t Channels() const override;
    AudioSampleFormat SampleFormat() const override;
    const std::string& EndpointName() const override;
    int32_t LastCaptureHresult() const override;
    bool LastBufferDeviceTiming(AudioDeviceTiming& out_timing) const override;
    void* BufferReadyEvent() const override;
    uint32_t CaptureSourceCount() const override;
    uint32_t DegradedSourceCount() const override;
    void Shutdown() override;

    // --- A/V clock slaving (H-3) --------------------------------------------
    // Set the resampler's rate compensation in ppm. p > 0 stretches the output
    // timeline (more output frames per input), pushing audio events later —
    // which corrects a positive measured drift (audio leading video). The first
    // call with p != 0 permanently leaves the byte-identical passthrough by
    // lazily building a rate-preserving (inner -> target, 48 k -> 48 k in the
    // default) SwrContext; p == 0 alone keeps the passthrough untouched. Must be
    // called on the audio worker thread.
    void SetCompensationPpm(double ppm);

    // --- End-of-stream drain -------------------------------------------------
    // Push the resampler's internal filter delay out at stop. Whenever a
    // SwrContext is active — a non-default target rate/channel count, or engaged
    // clock slaving on the default 48 kHz path — it holds already-captured audio
    // (~10 ms on a rate conversion, sub-ms to a few ms for a compensating
    // identity context) that would otherwise die with the context in Shutdown().
    // Returns the number of Float32 frames produced; out_buf views internal
    // storage that stays valid until the next AcquireBuffer / DrainResampler /
    // Shutdown. Returns 0 in passthrough mode (the fast path buffers nothing) and
    // on a second call (the context is empty by then). Call after the last
    // AcquireBuffer and before Shutdown(), on the audio worker thread.
    //
    // The flush loop is bounded by kMaxDrainIterations. If that bound is ever
    // reached with the context still producing (not reachable with
    // libswresample's real flush behaviour, which empties in one or two calls),
    // out_undrained_frames — when provided — receives the frames left behind, so
    // the caller can log the loss instead of it disappearing silently. It is set
    // to 0 on every complete drain. This decorator stays free of the logging
    // dependency so its unit test can compile it as a standalone source.
    uint32_t DrainResampler(RawAudioBuffer& out_buf, int64_t* out_undrained_frames = nullptr);

    // Bound on the end-of-stream flush loop; exposed so the caller can name it in
    // a diagnostic when the drain gives up.
    static constexpr int kMaxDrainIterations = 16;

    // Cumulative applied compensation in ms of the output timeline, from the real
    // frame accounting (out_total/target_rate - in_total/inner_rate). 0.0 until a
    // compensating context has processed frames. This is A in the controller's
    // residual R = D - A — measured, not integrated from p, because gap silence,
    // pauses and swr filter latency all decouple applied from commanded.
    [[nodiscard]] double AppliedCompensationMs() const;

  private:
    // Derive inner rate/channels/format and (re)build the SwrContext for the
    // current inner stream. Shared by Init and Reinit so a reacquired inner whose
    // native format changed (the default endpoint moved to another device) gets a
    // matching resampler instead of a stale one.
    bool ConfigureConversion(std::string& out_error);

    // Allocate + init swr_ for inner_rate_/inner_channels_/inner_format_ -> target
    // Float32. Used by the resample branch of ConfigureConversion and by the lazy
    // passthrough -> compensating transition in SetCompensationPpm (where inner
    // and target rates are equal — an identity resampler swr_set_compensation can
    // still nudge).
    bool BuildSwrContext(std::string& out_error);

    std::unique_ptr<IAudioCaptureSource> inner_;
    uint32_t target_sample_rate_;
    uint32_t target_channels_;

    // Inner source's rate/channels captured during ConfigureConversion (needed by
    // the lazy SwrContext build and the applied-compensation accounting).
    uint32_t inner_rate_ = 0;
    uint32_t inner_channels_ = 0;

    // Sample format the inner source delivers, captured at Init. The decorator
    // always emits Float32 (its SampleFormat() contract), so an Int16 inner is
    // converted to Float32 on both the passthrough and the swr path.
    AudioSampleFormat inner_format_ = AudioSampleFormat::Float32;

    bool passthrough_ = false;  // true when target rate/channels == inner
    SwrContext* swr_ = nullptr; // null in passthrough mode

    // Clock-slaving state. compensation_engaged_ latches true on the first
    // non-zero SetCompensationPpm; from then on every AcquireBuffer re-arms the
    // compensation window (a ppm of 0 re-arms a zero delta, cancelling cleanly
    // while keeping the context). in_total_/out_total_ are the consumed input and
    // produced output frame counts on the resample path (int64: > 60 years at
    // 48 kHz without overflow). All three reset on Reinit alongside the drift
    // estimator so the reacquired stream re-engages from a clean baseline.
    double compensation_ppm_ = 0.0;
    bool compensation_engaged_ = false;
    int64_t in_total_ = 0;
    int64_t out_total_ = 0;

    // The last buffer acquired from the inner source (valid until ReleaseBuffer).
    // In passthrough mode we hand out the inner bytes directly.
    // In resampling mode we convert into resample_buf_ and point out_buf at it.
    std::vector<float> resample_buf_;

    // Resampled buffer exposed to the caller (non-owning view into resample_buf_).
    RawAudioBuffer exposed_buf_{};

    bool initialized_ = false;
};

} // namespace recorder_core
