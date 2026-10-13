#pragma once

#include "audio_tee.h"
#include <exosnap/engine/session_stats.h>

#include <exosnap/engine/audio_routing_model.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace exosnap::engine {

using PhysicalClockQuality = AudioClockQuality;

struct PhysicalAudioStatistics {
    uint64_t capture_opens = 0;
    uint64_t input_blocks = 0;
    uint64_t input_frames = 0;
    uint64_t output_frames = 0;
    uint64_t compensation_updates = 0;
    uint64_t undrained_frames = 0;
    double applied_compensation_ms = 0;
    double drift_ms = 0;
    double residual_ms = 0;
    double compensation_ppm = 0;
    PhysicalClockQuality clock_quality = PhysicalClockQuality::Unknown;
    uint64_t reinit_count = 0;
    bool processing_time_available = false;
    double processing_ms = 0;
};

class PhysicalAudioClockSource final : public IAudioCaptureSource {
  public:
    PhysicalAudioClockSource(std::unique_ptr<IAudioCaptureSource> native, AudioCaptureDescriptor admission,
                             bool correction_enabled = true);
    ~PhysicalAudioClockSource() override;
    bool Init(std::string& error) override;
    bool Reinit(std::string& error) override;
    uint32_t PendingFrameCount() override;
    bool AcquireBuffer(RawAudioBuffer& buffer, std::string& error) override;
    void ReleaseBuffer() override;
    uint32_t SampleRate() const override;
    uint32_t Channels() const override;
    AudioSampleFormat SampleFormat() const override;
    const std::string& EndpointName() const override;
    bool LastBufferDeviceTiming(AudioDeviceTiming& timing) const override;
    uint64_t LastBufferQpcNs() const override;
    void* BufferReadyEvent() const override;
    int32_t LastCaptureHresult() const override;
    int32_t LastInitHresult() const override;
    bool ClockCorrected() const override;
    void Shutdown() override;
    // A snapshot must remain stable while capture advances its counters.
    // cppcheck-suppress returnByReference
    [[nodiscard]] PhysicalAudioStatistics Statistics() const;
    [[nodiscard]] AudioCaptureDescriptor Descriptor() const;
    [[nodiscard]] uint64_t BufferedFrames() const;
    uint32_t DrainResampler(RawAudioBuffer& buffer, int64_t* undrained_frames = nullptr);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct SharedAudioCaptureRequest {
    PhysicalCaptureKey key;
    AudioCaptureDescriptor admission;
    std::function<bool(AudioCaptureDescriptor&, std::string&)> describe_native;
    std::function<std::unique_ptr<IAudioCaptureSource>()> native_factory;
    bool correction_enabled = true;
    std::chrono::milliseconds preparation_timeout{2000};
};

class SharedAudioCaptureRegistry {
  public:
    explicit SharedAudioCaptureRegistry(size_t payload_budget = kAudioRuntimePayloadBytes);
    ~SharedAudioCaptureRegistry();
    // Admission receives descriptors in request order, including an empty set.
    // It runs before new captures initialize and may reserve payload. It must not
    // call registry operations that acquire the registry lock, including AttachReaders.
    std::vector<std::unique_ptr<IAudioCaptureSource>>
    AttachReaders(const std::vector<SharedAudioCaptureRequest>& requests, uint64_t start_qpc_ns, std::string& error,
                  std::function<bool(std::span<const AudioCaptureDescriptor>, std::string&)> admit = {});
    [[nodiscard]] std::shared_ptr<void> ReservePayload(size_t bytes, std::string& error);
    [[nodiscard]] size_t ReservedPayloadBytes() const;
    [[nodiscard]] PhysicalAudioStatistics Statistics() const;
    [[nodiscard]] std::vector<PhysicalMeterSnapshot> MeterSnapshots() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] AudioReaderEnd SealAudioReaderEnd(IAudioCaptureSource& reader, uint64_t end_qpc_ns);
[[nodiscard]] std::vector<AudioReaderEnd>
BatchSealAudioReadersEnd(std::span<IAudioCaptureSource* const> readers, uint64_t end_qpc_ns,
                         std::chrono::milliseconds timeout = std::chrono::milliseconds(500));
[[nodiscard]] bool AudioReaderEnded(const IAudioCaptureSource& reader);
[[nodiscard]] bool LastSharedAudioBlock(const IAudioCaptureSource& reader, AudioBlockView& block);
[[nodiscard]] std::optional<AudioCaptureDescriptor> SharedAudioReaderDescriptor(const IAudioCaptureSource& reader);
[[nodiscard]] std::string SharedAudioCaptureError(const IAudioCaptureSource& reader);

} // namespace exosnap::engine
