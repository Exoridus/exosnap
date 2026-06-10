#include "mf_aac_probe.h"

#include <functiondiscoverykeys_devpkey.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

// Link these in CMakeLists.txt:
//   ole32.lib  mfplat.lib  mf.lib  mfuuid.lib

namespace mf_aac_probe {

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static std::string HrToHex(HRESULT hr) {
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(hr));
    return buf;
}

std::string Probe::WideToUtf8(const wchar_t* wstr) {
    if (wstr == nullptr || wstr[0] == L'\0') {
        return "(unnamed)";
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        return "(unnamed)";
    }
    std::string result(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, result.data(), len, nullptr, nullptr);
    return result;
}

// Returns true if the mix format is 48000 Hz and 2 channels (any float or PCM format).
// Phase 07 no longer requires IEEE_FLOAT — conversion to PCM16 is done in the encode loop
// if the encoder requires PCM int16. Only sample rate and channel count are validated here.
bool Probe::IsFormatCompatible(const WAVEFORMATEX* pwfx) {
    if (pwfx == nullptr) {
        return false;
    }
    if (pwfx->nSamplesPerSec != kRequiredSampleRate) {
        return false;
    }
    if (pwfx->nChannels != kRequiredChannels) {
        return false;
    }
    return true;
}

// Converts interleaved Float32 samples to PCM int16.
// sampleCount = numFrames * numChannels
/*static*/
void Probe::ConvertFloat32ToPcm16(const float* src, int16_t* dst, size_t sampleCount) {
    for (size_t i = 0; i < sampleCount; ++i) {
        float clamped = std::clamp(src[i], -1.0f, 1.0f);
        dst[i] = static_cast<int16_t>(clamped * 32767.0f);
    }
}

// ---------------------------------------------------------------------------
// ctor / dtor
// ---------------------------------------------------------------------------

Probe::Probe() = default;

Probe::~Probe() {
    Cleanup();
}

void Probe::Cleanup() {
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
    if (m_pMFT) {
        m_pMFT->Release();
        m_pMFT = nullptr;
    }
    if (m_pActivate) {
        m_pActivate->Release();
        m_pActivate = nullptr;
    }
    if (m_mfStarted) {
        MFShutdown();
        m_mfStarted = false;
    }
}

// ---------------------------------------------------------------------------
// Phase 01 — MFStartup
// ---------------------------------------------------------------------------

bool Probe::Phase01_MFStartup() {
    fprintf(stdout, "[phase 01] MFStartup...\n");
    fflush(stdout);

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        fprintf(stderr, "[phase 01] FAIL: MFStartup 0x%08lX\n", static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }
    m_mfStarted = true;
    fprintf(stdout, "[phase 01] MFStartup OK\n");
    fflush(stdout);
    return true;
}

// ---------------------------------------------------------------------------
// Phase 02 — MFTEnumEx (with CLSID_AACMFTEncoder fallback)
// ---------------------------------------------------------------------------

bool Probe::Phase02_EnumerateMFT() {
    fprintf(stdout, "[phase 02] enumerating AAC encoder MFT...\n");
    fflush(stdout);

    MFT_REGISTER_TYPE_INFO inputTypeInfo = {MFMediaType_Audio, MFAudioFormat_Float};
    MFT_REGISTER_TYPE_INFO outputTypeInfo = {MFMediaType_Audio, MFAudioFormat_AAC};

    constexpr DWORD kEnumFlags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER;

    fprintf(stdout,
            "[phase 02] MFTEnumEx params: category=MFT_CATEGORY_AUDIO_ENCODER"
            " flags=0x%08lX input={Audio,Float} output={Audio,AAC}\n",
            static_cast<unsigned long>(kEnumFlags));
    fflush(stdout);

    IMFActivate** ppActivate = nullptr;
    UINT32 count = 0;

    HRESULT hr =
        MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, kEnumFlags, &inputTypeInfo, &outputTypeInfo, &ppActivate, &count);

    if (FAILED(hr)) {
        fprintf(stderr, "[phase 02] FAIL: MFTEnumEx %s\n", HrToHex(hr).c_str());
        fflush(stderr);
        return false;
    }

    if (count == 0) {
        if (ppActivate) {
            CoTaskMemFree(ppActivate);
            ppActivate = nullptr;
        }

        fprintf(stdout, "[phase 02] MFTEnumEx returned 0 — trying direct CLSID_AACMFTEncoder instantiation\n");
        fflush(stdout);

        hr = CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_pMFT));

        if (SUCCEEDED(hr)) {
            m_usedDirectClsid = true;
            fprintf(stdout, "[phase 02] AAC encoder instantiated directly via CLSID_AACMFTEncoder\n");
            fprintf(stdout, "[phase 02] PASS\n");
            fflush(stdout);
            return true;
        }

        fprintf(stderr, "[phase 02] FAIL: CoCreateInstance(CLSID_AACMFTEncoder) %s\n", HrToHex(hr).c_str());
        fprintf(stderr, "[phase 02] FAIL: Media Feature Pack is installed, but CLSID_AACMFTEncoder could "
                        "not be instantiated. On Windows N/ReviOS this may indicate removed or "
                        "unregistered codec components.\n");
        fflush(stderr);
        return false;
    }

    fprintf(stdout, "[phase 02] found %u AAC encoder MFT(s) — using first\n", count);
    fflush(stdout);

    // Keep first, release rest
    m_pActivate = ppActivate[0];
    for (UINT32 i = 1; i < count; ++i) {
        ppActivate[i]->Release();
    }
    CoTaskMemFree(ppActivate);

    return true;
}

