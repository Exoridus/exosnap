#pragma once
#include <array>
#include <exosnap/engine/recorder_session.h>
#include <memory>

namespace exosnap {
// The owner serializes Publish, Snapshot and Reset. Published payloads are immutable.
class WebcamFrameMailbox {
  public:
    struct Metrics {
        uint64_t generations = 0, snapshots = 0, legacy_copies = 0, payload_bytes = 0, buffers_created = 0;
    };
    void Publish(int width, int height, const std::vector<uint8_t>& pixels, uint64_t generation) {
        std::shared_ptr<engine::WebcamFrameSnapshot>* slot = nullptr;
        for (auto& candidate : buffers_) {
            if (!candidate || candidate.use_count() == 1) {
                slot = &candidate;
                break;
            }
        }
        // Retained client snapshots cannot be overwritten. Replace a pool entry
        // only when all reusable buffers are still owned by consumers.
        if (!slot)
            slot = &buffers_.front();
        if (!*slot || slot->use_count() != 1) {
            *slot = std::make_shared<engine::WebcamFrameSnapshot>();
            ++metrics_.buffers_created;
        }
        auto& frame = **slot;
        frame.width = width;
        frame.height = height;
        frame.generation = generation;
        frame.bgra.assign(pixels.begin(), pixels.end());
        latest_ = *slot;
        ++metrics_.generations;
        metrics_.payload_bytes += pixels.size();
    }
    std::shared_ptr<const engine::WebcamFrameSnapshot> Snapshot() {
        ++metrics_.snapshots;
        return latest_;
    }
    bool Copy(int& width, int& height, std::vector<uint8_t>& pixels, uint64_t& generation) {
        if (!latest_)
            return false;
        width = latest_->width;
        height = latest_->height;
        generation = latest_->generation;
        pixels = latest_->bgra;
        ++metrics_.legacy_copies;
        metrics_.payload_bytes += pixels.size();
        return true;
    }
    const Metrics& ReadMetrics() const {
        return metrics_;
    }
    void Reset() {
        latest_.reset();
        buffers_ = {};
        metrics_ = {};
    }

  private:
    std::array<std::shared_ptr<engine::WebcamFrameSnapshot>, 3> buffers_;
    std::shared_ptr<const engine::WebcamFrameSnapshot> latest_;
    Metrics metrics_;
};
} // namespace exosnap
