#pragma once

#include <recorder_core/interfaces/IAudioCaptureSource.h>
#include <recorder_core/recorder_session.h>

#include <Audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace recorder_core {

namespace mic_channel_detail {

struct AutoModeState {
    uint64_t analyzed_frames = 0;
    double energy_left = 0.0;
    double energy_right = 0.0;
    bool locked = false;
    MicChannelMode resolved_mode = MicChannelMode::PreserveStereo;
};

void MapStereoFrameFloat32(MicChannelMode mode, float input_l, float input_r, float& output_l, float& output_r);
void MapStereoFrameInt16(MicChannelMode mode, int16_t input_l, int16_t input_r, int16_t& output_l, int16_t& output_r);
MicChannelMode ResolveAutoChannelMode(double rms_left, double rms_right);
void UpdateAutoModeStateFloat32(AutoModeState& state, const float* interleaved_stereo, uint32_t num_frames,
                                bool silent);
void UpdateAutoModeStateInt16(AutoModeState& state, const int16_t* interleaved_stereo, uint32_t num_frames,
                              bool silent);
MicChannelMode EffectiveAutoMode(const AutoModeState& state);

} // namespace mic_channel_detail

class WasapiCaptureSrc : public IAudioCaptureSource {
  public:
    explicit WasapiCaptureSrc(MicChannelMode channel_mode = MicChannelMode::Auto,
                              std::optional<std::string> device_id = std::nullopt);
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
    void UpdateAutoModeFromBuffer(const uint8_t* data, uint32_t num_frames, bool silent);
    MicChannelMode EffectiveChannelMode() const;

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

    MicChannelMode requested_channel_mode_ = MicChannelMode::Auto;
    std::optional<std::string> device_id_;
    MicChannelMode auto_resolved_mode_ = MicChannelMode::PreserveStereo;
    bool auto_mode_locked_ = false;
    uint64_t auto_detect_frames_ = 0;
    double auto_energy_left_ = 0.0;
    double auto_energy_right_ = 0.0;

    std::vector<uint8_t> mapped_buffer_;
};

} // namespace recorder_core
