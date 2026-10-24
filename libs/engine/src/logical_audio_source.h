#pragma once

#include "audio_tee.h"
#include <exosnap/engine/audio_routing_model.h>
#include <memory>
#include <string>
#include <vector>

namespace exosnap::engine {
struct LogicalAudioMeter {
    AudioSourceId source_id;
    uint64_t source_generation = 0;
    uint64_t processed_blocks = 0;
    uint64_t processed_frames = 0;
    uint64_t missing_frames = 0;
    uint64_t history_reset_count = 0;
    bool has_measurement = false;
    float rms = 0;
    float peak = 0;
    bool muted = false;
    PhysicalClockId clock_id;
    uint64_t reader_count = 0;
    uint64_t reader_overflow_frames = 0;
    bool processing_time_available = false;
    double processing_ms = 0;
};

// One runtime-owned DSP chain; reader attachment does not reset its history.
class LogicalAudioSource {
  public:
    // Conservative reservation envelopes, including library state; not allocator measurements.
    static size_t RequiredPayloadBytes(const AudioCaptureDescriptor& descriptor);
    static size_t RequiredPayloadBytes(const ResolvedAudioSource& source, AudioCaptureDescriptor descriptor);
    static AudioCaptureDescriptor ProjectOutputDescriptor(const ResolvedAudioSource& source,
                                                          AudioCaptureDescriptor input);
    static std::unique_ptr<LogicalAudioSource> Create(ResolvedAudioSource source,
                                                      const AudioCaptureDescriptor& descriptor, std::string& error);
    ~LogicalAudioSource();
    bool Process(const AudioBlockView& input, std::string& error);
    std::vector<std::shared_ptr<AudioTeeReader>> AttachReaders(uint32_t count, uint64_t start_qpc_ns,
                                                               std::string& error);
    void SetMuted(bool muted) noexcept;
    [[nodiscard]] LogicalAudioMeter Meter() const;
    // Reconfiguration must not mutate a descriptor retained by a caller.
    // cppcheck-suppress returnByReference
    [[nodiscard]] AudioCaptureDescriptor Descriptor() const;
    [[nodiscard]] size_t ReservedPayloadBytes() const;
    // Advancing generations can shrink within admission; growth requires a new prepare transaction.
    bool Reconfigure(const AudioCaptureDescriptor& descriptor, std::string& error);
    bool Drain(std::string& error);
    // Called by the source owner after processing the physical queue snapshot.
    [[nodiscard]] uint64_t CompletedThrough(uint64_t physical_qpc_ns) const;
    void Stop();

  private:
    struct Impl;
    explicit LogicalAudioSource(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Only the processed-reader lease is owned here, never the physical endpoint.
std::unique_ptr<IAudioCaptureSource> MakeLogicalAudioReader(std::shared_ptr<AudioTeeReader> reader);
AudioReaderEnd SealLogicalAudioReaderEnd(IAudioCaptureSource& reader, uint64_t end_qpc_ns);
bool LogicalAudioReaderEnded(const IAudioCaptureSource& reader);
bool LastLogicalAudioBlock(const IAudioCaptureSource& reader, AudioBlockView& block);
std::optional<AudioCaptureDescriptor> LogicalAudioReaderDescriptor(const IAudioCaptureSource& reader);
} // namespace exosnap::engine
