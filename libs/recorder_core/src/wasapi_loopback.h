#pragma once

// WASAPI default render loopback capture wrapper.
// Lifted from M2.8 probe (probe_wgc_nvenc_aac_mkv).

#include <cstdint>
#include <string>

#include <Audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>

namespace recorder_core {

// Pure classification of a WASAPI loopback acquire result — the HRESULT that
// IAudioCaptureClient::GetNextPacketSize() or ::GetBuffer() returned. Hardware-
// free so the fail-closed policy is unit-pinned without live COM objects.
// Mirrors ClassifyWasapiAcquireFailure (wasapi_capture_src.h), the shared
// Idle/Fail decision already used by the mic-capture and process-loopback
// paths: only S_OK / AUDCLNT_S_BUFFER_EMPTY are benign "no data this tick"
// results; AUDCLNT_E_DEVICE_INVALIDATED and every other HRESULT mean the
// endpoint is gone and must be surfaced, not looped through silently.
struct LoopbackAcquireResult {
    bool fatal = false;
    std::string error_message; // populated only when fatal
};
LoopbackAcquireResult ClassifyLoopbackAcquire(const char* api_name, HRESULT hr);

// ---------------------------------------------------------------------------
// WasapiLoopback
// ---------------------------------------------------------------------------

class WasapiLoopback {
  public:
    WasapiLoopback() = default;
    ~WasapiLoopback();

    WasapiLoopback(const WasapiLoopback&) = delete;
    WasapiLoopback& operator=(const WasapiLoopback&) = delete;

    // Initialize and start the loopback stream.
    // Always requests 48000 Hz stereo float via AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
    // so devices mixing at a different rate/channel count are resampled/downmixed
    // by WASAPI rather than rejected.
    bool Init(std::string& out_error);

    // Retrieve the next available packet.
    // Returns false if no data is available or on a fatal acquire error (e.g. the
    // endpoint was invalidated) — see LastFatalErrorHresult()/LastFatalErrorMessage()
    // to tell the two apart.
    // Sets silent=true if AUDCLNT_BUFFERFLAGS_SILENT is set.
    // out_device_position (optional) receives the packet's device position in
    // frames — used to measure the gap a DATA_DISCONTINUITY spans.
    // out_qpc_position (optional) receives the QPC timestamp (100 ns units) the
    // device recorded that position at — used by the A/V clock-drift metric.
    bool GetNextPacket(BYTE** out_data, UINT32* out_num_frames, DWORD* out_capture_flags, bool* out_silent,
                       UINT64* out_device_position = nullptr, UINT64* out_qpc_position = nullptr);

    // Release the current packet (must be called after GetNextPacket succeeds).
    bool ReleasePacket(UINT32 num_frames);

    // Query how many frames are ready in the next packet (0 = none). On a fatal
    // acquire failure this returns 1 (not 0) so a caller's "pending > 0" drain
    // loop is forced into GetNextPacket(), which reports the failure instead of
    // the caller looping forever against a dead endpoint.
    UINT32 GetNextPacketSize();

    // Stop and release all WASAPI resources.
    void Shutdown();

    const std::string& EndpointName() const {
        return m_endpointName;
    }

    // Raw HRESULT of the last fatal GetNextPacketSize()/GetNextPacket() failure
    // (e.g. AUDCLNT_E_DEVICE_INVALIDATED), or S_OK when the most recent call
    // succeeded or simply found no data (benign).
    HRESULT LastFatalErrorHresult() const {
        return m_lastFatalErrorHr;
    }

    // Human-readable text for the last fatal failure (empty when none).
    const std::string& LastFatalErrorMessage() const {
        return m_lastFatalErrorMsg;
    }

  private:
    IMMDevice* m_pDevice = nullptr;
    IAudioClient* m_pAudioClient = nullptr;
    IAudioCaptureClient* m_pCaptureClient = nullptr;
    std::string m_endpointName;
    HRESULT m_lastFatalErrorHr = S_OK;
    std::string m_lastFatalErrorMsg;
};

} // namespace recorder_core
