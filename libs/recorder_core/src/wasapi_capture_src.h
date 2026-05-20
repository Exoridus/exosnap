#pragma once

#include <recorder_core/interfaces/IAudioCaptureSource.h>

#include <Audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace recorder_core {

class WasapiCaptureSrc : public IAudioCaptureSource {
  public:
    WasapiCaptureSrc() = default;
    ~WasapiCaptureSrc() override;

    WasapiCaptureSrc(const WasapiCaptureSrc&) = delete;
    WasapiCaptureSrc& operator=(const WasapiCaptureSrc&) = delete;

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
    IMMDevice* device_ = nullptr;
    IAudioClient* audio_client_ = nullptr;
    IAudioCaptureClient* capture_client_ = nullptr;
    std::string endpoint_name_;

    AudioSampleFormat sample_format_ = AudioSampleFormat::Float32;
    uint32_t sample_rate_ = 0;
    uint32_t channels_ = 0;       // source output channels
    uint32_t input_channels_ = 0; // capture device channels
    bool mono_to_stereo_ = false;

    bool buffer_acquired_ = false;
    uint32_t acquired_frames_ = 0;

    bool pending_capture_error_ = false;
    std::string pending_capture_error_msg_;

    std::vector<uint8_t> mono_expand_buffer_;
};

} // namespace recorder_core
