#include "mf_aac_encoder.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace recorder_core {

// ---------------------------------------------------------------------------
// Float32 -> PCM16 utility — lifted from M2.8 probe
// ---------------------------------------------------------------------------

void ConvertFloat32ToPcm16(const float* src, int16_t* dst, size_t sample_count) {
    for (size_t i = 0; i < sample_count; ++i) {
        float clamped = std::clamp(src[i], -1.0f, 1.0f);
        dst[i] = static_cast<int16_t>(clamped * 32767.0f);
    }
}

// ---------------------------------------------------------------------------
// Internal helper: build PCM/Float audio media type
// ---------------------------------------------------------------------------

namespace {

HRESULT BuildAudioMediaType(
    const GUID& subtype,
    UINT32 sample_rate, UINT32 channels, UINT32 bits_per_sample,
    IMFMediaType** pp_type)
{
    *pp_type = nullptr;
    IMFMediaType* pType = nullptr;
    HRESULT hr = MFCreateMediaType(&pType);
    if (FAILED(hr)) return hr;

    UINT32 blockAlign     = channels * (bits_per_sample / 8);
    UINT32 avgBytesPerSec = sample_rate * blockAlign;
    UINT32 channelMask    = 0x3; // SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT

    bool ok = true;
    ok = ok && SUCCEEDED(pType->SetGUID  (MF_MT_MAJOR_TYPE,               MFMediaType_Audio));
    ok = ok && SUCCEEDED(pType->SetGUID  (MF_MT_SUBTYPE,                  subtype));
    ok = ok && SUCCEEDED(pType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,       channels));
    ok = ok && SUCCEEDED(pType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate));
    ok = ok && SUCCEEDED(pType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,    bits_per_sample));
    ok = ok && SUCCEEDED(pType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,    blockAlign));
    ok = ok && SUCCEEDED(pType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, avgBytesPerSec));
    ok = ok && SUCCEEDED(pType->SetUINT32(MF_MT_AUDIO_CHANNEL_MASK,       channelMask));

    if (!ok) { pType->Release(); return E_FAIL; }
    *pp_type = pType;
    return S_OK;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

MfAacEncoder::~MfAacEncoder() {
    Shutdown();
}

// ---------------------------------------------------------------------------
// Init — lifted from M2.8 probe Phase10
// ---------------------------------------------------------------------------

