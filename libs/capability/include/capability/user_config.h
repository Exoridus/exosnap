#pragma once

#include "config_types.h"

#include <cstdint>

namespace exosnap::capability {

struct UserRecorderConfig {
    Container container = Container::Matroska;
    VideoCodec video_codec = VideoCodec::Av1Nvenc;
    AudioCodec audio_codec = AudioCodec::AacMf;
    ChromaSubsampling chroma = ChromaSubsampling::Cs420;
    BitDepth bit_depth = BitDepth::Bit8;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    uint32_t frame_rate_num = 60;
    uint32_t frame_rate_den = 1;
};

} // namespace exosnap::capability
