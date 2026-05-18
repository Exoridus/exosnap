#pragma once

#include <Audioclient.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace mf_aac_probe {

// Validated format requirements for the encoder pipeline.
// WASAPI loopback must deliver 48000 Hz, 2-channel audio.
// The MF AAC encoder input type is negotiated at runtime via GetInputAvailableType;
// PCM int16 is preferred, with Float32 as fallback. If PCM int16 is required but
// WASAPI delivers Float32, conversion is done in the encode loop.

constexpr uint32_t kRequiredSampleRate = 48000;
constexpr uint32_t kRequiredChannels = 2;
constexpr uint32_t kBitsPerSample = 32;                                    // WASAPI float32 frame size
constexpr uint32_t kBlockAlign = kRequiredChannels * (kBitsPerSample / 8); // 8 (float32 frame)
constexpr uint32_t kAvgBytesPerSec = kRequiredSampleRate * kBlockAlign;    // 384000
constexpr uint32_t kAacAvgBytesPerSec = 24000;                             // 192 kbps
constexpr uint32_t kAdtsPayloadType = 1;
constexpr uint32_t kAacProfileLevel = 0x29; // AAC-LC stereo 48 kHz
constexpr uint32_t kDurationSec = 30;
constexpr uint32_t kPollSleepMs = 5;
constexpr LONGLONG kHnsBuffer = 2000000LL; // 200ms in 100ns units

struct ProbeResult {
    std::string endpointName;
    uint64_t capturedFrames = 0;
    uint64_t encodedBytes = 0;
    double elapsedSec = 0.0;
    uint64_t discontinuities = 0;
    uint64_t fileSize = 0;
    bool success = false;
};

class Probe {
  public:
    Probe();
    ~Probe();

    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;

    // Runs all phases 01-13. Returns true on full success.
    bool Run();

  private:
    // Phase helpers — each returns false on failure
    bool Phase01_MFStartup();
    bool Phase02_EnumerateMFT();
    bool Phase03_InstantiateMFT();
    bool Phase04_SetInputType();
    bool Phase05_SetOutputType();
    bool Phase06_QueryStreamInfo();
    bool Phase07_OpenWasapiLoopback();
    bool Phase08_09_BeginStreaming();
    bool Phase10_EncodeLoop();
    bool Phase11_EosDrain();
    bool Phase12_WriteAdtsFile();
    bool Phase13_VerifyOutput();

    // Output drain helper — appends encoded bytes to m_aacPackets
    void DrainOutput();

    // Cleanup
    void Cleanup();

    // Utilities
    static std::string WideToUtf8(const wchar_t* wstr);
    static bool IsFormatCompatible(const WAVEFORMATEX* pwfx);

    // Converts a buffer of Float32 interleaved samples to PCM int16.
    // sampleCount = numFrames * numChannels
    static void ConvertFloat32ToPcm16(const float* src, int16_t* dst, size_t sampleCount);

    // MF state
    bool m_mfStarted = false;
    bool m_usedDirectClsid = false;
    IMFActivate* m_pActivate = nullptr;
    IMFTransform* m_pMFT = nullptr;

    // WASAPI state
    IMMDevice* m_pDevice = nullptr;
    IAudioClient* m_pAudioClient = nullptr;
    IAudioCaptureClient* m_pCaptureClient = nullptr;

    // Encoded output accumulator
    std::vector<uint8_t> m_aacPackets;

    // Selected encoder input type (resolved in Phase 04)
    GUID m_inputSubtype = {}; // MFAudioFormat_PCM or MFAudioFormat_Float
    UINT32 m_inputBitsPerSample = 0;
    UINT32 m_inputBlockAlign = 0;
    UINT32 m_inputAvgBytesPerSec = 0;

    // Metrics
    std::string m_endpointName;
    uint64_t m_capturedFrames = 0;
    uint64_t m_discontinuities = 0;
    double m_elapsedSec = 0.0;
};

} // namespace mf_aac_probe