// ---------------------------------------------------------------------------
// Phase 03 — instantiate MFT (skipped if direct CLSID path was used in phase 02)
// ---------------------------------------------------------------------------

bool Probe::Phase03_InstantiateMFT() {
    if (m_usedDirectClsid) {
        fprintf(stdout, "[phase 03] skipped — MFT already instantiated via CLSID_AACMFTEncoder\n");
        fflush(stdout);
        return true;
    }

    fprintf(stdout, "[phase 03] instantiating AAC encoder MFT...\n");
    fflush(stdout);

    HRESULT hr = m_pActivate->ActivateObject(IID_PPV_ARGS(&m_pMFT));
    if (FAILED(hr)) {
        fprintf(stderr, "[phase 03] FAIL: ActivateObject(IMFTransform) %s\n", HrToHex(hr).c_str());
        fflush(stderr);
        return false;
    }

    fprintf(stdout, "[phase 03] MFT instantiated OK\n");
    fflush(stdout);
    return true;
}

// ---------------------------------------------------------------------------
// Phase 04 — set input type (with GetInputAvailableType negotiation)
// ---------------------------------------------------------------------------

// Builds a fully-specified audio media type with all required attributes.
static HRESULT BuildAudioMediaType(const GUID& subtype, UINT32 sampleRate, UINT32 channels, UINT32 bitsPerSample,
                                   IMFMediaType** ppType) {
    *ppType = nullptr;
    IMFMediaType* pType = nullptr;
    HRESULT hr = MFCreateMediaType(&pType);
    if (FAILED(hr))
        return hr;

    UINT32 blockAlign = channels * (bitsPerSample / 8);
    UINT32 avgBytesPerSec = sampleRate * blockAlign;
    UINT32 channelMask = 0x3; // SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT

    bool ok = true;
    ok = ok && SUCCEEDED(pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    ok = ok && SUCCEEDED(pType->SetGUID(MF_MT_SUBTYPE, subtype));
    ok = ok && SUCCEEDED(pType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels));
    ok = ok && SUCCEEDED(pType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate));
    ok = ok && SUCCEEDED(pType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, bitsPerSample));
    ok = ok && SUCCEEDED(pType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign));
    ok = ok && SUCCEEDED(pType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, avgBytesPerSec));
    ok = ok && SUCCEEDED(pType->SetUINT32(MF_MT_AUDIO_CHANNEL_MASK, channelMask));

    if (!ok) {
        pType->Release();
        return E_FAIL;
    }

    *ppType = pType;
    return S_OK;
}

