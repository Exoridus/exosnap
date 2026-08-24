#pragma once

#include <cstddef>
#include <cstdint>

namespace exosnap::engine {

enum class Container {
    WebM,     // .webm — libwebm/mkvmuxer (DocType=webm, V_AV1+A_OPUS)
    Matroska, // .mkv  — libwebm/mkvmuxer (DocType=matroska via ForceDocTypeMatroska); primary profile: AV1+Opus
    Mp4,      // .mp4  — IMFSinkWriter (MPEG4MediaSink); H.264 + AAC path
};

// Codec identity only — never the encoder that produces it. NVENC is today's
// only backend; an AMF/QSV/software encoder for the same codec must reuse these
// enumerators instead of adding vendor-suffixed twins.
enum class VideoCodec {
    Av1,  // AV1 — primary validated codec
    H264, // H.264 — MP4 path
    Hevc, // H.265 / HEVC — Matroska and MP4 (V_MPEGH/ISO/HEVC)
};

enum class AudioCodec {
    Aac,  // FFmpeg native AAC-LC (ADR 0052) — valid for Matroska and MP4
    Opus, // libopus — valid for WebM and Matroska
    Pcm,  // uncompressed S16LE — valid for Matroska only (A_PCM/INT/LIT)
    Flac, // libFLAC lossless — valid for Matroska only (A_FLAC)
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

// Constant-quality targets. Named shorthands for five canonical CQ values;
// the CQ value itself is what a preset persists (RecorderConfig::cq), and the
// encoder consumes the codec's own quantizer for it (NvencNativeQuantizer).
// New members are appended after the original three so existing
// static_cast<int> item data (persisted presets, combo-box userData) stays
// stable across the renames (Small -> Efficient -> Low).
enum class QualityPreset { High, Balanced, Low, Draft, Ultra };

// Valid CQ (constant-quality) range. 1 = best, 51 = worst. NVENC maps it onto
// CQP; another backend maps it onto its own quality parameter.
inline constexpr uint32_t kCqMin = 1;
inline constexpr uint32_t kCqMax = 51;

// Valid audio bitrate ranges, in kbps, one per lossy audio codec. Single source
// of truth for both the Settings UI's bitrate spinbox range (ConfigPage,
// codec-conditional) and each encoder's own clamp
// (OpusAudioEncoder::ClampOpusBitrateKbps / FfmpegAacEncoder::ResolveBitrateKbps)
// so a value the UI accepts is never silently overridden by the encoder.
// AAC's ceiling is a deliberate product choice, not FFmpeg's native AAC-LC
// encoder's raw mechanical limit (it silently caps around 288 kbps/channel --
// 576 kbps for stereo -- confirmed empirically against the vendored r5 build);
// 320 kbps matches common AAC-LC practice, where quality has already plateaued.
inline constexpr uint32_t kOpusBitrateKbpsMin = 32;
inline constexpr uint32_t kOpusBitrateKbpsMax = 510;
inline constexpr uint32_t kAacBitrateKbpsMin = 64;
inline constexpr uint32_t kAacBitrateKbpsMax = 320;

// The canonical CQ value a named preset stands for. Single source of truth for
// preset -> CQ; nothing else may hardcode 16/19/24/30/35.
inline constexpr uint32_t CanonicalCq(QualityPreset preset) noexcept {
    switch (preset) {
    case QualityPreset::Ultra:
        return 16;
    case QualityPreset::High:
        return 19;
    case QualityPreset::Low:
        return 30;
    case QualityPreset::Draft:
        return 35;
    case QualityPreset::Balanced:
        break;
    }
    return 24;
}

// The named preset a CQ value is closest to, for UI segment selection. Ties
// resolve toward the lower CQ (higher quality), matching the ladder order.
inline constexpr QualityPreset NearestQualityPreset(uint32_t cq) noexcept {
    constexpr QualityPreset kByQuality[] = {QualityPreset::Ultra, QualityPreset::High, QualityPreset::Balanced,
                                            QualityPreset::Low, QualityPreset::Draft};
    QualityPreset best = QualityPreset::Ultra;
    uint32_t best_d = ~0u;
    for (QualityPreset p : kByQuality) {
        const uint32_t c = CanonicalCq(p);
        const uint32_t d = cq > c ? cq - c : c - cq;
        if (d < best_d) {
            best_d = d;
            best = p;
        } // strict '<': earlier (lower-CQ) wins ties
    }
    return best;
}

// True when the CQ value is exactly one of the named presets (the UI shows "~"
// in front of the tier name otherwise).
inline constexpr bool IsCanonicalCq(uint32_t cq) noexcept {
    return cq == 16u || cq == 19u || cq == 24u || cq == 30u || cq == 35u;
}

// ---------------------------------------------------------------------------
// NvencNativeQuantizer — the canonical CQ is a product scale; NVENC's constQP
// is a codec one, and the two are neither the same size nor related by a
// constant.
//
// The canonical scale is DEFINED to be H.264's quantizer scale. H.264 is
// therefore the identity and a CQ a user saved keeps meaning what it meant.
//
// HEVC is the identity too, on measurement rather than by definition. At the
// same QP as H.264, NVENC's HEVC lands within 0.35 VMAF at the top of the
// ladder, scores better below it, and costs 22-51% less bitrate throughout --
// which is already the tier contract. A finer HEVC curve was fitted the same way
// AV1's was and rejected: at the Ultra tier it spent MORE bitrate than H.264 for
// slightly less quality, inverting the efficiency ordering the shared tier name
// exists to preserve.
//
// AV1 needs a table, because the ratio it needs to match H.264 runs from under
// 2x at the top of the ladder to nearly 5x at the bottom: a single factor is
// right at one point on the curve and progressively wrong everywhere else. At a
// flat 5x, AV1's best tier scored below H.264's default tier on real gameplay.
// The anchors are interpolated piecewise-linearly and were measured at equal
// perceived quality over two 1440p60 clips with near-lossless references -- real
// gameplay and a real browser scroll -- using pooled VMAF on the motion clip and
// the per-frame tail on the screen-content clip, where the upper percentiles
// saturate at 100 and only the worst frames separate the ladder.
//
// This is product policy, not encoder plumbing, which is why it sits beside
// CanonicalCq. The anchor values are NVENC-specific; a second backend needs its
// own.
//
// Callers that need the value the encoder is actually configured with -- the
// Expert UI, diagnostics -- use this rather than showing the canonical CQ as if
// it were the quantizer.
// ---------------------------------------------------------------------------
inline constexpr uint32_t NvencNativeQuantizerCeiling(VideoCodec codec) noexcept {
    return codec == VideoCodec::Av1 ? 255u : 51u;
}

inline constexpr uint32_t NvencNativeQuantizer(VideoCodec codec, uint32_t cq) noexcept {
    struct Anchor {
        uint32_t canonical;
        uint32_t native;
    };
    // Anchors at CQ 1 and 51 pin the ends of the scale; the five in between are
    // the shipped quality tiers, which is where the calibration was measured.
    constexpr Anchor kAv1[] = {{1, 5}, {16, 42}, {19, 65}, {24, 94}, {30, 135}, {35, 167}, {51, 255}};

    // Clamped in the canonical domain, before conversion, so no codec ceiling
    // can be exceeded whatever the caller passed.
    const uint32_t canonical = cq < kCqMin ? kCqMin : (cq > kCqMax ? kCqMax : cq);
    if (codec != VideoCodec::Av1) {
        return canonical;
    }
    constexpr std::size_t kCount = sizeof(kAv1) / sizeof(kAv1[0]);
    for (std::size_t i = 1; i < kCount; ++i) {
        if (canonical > kAv1[i].canonical) {
            continue;
        }
        const uint32_t span = kAv1[i].canonical - kAv1[i - 1].canonical;
        const uint32_t rise = kAv1[i].native - kAv1[i - 1].native;
        // Rounded integer interpolation: a truncating divide would bias every
        // segment toward its better-quality end.
        return kAv1[i - 1].native + ((canonical - kAv1[i - 1].canonical) * rise + span / 2) / span;
    }
    return kAv1[kCount - 1].native;
}

// NVENC encoder speed/quality preset (SDK presets P1-P7). P1 is fastest with the
// lowest quality/highest throughput; P7 is slowest with the best quality. This is
// independent of QualityPreset (which only tunes CQP QP values) and of
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

} // namespace exosnap::engine
