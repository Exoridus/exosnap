#pragma once

// Media Foundation AAC-LC encoder wrapper.
// Lifted from M2.8 probe (probe_wgc_nvenc_aac_mkv).

#include <recorder_core/packet_types.h>

#include <cstdint>
#include <string>
#include <vector>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <windows.h>
#include <wmcodecdsp.h>

namespace recorder_core {

// ---------------------------------------------------------------------------
// MfAacEncoder
// ---------------------------------------------------------------------------

class MfAacEncoder {
public:
    MfAacEncoder() = default;
    ~MfAacEncoder();

    MfAacEncoder(const MfAacEncoder&)            = delete;
    MfAacEncoder& operator=(const MfAacEncoder&) = delete;

    // Enumerate, activate, configure input/output types and begin streaming.
    // sample_rate must be 48000, channels must be 2.
    bool Init(uint32_t sample_rate, uint32_t channels, std::string& out_error);

    // Get the output media type (retained for codec-private extraction).
    // Only valid after Init().
    IMFMediaType* OutputMediaType() const { return m_pOutputType; }

    // Feed a block of audio samples. Calls DrainOutput() internally.
    // The float_data pointer and sample_count refer to interleaved float32 samples.
    // pts_ns is the sample-count-derived PTS in nanoseconds.
    void FeedFloat32(
        const float* float_data,
        size_t       total_float_samples,
        uint64_t     pts_ns,
        uint64_t&    accumulated_frames,
        uint32_t     sample_rate,
        uint32_t     channels,
        std::vector<EncodedAudioPacket>& out_packets);

    // Feed raw PCM (when the encoder accepts PCM directly).
    void FeedRaw(
        const BYTE*  data,
        DWORD        data_bytes,
        uint64_t     pts_ns,
        LONGLONG     duration_100ns,
        std::vector<EncodedAudioPacket>& out_packets);

    // Drain remaining output after DRAIN/EOS.
    void DrainOutput(std::vector<EncodedAudioPacket>& out_packets);

    // Send END_OF_STREAM + DRAIN, collect remaining packets.
    void Flush(std::vector<EncodedAudioPacket>& out_packets);

    // Shut down and release all MF resources.
    void Shutdown();

    // Input format negotiated during Init
    bool  UsesPcm16()        const { return m_inputSubtype == MFAudioFormat_PCM && m_inputBitsPerSample == 16; }
    GUID  InputSubtype()     const { return m_inputSubtype; }
    UINT32 InputBps()        const { return m_inputBitsPerSample; }
    UINT32 InputBlockAlign() const { return m_inputBlockAlign; }

private:
    bool          m_mfStarted       = false;
    bool          m_usedDirectClsid = false;
    IMFActivate*  m_pActivate       = nullptr;
    IMFTransform* m_pMFT            = nullptr;
    IMFMediaType* m_pOutputType     = nullptr;

    GUID   m_inputSubtype         = {};
    UINT32 m_inputBitsPerSample   = 0;
    UINT32 m_inputBlockAlign      = 0;
    UINT32 m_inputAvgBytesPerSec  = 0;

    DWORD m_mftOutBufSize      = 8192;
    bool  m_mftProvidesSamples = false;
};

// Utility: Float32 -> PCM16 conversion
void ConvertFloat32ToPcm16(const float* src, int16_t* dst, size_t sample_count);

} // namespace recorder_core
