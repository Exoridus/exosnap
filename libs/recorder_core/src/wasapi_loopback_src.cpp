#include "wasapi_loopback_src.h"

#include "discontinuity_gap.h"

namespace recorder_core {

WasapiLoopbackSrc::~WasapiLoopbackSrc() {
    Shutdown();
}

bool WasapiLoopbackSrc::Init(std::string& out_error) {
    last_capture_hr_ = 0;
    return wasapi_.Init(out_error);
}

uint32_t WasapiLoopbackSrc::PendingFrameCount() {
    return wasapi_.GetNextPacketSize();
}

bool WasapiLoopbackSrc::AcquireBuffer(RawAudioBuffer& out_buf, std::string& out_error) {
    if (buffer_acquired_) {
        out_error = "WasapiLoopbackSrc::AcquireBuffer called while a packet is already held";
        return false;
    }

    BYTE* data = nullptr;
    UINT32 numFrames = 0;
    DWORD captureFlags = 0;
    bool silent = false;
    UINT64 devicePos = 0;
    if (!wasapi_.GetNextPacket(&data, &numFrames, &captureFlags, &silent, &devicePos)) {
        const HRESULT fatalHr = wasapi_.LastFatalErrorHresult();
        if (fatalHr != S_OK) {
            // The endpoint is gone (invalidated / service down / unexpected
            // acquire failure) — surface it so the drain ends the recording
            // cleanly instead of looping on a track that has gone silently
            // mute (see wasapi_capture_src.cpp's mic-capture precedent this
            // mirrors: a non-empty out_error is a fatal capture loss).
            last_capture_hr_ = static_cast<int32_t>(fatalHr);
            out_error = wasapi_.LastFatalErrorMessage();
            return false;
        }
        // Benign: no packet ready this tick.
        out_error.clear();
        return false;
    }

    buffer_acquired_ = true;
    last_frames_ = numFrames;

    const bool discontinuity = (captureFlags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
    const uint32_t gap_frames = ComputeDiscontinuityGapFrames(discontinuity, device_position_tracked_,
                                                              expected_device_position_, devicePos, SampleRate());
    device_position_tracked_ = true;
    expected_device_position_ = devicePos + numFrames;

    out_buf.bytes = reinterpret_cast<const uint8_t*>(data);
    out_buf.num_frames = numFrames;
    out_buf.silent = silent;
    out_buf.data_discontinuity = discontinuity;
    out_buf.gap_frames = gap_frames;
    return true;
}

void WasapiLoopbackSrc::ReleaseBuffer() {
    if (!buffer_acquired_)
        return;
    wasapi_.ReleasePacket(last_frames_);
    buffer_acquired_ = false;
    last_frames_ = 0;
}

uint32_t WasapiLoopbackSrc::SampleRate() const {
    return 48000;
}

uint32_t WasapiLoopbackSrc::Channels() const {
    return 2;
}

AudioSampleFormat WasapiLoopbackSrc::SampleFormat() const {
    return AudioSampleFormat::Float32;
}

const std::string& WasapiLoopbackSrc::EndpointName() const {
    return wasapi_.EndpointName();
}

int32_t WasapiLoopbackSrc::LastCaptureHresult() const {
    return last_capture_hr_;
}

void WasapiLoopbackSrc::Shutdown() {
    ReleaseBuffer();
    wasapi_.Shutdown();
}

} // namespace recorder_core
