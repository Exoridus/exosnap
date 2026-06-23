#pragma once

namespace recorder_core {

enum class Container {
    WebM,     // .webm — libwebm/mkvmuxer (DocType=webm, V_AV1+A_OPUS)
    Matroska, // .mkv  — libwebm/mkvmuxer (DocType=matroska via ForceDocTypeMatroska); primary profile: AV1+Opus
    Mp4,      // .mp4  — IMFSinkWriter (MPEG4MediaSink); H.264 + AAC path
};

enum class VideoCodec {
    Av1Nvenc,  // NVENC AV1 — primary validated codec
    H264Nvenc, // NVENC H.264 — MP4 path
    HevcNvenc, // NVENC H.265 / HEVC — Matroska and MP4 (V_MPEGH/ISO/HEVC)
};

enum class AudioCodec {
    AacMf, // Media Foundation AAC-LC — valid for Matroska
    Opus,  // libopus — valid for WebM and Matroska
    Pcm,   // uncompressed S16LE — valid for Matroska only (A_PCM/INT_LIT)
    Flac,  // libFLAC lossless — valid for Matroska only (A_FLAC)
};

enum class ChromaSubsampling {
    Cs420, // 4:2:0 — only supported value for M3.1
};

enum class BitDepth {
    Bit8,  // 8-bit — NV12 input, HEVC Main / AV1 Main 8-bit, H.264 High
    Bit10, // 10-bit — P010 input, HEVC Main10 / AV1 Main 10-bit (SDR BT.709); H.264 unsupported
};

// CQP quality targets for NVENC. Maps to constQP.qpIntra / qpInterP pairs.
enum class NvencQualityPreset {
    High,     // qpIntra=19, qpInterP=21 — large files, best quality
    Balanced, // qpIntra=24, qpInterP=26 — default
    Small,    // qpIntra=30, qpInterP=32 — smaller files, lower quality
};

// Canonical rate-control modes (ADR 0009). Encoders map from this model to
// their native parameters internally. The UI never uses per-vendor terminology.
enum class RateControlMode {
    ConstantQuality, // NVENC: CQP — quality-target, encoder chooses bitrate
    VariableBitrate, // NVENC: VBR — encoder targets a bitrate, quality varies
    ConstantBitrate, // NVENC: CBR — strict bitrate, quality managed by encoder
    Lossless,        // Not yet implemented for any encoder; capability-gated/hidden in UI
};

} // namespace recorder_core
