#pragma once

#include <exosnap/engine/audio_limits.h>
#include <exosnap/engine/interfaces/IAudioCaptureSource.h>
#include <exosnap/engine/recording_media_types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace exosnap::engine {

enum class AudioClockQuality { Unknown, Measured, Degraded };

struct AudioCaptureDescriptor {
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    uint32_t maximum_packet_frames = 0;
    PhysicalClockId clock_id;
    uint64_t source_generation = 1;
    uint64_t format_generation = 1;
    bool clock_corrected = true;
    AudioSampleFormat sample_format = AudioSampleFormat::Float32;
    AudioClockQuality clock_quality = AudioClockQuality::Unknown;
};

struct AudioBlockMetadata {
    uint64_t sequence = 0;
    uint64_t native_sample_position = 0;
    bool native_sample_position_valid = false;
    uint64_t capture_qpc_ns = 0;
    uint64_t output_qpc_ns = 0;
    uint64_t filter_latency_ns = 0;
    uint32_t frames = 0;
    uint32_t device_gap_frames = 0;
    bool silence = false;
    bool discontinuity = false;
    AudioDeviceTiming device_timing;
    bool device_timing_valid = false;
};

struct AudioBlockView {
    AudioCaptureDescriptor descriptor;
    AudioBlockMetadata metadata;
    std::span<const float> samples;
    uint64_t reader_overflow_frames = 0;
};

struct AudioReaderEnd {
    uint64_t end_sequence_exclusive = 0;
    uint64_t end_qpc_ns = 0;
    bool stalled = false;
    std::optional<uint64_t> undrained_frames;
};

[[nodiscard]] size_t AudioCorrectionPayloadBytes(const AudioCaptureDescriptor& admission);

struct AudioTeeState;

class AudioTeeReader {
  public:
    ~AudioTeeReader();
    AudioTeeReader(const AudioTeeReader&) = delete;
    AudioTeeReader& operator=(const AudioTeeReader&) = delete;
    bool Acquire(AudioBlockView& block, std::string& error);
    void Release();
    [[nodiscard]] uint32_t PendingFrames() const;
    [[nodiscard]] AudioCaptureDescriptor Descriptor() const;
    [[nodiscard]] AudioReaderEnd SealEnd(uint64_t end_qpc_ns);
    [[nodiscard]] bool Ended() const;
    [[nodiscard]] void* ReadyEvent() const;
    [[nodiscard]] uint64_t WakeSequence() const;

  private:
    friend class AudioTee;
    AudioTeeReader(std::shared_ptr<AudioTeeState> state, uint32_t index);
    std::shared_ptr<AudioTeeState> state_;
    uint32_t index_;
    bool committed_ = false;
};

class AudioTee {
  public:
    static std::shared_ptr<AudioTee> Create(const AudioCaptureDescriptor& admission, std::string& error,
                                            uint32_t maximum_readers = kMaxAudioTeeReaders);
    static size_t RequiredPayloadBytes(const AudioCaptureDescriptor& admission,
                                       uint32_t maximum_readers = kMaxAudioTeeReaders);
    std::vector<std::shared_ptr<AudioTeeReader>> AttachReaders(uint32_t count, uint64_t start_qpc_ns,
                                                               std::string& error);
    bool Publish(std::span<const float> samples, AudioBlockMetadata metadata, std::string& error);
    bool SetDescriptor(const AudioCaptureDescriptor& descriptor, std::string& error);
    void Stop();
    [[nodiscard]] size_t ReservedPayloadBytes() const;
    [[nodiscard]] size_t MetadataBytes() const;
    [[nodiscard]] uint32_t ReaderCount() const;
    // Cumulative evicted frames, counted separately for each attached reader.
    [[nodiscard]] uint64_t ReaderOverflowFrames() const;

  private:
    explicit AudioTee(std::shared_ptr<AudioTeeState> state);
    std::shared_ptr<AudioTeeState> state_;
};

} // namespace exosnap::engine