bool Probe::Phase04_SetInputType() {
    fprintf(stdout, "[phase 04] negotiating input media type via GetInputAvailableType...\n");
    fflush(stdout);

    // -----------------------------------------------------------------------
    // Step 1 — enumerate and print all available input types
    // -----------------------------------------------------------------------

    // Candidate to carry the best matching type found from GetInputAvailableType
    struct Candidate {
        GUID subtype = {};
        UINT32 sampleRate = 0;
        UINT32 channels = 0;
        UINT32 bitsPerSample = 0;
        UINT32 blockAlign = 0;
        UINT32 avgBytesPerSec = 0;
        UINT32 channelMask = 0;
        int priority = INT_MAX; // lower is better
    };

    Candidate bestCandidate;

    for (DWORD i = 0;; ++i) {
        IMFMediaType* pCand = nullptr;
        HRESULT hr = m_pMFT->GetInputAvailableType(0, i, &pCand);
        if (hr == MF_E_NO_MORE_TYPES)
            break;
        if (FAILED(hr)) {
            fprintf(stdout, "[phase 04] GetInputAvailableType[%lu] failed %s — stopping enumeration\n",
                    static_cast<unsigned long>(i), HrToHex(hr).c_str());
            fflush(stdout);
            break;
        }

        GUID subtype = {};
        UINT32 sampleRate = 0;
        UINT32 channels = 0;
        UINT32 bitsPerSample = 0;
        UINT32 blockAlign = 0;
        UINT32 avgBytesPerSec = 0;
        UINT32 channelMask = 0;

        pCand->GetGUID(MF_MT_SUBTYPE, &subtype);
        pCand->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate);
        pCand->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        pCand->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bitsPerSample);
        pCand->GetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, &blockAlign);
        pCand->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &avgBytesPerSec);
        pCand->GetUINT32(MF_MT_AUDIO_CHANNEL_MASK, &channelMask);
        pCand->Release();
        pCand = nullptr;

        // Format subtype as human-readable tag
        const char* subtypeTag = "(other)";
        if (subtype == MFAudioFormat_PCM)
            subtypeTag = "PCM";
        if (subtype == MFAudioFormat_Float)
            subtypeTag = "Float";

        fprintf(stdout,
                "[phase 04]   candidate[%lu]: subtype=%s sampleRate=%u ch=%u bps=%u "
                "blockAlign=%u avgBytes/s=%u channelMask=0x%X\n",
                static_cast<unsigned long>(i), subtypeTag, sampleRate, channels, bitsPerSample, blockAlign,
                avgBytesPerSec, channelMask);
        fflush(stdout);

        // -----------------------------------------------------------------------
        // Step 2 — assign priority to this candidate
        // Preference order:
        //   A. PCM 48000 Hz stereo 16-bit  (priority 1)
        //   B. PCM 48000 Hz stereo 32-bit  (priority 2)
        //   C. Float 48000 Hz stereo       (priority 3)
        //   D. Any PCM/Float 48000 Hz stereo (priority 4)
        //   (lower priority number = better)
        // -----------------------------------------------------------------------

        if (sampleRate != kRequiredSampleRate || channels != kRequiredChannels) {
            continue; // wrong sample rate or channels — skip entirely
        }

        int priority = INT_MAX;

        if (subtype == MFAudioFormat_PCM && bitsPerSample == 16) {
            priority = 1; // A: PCM 16-bit — best
        } else if (subtype == MFAudioFormat_PCM && bitsPerSample == 32) {
            priority = 2; // B: PCM 32-bit
        } else if (subtype == MFAudioFormat_Float && bitsPerSample == 32) {
            priority = 3; // C: Float 32-bit
        } else if (subtype == MFAudioFormat_PCM || subtype == MFAudioFormat_Float) {
            priority = 4; // D: any PCM or Float at correct rate/channels
        }

        if (priority < bestCandidate.priority) {
            bestCandidate.subtype = subtype;
            bestCandidate.sampleRate = sampleRate;
            bestCandidate.channels = channels;
            bestCandidate.bitsPerSample = bitsPerSample;
            bestCandidate.blockAlign = blockAlign;
            bestCandidate.avgBytesPerSec = avgBytesPerSec;
            bestCandidate.channelMask = channelMask;
            bestCandidate.priority = priority;
        }
    }

    // -----------------------------------------------------------------------
    // Step 3 — build and try candidates in priority order
    // If GetInputAvailableType found nothing usable (priority still INT_MAX),
    // fall through to the manually constructed types below (Step E).
    // -----------------------------------------------------------------------

    struct TryEntry {
        GUID subtype;
        UINT32 bitsPerSample;
        const char* label;
    };

    // Always try in this order regardless of what enumeration found:
    //   E1: PCM 16-bit  (priority A)
    //   E2: PCM 32-bit  (priority B)
    //   E3: Float 32-bit (priority C)
    // but if enumeration found a best candidate, try it first.

    // Build ordered try list
    std::vector<TryEntry> tryList;

    // If enumeration gave us a result, insert it at the front
    if (bestCandidate.priority != INT_MAX) {
        const char* subtypeLabel = (bestCandidate.subtype == MFAudioFormat_PCM)     ? "PCM"
                                   : (bestCandidate.subtype == MFAudioFormat_Float) ? "Float"
                                                                                    : "other";
        fprintf(stdout, "[phase 04] best candidate from enumeration: subtype=%s %u bps (priority %d)\n", subtypeLabel,
                bestCandidate.bitsPerSample, bestCandidate.priority);
        fflush(stdout);
        tryList.push_back({bestCandidate.subtype, bestCandidate.bitsPerSample, subtypeLabel});
    }

    // Append fallback types that are not already in the list
    auto alreadyInList = [&](const GUID& sub, UINT32 bps) {
        for (const auto& e : tryList) {
            if (e.subtype == sub && e.bitsPerSample == bps)
                return true;
        }
        return false;
    };
    if (!alreadyInList(MFAudioFormat_PCM, 16))
        tryList.push_back({MFAudioFormat_PCM, 16, "PCM-16"});
    if (!alreadyInList(MFAudioFormat_PCM, 32))
        tryList.push_back({MFAudioFormat_PCM, 32, "PCM-32"});
    if (!alreadyInList(MFAudioFormat_Float, 32))
        tryList.push_back({MFAudioFormat_Float, 32, "Float-32"});

    // -----------------------------------------------------------------------
    // Step 3 (continued) — attempt SetInputType for each candidate in order
    // -----------------------------------------------------------------------

    for (const auto& entry : tryList) {
        fprintf(stdout, "[phase 04] trying SetInputType: %s %u bps, %u Hz, %u ch...\n", entry.label,
                entry.bitsPerSample, kRequiredSampleRate, kRequiredChannels);
        fflush(stdout);

        IMFMediaType* pType = nullptr;
        HRESULT hr =
            BuildAudioMediaType(entry.subtype, kRequiredSampleRate, kRequiredChannels, entry.bitsPerSample, &pType);

        if (FAILED(hr)) {
            fprintf(stderr, "[phase 04] WARN: BuildAudioMediaType failed %s — skipping\n", HrToHex(hr).c_str());
            fflush(stderr);
            continue;
        }

        hr = m_pMFT->SetInputType(0, pType, 0);
        pType->Release();
        pType = nullptr;

        if (SUCCEEDED(hr)) {
            // ---------------------------------------------------------------
            // Step 4 — store selected format in member variables
            // ---------------------------------------------------------------
            m_inputSubtype = entry.subtype;
            m_inputBitsPerSample = entry.bitsPerSample;
            m_inputBlockAlign = kRequiredChannels * (entry.bitsPerSample / 8);
            m_inputAvgBytesPerSec = kRequiredSampleRate * m_inputBlockAlign;

            fprintf(stdout,
                    "[phase 04] SetInputType OK: %s %u bps, %u Hz, %u ch "
                    "(blockAlign=%u avgBytes/s=%u)\n",
                    entry.label, m_inputBitsPerSample, kRequiredSampleRate, kRequiredChannels, m_inputBlockAlign,
                    m_inputAvgBytesPerSec);
            fflush(stdout);
            return true;
        }

        fprintf(stdout, "[phase 04] SetInputType %s %u bps failed %s — trying next\n", entry.label, entry.bitsPerSample,
                HrToHex(hr).c_str());
        fflush(stdout);
    }

    // All options exhausted
    fprintf(stderr, "[phase 04] FAIL: all input type candidates rejected by the encoder\n");
    fflush(stderr);
    return false;
}

