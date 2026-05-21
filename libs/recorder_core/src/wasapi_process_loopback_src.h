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
    void Shutdown() override;

  private:
    DWORD pid_ = 0;
    ProcessLoopbackMode mode_ = ProcessLoopbackMode::IncludeProcessTree;

    IAudioClient* audio_client_ = nullptr;
    IAudioCaptureClient* capture_client_ = nullptr;

    std::string endpoint_name_;
    bool buffer_acquired_ = false;
    uint32_t acquired_frames_ = 0;

    bool pending_capture_error_ = false;
    std::string pending_capture_error_msg_;
};

} // namespace recorder_core
