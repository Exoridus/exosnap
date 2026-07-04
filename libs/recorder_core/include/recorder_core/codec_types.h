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

// NVENC encoder speed/quality preset (SDK presets P1-P7). P1 is fastest with the
// lowest quality/highest throughput; P7 is slowest with the best quality. This is
// independent of NvencQualityPreset (which only tunes CQP QP values) and of
// RateControlMode (which selects CQP/VBR/CBR) — the preset instead selects the
// NVENC internal encoding pipeline/algorithm tradeoff. Applies uniformly across
// all three NVENC codecs (H.264, HEVC, AV1); never capability-gated. Default P4
// (balanced) — matches the prior hardcoded AV1/HEVC default; H.264 previously
// used P6 (visible default change, expert-overridable — see ADR 0039).
enum class NvencPreset {
    P1, // fastest, lowest quality
    P2,
    P3,
    P4, // balanced — default
    P5,
    P6,
    P7, // slowest, best quality
};

// Canonical rate-control modes (ADR 0009). Encoders map from this model to
// their native parameters internally. The UI never uses per-vendor terminology.
enum class RateControlMode {
    ConstantQuality, // NVENC: CQP — quality-target, encoder chooses bitrate
    VariableBitrate, // NVENC: VBR — encoder targets a bitrate, quality varies
    ConstantBitrate, // NVENC: CBR — strict bitrate, quality managed by encoder
    Lossless,        // Not yet implemented for any encoder; capability-gated/hidden in UI
};

// HDR handling mode (config plumbing only for now). An HDR-capable desktop is
// auto-detected elsewhere; this enum selects what the pipeline does once one
// is found. Same enum is reused unchanged by capability::UserRecorderConfig,
// RecorderConfig, and OutputSettingsModel (no per-layer duplication — mirrors
// NvencPreset/FramePacingMode).
enum class HdrMode {
    Off,        // HDR handling disabled — legacy SDR-only behavior
    TonemapSdr, // Default: an HDR-capable desktop is tone-mapped down to SDR
    Hdr10,      // Expert opt-in: keep the native PQ/BT.2020 HDR10 signal
};

} // namespace recorder_core
