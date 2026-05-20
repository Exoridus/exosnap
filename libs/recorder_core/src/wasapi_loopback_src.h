#pragma once

#include <recorder_core/interfaces/IAudioCaptureSource.h>

#include "wasapi_loopback.h"

#include <cstdint>
#include <string>

namespace recorder_core {

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

    void Shutdown() override;

  private:
    WasapiLoopback wasapi_;
    uint32_t last_frames_ = 0;
    bool buffer_acquired_ = false;
};

} // namespace recorder_core
