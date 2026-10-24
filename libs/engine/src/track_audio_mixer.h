#pragma once
#include "logical_audio_source.h"
#include <exosnap/engine/session_stats.h>
#include <functional>
#include <mutex>

namespace exosnap::engine {
struct TrackAudioTelemetry {
    // Readback survives mixer transfer and destruction. Access snapshot under mutex.
    mutable std::mutex mutex;
    TrackMeterSnapshot snapshot;
};
// Null inputs retain their fixed-divisor slot and contribute silence.
class TrackAudioMixer final : public IAudioCaptureSource {
  public:
    static size_t RequiredPayloadBytes(uint32_t sample_rate, uint32_t channels, uint32_t input_count);
    static size_t RequiredPayloadBytes(uint32_t sample_rate, uint32_t channels,
                                       std::span<const AudioCaptureDescriptor> inputs);
    [[nodiscard]] size_t ReservedPayloadBytes() const;
    [[nodiscard]] std::shared_ptr<TrackAudioTelemetry> Telemetry() const;
    // The copied failure remains valid after the mixer is destroyed.
    // cppcheck-suppress returnByReference
    [[nodiscard]] std::string Failure() const;
    TrackAudioMixer(ResolvedRoutingTrack track, std::vector<std::unique_ptr<IAudioCaptureSource>> inputs,
                    uint32_t sample_rate, uint32_t channels, std::function<uint64_t()> clock_now_ns = {},
                    uint64_t start_qpc_ns = 0, std::function<uint64_t()> completed_through = {});
    ~TrackAudioMixer() override;
    bool Init(std::string& error) override;
    bool Reinit(std::string& error) override;
    uint32_t PendingFrameCount() override;
    bool AcquireBuffer(RawAudioBuffer& buffer, std::string& error) override;
    void ReleaseBuffer() override;
    uint32_t SampleRate() const override;
    uint32_t Channels() const override;
    AudioSampleFormat SampleFormat() const override;
    const std::string& EndpointName() const override;
    bool ClockCorrected() const override;
    uint64_t LastBufferQpcNs() const override;
    uint32_t CaptureSourceCount() const override;
    uint32_t DegradedSourceCount() const override;
    void BeginDrain(uint64_t end_qpc_ns);
    bool BeginDrain(uint64_t end_qpc_ns, std::string& error);
    [[nodiscard]] bool Ended() const;
    [[nodiscard]] AudioReaderEnd DrainOutcome() const;
    void Shutdown() override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace exosnap::engine
