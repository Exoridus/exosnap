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
};

enum class AudioCodec {
    AacMf, // Media Foundation AAC-LC — valid for Matroska
    Opus,  // libopus — valid for WebM and Matroska
};

enum class ChromaSubsampling {
    Cs420, // 4:2:0 — only supported value for M3.1
};

enum class BitDepth {
    Bit8, // 8-bit — only supported value for M3.1
};

// CQP quality targets for NVENC. Maps to constQP.qpIntra / qpInterP pairs.
enum class NvencQualityPreset {
    High,     // qpIntra=19, qpInterP=21 — large files, best quality
    Balanced, // qpIntra=24, qpInterP=26 — default
    Small,    // qpIntra=30, qpInterP=32 — smaller files, lower quality
};

} // namespace recorder_core
