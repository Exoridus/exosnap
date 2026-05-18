#pragma once
// Windows adapter: wraps WasapiLoopback behind IAudioCaptureSource.
// Full implementation wiring (session thread integration) is deferred to a later phase.

#include <recorder_core/interfaces/IAudioCaptureSource.h>

#include "../../wasapi_loopback.h"

#include <cstdint>
#include <string>

namespace recorder_core {

class WasapiLoopbackSrc : public IAudioCaptureSource {
  public:
    WasapiLoopbackSrc();
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

    void Shutdown() override;

  private:
    WasapiLoopback m_loopback;
    // Current buffer state — valid between AcquireBuffer() and ReleaseBuffer().
    uint32_t m_pending_frames = 0;
};

} // namespace recorder_core
