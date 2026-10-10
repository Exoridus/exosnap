#pragma once
#include <array>
#include <cstring>
#include <exosnap/engine/recorder_session.h>
#include <memory>

namespace exosnap {

// Everything about a published frame except its bytes and generation.
struct WebcamFrameLayout {
    int width = 0;
    int height = 0;
    engine::WebcamPixelFormat format = engine::WebcamPixelFormat::Bgra;
    engine::MatrixCoefficients matrix = engine::MatrixCoefficients::Bt601;
    engine::ColorRange range = engine::ColorRange::Limited;
};

// The owner serializes Publish, Snapshot and Reset. Published payloads are immutable.
class WebcamFrameMailbox {
  public:
    struct Metrics {
        uint64_t generations = 0, snapshots = 0, legacy_copies = 0, payload_bytes = 0, buffers_created = 0;
    };
    // Copies `size` bytes from `data` straight into a reusable slot, so a camera
    // buffer reaches every consumer with this single copy.
    void Publish(const WebcamFrameLayout& layout, const uint8_t* data, size_t size, uint64_t generation) {
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
        frame.width = layout.width;
        frame.height = layout.height;
        frame.format = layout.format;
        frame.matrix = layout.matrix;
        frame.range = layout.range;
        frame.generation = generation;
        frame.pixels.resize(size);
        if (size != 0)
            std::memcpy(frame.pixels.data(), data, size);
        latest_ = *slot;
        ++metrics_.generations;
        metrics_.payload_bytes += size;
    }
    void Publish(int width, int height, const std::vector<uint8_t>& bgra, uint64_t generation) {
        Publish(WebcamFrameLayout{width, height}, bgra.data(), bgra.size(), generation);
    }
    std::shared_ptr<const engine::WebcamFrameSnapshot> Snapshot() {
        ++metrics_.snapshots;
        return latest_;
    }
    // BGRA frames only: the legacy pull interface cannot describe any other layout.
    bool Copy(int& width, int& height, std::vector<uint8_t>& pixels, uint64_t& generation) {
        if (!latest_ || latest_->format != engine::WebcamPixelFormat::Bgra)
            return false;
        width = latest_->width;
        height = latest_->height;
        generation = latest_->generation;
        pixels = latest_->pixels;
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
