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
    // Frames known to have been lost immediately BEFORE this buffer's first
    // frame, at this source's advertised sample rate. Sources derive it from the
    // device-position jump across a DATA_DISCONTINUITY (0 when the gap length is
    // unknown or there is none). The consumer must fill the gap with silence so
    // the sample timeline — and therefore every later PTS — stays continuous.
    uint32_t gap_frames = 0;
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

    // Raw platform HRESULT of the last fatal acquire failure (e.g. a WASAPI
    // endpoint that was invalidated mid-recording), or 0 (S_OK) when the source
    // does not track one / none has occurred. Lets the drain surface the real
    // capture-loss code to the app log instead of a generic failure code.
    // Decorators forward their inner source's value; the default is 0.
    virtual int32_t LastCaptureHresult() const {
        return 0;
    }

    virtual void Shutdown() = 0;
};

} // namespace recorder_core
