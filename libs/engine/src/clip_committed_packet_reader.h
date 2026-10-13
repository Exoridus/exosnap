#pragma once

#include "committed_matroska_view.h"
#include "matroska_packet_timestamps.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace exosnap::engine {

struct ClipCommittedPacket {
    uint64_t track_number = 0;
    bool is_key = false;
    MatroskaPacketTimestamps timestamps{};
    std::vector<uint8_t> bytes;
};

// Reads the closed clusters in a committed view without admitting encoded
// payload bytes until their EBML size has passed the packet limit.
class ClipCommittedPacketReader {
  public:
    ClipCommittedPacketReader(CommittedMatroskaView& view, uint64_t first_cluster_offset, size_t max_packet_bytes);
    Outcome<std::optional<ClipCommittedPacket>> Next();

  private:
    CommittedMatroskaView& view_;
    uint64_t position_;
    uint64_t cluster_end_ = 0;
    size_t max_packet_bytes_;
};

} // namespace exosnap::engine
