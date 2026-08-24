#pragma once

#include <exosnap/engine/interfaces/IAudioCaptureSource.h>

#include "brickwall_limiter.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace exosnap::engine {

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
    // Reactivate every currently-degraded inner source (ADR 0046). Each inner is
    // reacquired with its own identity rules (a PID-keyed loopback may refuse and
    // stay silent; a default endpoint follows the current default). Sources that
    // recover clear their degraded flag; the survivors are never touched. Returns
    // true when no degraded source remains after the attempt.
    bool Reinit(std::string& out_error) override;
    uint32_t PendingFrameCount() override;
    bool AcquireBuffer(RawAudioBuffer& out_buf, std::string& out_error) override;
    void ReleaseBuffer() override;

    uint32_t SampleRate() const override;
    uint32_t Channels() const override;
    AudioSampleFormat SampleFormat() const override;
    const std::string& EndpointName() const override;
    // Source-granular health (ADR 0046): total inner sources and how many are
    // currently degraded (endpoint lost, contributing honest silence). Lets the
    // audio thread surface a partly-degraded merged track without ending it.
    uint32_t CaptureSourceCount() const override;
    uint32_t DegradedSourceCount() const override;

    // Single-source pass-through of the inner device timing (H-3). A one-source
    // mixer is the gain-wrapped single track (a MIC row with gain != 1); it has
    // exactly one device clock, so it forwards the inner's timing and the honest
    // A/V drift metric (and clock slaving) cover it. A multi-source merge mixes
    // several device clocks and stays Unavailable (returns false).
    bool LastBufferDeviceTiming(AudioDeviceTiming& out_timing) const override;

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

    // Per-source degraded flag (ADR 0046). Set when an inner source's acquire
    // fails mid-recording (endpoint lost): the mixer stops polling it (it would
    // only fail again) so the survivors keep mixing, and Reinit reacquires it.
    // This replaces the old silent-swallow that made a dead inner vanish without
    // trace. Sized to sources_ in Init.
    std::vector<bool> source_degraded_;

    // Single-source only (H-3): gap frames pulled from the one inner packet but
    // not yet attached to an emitted buffer. The pump records the inner packet's
    // gap; the next AcquireBuffer that emits frames carries it (scaled to 48 kHz)
    // so the audio thread's gap-fill keeps the sample timeline continuous — a
    // gain-wrapped single track no longer silently drops measured gaps. Always 0
    // for multi-source merges.
    uint32_t single_source_pending_gap_frames_ = 0;

    std::vector<float> mix_buffer_;     // emitted block: N * kOutputChannels floats
    std::vector<float> scratch_buffer_; // per-source conversion scratch

    bool limiter_enabled_ = false;
    float limiter_ceiling_linear_ = 1.0f;
    BrickwallLimiter limiter_;

    std::string endpoint_name_{"Mixed Audio"};
    bool initialized_ = false;
};

} // namespace exosnap::engine
