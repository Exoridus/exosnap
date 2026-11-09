#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace exosnap::engine::testutil {

enum class SyntheticOutputFaultPoint { Payload, Cluster, Trailer, Flush, Close, Durability };

inline std::optional<SyntheticOutputFaultPoint> ParseSyntheticOutputFault(std::string_view name) {
    if (name == "payload")
        return SyntheticOutputFaultPoint::Payload;
    if (name == "cluster")
        return SyntheticOutputFaultPoint::Cluster;
    if (name == "trailer")
        return SyntheticOutputFaultPoint::Trailer;
    if (name == "flush")
        return SyntheticOutputFaultPoint::Flush;
    if (name == "close")
        return SyntheticOutputFaultPoint::Close;
    if (name == "durability")
        return SyntheticOutputFaultPoint::Durability;
    return std::nullopt;
}

struct SyntheticOutputFaultSpec {
    SyntheticOutputFaultPoint point;
    uint64_t occurrence = 1;
};

// Only the test utility owns this schedule. A point and occurrence are a
// complete replay identity, independent of wall clock and thread scheduling.
// Atomic observations remain safe if a timed-out mux worker retains its state.
class SyntheticOutputFault {
  public:
    explicit SyntheticOutputFault(SyntheticOutputFaultSpec specification) : specification_(specification) {
        if (!specification_.occurrence)
            throw std::invalid_argument("Output fault occurrence must be positive");
    }

    bool Visit(SyntheticOutputFaultPoint point) {
        if (point != specification_.point)
            return false;
        if (visits_.fetch_add(1) + 1 != specification_.occurrence)
            return false;
        triggered_.store(true);
        return true;
    }

    [[nodiscard]] uint64_t visits() const {
        return visits_.load();
    }
    [[nodiscard]] bool triggered() const {
        return triggered_.load();
    }

  private:
    SyntheticOutputFaultSpec specification_;
    std::atomic<uint64_t> visits_{0};
    std::atomic<bool> triggered_{false};
};

} // namespace exosnap::engine::testutil
