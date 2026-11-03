#pragma once

#include "BuiltinVisualSettings.h"

namespace exosnap {

// Preparation enqueues work, retains the existing producer owner and publishes only under the supplied token.
// Completion is queued and receiver-guarded. Quiescence includes outstanding preparation jobs.
struct GamepadControlHooks {
    std::function<void(engine::SourceToken, GamepadOverlaySettings, std::function<void(SourceStatus)>)> request_prepare;
    std::function<void()> request_stop;
    std::function<bool()> is_quiescent;
};

class GamepadSource final : public VisualSource {
  public:
    GamepadSource(SourceCreateRequest, GamepadControlHooks);
    engine::Outcome<std::monostate> Apply(const SourceRequest&) override;
    void RequestStop() noexcept override;
    bool IsQuiescent() const noexcept override;

  private:
    SourceCreateRequest creation_;
    GamepadControlHooks hooks_;
    std::optional<SourceRequest> applied_;
    bool has_work_ = false;
    bool running_requested_ = false;
    bool retired_ = false;
};

engine::Outcome<std::monostate> RegisterGamepadModule(CapabilityRegistry&, GamepadControlHooks,
                                                      CachedVisualOptions devices, CachedVisualOptions layouts,
                                                      CachedVisualOptions views = {});

} // namespace exosnap
