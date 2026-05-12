#pragma once

// WASAPI default render loopback capture wrapper.
// Lifted from M2.8 probe (probe_wgc_nvenc_aac_mkv).

#include <cstdint>
#include <string>

#include <Audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>

namespace recorder_core {

// ---------------------------------------------------------------------------
// WasapiLoopback
// ---------------------------------------------------------------------------

class WasapiLoopback {
public:
    WasapiLoopback() = default;
    ~WasapiLoopback();

    WasapiLoopback(const WasapiLoopback&)            = delete;
    WasapiLoopback& operator=(const WasapiLoopback&) = delete;

    // Initialize and start the loopback stream.
    // Required: 48000 Hz stereo (validated against mix format).
    bool Init(std::string& out_error);

    // Retrieve the next available packet.
    // Returns false if no data is available or on error.
    // Sets silent=true if AUDCLNT_BUFFERFLAGS_SILENT is set.
    bool GetNextPacket(
        BYTE**   out_data,
        UINT32*  out_num_frames,
        DWORD*   out_capture_flags,
        bool*    out_silent);

    // Release the current packet (must be called after GetNextPacket succeeds).
    bool ReleasePacket(UINT32 num_frames);

    // Query how many frames are ready in the next packet (0 = none).
    UINT32 GetNextPacketSize();

    // Stop and release all WASAPI resources.
    void Shutdown();

    const std::string& EndpointName() const { return m_endpointName; }

private:
    IMMDevice*           m_pDevice        = nullptr;
    IAudioClient*        m_pAudioClient   = nullptr;
    IAudioCaptureClient* m_pCaptureClient = nullptr;
    std::string          m_endpointName;
};

} // namespace recorder_core
