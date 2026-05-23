#pragma once

namespace recorder_core {

enum class Container {
    WebM,     // .webm — libwebm/mkvmuxer; primary runtime container (DocType=webm, V_AV1+A_OPUS)
    Matroska, // .mkv  — libwebm/mkvmuxer; future extended MKV support
};

enum class VideoCodec {
    Av1Nvenc, // NVENC AV1 — primary supported codec for M3.1
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

} // namespace recorder_core