// ---------------------------------------------------------------------------
// Phase 05 — set output type
// ---------------------------------------------------------------------------

bool Probe::Phase05_SetOutputType() {
    fprintf(stdout, "[phase 05] setting output media type (AAC-LC, 48000 Hz, 2 ch, 192 kbps, ADTS)...\n");
    fflush(stdout);

    IMFMediaType* pOutputType = nullptr;
    HRESULT hr = MFCreateMediaType(&pOutputType);
    if (FAILED(hr)) {
        fprintf(stderr, "[phase 05] FAIL: MFCreateMediaType 0x%08lX\n", static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }

    auto release_on_exit = [&]() { pOutputType->Release(); };

    bool ok = true;
    ok = ok && SUCCEEDED(pOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    ok = ok && SUCCEEDED(pOutputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC));
    ok = ok && SUCCEEDED(pOutputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRequiredSampleRate));
    ok = ok && SUCCEEDED(pOutputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kRequiredChannels));
    ok = ok && SUCCEEDED(pOutputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
    ok = ok && SUCCEEDED(pOutputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kAacAvgBytesPerSec));
    ok = ok && SUCCEEDED(pOutputType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, kAdtsPayloadType));
    ok = ok && SUCCEEDED(pOutputType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, kAacProfileLevel));

    if (!ok) {
        fprintf(stderr, "[phase 05] FAIL: setting output media type attributes\n");
        fflush(stderr);
        release_on_exit();
        return false;
    }

    hr = m_pMFT->SetOutputType(0, pOutputType, 0);
    release_on_exit();

    if (FAILED(hr)) {
        fprintf(stderr, "[phase 05] FAIL: SetOutputType 0x%08lX\n", static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }

    fprintf(stdout, "[phase 05] output type set OK\n");
    fflush(stdout);
    return true;
}

// ---------------------------------------------------------------------------
// Phase 06 — query stream IDs and output buffer requirements
// ---------------------------------------------------------------------------

