#pragma once

#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <cstdint>
#include <string>
#include <vector>

#include "nvEncodeAPI.h"

namespace wgc_nvenc {

struct CaptureTarget {
    enum class Kind { Monitor, Window };

    Kind kind = Kind::Monitor;
    std::wstring description;
    HMONITOR hmonitor = nullptr;
    HWND hwnd = nullptr;
};

class WgcNvencProbe {
  public:
    static std::vector<CaptureTarget> EnumerateTargets();

    explicit WgcNvencProbe(const CaptureTarget& target);
    ~WgcNvencProbe();

    WgcNvencProbe(const WgcNvencProbe&) = delete;
    WgcNvencProbe& operator=(const WgcNvencProbe&) = delete;

    int Run();

  private:
    bool Phase01_InitD3D11();
    bool Phase02_CreateCaptureItem();
    bool Phase03_ValidateDimensions();
    bool Phase04_LoadNvencDll();
    bool Phase05_OpenNvencSession();
    bool Phase06_QueryAv1Support();
    bool Phase07_FetchPresetConfig();
    bool Phase08_InitAv1Encoder();
    bool Phase09_CreateNvencBuffers();
    bool Phase10_CreateStagingTexture();
    bool Phase11_StartWgcCapture();
    bool Phase12_WaitForFirstFrame();
    bool Phase13_CaptureEncodeLoop();
    bool Phase14_EosFlushAndDrain();
    bool Phase15_WriteIvfFile();
    bool Phase16_VerifyIvfFile();

    static void ConvertBgraToNv12(const uint8_t* bgraSrc, uint32_t srcPitch, uint8_t* nv12Dst, uint32_t dstPitch,
                                  uint32_t width, uint32_t height);
    void CleanupCapture();
    void CleanupEncoder();
    void PrintSummary();

    // Target
    CaptureTarget m_target;
    bool m_sourceLost = false;

    // D3D11 (shared by WGC and NVENC)
    winrt::com_ptr<ID3D11Device> m_d3dDevice;
    winrt::com_ptr<ID3D11DeviceContext> m_d3dContext;
    winrt::com_ptr<ID3D11Texture2D> m_stagingTex;

    // WGC
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_item{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session{nullptr};
    winrt::event_token m_closedToken{};

    // Dimensions: set from m_item.Size() in Phase02, rounded-to-even in Phase03
    uint32_t m_sourceWidth = 0;
    uint32_t m_sourceHeight = 0;
    uint32_t m_encodeWidth = 0;
    uint32_t m_encodeHeight = 0;

    // NVENC
    HMODULE m_nvencDll = nullptr;
    NV_ENCODE_API_FUNCTION_LIST m_nvencFuncs{};
    void* m_encoder = nullptr;
    NV_ENC_PRESET_CONFIG m_presetConfig{};
    NV_ENC_CONFIG m_encodeConfig{};
    NV_ENC_INPUT_PTR m_inputBuffer = nullptr;
    NV_ENC_OUTPUT_PTR m_bitstreamBuffer = nullptr;
    uint32_t m_inputPitch = 0;

    const GUID m_presetGuid = NV_ENC_PRESET_P4_GUID;
    const NV_ENC_TUNING_INFO m_tuningInfo = NV_ENC_TUNING_INFO_HIGH_QUALITY;
    const uint32_t m_frameRateNum = 60;
    const uint32_t m_frameRateDen = 1;

    // Capture-encode statistics
    int m_wgcFramesTotal = 0; // all non-null TryGetNextFrame returns
    int m_capturedFrames = 0; // frames submitted to nvEncEncodePicture
    int m_needMoreInputCount = 0;
    int m_encodedPackets = 0; // successful nvEncLockBitstream calls
    uint64_t m_totalEncodedBytes = 0;
    double m_elapsedSeconds = 0.0;

    // Termination reason for Phase13 PASS message
    enum class TerminationReason { Unknown, MaxFrames, TimeLimit, SourceLoss };
    TerminationReason m_terminationReason = TerminationReason::Unknown;

    // Collected IVF packets (from Phase13 + Phase14 drain)
    struct EncodedPacket {
        std::vector<uint8_t> bytes;
        uint64_t timestamp;
    };
    std::vector<EncodedPacket> m_packets;
};

const char* NvencStatusName(NVENCSTATUS st);

} // namespace wgc_nvenc
