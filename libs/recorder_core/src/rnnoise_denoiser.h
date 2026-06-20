#pragma once

#include <cstdint>
#include <vector>

// Forward declaration of the opaque RNNoise state (defined in rnnoise.h). We do
// not include the C header here so consumers of this stage need no RNNoise
// include path; the .cpp owns the C interop.
struct DenoiseState;

namespace recorder_core {

// ---------------------------------------------------------------------------
// RnnoiseDenoiser (Audio v2 — 0.6.0)
// ---------------------------------------------------------------------------
//
// RNNoise (xiph/rnnoise) neural noise suppression for interleaved Float32 audio.
// The fourth and final stage of the MicDspAudioSrc chain (after the high-pass
// filter, the noise gate, and the AGC): a trained recurrent network attenuates
// stationary and non-stationary background noise (fans, keyboards, hum, hiss)
// while preserving speech, beyond what the HPF/gate/AGC achieve.
//
// RNNoise is strictly mono and runs on EXACTLY 480-sample (10 ms) blocks of
// 48 kHz audio, expecting samples scaled to the int16 range (i.e. a normalized
// [-1, 1] float must be multiplied by 32768 before processing and divided by
// 32768 after). We therefore keep:
//   - one DenoiseState per channel (RNNoise has no notion of stereo), and
//   - a per-channel input accumulator + a per-channel denoised-output FIFO,
// so a caller can hand us any block size: we accumulate input, process whole
// 480-sample blocks, and emit denoised output aligned to the input length. This
// introduces a fixed one-block (480 samples / 10 ms) latency — the first 480
// emitted samples are silence while the first block fills. The common case in
// our pipeline is a 480-frame mic buffer (MixedAudioSrc kMixFrameCount=480), so
// each Process() consumes exactly one block and emits the previous block.
//
// RNNoise only supports 48 kHz. When sample_rate != 48000 the stage is a no-op
// passthrough (our capture pipeline is always 48 kHz, so this is never the live
// path; it just keeps the stage safe under odd configurations).
//
// State carries across Process() calls so a stream can be processed in blocks;
// Reset() clears the accumulators and FIFOs and re-seeds the per-channel states.
// The class is not thread-safe; process one stream from one thread (the audio
// thread). rnnoise_process_frame does not throw, so Process() is non-throwing.
class RnnoiseDenoiser {
  public:
    // Maximum interleaved channel count we keep state for. WASAPI mic capture is
    // mono or stereo; 8 covers any reasonable multichannel endpoint.
    static constexpr uint32_t kMaxChannels = 8;

    // RNNoise's fixed frame size: 480 samples (10 ms at 48 kHz).
    static constexpr uint32_t kFrameSize = 480;

    // RNNoise's required sample rate.
    static constexpr uint32_t kSampleRate = 48000;

    struct Config {
        uint32_t sample_rate = 48000;
        uint32_t channels = 1;
    };

    RnnoiseDenoiser() = default;
    explicit RnnoiseDenoiser(const Config& cfg);
    ~RnnoiseDenoiser();

    // Non-copyable / non-movable: owns raw RNNoise C states.
    RnnoiseDenoiser(const RnnoiseDenoiser&) = delete;
    RnnoiseDenoiser& operator=(const RnnoiseDenoiser&) = delete;
    RnnoiseDenoiser(RnnoiseDenoiser&&) = delete;
    RnnoiseDenoiser& operator=(RnnoiseDenoiser&&) = delete;

    // (Re)configure. Sanitizes degenerate inputs (channels in [1, kMaxChannels],
    // sample_rate >= 1). (Re)creates one DenoiseState per channel and clears all
    // buffers. When sample_rate != 48000 the stage becomes a no-op passthrough.
    void Configure(const Config& cfg);

    // Clear the per-channel input accumulators and denoised-output FIFOs and
    // re-seed the per-channel states (so the network's recurrent memory and the
    // block-latency priming start fresh).
    void Reset() noexcept;

    // Process `frames` interleaved samples in place. The buffer must hold
    // frames * Config.channels floats. No-op when sample_rate != 48000.
    void Process(float* interleaved, uint32_t frames) noexcept;

    // True when the stage is the active (48 kHz) denoising path rather than a
    // passthrough. Exposed for tests.
    [[nodiscard]] bool IsActive() const noexcept {
        return active_;
    }

    [[nodiscard]] const Config& GetConfig() const noexcept {
        return cfg_;
    }

  private:
    void DestroyStates() noexcept;
    void RecreateStates();

    Config cfg_{};
    bool active_ = false; // true only at 48 kHz

    // One RNNoise state per channel (RNNoise is mono).
    std::vector<DenoiseState*> states_;

    // Per-channel accumulator of not-yet-processed input samples, and per-channel
    // FIFO of denoised samples ready to emit. Flattened [channel][sample].
    std::vector<std::vector<float>> in_accum_;
    std::vector<std::vector<float>> out_fifo_;

    // Number of leading samples still owed as priming silence, per channel — the
    // one-block latency. Decremented as we emit zeros before the first denoised
    // block is available.
    std::vector<uint32_t> priming_remaining_;
};

} // namespace recorder_core
