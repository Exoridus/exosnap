#pragma once

#include <windows.h>

#include <Audioclient.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mmdeviceapi.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <cstdint>
#include <queue>
#include <string>
#include <vector>

#include "nvEncodeAPI.h"

namespace wgc_nvenc_aac_mkv {

// ---------------------------------------------------------------------------
// CaptureTarget — same pattern as probe_wgc_nvenc_gpu
// ---------------------------------------------------------------------------

struct CaptureTarget {
    enum class Kind { Monitor, Window };

    Kind         kind        = Kind::Monitor;
    std::wstring description;
    HMONITOR     hmonitor    = nullptr;
    HWND         hwnd        = nullptr;
};

// ---------------------------------------------------------------------------
// Packet types
// ---------------------------------------------------------------------------

struct VideoPacket {
    std::vector<uint8_t> bytes;
    uint64_t             timestamp_ns;  // QPC-derived, nanoseconds
    bool                 is_keyframe;
};

struct AudioPacket {
    std::vector<uint8_t> bytes;
    uint64_t             timestamp_ns;  // sample-count-derived, nanoseconds
};

struct MuxPacket {
    uint64_t             timestamp_ns;
    uint64_t             track_number;
    bool                 is_key;
    const uint8_t*       data;
    size_t               size;
};

// ---------------------------------------------------------------------------
// WgcNvencAacMkvProbe
// ---------------------------------------------------------------------------

class WgcNvencAacMkvProbe {
  public:
    static std::vector<CaptureTarget> EnumerateTargets();

    explicit WgcNvencAacMkvProbe(const CaptureTarget& target);
    ~WgcNvencAacMkvProbe();

    WgcNvencAacMkvProbe(const WgcNvencAacMkvProbe&)            = delete;
    WgcNvencAacMkvProbe& operator=(const WgcNvencAacMkvProbe&) = delete;

    int Run();

  private:
    // 20 phases
    bool Phase01_InitComAndMF();
    bool Phase02_InitD3D11VideoDevice();
    bool Phase03_CreateCaptureItem();
    bool Phase04_ValidateDimensions();
    bool Phase05_LoadNvencDll();
    bool Phase06_QueryAv1Support();
    bool Phase07_FetchPresetConfig();
    bool Phase08_InitAv1Encoder();
    bool Phase09_BitstreamNv12VideoProcessorRegister();
    bool Phase10_InitAacEncoder();
    bool Phase11_InitWasapiLoopback();
    bool Phase12_EstablishEpochAndStartWgc();
    bool Phase13_WaitForFirstFrame();
    bool Phase14_DualCaptureLoop();
    bool Phase15_DrainAv1Encoder();
    bool Phase16_DrainAacEncoder();
    bool Phase17_UnregisterNvencResource();
    bool Phase18_DeriveAv1CodecPrivate();
    bool Phase19_DeriveAacCodecPrivate();
    bool Phase20_WriteAndVerifyMkv();

    // Helpers
    void CleanupCapture();
    void CleanupEncoder();
    void CleanupAudio();
    void PrintSummary();

    // Drain AAC output after each ProcessInput; stores AudioPackets
    void DrainAacOutput();

    // Float32 -> PCM16 conversion (used if encoder requires PCM16)
    static void ConvertFloat32ToPcm16(const float* src, int16_t* dst, size_t sampleCount);
    static std::string WideToUtf8(const wchar_t* wstr);

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------

    CaptureTarget m_target;
    bool          m_sourceLost = false;

    // D3D11
    winrt::com_ptr<ID3D11Device>        m_d3dDevice;
    winrt::com_ptr<ID3D11DeviceContext> m_d3dContext;

    // D3D11 video
    winrt::com_ptr<ID3D11VideoDevice>   m_videoDevice;
    winrt::com_ptr<ID3D11VideoContext>  m_videoContext;

    // Video processor
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> m_videoEnum;
    winrt::com_ptr<ID3D11VideoProcessor>           m_videoProcessor;

