#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace exosnap::capability {

enum class Container { Matroska, Mp4, WebM };
enum class VideoCodec { Av1Nvenc, HevcNvenc, H264Nvenc };
enum class AudioCodec { Opus, AacMf, Pcm, Flac };
enum class ChromaSubsampling { Cs420, Cs422, Cs444 };
enum class BitDepth { Bit8, Bit10 };

struct ResolutionConstraint {
    uint32_t max_width = 0;
    uint32_t max_height = 0;
    bool must_be_even = true;
};

constexpr auto AllContainers() noexcept -> std::array<Container, 3> {
    return {Container::Matroska, Container::Mp4, Container::WebM};
}

constexpr auto AllVideoCodecs() noexcept -> std::array<VideoCodec, 3> {
    return {VideoCodec::Av1Nvenc, VideoCodec::HevcNvenc, VideoCodec::H264Nvenc};
}

constexpr auto AllAudioCodecs() noexcept -> std::array<AudioCodec, 4> {
    return {AudioCodec::Opus, AudioCodec::AacMf, AudioCodec::Pcm, AudioCodec::Flac};
}

constexpr auto AllChromaModes() noexcept -> std::array<ChromaSubsampling, 3> {
    return {ChromaSubsampling::Cs420, ChromaSubsampling::Cs422, ChromaSubsampling::Cs444};
}

constexpr auto AllBitDepths() noexcept -> std::array<BitDepth, 2> {
    return {BitDepth::Bit8, BitDepth::Bit10};
}

std::string_view ToString(Container value) noexcept;
std::string_view ToString(VideoCodec value) noexcept;
std::string_view ToString(AudioCodec value) noexcept;
std::string_view ToString(ChromaSubsampling value) noexcept;
std::string_view ToString(BitDepth value) noexcept;

} // namespace exosnap::capability
