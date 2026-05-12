#pragma once

#include <cstdint>
#include <vector>

namespace recorder_core {

struct EncodedVideoPacket {
    std::vector<uint8_t> bytes;
    uint64_t             pts_ns    = 0;
    bool                 keyframe  = false;
};

struct EncodedAudioPacket {
    std::vector<uint8_t> bytes;
    uint64_t             pts_ns = 0;
};

} // namespace recorder_core
