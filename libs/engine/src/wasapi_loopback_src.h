#pragma once

#include <exosnap/engine/interfaces/IAudioCaptureSource.h>

#include "wasapi_loopback.h"

#include <cstdint>
#include <string>

namespace exosnap::engine {

class WasapiLoopbackSrc : public IAudioCaptureSource {
  public:
    WasapiLoopbackSrc() = default;
    ~WasapiLoopbackSrc() override;

    WasapiLoopbackSrc(const WasapiLoopbackSrc&) = delete;
    WasapiLoopbackSrc& operator=(const WasapiLoopbackSrc&) = delete;

    bool Init(std::string& out_error) override;
    uint32_t PendingFrameCount() override;
    bool AcquireBuffer(RawAudioBuffer& out_buf, std::string& out_error) override;
    void ReleaseBuffer() override;

    uint32_t SampleRate() const override;
    uint32_t Channels() const override;
    AudioSampleFormat SampleFormat() const override;
    const std::string& EndpointName() const override;
    int32_t LastCaptureHresult() const override;
    bool LastBufferDeviceTiming(AudioDeviceTiming& out_timing) const override;

    void Shutdown() override;

  private:
    WasapiLoopback wasapi_;
    uint32_t last_frames_ = 0;
    bool buffer_acquired_ = false;
    // Raw HRESULT of the last fatal acquire failure (endpoint loss). Surfaced
    // to the drain so it reaches the app log as the recording's error code
    // instead of a generic E_FAIL. 0 (S_OK) when no fatal failure has occurred.
    int32_t last_capture_hr_ = 0;

    // Device-position tracking for discontinuity gap measurement
    // (discontinuity_gap.h).
    bool device_position_tracked_ = false;
    uint64_t expected_device_position_ = 0;

    // Device-clock timing of the most recently acquired packet, for the A/V
    // clock-drift metric (audio_clock_drift.h).
    bool last_timing_valid_ = false;
    uint64_t last_device_position_ns_ = 0;
    uint64_t last_qpc_position_ns_ = 0;
};

} // namespace exosnap::engine