bool Probe::Phase06_QueryStreamInfo() {
    fprintf(stdout, "[phase 06] querying stream info...\n");
    fflush(stdout);

    MFT_OUTPUT_STREAM_INFO streamInfo{};
    HRESULT hr = m_pMFT->GetOutputStreamInfo(0, &streamInfo);
    if (FAILED(hr)) {
        fprintf(stderr, "[phase 06] FAIL: GetOutputStreamInfo 0x%08lX\n", static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }

    fprintf(stdout, "[phase 06] output stream info: cbSize=%u flags=0x%08lX alignment=%u\n", streamInfo.cbSize,
            static_cast<unsigned long>(streamInfo.dwFlags), streamInfo.cbAlignment);
    fflush(stdout);
    return true;
}

// ---------------------------------------------------------------------------
// Phase 07 — open default render endpoint in WASAPI loopback
// ---------------------------------------------------------------------------

bool Probe::Phase07_OpenWasapiLoopback() {
    fprintf(stdout, "[phase 07] opening default render endpoint in WASAPI loopback...\n");
    fflush(stdout);

    HRESULT hr = S_OK;

    // Get device enumerator
    IMMDeviceEnumerator* pEnumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pEnumerator));
    if (FAILED(hr)) {
        fprintf(stderr, "[phase 07] FAIL: CoCreateInstance(MMDeviceEnumerator) 0x%08lX\n",
                static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }

    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_pDevice);
    pEnumerator->Release();
    if (FAILED(hr)) {
        fprintf(stderr, "[phase 07] FAIL: GetDefaultAudioEndpoint(eRender) 0x%08lX\n", static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }

    // Read friendly name
    IPropertyStore* pProps = nullptr;
    hr = m_pDevice->OpenPropertyStore(STGM_READ, &pProps);
    if (SUCCEEDED(hr)) {
        PROPVARIANT varName;
        PropVariantInit(&varName);
        if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.vt == VT_LPWSTR) {
            m_endpointName = WideToUtf8(varName.pwszVal);
        }
        PropVariantClear(&varName);
        pProps->Release();
    }
    if (m_endpointName.empty()) {
        m_endpointName = "(unnamed)";
    }
    fprintf(stdout, "[phase 07] endpoint: %s\n", m_endpointName.c_str());
    fflush(stdout);

    // Activate IAudioClient
    hr = m_pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&m_pAudioClient));
    if (FAILED(hr)) {
        fprintf(stderr, "[phase 07] FAIL: Activate(IAudioClient) 0x%08lX\n", static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }

    // Get and validate mix format
    WAVEFORMATEX* pwfx = nullptr;
    hr = m_pAudioClient->GetMixFormat(&pwfx);
    if (FAILED(hr) || pwfx == nullptr) {
        fprintf(stderr, "[phase 07] FAIL: GetMixFormat 0x%08lX\n", static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }

    fprintf(stdout, "[phase 07] mix format: tag=0x%04X %u Hz, %u ch, %u-bit, block=%u\n", pwfx->wFormatTag,
            pwfx->nSamplesPerSec, pwfx->nChannels, pwfx->wBitsPerSample, pwfx->nBlockAlign);
    fflush(stdout);

    if (!IsFormatCompatible(pwfx)) {
        fprintf(stderr,
                "[phase 07] FAIL: mix format does not meet requirements (need 48000 Hz, 2 ch).\n"
                "  actual: tag=0x%04X %u Hz, %u ch, %u-bit\n"
                "  required: %u Hz, %u ch (any sample format — Float32 is converted to PCM16 if needed)\n",
                pwfx->wFormatTag, pwfx->nSamplesPerSec, pwfx->nChannels, pwfx->wBitsPerSample, kRequiredSampleRate,
                kRequiredChannels);
        fflush(stderr);
        CoTaskMemFree(pwfx);
        return false;
    }

    // Initialize loopback with 200ms buffer
    hr = m_pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, kHnsBuffer, 0, pwfx,
                                    nullptr);
    CoTaskMemFree(pwfx);

    if (FAILED(hr)) {
        fprintf(stderr, "[phase 07] FAIL: IAudioClient::Initialize(LOOPBACK) 0x%08lX\n",
                static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }

    // Get capture client
    hr = m_pAudioClient->GetService(IID_PPV_ARGS(&m_pCaptureClient));
    if (FAILED(hr)) {
        fprintf(stderr, "[phase 07] FAIL: GetService(IAudioCaptureClient) 0x%08lX\n", static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }

    // Start the stream
    hr = m_pAudioClient->Start();
    if (FAILED(hr)) {
        fprintf(stderr, "[phase 07] FAIL: IAudioClient::Start 0x%08lX\n", static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }

    fprintf(stdout, "[phase 07] WASAPI loopback started OK\n");
    fflush(stdout);
    return true;
}

// ---------------------------------------------------------------------------
// Phase 08 + 09 — begin streaming messages
// ---------------------------------------------------------------------------

bool Probe::Phase08_09_BeginStreaming() {
    fprintf(stdout, "[phase 08/09] sending MFT begin-streaming messages...\n");
    fflush(stdout);

    HRESULT hr = m_pMFT->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[phase 08] FAIL: MFT_MESSAGE_NOTIFY_BEGIN_STREAMING 0x%08lX\n",
                static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }

    hr = m_pMFT->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[phase 09] FAIL: MFT_MESSAGE_NOTIFY_START_OF_STREAM 0x%08lX\n",
                static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }

    fprintf(stdout, "[phase 08/09] streaming messages sent OK\n");
    fflush(stdout);
    return true;
}

// ---------------------------------------------------------------------------
// DrainOutput — collect encoded packets from MFT
// ---------------------------------------------------------------------------

void Probe::DrainOutput() {
    // Query output stream info once to know if the MFT provides its own samples.
    // MFT_OUTPUT_STREAM_PROVIDES_SAMPLES (0x100): MFT allocates the output sample.
    // If that flag is NOT set, we must supply a sample+buffer before each ProcessOutput call.
    MFT_OUTPUT_STREAM_INFO streamInfo{};
    bool mftProvidesSamples = false;
    if (SUCCEEDED(m_pMFT->GetOutputStreamInfo(0, &streamInfo))) {
        mftProvidesSamples = (streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
    }
    // Use the reported cbSize for the output buffer; fall back to a safe default.
    DWORD outBufSize = (streamInfo.cbSize > 0) ? streamInfo.cbSize : 8192;

    while (true) {
        MFT_OUTPUT_DATA_BUFFER outBuf{};
        outBuf.dwStreamID = 0;
        outBuf.pSample = nullptr;
        outBuf.dwStatus = 0;
        outBuf.pEvents = nullptr;

        // If the MFT does not provide its own samples, allocate one for it.
        if (!mftProvidesSamples) {
            IMFMediaBuffer* pOutMFBuf = nullptr;
            if (FAILED(MFCreateMemoryBuffer(outBufSize, &pOutMFBuf))) {
                break; // Can't allocate — give up silently
            }
            IMFSample* pOutSample = nullptr;
            if (FAILED(MFCreateSample(&pOutSample))) {
                pOutMFBuf->Release();
                break;
            }
            pOutSample->AddBuffer(pOutMFBuf);
            pOutMFBuf->Release();
            outBuf.pSample = pOutSample; // caller owns this ref
        }

        DWORD status = 0;
        HRESULT hr = m_pMFT->ProcessOutput(0, 1, &outBuf, &status);

        if (outBuf.pEvents) {
            outBuf.pEvents->Release();
            outBuf.pEvents = nullptr;
        }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (outBuf.pSample) {
                outBuf.pSample->Release();
            }
            break;
        }
        if (FAILED(hr)) {
            fprintf(stderr, "[drain] WARN: ProcessOutput failed 0x%08lX\n", static_cast<unsigned long>(hr));
            fflush(stderr);
            if (outBuf.pSample) {
                outBuf.pSample->Release();
            }
            break;
        }

        if (outBuf.pSample) {
            // Extract bytes from all buffers in the sample
            DWORD bufCount = 0;
            if (SUCCEEDED(outBuf.pSample->GetBufferCount(&bufCount))) {
                for (DWORD b = 0; b < bufCount; ++b) {
                    IMFMediaBuffer* pBuf = nullptr;
                    if (SUCCEEDED(outBuf.pSample->GetBufferByIndex(b, &pBuf))) {
                        BYTE* pData = nullptr;
                        DWORD cbCurrent = 0;
                        if (SUCCEEDED(pBuf->Lock(&pData, nullptr, &cbCurrent)) && cbCurrent > 0) {
                            size_t oldSize = m_aacPackets.size();
                            m_aacPackets.resize(oldSize + cbCurrent);
                            std::memcpy(m_aacPackets.data() + oldSize, pData, cbCurrent);
                            pBuf->Unlock();
                        }
                        pBuf->Release();
                    }
                }
            }
            outBuf.pSample->Release();
            outBuf.pSample = nullptr;
        }
    }
}

// ---------------------------------------------------------------------------
// Phase 10 — 30-second encode loop
// ---------------------------------------------------------------------------

bool Probe::Phase10_EncodeLoop() {
    fprintf(stdout, "[phase 10] starting 30-second encode loop...\n");
    fflush(stdout);

    auto startTime = std::chrono::steady_clock::now();
    auto endTime = startTime + std::chrono::seconds(kDurationSec);

    LONGLONG sampleTime = 0; // in 100ns units

    bool deviceInvalidated = false;

    while (true) {
        auto now = std::chrono::steady_clock::now();
        if (now >= endTime) {
            break;
        }

        UINT32 numFrames = 0;
        HRESULT hr = m_pCaptureClient->GetNextPacketSize(&numFrames);

        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            fprintf(stderr, "[phase 10] WARN: AUDCLNT_E_DEVICE_INVALIDATED — stopping loop early\n");
            fflush(stderr);
            deviceInvalidated = true;
            break;
        }
        if (FAILED(hr)) {
            fprintf(stderr, "[phase 10] WARN: GetNextPacketSize failed 0x%08lX — stopping loop\n",
                    static_cast<unsigned long>(hr));
            fflush(stderr);
            break;
        }

        if (numFrames == 0) {
            Sleep(kPollSleepMs);
            continue;
        }

        BYTE* pData = nullptr;
        DWORD captureFlags = 0;
        UINT64 devicePosition = 0;
        UINT64 qpcPosition = 0;

        hr = m_pCaptureClient->GetBuffer(&pData, &numFrames, &captureFlags, &devicePosition, &qpcPosition);

        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            fprintf(stderr, "[phase 10] WARN: AUDCLNT_E_DEVICE_INVALIDATED during GetBuffer\n");
            fflush(stderr);
            deviceInvalidated = true;
            break;
        }
        if (FAILED(hr)) {
            fprintf(stderr, "[phase 10] WARN: GetBuffer failed 0x%08lX\n", static_cast<unsigned long>(hr));
            fflush(stderr);
            break;
        }

        if (captureFlags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
            fprintf(stderr, "[phase 10] WARN: DATA_DISCONTINUITY (discontinuity #%llu)\n",
                    static_cast<unsigned long long>(m_discontinuities + 1));
            fflush(stderr);
            ++m_discontinuities;
        }

        // Compute timing for this packet
        LONGLONG sampleDuration =
            static_cast<LONGLONG>(numFrames) * 10000000LL / static_cast<LONGLONG>(kRequiredSampleRate);

        // Determine whether we need Float32 -> PCM16 conversion.
        // WASAPI loopback delivers Float32; if the encoder requires PCM int16, convert.
        const bool needPcm16Conversion = (m_inputSubtype == MFAudioFormat_PCM && m_inputBitsPerSample == 16);

        UINT32 dataBytes = numFrames * m_inputBlockAlign;

        // Build IMFSample
        IMFMediaBuffer* pMFBuf = nullptr;
        hr = MFCreateMemoryBuffer(dataBytes, &pMFBuf);
        if (FAILED(hr)) {
            fprintf(stderr, "[phase 10] FAIL: MFCreateMemoryBuffer 0x%08lX\n", static_cast<unsigned long>(hr));
            fflush(stderr);
            m_pCaptureClient->ReleaseBuffer(numFrames);
            return false;
        }

        BYTE* pDst = nullptr;
        pMFBuf->Lock(&pDst, nullptr, nullptr);

        if (captureFlags & AUDCLNT_BUFFERFLAGS_SILENT) {
            // Silent flag — write zeroed PCM to maintain timing
            std::memset(pDst, 0, dataBytes);
        } else if (needPcm16Conversion) {
            // Step 5 — convert WASAPI Float32 loopback data to PCM int16
            size_t sampleCount = static_cast<size_t>(numFrames) * kRequiredChannels;
            ConvertFloat32ToPcm16(reinterpret_cast<const float*>(pData), reinterpret_cast<int16_t*>(pDst), sampleCount);
        } else {
            std::memcpy(pDst, pData, dataBytes);
        }
        pMFBuf->Unlock();
        pMFBuf->SetCurrentLength(dataBytes);

        IMFSample* pSample = nullptr;
        hr = MFCreateSample(&pSample);
        if (FAILED(hr)) {
            fprintf(stderr, "[phase 10] FAIL: MFCreateSample 0x%08lX\n", static_cast<unsigned long>(hr));
            fflush(stderr);
            pMFBuf->Release();
            m_pCaptureClient->ReleaseBuffer(numFrames);
            return false;
        }

        pSample->AddBuffer(pMFBuf);
        pMFBuf->Release();
        pMFBuf = nullptr;

        pSample->SetSampleTime(sampleTime);
        pSample->SetSampleDuration(sampleDuration);
        sampleTime += sampleDuration;

        // Release WASAPI buffer before ProcessInput
        m_pCaptureClient->ReleaseBuffer(numFrames);
        m_capturedFrames += numFrames;

        // Push sample to encoder
        hr = m_pMFT->ProcessInput(0, pSample, 0);
        pSample->Release();
        pSample = nullptr;

        if (hr == MF_E_NOTACCEPTING) {
            // Drain first, then retry is not required by spec —
            // just drain and move on to next packet
            DrainOutput();
        } else if (FAILED(hr)) {
            fprintf(stderr, "[phase 10] WARN: ProcessInput failed 0x%08lX\n", static_cast<unsigned long>(hr));
            fflush(stderr);
        } else {
            // Normal path: drain whatever is ready
            DrainOutput();
        }
    }

    auto actualEnd = std::chrono::steady_clock::now();
    m_elapsedSec = std::chrono::duration<double>(actualEnd - startTime).count();

    fprintf(stdout, "[phase 10] encode loop done — captured %llu frames in %.2f s\n",
            static_cast<unsigned long long>(m_capturedFrames), m_elapsedSec);
    fflush(stdout);

    (void)deviceInvalidated; // documented fallthrough to EOS drain
    return true;
}