bool MfAacEncoder::Init(uint32_t sample_rate, uint32_t channels, std::string& out_error) {
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "MFStartup failed 0x%08lX", static_cast<unsigned long>(hr));
        out_error = buf;
        return false;
    }
    m_mfStarted = true;

    // Step 1: MFTEnumEx
    MFT_REGISTER_TYPE_INFO inType  = { MFMediaType_Audio, MFAudioFormat_Float };
    MFT_REGISTER_TYPE_INFO outType = { MFMediaType_Audio, MFAudioFormat_AAC   };
    constexpr DWORD kEnumFlags =
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER;

    IMFActivate** ppActivate = nullptr;
    UINT32 count = 0;
    hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, kEnumFlags,
                   &inType, &outType, &ppActivate, &count);
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "MFTEnumEx failed 0x%08lX", static_cast<unsigned long>(hr));
        out_error = buf;
        return false;
    }

    if (count == 0) {
        if (ppActivate) { CoTaskMemFree(ppActivate); ppActivate = nullptr; }
        hr = CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&m_pMFT));
        if (FAILED(hr)) {
            char buf[96];
            snprintf(buf, sizeof(buf),
                     "MFTEnumEx=0 + CoCreateInstance(CLSID_AACMFTEncoder) 0x%08lX",
                     static_cast<unsigned long>(hr));
            out_error = buf;
            return false;
        }
        m_usedDirectClsid = true;
    } else {
        m_pActivate = ppActivate[0];
        for (UINT32 i = 1; i < count; ++i) ppActivate[i]->Release();
        CoTaskMemFree(ppActivate);

        hr = m_pActivate->ActivateObject(IID_PPV_ARGS(&m_pMFT));
        if (FAILED(hr)) {
            char buf[80];
            snprintf(buf, sizeof(buf), "ActivateObject(IMFTransform) 0x%08lX",
                     static_cast<unsigned long>(hr));
            out_error = buf;
            return false;
        }
    }

    // Step 2: SetInputType — negotiate
    struct TryEntry { GUID subtype; UINT32 bps; };
    TryEntry tryList[] = {
        { MFAudioFormat_PCM,   16 },
        { MFAudioFormat_PCM,   32 },
        { MFAudioFormat_Float, 32 },
    };

    bool inputSet = false;
    for (const auto& e : tryList) {
        IMFMediaType* pType = nullptr;
        if (FAILED(BuildAudioMediaType(e.subtype, sample_rate, channels, e.bps, &pType)))
            continue;
        hr = m_pMFT->SetInputType(0, pType, 0);
        pType->Release();
        if (SUCCEEDED(hr)) {
            m_inputSubtype        = e.subtype;
            m_inputBitsPerSample  = e.bps;
            m_inputBlockAlign     = channels * (e.bps / 8);
            m_inputAvgBytesPerSec = sample_rate * m_inputBlockAlign;
            inputSet = true;
            break;
        }
    }
    if (!inputSet) {
        out_error = "MfAacEncoder: all input type candidates rejected by MFT";
        return false;
    }

    // Step 3: SetOutputType — raw AAC (payload type 0), NOT ADTS
    {
        IMFMediaType* pOutputType = nullptr;
        hr = MFCreateMediaType(&pOutputType);
        if (FAILED(hr)) {
            out_error = "MFCreateMediaType for AAC output type failed";
            return false;
        }

        bool ok = true;
        ok = ok && SUCCEEDED(pOutputType->SetGUID  (MF_MT_MAJOR_TYPE,                        MFMediaType_Audio));
        ok = ok && SUCCEEDED(pOutputType->SetGUID  (MF_MT_SUBTYPE,                            MFAudioFormat_AAC));
        ok = ok && SUCCEEDED(pOutputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,           sample_rate));
        ok = ok && SUCCEEDED(pOutputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,                 channels));
        ok = ok && SUCCEEDED(pOutputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,              16));
        ok = ok && SUCCEEDED(pOutputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,         24000)); // ~192 kbps
        ok = ok && SUCCEEDED(pOutputType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE,                   0));     // RAW
        ok = ok && SUCCEEDED(pOutputType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29));  // AAC-LC stereo 48kHz

        if (!ok) {
            pOutputType->Release();
            out_error = "MfAacEncoder: failed to set output type attributes";
            return false;
        }

        hr = m_pMFT->SetOutputType(0, pOutputType, 0);
        pOutputType->Release();
        if (FAILED(hr)) {
            char buf[80];
            snprintf(buf, sizeof(buf), "SetOutputType(AAC raw) failed 0x%08lX",
                     static_cast<unsigned long>(hr));
            out_error = buf;
            return false;
        }
    }

    // Step 4: Get output type for codec-private extraction
    hr = m_pMFT->GetOutputCurrentType(0, &m_pOutputType);
    if (FAILED(hr) || !m_pOutputType) {
        char buf[80];
        snprintf(buf, sizeof(buf), "GetOutputCurrentType failed 0x%08lX",
                 static_cast<unsigned long>(hr));
        out_error = buf;
        return false;
    }

    // Step 5: GetOutputStreamInfo
    {
        MFT_OUTPUT_STREAM_INFO si{};
        hr = m_pMFT->GetOutputStreamInfo(0, &si);
        if (FAILED(hr)) {
            char buf[80];
            snprintf(buf, sizeof(buf), "GetOutputStreamInfo failed 0x%08lX",
                     static_cast<unsigned long>(hr));
            out_error = buf;
            return false;
        }
        m_mftProvidesSamples = (si.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        m_mftOutBufSize      = (si.cbSize > 0) ? si.cbSize : 8192;
    }

    // Step 6: Begin streaming
    hr = m_pMFT->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "MFT_MESSAGE_NOTIFY_BEGIN_STREAMING failed 0x%08lX",
                 static_cast<unsigned long>(hr));
        out_error = buf;
        return false;
    }
    hr = m_pMFT->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "MFT_MESSAGE_NOTIFY_START_OF_STREAM failed 0x%08lX",
                 static_cast<unsigned long>(hr));
        out_error = buf;
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// DrainOutput — lifted from M2.8 probe
// ---------------------------------------------------------------------------

void MfAacEncoder::DrainOutput(std::vector<EncodedAudioPacket>& out_packets) {
    while (true) {
        MFT_OUTPUT_DATA_BUFFER outBuf{};
        outBuf.dwStreamID = 0;
        outBuf.pSample    = nullptr;
        outBuf.dwStatus   = 0;
        outBuf.pEvents    = nullptr;

        if (!m_mftProvidesSamples) {
            IMFMediaBuffer* pMFBuf = nullptr;
            if (FAILED(MFCreateMemoryBuffer(m_mftOutBufSize, &pMFBuf))) break;
            IMFSample* pOutSample = nullptr;
            if (FAILED(MFCreateSample(&pOutSample))) { pMFBuf->Release(); break; }
            pOutSample->AddBuffer(pMFBuf);
            pMFBuf->Release();
            outBuf.pSample = pOutSample;
        }

        DWORD status = 0;
        HRESULT hr = m_pMFT->ProcessOutput(0, 1, &outBuf, &status);

        if (outBuf.pEvents) { outBuf.pEvents->Release(); outBuf.pEvents = nullptr; }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (outBuf.pSample) { outBuf.pSample->Release(); }
            break;
        }
        if (FAILED(hr)) {
            if (outBuf.pSample) { outBuf.pSample->Release(); }
            break;
        }

        if (outBuf.pSample) {
            LONGLONG sampleTime100ns = 0;
            bool hasTime = SUCCEEDED(outBuf.pSample->GetSampleTime(&sampleTime100ns));

            EncodedAudioPacket pkt;
            pkt.pts_ns = hasTime ? static_cast<uint64_t>(sampleTime100ns) * 100ULL : 0ULL;

            DWORD bufCount = 0;
            if (SUCCEEDED(outBuf.pSample->GetBufferCount(&bufCount))) {
                for (DWORD b = 0; b < bufCount; ++b) {
                    IMFMediaBuffer* pBuf = nullptr;
                    if (SUCCEEDED(outBuf.pSample->GetBufferByIndex(b, &pBuf))) {
                        BYTE*  pData  = nullptr;
                        DWORD  cbCurr = 0;
                        if (SUCCEEDED(pBuf->Lock(&pData, nullptr, &cbCurr)) && cbCurr > 0) {
                            size_t old = pkt.bytes.size();
                            pkt.bytes.resize(old + cbCurr);
                            std::memcpy(pkt.bytes.data() + old, pData, cbCurr);
                            pBuf->Unlock();
                        }
                        pBuf->Release();
                    }
                }
            }

            if (!pkt.bytes.empty())
                out_packets.push_back(std::move(pkt));

            outBuf.pSample->Release();
        }
    }
}

