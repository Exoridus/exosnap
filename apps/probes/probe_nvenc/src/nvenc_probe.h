#pragma once

#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <winrt/base.h>

#include <cstdint>
#include <string>
#include <vector>

#include "nvEncodeAPI.h"

namespace nvenc_probe {

class BaselineProbe {
  public:
    BaselineProbe();
    ~BaselineProbe();

    BaselineProbe(const BaselineProbe&) = delete;
    BaselineProbe& operator=(const BaselineProbe&) = delete;

    int Run();

  private:
    bool Phase01_LoadDll();
    bool Phase02_CreateApiInstance();
    bool Phase03_CreateD3D11Device();
    bool Phase04_OpenEncodeSession();
    bool Phase05_QueryH264Support();
    bool Phase06_QueryNv12Support();
    bool Phase07_FetchPresetConfig();
    bool Phase08_InitializeEncoder();
    bool Phase09_CreateBuffers();
    bool Phase10_FillFrame();
    bool Phase11_SubmitFrame();
    bool Phase12_LockBitstream();
    bool Phase13_FlushAndCleanup();

    bool Phase14_OpenAv1Session();
    bool Phase15_QueryAv1Support();
    bool Phase16_QueryAv1Nv12Format();
    bool Phase17_FetchAv1PresetConfig();
    bool Phase18_InitAv1Encoder();
    bool Phase19_CreateAv1Buffers();
    bool Phase20_FillAv1Frame();
    bool Phase21_SubmitAv1Frame();
    bool Phase22_LockAv1Bitstream();
    bool Phase23_Av1FlushAndCleanup();

    bool Phase24_OpenAv1SustainedSession();
    bool Phase25_Av1SustainedEncodeLoop();
    bool Phase26_Av1SustainedDrainAndReport();
    bool Phase27_WriteIvfFile();
    bool Phase28_VerifyIvfFile();

    void CleanupEncoder();
    void Cleanup();
    bool RunAv1Validation();
    bool RunAv1SustainedValidation();

    HMODULE m_dll = nullptr;
    NV_ENCODE_API_FUNCTION_LIST m_funcs{};
    winrt::com_ptr<ID3D11Device> m_d3dDevice;
    winrt::com_ptr<ID3D11DeviceContext> m_d3dContext;
    std::wstring m_adapterName;
    uint32_t m_adapterVendorId = 0;
    void* m_encoder = nullptr;

    NV_ENC_PRESET_CONFIG m_presetConfig{};
    NV_ENC_CONFIG m_encodeConfig{};

    NV_ENC_PRESET_CONFIG m_av1PresetConfig{};
    NV_ENC_CONFIG        m_av1EncodeConfig{};

    NV_ENC_INPUT_PTR  m_inputBuffer     = nullptr;
    NV_ENC_OUTPUT_PTR m_bitstreamBuffer = nullptr;
    bool              m_needMoreInput   = false;
    uint32_t          m_inputPitch      = 0;

    struct Av1SustainedPacket {
        std::vector<uint8_t> bytes;
        uint64_t             timestamp;
    };

    // Sustained-encode state (reset at start of Phase24)
    int      m_sv_framesSubmitted    = 0;
    int      m_sv_packetsLocked      = 0;
    int      m_sv_needMoreInputCount = 0;
    uint64_t m_sv_totalBytes         = 0;
    double   m_sv_elapsedSeconds     = 0.0;
    std::vector<Av1SustainedPacket> m_sv_packets;

    const uint32_t m_encodeWidth = 1920;
    const uint32_t m_encodeHeight = 1080;
    const uint32_t m_frameRateNum = 60;
    const uint32_t m_frameRateDen = 1;
    const GUID m_presetGuid = NV_ENC_PRESET_P4_GUID;
    const NV_ENC_TUNING_INFO m_tuningInfo = NV_ENC_TUNING_INFO_HIGH_QUALITY;
};

const char* NvencStatusName(NVENCSTATUS st);

} // namespace nvenc_probe
