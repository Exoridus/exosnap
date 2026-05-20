#pragma once

namespace recorder_core {

enum class Container {
    Matroska, // .mkv — primary supported container for M3.1
};

enum class VideoCodec {
    Av1Nvenc, // NVENC AV1 — primary supported codec for M3.1
};

enum class AudioCodec {
    AacMf, // Media Foundation AAC-LC — primary supported codec for M3.1
    Opus,  // libopus — implemented in M4 Phase 3; MKV only
};

enum class ChromaSubsampling {
    Cs420, // 4:2:0 — only supported value for M3.1
};

enum class BitDepth {
    Bit8, // 8-bit — only supported value for M3.1
};

} // namespace recorder_core
