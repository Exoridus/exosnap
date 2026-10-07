#pragma once

#include <exosnap/engine/packet_types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <optional>
#include <utility>

namespace exosnap::engine {

// Aligned audio stays reversible until video establishes its segment. The
// greatest emitted picture PTS is safe even when intervening B-frames reorder.
// A producer may also establish a lower bound for every future picture's PTS.
class MuxAudioHold {
  public:
    struct Limits {
        size_t bytes = 256ull * 1024 * 1024;
        uint64_t duration_ns = 60ull * 1000000000;
    };

    MuxAudioHold() = default;
    explicit MuxAudioHold(Limits limits) : limits_(limits) {
    }

    bool Push(EncodedAudioPacket packet) {
        const size_t packet_bytes = packet.bytes.size() + sizeof(EncodedAudioPacket);
        if (packet_bytes > limits_.bytes - bytes_)
            return false;
        uint64_t earliest = packet.pts_ns;
        if (!packets_.empty())
            earliest = (std::min)(earliest, packets_.front().pts_ns);
        if (watermark_)
            earliest = (std::min)(earliest, *watermark_);
        const uint64_t latest = packets_.empty() ? packet.pts_ns : (std::max)(packet.pts_ns, packets_.back().pts_ns);
        if (latest - earliest > limits_.duration_ns)
            return false;
        auto pos = packets_.end();
        while (pos != packets_.begin() && std::prev(pos)->pts_ns > packet.pts_ns)
            --pos;
        bytes_ += packet_bytes;
        packets_.insert(pos, std::move(packet));
        return true;
    }

    void AdvanceVideo(uint64_t pts_ns) noexcept {
        watermark_ = watermark_ ? (std::max)(*watermark_, pts_ns) : pts_ns;
    }

    std::optional<EncodedAudioPacket> PopEligible() {
        return watermark_ ? PopBefore(*watermark_) : std::nullopt;
    }

    std::optional<EncodedAudioPacket> PopBefore(uint64_t boundary_ns) {
        if (packets_.empty() || packets_.front().pts_ns >= boundary_ns)
            return std::nullopt;
        return Pop();
    }

    std::optional<EncodedAudioPacket> Pop() {
        if (packets_.empty())
            return std::nullopt;
        auto packet = std::move(packets_.front());
        packets_.pop_front();
        bytes_ -= packet.bytes.size() + sizeof(EncodedAudioPacket);
        return packet;
    }

    [[nodiscard]] size_t bytes() const noexcept {
        return bytes_;
    }

  private:
    Limits limits_;
    std::deque<EncodedAudioPacket> packets_;
    size_t bytes_ = 0;
    std::optional<uint64_t> watermark_;
};

} // namespace exosnap::engine
