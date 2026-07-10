#pragma once

#include <cstdint>

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
    Cs420, // 4:2:0 — NV12/P010 input, universal (all codecs, 8- and 10-bit)
    Cs444, // 4:4:4 — AYUV input, expert 8-bit H.264/HEVC only (NVENC High 4:4:4 / HEVC FREXT).
           // AV1 NVENC has no 4:4:4; 4:4:4 + 10-bit is out of scope. 4:2:2 is unsupported
           // (Ada NVENC has no 4:2:2) and intentionally absent here.
};

enum class BitDepth {
    Bit8,  // 8-bit — NV12 input, HEVC Main / AV1 Main 8-bit, H.264 High
    Bit10, // 10-bit — P010 input, HEVC Main10 / AV1 Main 10-bit (SDR BT.709); H.264 unsupported
};

// CQP quality targets for NVENC. Named shorthands for three canonical CQ values;
// the CQ value itself is what the encoder consumes (RecorderConfig::nvenc_cq).
enum class NvencQualityPreset {
    High,     // cq=19 — large files, best quality
    Balanced, // cq=24 — default
    Small,    // cq=30 — smaller files, lower quality
};

// Valid CQ (constant-quality) range for NVENC CQP. 1 = best, 51 = worst.
inline constexpr uint32_t kNvencCqMin = 1;
inline constexpr uint32_t kNvencCqMax = 51;

// The canonical CQ value a named preset stands for. Single source of truth for
// preset -> CQ; nothing else may hardcode 19/24/30.
inline constexpr uint32_t CanonicalCq(NvencQualityPreset preset) noexcept {
    switch (preset) {
    case NvencQualityPreset::High:
        return 19;
    case NvencQualityPreset::Small:
        return 30;
    case NvencQualityPreset::Balanced:
        break;
    }
    return 24;
}

// The named preset a CQ value is closest to, for UI segment selection. Ties
// resolve toward the higher quality (lower CQ), matching the segment order.
inline constexpr NvencQualityPreset NearestQualityPreset(uint32_t cq) noexcept {
    const auto distance = [cq](NvencQualityPreset p) constexpr -> uint32_t {
        const uint32_t c = CanonicalCq(p);
        return cq > c ? cq - c : c - cq;
    };
    const uint32_t d_high = distance(NvencQualityPreset::High);
    const uint32_t d_balanced = distance(NvencQualityPreset::Balanced);
    const uint32_t d_small = distance(NvencQualityPreset::Small);
    if (d_high <= d_balanced && d_high <= d_small) {
        return NvencQualityPreset::High;
    }
    if (d_balanced <= d_small) {
        return NvencQualityPreset::Balanced;
    }
    return NvencQualityPreset::Small;
}

// True when the CQ value is exactly one of the named presets (the UI shows "~"
// in front of the tier name otherwise).
inline constexpr bool IsCanonicalCq(uint32_t cq) noexcept {
    return cq == CanonicalCq(NvencQualityPreset::High) || cq == CanonicalCq(NvencQualityPreset::Balanced) ||
           cq == CanonicalCq(NvencQualityPreset::Small);
}

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
