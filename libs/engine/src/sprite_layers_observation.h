#pragma once
#include <exosnap/engine/overlay_sprites.h>
#include <exosnap/engine/visual_generations.h>

#include <memory>
#include <vector>

namespace exosnap::engine {

// Holds the sprite snapshots the current frame composites, one per configured
// source, and advances the sprite generation when any of them draws something
// different from the previous observation.
class SpriteLayersObservation {
  public:
    // Returns whether the sprite generation advanced.
    bool Observe(VisualGenerations& generations, const std::vector<SpriteSourceProvider*>& sources,
                 uint64_t frame_time_qpc_100ns) {
        layers_.resize(sources.size());
        bool changed = false;
        for (size_t i = 0; i < sources.size(); ++i) {
            auto next = sources[i] != nullptr ? sources[i]->Snapshot(frame_time_qpc_100ns) : nullptr;
            changed = changed || SpriteLayerChanged(layers_[i].get(), next.get());
            layers_[i] = std::move(next);
        }
        if (changed)
            ++generations.sprites;
        return changed;
    }

    // Index-aligned with the configured sources; an entry may be null.
    [[nodiscard]] const std::vector<std::shared_ptr<const SpriteLayerSnapshot>>& Layers() const noexcept {
        return layers_;
    }

    // Whether any layer has a quad to draw this frame.
    [[nodiscard]] bool AnyVisible() const noexcept {
        for (const auto& layer : layers_) {
            if (layer && layer->atlas && layer->atlas->HasCompletePayload() && !layer->quads.empty())
                return true;
        }
        return false;
    }

  private:
    std::vector<std::shared_ptr<const SpriteLayerSnapshot>> layers_;
};

} // namespace exosnap::engine