// ---------------------------------------------------------------------------
// Phase 11 — EOS drain
// ---------------------------------------------------------------------------

bool Probe::Phase11_EosDrain() {
    fprintf(stdout, "[phase 11] EOS drain...\n");
    fflush(stdout);

    HRESULT hr = m_pMFT->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[phase 11] WARN: MFT_MESSAGE_NOTIFY_END_OF_STREAM 0x%08lX\n", static_cast<unsigned long>(hr));
        fflush(stderr);
        // Non-fatal — still try DRAIN
    }

    hr = m_pMFT->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[phase 11] WARN: MFT_MESSAGE_COMMAND_DRAIN 0x%08lX\n", static_cast<unsigned long>(hr));
        fflush(stderr);
        // Non-fatal — still drain output
    }

    DrainOutput();

    fprintf(stdout, "[phase 11] EOS drain done — total encoded bytes: %zu\n", m_aacPackets.size());
    fflush(stdout);
    return true;
}

// ---------------------------------------------------------------------------
// Phase 12 — write ADTS file
// ---------------------------------------------------------------------------

bool Probe::Phase12_WriteAdtsFile() {
    fprintf(stdout, "[phase 12] writing ADTS .aac file...\n");
    fflush(stdout);

    // Create output directory
    BOOL created = CreateDirectoryW(L"probe_mf_aac_encode_output", nullptr);
    if (!created && GetLastError() != ERROR_ALREADY_EXISTS) {
        fprintf(stderr, "[phase 12] FAIL: CreateDirectoryW error %lu\n", GetLastError());
        fflush(stderr);
        return false;
    }

    const char* outPath = "probe_mf_aac_encode_output\\wasapi_loopback_aac.aac";
    FILE* fp = nullptr;
    errno_t fopenErr = fopen_s(&fp, outPath, "wb");
    if (fopenErr != 0 || fp == nullptr) {
        fprintf(stderr, "[phase 12] FAIL: fopen_s('%s') errno=%d\n", outPath, static_cast<int>(fopenErr));
        fflush(stderr);
        return false;
    }

    size_t written = fwrite(m_aacPackets.data(), 1, m_aacPackets.size(), fp);
    fclose(fp);

    if (written != m_aacPackets.size()) {
        fprintf(stderr, "[phase 12] FAIL: fwrite wrote %zu of %zu bytes\n", written, m_aacPackets.size());
        fflush(stderr);
        return false;
    }

    fprintf(stdout, "[phase 12] wrote %zu bytes to '%s'\n", written, outPath);
    fflush(stdout);
    return true;
}

