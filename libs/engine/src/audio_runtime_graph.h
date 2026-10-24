#pragma once

#include "logical_audio_source.h"
#include "shared_audio_capture.h"
#include "track_audio_mixer.h"

#include <atomic>
#include <functional>
#include <mutex>

namespace exosnap::engine {

class AudioRuntimeGraph;

class AudioRuntimeTrackSource final : public IAudioCaptureSource {
  public:
    AudioRuntimeTrackSource(std::shared_ptr<AudioRuntimeGraph> graph, std::shared_ptr<void> reservation,
                            std::unique_ptr<TrackAudioMixer> mixer);
    ~AudioRuntimeTrackSource() override;
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
    bool BeginDrain(uint64_t end_qpc_ns, std::string& error);
    void Shutdown() override;

  private:
    std::shared_ptr<AudioRuntimeGraph> graph_;
    std::shared_ptr<void> reservation_;
    std::unique_ptr<TrackAudioMixer> mixer_;
};

// Physical capture is shared by binding. Each SourceId has exactly one DSP
// processor and distributes its processed PCM to all routes.
class AudioRuntimeGraph : public std::enable_shared_from_this<AudioRuntimeGraph> {
  public:
    using RequestFactory = std::function<SharedAudioCaptureRequest(const ResolvedAudioSource&)>;
    static std::shared_ptr<AudioRuntimeGraph> Prepare(const ResolvedAudioPlan& plan,
                                                      std::shared_ptr<SharedAudioCaptureRegistry> registry,
                                                      uint64_t start_qpc_ns, uint32_t output_rate,
                                                      uint32_t output_channels, bool clock_correction,
                                                      std::string& error, RequestFactory factory = {});
    ~AudioRuntimeGraph();
    std::vector<std::unique_ptr<IAudioCaptureSource>> TakeTracks();
    void Start(std::function<void(std::string)> on_error = {});
    void RequestStop(uint64_t end_qpc_ns) noexcept;
    bool WaitDrained(uint32_t timeout_ms, std::string& error);
    bool SetSourceMuted(const AudioSourceId& id, bool muted);
    [[nodiscard]] std::vector<LogicalAudioMeter> SourceMeters() const;
    [[nodiscard]] std::vector<TrackMeterSnapshot> TrackMeters() const;
    [[nodiscard]] std::vector<PhysicalMeterSnapshot> PhysicalMeters() const;

  private:
    friend class AudioRuntimeTrackSource;
    struct Node {
        std::unique_ptr<IAudioCaptureSource> physical;
        std::shared_ptr<void> reservation;
        std::unique_ptr<LogicalAudioSource> logical;
        std::shared_ptr<std::atomic<uint64_t>> completed_through;
    };
    AudioRuntimeGraph();
    void Run();
    void Fail(std::string error);
    std::shared_ptr<SharedAudioCaptureRegistry> registry_;
    std::vector<Node> nodes_;
    struct TrackEntry {
        std::shared_ptr<void> reservation;
        std::unique_ptr<TrackAudioMixer> mixer;
    };
    std::vector<TrackEntry> tracks_;
    std::vector<std::shared_ptr<TrackAudioTelemetry>> track_telemetry_;
    std::atomic<uint64_t> end_qpc_ns_{0};
    std::atomic<bool> started_{false};
    std::function<void(std::string)> on_error_;
    mutable std::mutex error_mutex_;
    std::string error_;
    void* drained_event_ = nullptr;
};

} // namespace exosnap::engine
