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

// Device-clock timing of a capture packet: the packet's position on the audio
// device's own timeline paired with the QPC time at which the device recorded
// that position (both in nanoseconds). WASAPI reports both with every
// IAudioCaptureClient::GetBuffer call; together they measure the audio device
// clock against the QPC timeline video frames are paced on — the input to the
// A/V clock-drift metric (audio_clock_drift.h).
struct AudioDeviceTiming {
    uint64_t device_position_ns = 0;
    uint64_t qpc_position_ns = 0;
};

// Convert a device sample position (frames) to nanoseconds on that device's
// timeline without 64-bit overflow (frames * 1e9 would overflow uint64 after
// a few hours at 48 kHz).
inline uint64_t DeviceFramesToNs(uint64_t frames, uint32_t sample_rate) noexcept {
    if (sample_rate == 0) {
        return 0;
    }
    const uint64_t whole_seconds = frames / sample_rate;
    const uint64_t remainder_frames = frames % sample_rate;
    return whole_seconds * 1000000000ULL + (remainder_frames * 1000000000ULL) / sample_rate;
}

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

    // Device-clock timing of the most recently acquired buffer (valid after a
    // successful AcquireBuffer until the next one). Returns false when the
    // source cannot attribute a single device clock — mixed/merged sources,
    // synthetic test sources — or when the platform did not report a QPC
    // timestamp. Decorators forward their inner source's value.
    virtual bool LastBufferDeviceTiming(AudioDeviceTiming& /*out_timing*/) const {
        return false;
    }

    // Optional event-driven mode: a Win32 auto-reset event (HANDLE as void* to
    // keep this header platform-agnostic) the audio engine signals when a
    // capture buffer becomes ready (AUDCLNT_STREAMFLAGS_EVENTCALLBACK +
    // SetEventHandle). nullptr when the source only supports polling — the
    // consumer then falls back to its polling cadence. The handle is owned by
    // the source and valid between a successful Init() and Shutdown().
    // Decorators forward their inner source's handle.
    virtual void* BufferReadyEvent() const {
        return nullptr;
    }

    virtual void Shutdown() = 0;
};

} // namespace recorder_core
