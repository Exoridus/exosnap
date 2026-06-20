#include <capability/config_types.h>

namespace exosnap::capability {

std::string_view ToString(Container value) noexcept {
    switch (value) {
    case Container::Matroska:
        return "Matroska";
    case Container::Mp4:
        return "MP4";
    case Container::WebM:
        return "WebM";
    }
    return "UnknownContainer";
}

std::string_view ToString(VideoCodec value) noexcept {
    switch (value) {
    case VideoCodec::Av1Nvenc:
        return "AV1 NVENC";
    case VideoCodec::HevcNvenc:
        return "HEVC NVENC";
    case VideoCodec::H264Nvenc:
        return "H.264 NVENC";
    }
    return "UnknownVideoCodec";
}

std::string_view ToString(AudioCodec value) noexcept {
    switch (value) {
    case AudioCodec::Opus:
        return "Opus";
    case AudioCodec::AacMf:
        return "AAC (Media Foundation)";
    case AudioCodec::Pcm:
        return "PCM";
    case AudioCodec::Flac:
        return "FLAC";
    }
    return "UnknownAudioCodec";
}

std::string_view ToString(ChromaSubsampling value) noexcept {
    switch (value) {
    case ChromaSubsampling::Cs420:
        return "4:2:0";
    case ChromaSubsampling::Cs422:
        return "4:2:2";
    case ChromaSubsampling::Cs444:
        return "4:4:4";
    }
    return "UnknownChroma";
}

std::string_view ToString(BitDepth value) noexcept {
    switch (value) {
    case BitDepth::Bit8:
        return "8-bit";
    case BitDepth::Bit10:
        return "10-bit";
    }
    return "UnknownBitDepth";
}

} // namespace exosnap::capability
