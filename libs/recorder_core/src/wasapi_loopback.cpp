#include "wasapi_loopback.h"

// WideToUtf8 is declared in wgc_capture.h; include it directly
#include "wgc_capture.h"

#include <functiondiscoverykeys_devpkey.h>
#include <initguid.h>

#include <cstdio>

namespace recorder_core {

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

    WAVEFORMATEX* pwfx = nullptr;
    hr = m_pAudioClient->GetMixFormat(&pwfx);
    if (FAILED(hr) || !pwfx) {
        char buf[80];
        snprintf(buf, sizeof(buf), "GetMixFormat 0x%08lX", static_cast<unsigned long>(hr));
        out_error = buf;
        return false;
    }

    if (pwfx->nSamplesPerSec != kRequiredSampleRate || pwfx->nChannels != kRequiredChannels) {
        char buf[128];
        snprintf(buf, sizeof(buf), "mix format mismatch: %u Hz %u ch (need %u Hz 2 ch)", pwfx->nSamplesPerSec,
                 pwfx->nChannels, kRequiredSampleRate);
        CoTaskMemFree(pwfx);
        out_error = buf;
        return false;
    }

    hr = m_pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, kHnsBuffer, 0, pwfx,
                                    nullptr);
    CoTaskMemFree(pwfx);
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
    if (FAILED(m_pCaptureClient->GetNextPacketSize(&n)))
        return 0;
    return n;
}

// ---------------------------------------------------------------------------
// GetNextPacket
// ---------------------------------------------------------------------------

bool WasapiLoopback::GetNextPacket(BYTE** out_data, UINT32* out_num_frames, DWORD* out_capture_flags,
                                   bool* out_silent) {
    if (!m_pCaptureClient)
        return false;
    UINT64 devicePos = 0, qpcPos = 0;
    HRESULT hr = m_pCaptureClient->GetBuffer(out_data, out_num_frames, out_capture_flags, &devicePos, &qpcPos);
    if (FAILED(hr) || *out_num_frames == 0)
        return false;
    if (out_silent)
        *out_silent = (*out_capture_flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
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
