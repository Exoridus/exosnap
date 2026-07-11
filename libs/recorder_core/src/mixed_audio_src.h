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
// Each source uses gain: (1.0f / source_count) * source_gain_multipliers[i].
//
// Frame accounting (sample-count preserving)
// ------------------------------------------
// Each source is drained into its own FIFO; the mixer emits only frames that a
// source actually delivered — it never fabricates trailing silence to reach a
// fixed block size, and it never discards a source packet's tail. Per call it
// emits N = the smallest FIFO depth among the sources that currently hold
// samples ("min-of-ready"); longer sources keep their surplus buffered for the
// next call (no discarding), and sources that are momentarily idle contribute
// silence for the N frames without stalling the others (a WASAPI loopback
// endpoint delivers no packets at all while the system is silent, so waiting on
// every source would stall a live mic). num_frames on the emitted buffer is the
// real N, so the sample-count-based PTS downstream stays continuous and free of
// the drift that a fixed 480-frame block introduced on non-10 ms device
// periods. The invariant is: per source, Σ delivered frames == Σ mixed frames
// (up to at most one device period held in flight), with no inserted silence
// and no dropped samples.
class MixedAudioSrc final : public IAudioCaptureSource {
  public:
    // Nominal device-period frame count (48 kHz * 10 ms). Retained as the
    // conventional buffer size sources and tests deal in; it no longer bounds
    // how many frames a single AcquireBuffer emits.
    static constexpr uint32_t kMixFrameCount = 480;
    static constexpr uint32_t kOutputSampleRate = 48000;
    static constexpr uint32_t kOutputChannels = 2;

    // Safety valve against unbounded buffering under a sustained capture-clock
    // mismatch between sources (each source is an independent, non-resampled
    // clock). Normal packet-boundary jitter stays far below this; only real
    // drift accumulates, and dropping the oldest surplus past ~500 ms bounds
    // latency and memory rather than growing without limit.
    static constexpr uint32_t kMaxFifoFrames = kOutputSampleRate / 2;

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
    // Pull at most one packet from each source into its FIFO (gain-applied,
    // converted to Float32 stereo). Records discontinuity for this call.
    void PumpOnePacketPerSource(bool& any_discontinuity);

    // Frames emittable right now: the smallest depth among sources that hold
    // buffered samples, or 0 if none do.
    uint32_t EmittableFrames() const;

    std::vector<std::unique_ptr<IAudioCaptureSource>> sources_;
    std::vector<float> source_gain_multipliers_;

    // Per-source FIFO of gain-applied interleaved Float32 stereo samples. Holds
    // the surplus a source delivered beyond what has been mixed so far.
    std::vector<std::vector<float>> source_fifo_;

    std::vector<float> mix_buffer_;     // emitted block: N * kOutputChannels floats
    std::vector<float> scratch_buffer_; // per-source conversion scratch

    bool limiter_enabled_ = false;
    float limiter_ceiling_linear_ = 1.0f;
    BrickwallLimiter limiter_;

    std::string endpoint_name_{"Mixed Audio"};
    bool initialized_ = false;
};

} // namespace recorder_core
