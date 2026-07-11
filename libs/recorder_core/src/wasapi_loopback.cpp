#include "wasapi_loopback.h"

// ClassifyWasapiAcquireFailure/WasapiAcquireFailAction — the shared WASAPI
// acquire fail-closed policy this mirrors (see wasapi_capture_src.h).
#include "wasapi_capture_src.h"

// WideToUtf8 is declared in wgc_capture.h; include it directly
#include "wgc_capture.h"

#include <functiondiscoverykeys_devpkey.h>
#include <initguid.h>

#include <cstdio>

namespace recorder_core {

// ---------------------------------------------------------------------------
// ClassifyLoopbackAcquire
// ---------------------------------------------------------------------------

LoopbackAcquireResult ClassifyLoopbackAcquire(const char* api_name, HRESULT hr) {
    LoopbackAcquireResult result;
    if (ClassifyWasapiAcquireFailure(hr) != WasapiAcquireFailAction::Fail) {
        return result; // benign: S_OK / AUDCLNT_S_BUFFER_EMPTY — no data this tick
    }

    result.fatal = true;
    char buf[192];
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        snprintf(buf, sizeof(buf),
                 "%s failed: AUDCLNT_E_DEVICE_INVALIDATED (0x%08lX) - system audio device invalidated", api_name,
                 static_cast<unsigned long>(hr));
    } else if (hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
        snprintf(buf, sizeof(buf),
                 "%s failed: AUDCLNT_E_SERVICE_NOT_RUNNING (0x%08lX) - Windows Audio service not running", api_name,
                 static_cast<unsigned long>(hr));
    } else {
        snprintf(buf, sizeof(buf), "%s failed 0x%08lX", api_name, static_cast<unsigned long>(hr));
    }
    result.error_message = buf;
    return result;
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

WasapiLoopback::~WasapiLoopback() {
    Shutdown();
}

// ---------------------------------------------------------------------------
// Init — lifted from M2.8 probe Phase11
// ---------------------------------------------------------------------------

bool WasapiLoopback::Init(std::string& out_error) {
    constexpr uint32_t kRequiredSampleRate = 48000;
    constexpr uint32_t kRequiredChannels = 2;
    constexpr LONGLONG kHnsBuffer = 2000000LL; // 200 ms

    m_lastFatalErrorHr = S_OK;
    m_lastFatalErrorMsg.clear();

    IMMDeviceEnumerator* pEnum = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pEnum));
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "CoCreateInstance(MMDeviceEnumerator) 0x%08lX", static_cast<unsigned long>(hr));
        out_error = buf;
        return false;
    }

    hr = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &m_pDevice);
    pEnum->Release();
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "GetDefaultAudioEndpoint 0x%08lX", static_cast<unsigned long>(hr));
        out_error = buf;
        return false;
    }

    // Friendly name
    {
        IPropertyStore* pProps = nullptr;
        if (SUCCEEDED(m_pDevice->OpenPropertyStore(STGM_READ, &pProps))) {
            PROPVARIANT var;
            PropVariantInit(&var);
            if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &var)) && var.vt == VT_LPWSTR) {
                m_endpointName = WideToUtf8(var.pwszVal);
            }
            PropVariantClear(&var);
            pProps->Release();
        }
        if (m_endpointName.empty())
            m_endpointName = "(unnamed)";
    }

    hr = m_pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&m_pAudioClient));
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "Activate(IAudioClient) 0x%08lX", static_cast<unsigned long>(hr));
        out_error = buf;
        return false;
    }

    // The render endpoint's shared-mode mix format is whatever the user has
    // configured (e.g. a 44.1 kHz DAC, or a 5.1/7.1 speaker layout) — it is not
    // guaranteed to be 48 kHz/stereo. Rather than rejecting those endpoints,
    // request our fixed 48 kHz/2 ch float format and set
    // AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM so the audio engine's own sample-rate
    // converter and channel matrixer resample/downmix into it, mirroring the
    // process-loopback path (see wasapi_process_loopback_src.cpp).
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels = static_cast<WORD>(kRequiredChannels);
    fmt.nSamplesPerSec = kRequiredSampleRate;
    fmt.wBitsPerSample = 32;
    fmt.nBlockAlign = static_cast<WORD>(fmt.nChannels * fmt.wBitsPerSample / 8);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    fmt.cbSize = 0;

    hr = m_pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                    kHnsBuffer, 0, &fmt, nullptr);
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "IAudioClient::Initialize(LOOPBACK) 0x%08lX", static_cast<unsigned long>(hr));
        out_error = buf;
        return false;
    }

    hr = m_pAudioClient->GetService(IID_PPV_ARGS(&m_pCaptureClient));
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "GetService(IAudioCaptureClient) 0x%08lX", static_cast<unsigned long>(hr));
        out_error = buf;
        return false;
    }

    hr = m_pAudioClient->Start();
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "IAudioClient::Start 0x%08lX", static_cast<unsigned long>(hr));
        out_error = buf;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// GetNextPacketSize