    // NV12 intermediate texture + output view
    winrt::com_ptr<ID3D11Texture2D>                m_nv12Texture;
    winrt::com_ptr<ID3D11VideoProcessorOutputView> m_videoOutputView;

    // WGC
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem        m_item{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession     m_session{nullptr};
    winrt::event_token                                             m_closedToken{};

    // Dimensions
    uint32_t m_sourceWidth  = 0;
    uint32_t m_sourceHeight = 0;
    uint32_t m_encodeWidth  = 0;
    uint32_t m_encodeHeight = 0;

    // NVENC
    HMODULE                     m_nvencDll        = nullptr;
    NV_ENCODE_API_FUNCTION_LIST m_nvencFuncs{};
    void*                       m_encoder         = nullptr;
    NV_ENC_PRESET_CONFIG        m_presetConfig{};
    NV_ENC_CONFIG               m_encodeConfig{};
    NV_ENC_OUTPUT_PTR           m_bitstreamBuffer = nullptr;
    NV_ENC_REGISTERED_PTR       m_registeredResource = nullptr;

    const GUID               m_presetGuid   = NV_ENC_PRESET_P4_GUID;
    const NV_ENC_TUNING_INFO m_tuningInfo   = NV_ENC_TUNING_INFO_HIGH_QUALITY;
    const uint32_t           m_frameRateNum = 60;
    const uint32_t           m_frameRateDen = 1;

    // MF AAC encoder
    bool           m_mfStarted       = false;
    bool           m_usedDirectClsid = false;
    IMFActivate*   m_pActivate       = nullptr;
    IMFTransform*  m_pMFT            = nullptr;
    IMFMediaType*  m_pOutputType     = nullptr;  // retained for Phase 19

    // MFT input type resolved in Phase 10
    GUID   m_inputSubtype        = {};
    UINT32 m_inputBitsPerSample  = 0;
    UINT32 m_inputBlockAlign     = 0;
    UINT32 m_inputAvgBytesPerSec = 0;

    // MFT output stream info (for DrainOutput)
    DWORD m_mftOutBufSize         = 8192;
    bool  m_mftProvidesSamples    = false;

    // WASAPI
    IMMDevice*           m_pDevice        = nullptr;
    IAudioClient*        m_pAudioClient   = nullptr;
    IAudioCaptureClient* m_pCaptureClient = nullptr;
    std::string          m_endpointName;

    // QPC epoch (set in Phase 12)
    LARGE_INTEGER m_qpcFreq{};
    LARGE_INTEGER m_qpcEpoch{};

    // Collected packets
    std::vector<VideoPacket> m_videoPackets;
    std::vector<AudioPacket> m_audioPackets;

    // Pending video PTS FIFO — PTS for every frame submitted to NVENC but
    // not yet returned as an encoded packet.  Populated on Submit; consumed
    // on LockBitstream (main loop) and EOS drain.
    std::queue<uint64_t> m_pendingVideoPts;

    // WGC video epoch: SystemRelativeTime (100 ns ticks) of the first
    // submitted frame.  Set on first submission; subsequent PTS are relative
    // to this epoch in nanoseconds.
    bool     m_videoEpochSet         = false;
    int64_t  m_videoEpochTicks100ns  = 0;

    // CodecPrivate data
    uint8_t m_av1CodecPrivate[4] = {};
    uint8_t m_aacCodecPrivate[2] = {};

    // Statistics
    int      m_wgcFramesTotal       = 0;
    int      m_capturedVideoFrames  = 0;
    int      m_needMoreInputCount   = 0;
    uint64_t m_totalVideoBytes      = 0;
    uint64_t m_totalAudioBytes      = 0;
    double   m_elapsedSeconds       = 0.0;

    enum class TerminationReason { Unknown, TimeLimit, SourceLoss };
    TerminationReason m_terminationReason = TerminationReason::Unknown;
};

const char* NvencStatusName(NVENCSTATUS st);

} // namespace wgc_nvenc_aac_mkv
