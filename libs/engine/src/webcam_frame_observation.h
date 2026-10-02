#pragma once
#include <exosnap/engine/recorder_session.h>
#include <exosnap/engine/visual_generations.h>

namespace exosnap::engine {
class WebcamFrameObservation {
  public:
    // The key and immutable payload describe the same observation, before reuse is decided.
    VisualFrameKey Observe(VisualGenerations& generations, WebcamFrameProvider* provider, bool enabled) {
        auto next = enabled && provider ? provider->Snapshot() : nullptr;
        if (bool(frame_) != bool(next) || (next && frame_->generation != next->generation))
            ++generations.webcam;
        frame_ = std::move(next);
        return MakeVisualFrameKey(generations);
    }
    const std::shared_ptr<const WebcamFrameSnapshot>& Frame() const {
        return frame_;
    }

  private:
    std::shared_ptr<const WebcamFrameSnapshot> frame_;
};
} // namespace exosnap::engine