// ---------------------------------------------------------------------------

UINT32 WasapiLoopback::GetNextPacketSize() {
    if (!m_pCaptureClient)
        return 0;
    UINT32 n = 0;
    const HRESULT hr = m_pCaptureClient->GetNextPacketSize(&n);
    const LoopbackAcquireResult result = ClassifyLoopbackAcquire("IAudioCaptureClient::GetNextPacketSize", hr);
    if (result.fatal) {
        m_lastFatalErrorHr = hr;
        m_lastFatalErrorMsg = result.error_message;
        // Force the caller into GetNextPacket(), which surfaces the failure via
        // LastFatalError{Hresult,Message}() instead of returning 0 forever
        // against a dead endpoint (the track would otherwise go silently mute
        // on a device unplug/invalidation instead of failing visibly).
        return 1;
    }
    m_lastFatalErrorHr = S_OK;
    m_lastFatalErrorMsg.clear();
    return n;
}

// ---------------------------------------------------------------------------
// GetNextPacket
// ---------------------------------------------------------------------------

bool WasapiLoopback::GetNextPacket(BYTE** out_data, UINT32* out_num_frames, DWORD* out_capture_flags, bool* out_silent,
                                   UINT64* out_device_position, UINT64* out_qpc_position) {
    if (!m_pCaptureClient)
        return false;
    UINT64 devicePos = 0, qpcPos = 0;
    const HRESULT hr = m_pCaptureClient->GetBuffer(out_data, out_num_frames, out_capture_flags, &devicePos, &qpcPos);
    const LoopbackAcquireResult result = ClassifyLoopbackAcquire("IAudioCaptureClient::GetBuffer", hr);
    if (result.fatal) {
        m_lastFatalErrorHr = hr;
        m_lastFatalErrorMsg = result.error_message;
        return false;
    }
    m_lastFatalErrorHr = S_OK;
    m_lastFatalErrorMsg.clear();
    if (*out_num_frames == 0)
        return false;
    if (out_silent)
        *out_silent = (*out_capture_flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
    if (out_device_position)
        *out_device_position = devicePos;
    if (out_qpc_position)
        *out_qpc_position = qpcPos;
    return true;
}

// ---------------------------------------------------------------------------
// ReleasePacket
// ---------------------------------------------------------------------------

bool WasapiLoopback::ReleasePacket(UINT32 num_frames) {
    if (!m_pCaptureClient)
        return false;
    return SUCCEEDED(m_pCaptureClient->ReleaseBuffer(num_frames));
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void WasapiLoopback::Shutdown() {
    m_lastFatalErrorHr = S_OK;
    m_lastFatalErrorMsg.clear();
    if (m_pCaptureClient) {
        m_pCaptureClient->Release();
        m_pCaptureClient = nullptr;
    }
    if (m_pAudioClient) {
        m_pAudioClient->Stop();
        m_pAudioClient->Release();
        m_pAudioClient = nullptr;
    }
    if (m_pDevice) {
        m_pDevice->Release();
        m_pDevice = nullptr;
    }
}

} // namespace recorder_core
