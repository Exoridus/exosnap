#pragma once

#include <recorder_core/interfaces/IAudioCaptureSource.h>

#include <Audioclient.h>
#include <windows.h>

#include <cstdint>
#include <string>

namespace recorder_core {

enum class ProcessLoopbackMode {
    IncludeProcessTree,
    ExcludeProcessTree,
};

class WasapiProcessLoopbackSrc : public IAudioCaptureSource {
  public:
    WasapiProcessLoopbackSrc(DWORD target_pid, ProcessLoopbackMode mode);
    ~WasapiProcessLoopbackSrc() override;

    WasapiProcessLoopbackSrc(const WasapiProcessLoopbackSrc&) = delete;
    WasapiProcessLoopbackSrc& operator=(const WasapiProcessLoopbackSrc&) = delete;

    bool Init(std::string& out_error) override;
    uint32_t PendingFrameCount() override;
    bool AcquireBuffer(RawAudioBuffer& out_buf, std::string& out_error) override;
    void ReleaseBuffer() override;

    uint32_t SampleRate() const override;
    uint32_t Channels() const override;
    AudioSampleFormat SampleFormat() const override;
    const std::string& EndpointName() const override;
    bool LastBufferDeviceTiming(AudioDeviceTiming& out_timing) const override;
    void* BufferReadyEvent() const override;
    void Shutdown() override;

  private:
    DWORD pid_ = 0;
    ProcessLoopbackMode mode_ = ProcessLoopbackMode::IncludeProcessTree;

    IAudioClient* audio_client_ = nullptr;
    IAudioCaptureClient* capture_client_ = nullptr;
    // Auto-reset event the audio engine signals per ready packet
    // (AUDCLNT_STREAMFLAGS_EVENTCALLBACK); owned here, exposed via
    // BufferReadyEvent() for the event-driven drain.
    HANDLE buffer_event_ = nullptr;

    std::string endpoint_name_;
    bool buffer_acquired_ = false;
    uint32_t acquired_frames_ = 0;

    // Device-position tracking for discontinuity gap measurement
    // (discontinuity_gap.h).
    bool device_position_tracked_ = false;
    uint64_t expected_device_position_ = 0;

    // Device-clock timing of the most recently acquired packet, for the A/V
    // clock-drift metric (audio_clock_drift.h).
    bool last_timing_valid_ = false;
    uint64_t last_device_position_ns_ = 0;
    uint64_t last_qpc_position_ns_ = 0;

    bool pending_capture_error_ = false;
    std::string pending_capture_error_msg_;
};

} // namespace recorder_core
