#pragma once

#include <exosnap/engine/gamepad_sprite_data.h>
#include <exosnap/engine/visual_frame.h>

namespace exosnap::engine {

// The caller keeps the source's scene pin while using this borrowed sprite data.
class GamepadFrameSampler final {
  public:
    static Outcome<std::unique_ptr<GamepadFrameSampler>> Prepare(const GamepadSpriteSet&, gamepad::HistoryReadView);
    Outcome<std::monostate> PrepareOutput(SpriteLayerSnapshot&);
    // Busy/NotReady still fill a safe historical or neutral state. Consumers use
    // this output, not an older pressed-state cache. Invalid scratch is cleared.
    ReadResult Sample(CaptureTime100ns, const SceneItemProperties&, const WebcamPixelRect& content,
                      SpriteLayerSnapshot& prepared_output) noexcept;

  private:
    GamepadFrameSampler(const GamepadSpriteSet&, gamepad::HistoryReadView);
    const GamepadSpriteSet& sprites_;
    gamepad::HistoryReadView history_;
    std::optional<gamepad::GamepadReading> previous_;
    uint64_t selection_generation_ = 0;
};

} // namespace exosnap::engine