// ---------------------------------------------------------------------------
// Phase 13 — verify output file size
// ---------------------------------------------------------------------------

bool Probe::Phase13_VerifyOutput() {
    fprintf(stdout, "[phase 13] verifying output file...\n");
    fflush(stdout);

    WIN32_FILE_ATTRIBUTE_DATA fileInfo{};
    BOOL ok =
        GetFileAttributesExW(L"probe_mf_aac_encode_output\\wasapi_loopback_aac.aac", GetFileExInfoStandard, &fileInfo);
    if (!ok) {
        fprintf(stderr, "[phase 13] FAIL: GetFileAttributesExW error %lu\n", GetLastError());
        fflush(stderr);
        return false;
    }

    ULARGE_INTEGER fileSize;
    fileSize.HighPart = fileInfo.nFileSizeHigh;
    fileSize.LowPart = fileInfo.nFileSizeLow;
    uint64_t sz = fileSize.QuadPart;

    if (sz <= 4096) {
        fprintf(stderr, "[phase 13] FAIL: file too small — size: %llu bytes (expected > 4096)\n",
                static_cast<unsigned long long>(sz));
        fflush(stderr);
        return false;
    }

    fprintf(stdout, "[phase 13] size: %llu bytes (OK)\n", static_cast<unsigned long long>(sz));
    fflush(stdout);
    return true;
}

