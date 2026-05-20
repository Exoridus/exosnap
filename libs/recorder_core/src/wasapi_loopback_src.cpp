#include "wasapi_loopback_src.h"

namespace recorder_core {

WasapiLoopbackSrc::~WasapiLoopbackSrc() {
    Shutdown();
}

bool WasapiLoopbackSrc::Init(std::string& out_error) {
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
    if (!wasapi_.GetNextPacket(&data, &numFrames, &captureFlags, &silent)) {
        out_error.clear();
        return false;
    }

    buffer_acquired_ = true;
    last_frames_ = numFrames;

    out_buf.bytes = reinterpret_cast<const uint8_t*>(data);
    out_buf.num_frames = numFrames;
    out_buf.silent = silent;
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

void WasapiLoopbackSrc::Shutdown() {
    ReleaseBuffer();
    wasapi_.Shutdown();
}

} // namespace recorder_core