// ---------------------------------------------------------------------------
// FeedFloat32
// ---------------------------------------------------------------------------

void MfAacEncoder::FeedFloat32(
    const float* float_data,
    size_t       total_float_samples,
    uint64_t     pts_ns,
    uint64_t&    accumulated_frames,
    uint32_t     sample_rate,
    uint32_t     channels,
    std::vector<EncodedAudioPacket>& out_packets)
{
    uint32_t numFrames = static_cast<uint32_t>(total_float_samples / channels);
    LONGLONG sampleDuration = static_cast<LONGLONG>(numFrames) * 10000000LL
                              / static_cast<LONGLONG>(sample_rate);

    UINT32 dataBytes = numFrames * m_inputBlockAlign;

    IMFMediaBuffer* pMFBuf = nullptr;
    if (FAILED(MFCreateMemoryBuffer(dataBytes, &pMFBuf))) return;

    BYTE* pDst = nullptr;
    pMFBuf->Lock(&pDst, nullptr, nullptr);

    if (UsesPcm16()) {
        ConvertFloat32ToPcm16(
            float_data,
            reinterpret_cast<int16_t*>(pDst),
            static_cast<size_t>(numFrames) * channels);
    } else {
        std::memcpy(pDst, float_data, dataBytes);
    }

    pMFBuf->Unlock();
    pMFBuf->SetCurrentLength(dataBytes);

    accumulated_frames += numFrames;

    IMFSample* pSample = nullptr;
    if (FAILED(MFCreateSample(&pSample))) { pMFBuf->Release(); return; }

    pSample->AddBuffer(pMFBuf);
    pMFBuf->Release();

    pSample->SetSampleTime(static_cast<LONGLONG>(pts_ns / 100ULL));
    pSample->SetSampleDuration(sampleDuration);

    HRESULT hr = m_pMFT->ProcessInput(0, pSample, 0);
    pSample->Release();

    if (hr == MF_E_NOTACCEPTING || SUCCEEDED(hr)) {
        DrainOutput(out_packets);
    }
}

// ---------------------------------------------------------------------------
// FeedRaw
// ---------------------------------------------------------------------------

void MfAacEncoder::FeedRaw(
    const BYTE*  data,
    DWORD        data_bytes,
    uint64_t     pts_ns,
    LONGLONG     duration_100ns,
    std::vector<EncodedAudioPacket>& out_packets)
{
    IMFMediaBuffer* pMFBuf = nullptr;
    if (FAILED(MFCreateMemoryBuffer(data_bytes, &pMFBuf))) return;

    BYTE* pDst = nullptr;
    pMFBuf->Lock(&pDst, nullptr, nullptr);
    std::memcpy(pDst, data, data_bytes);
    pMFBuf->Unlock();
    pMFBuf->SetCurrentLength(data_bytes);

    IMFSample* pSample = nullptr;
    if (FAILED(MFCreateSample(&pSample))) { pMFBuf->Release(); return; }

    pSample->AddBuffer(pMFBuf);
    pMFBuf->Release();

    pSample->SetSampleTime(static_cast<LONGLONG>(pts_ns / 100ULL));
    pSample->SetSampleDuration(duration_100ns);

    HRESULT hr = m_pMFT->ProcessInput(0, pSample, 0);
    pSample->Release();

    if (hr == MF_E_NOTACCEPTING || SUCCEEDED(hr)) {
        DrainOutput(out_packets);
    }
}

// ---------------------------------------------------------------------------
// Flush
// ---------------------------------------------------------------------------

void MfAacEncoder::Flush(std::vector<EncodedAudioPacket>& out_packets) {
    if (!m_pMFT) return;
    m_pMFT->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    m_pMFT->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    DrainOutput(out_packets);
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void MfAacEncoder::Shutdown() {
    if (m_pOutputType)  { m_pOutputType->Release(); m_pOutputType = nullptr; }
    if (m_pMFT)         { m_pMFT->Release(); m_pMFT = nullptr; }
    if (m_pActivate)    { m_pActivate->Release(); m_pActivate = nullptr; }
    if (m_mfStarted)    { MFShutdown(); m_mfStarted = false; }
}

} // namespace recorder_core