// ---------------------------------------------------------------------------
// Run — execute all phases in order
// ---------------------------------------------------------------------------

bool Probe::Run() {
    if (!Phase01_MFStartup())
        return false;
    if (!Phase02_EnumerateMFT())
        return false;
    if (!Phase03_InstantiateMFT())
        return false;
    if (!Phase04_SetInputType())
        return false;
    if (!Phase05_SetOutputType())
        return false;
    if (!Phase06_QueryStreamInfo())
        return false;
    if (!Phase07_OpenWasapiLoopback())
        return false;
    if (!Phase08_09_BeginStreaming())
        return false;
    if (!Phase10_EncodeLoop())
        return false;
    if (!Phase11_EosDrain())
        return false;
    if (!Phase12_WriteAdtsFile())
        return false;
    if (!Phase13_VerifyOutput())
        return false;

    // Print summary metrics
    fprintf(stdout, "\n=== MF AAC ENCODE PROBE SUMMARY ===\n");
    const char* inputSubtypeLabel = (m_inputSubtype == MFAudioFormat_PCM)     ? "PCM"
                                    : (m_inputSubtype == MFAudioFormat_Float) ? "Float32"
                                                                              : "unknown";
    fprintf(stdout, "  endpoint        : %s\n", m_endpointName.c_str());
    fprintf(stdout, "  input format    : %u Hz, %u ch, %s %u-bit\n", kRequiredSampleRate, kRequiredChannels,
            inputSubtypeLabel, m_inputBitsPerSample);
    fprintf(stdout, "  captured frames : %llu\n", static_cast<unsigned long long>(m_capturedFrames));
    fprintf(stdout, "  encoded bytes   : %zu\n", m_aacPackets.size());
    fprintf(stdout, "  elapsed         : %.1f s\n", m_elapsedSec);
    fprintf(stdout, "  discontinuities : %llu\n", static_cast<unsigned long long>(m_discontinuities));
    fprintf(stdout, "  output path     : probe_mf_aac_encode_output\\wasapi_loopback_aac.aac\n");

    WIN32_FILE_ATTRIBUTE_DATA fileInfo{};
    if (GetFileAttributesExW(L"probe_mf_aac_encode_output\\wasapi_loopback_aac.aac", GetFileExInfoStandard,
                             &fileInfo)) {
        ULARGE_INTEGER fileSize;
        fileSize.HighPart = fileInfo.nFileSizeHigh;
        fileSize.LowPart = fileInfo.nFileSizeLow;
        fprintf(stdout, "  file size       : %llu bytes\n", static_cast<unsigned long long>(fileSize.QuadPart));
    }
    fflush(stdout);

    return true;
}

} // namespace mf_aac_probe
