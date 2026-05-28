#pragma once
// Platform-agnostic audio capture interface.
// Windows implementation: WasapiLoopbackSrc (wraps WasapiLoopback).

#include <cstdint>
#include <string>

namespace recorder_core {

enum class AudioSampleFormat { Float32, Int16 };

struct RawAudioBuffer {
    const uint8_t* bytes = nullptr; // non-owning, valid until ReleaseBuffer()
    uint32_t num_frames = 0;
    bool silent = false;             // backend signals digitally silent buffer
    bool data_discontinuity = false; // WASAPI reported AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY
};

class IAudioCaptureSource {
  public:
    virtual ~IAudioCaptureSource() = default;

    // Initialize and start the capture stream.
    virtual bool Init(std::string& out_error) = 0;

    // Number of frames ready in the pending buffer (0 = none).
    virtual uint32_t PendingFrameCount() = 0;

    // Acquire the next available buffer. Only call when PendingFrameCount() > 0.
    virtual bool AcquireBuffer(RawAudioBuffer& out_buf, std::string& out_error) = 0;

    // Release the buffer from the last AcquireBuffer().
    virtual void ReleaseBuffer() = 0;

    virtual uint32_t SampleRate() const = 0;
    virtual uint32_t Channels() const = 0;
    virtual AudioSampleFormat SampleFormat() const = 0;
    virtual const std::string& EndpointName() const = 0;

    virtual void Shutdown() = 0;
};

} // namespace recorder_core
