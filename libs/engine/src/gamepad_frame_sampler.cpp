#include "gamepad_frame_sampler.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace exosnap::engine {
namespace {

EngineError InvalidSprites() {
    EngineError error;
    error.code = EngineErrorCode::InvalidConfig;
    error.owner = EngineModule::Runtime;
    error.context = "sprite layout";
    return error;
}

bool ValidSprites(const GamepadSpriteSet& sprites) noexcept {
    if (!sprites.atlas || !sprites.atlas->HasCompletePayload())
        return false;
    const auto valid_rect = [&](SpriteAtlasRect rect, bool optional = false) {
        if (optional && rect.w == 0 && rect.h == 0)
            return true;
        return rect.x >= 0 && rect.y >= 0 && rect.w > 0 && rect.h > 0 && rect.w <= sprites.atlas->width &&
               rect.h <= sprites.atlas->height && rect.x <= sprites.atlas->width - rect.w &&
               rect.y <= sprites.atlas->height - rect.h;
    };
    const auto finite_bounds = [](SpriteContentRect rect) {
        return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.w) && std::isfinite(rect.h) &&
               rect.w >= 0 && rect.h >= 0;
    };
    for (const auto& part : sprites.parts) {
        if (!valid_rect(part.idle) || !valid_rect(part.pressed) || !valid_rect(part.knockout, true) ||
            !finite_bounds(part.bounds) || !finite_bounds(part.knockout_bounds) || !std::isfinite(part.travel_x) ||
            !std::isfinite(part.travel_y))
            return false;
    }
    return GamepadLayerQuadCapacity(sprites) != std::numeric_limits<size_t>::max();
}

engine::SpriteContentRect Placed(engine::SpriteContentRect local, engine::SpriteContentRect placement) {
    return {placement.x + local.x * placement.w, placement.y + local.y * placement.h, local.w * placement.w,
            local.h * placement.h};
}

void MoveStick(engine::SpriteContentRect& bounds, float x, float y, const GamepadSpritePart& part) {
    const float length = std::hypot(x, y);
    if (length <= 0.08F)
        return;
    const float travel = std::min(1.0F, (length - 0.08F) / 0.92F);
    bounds.x += x / length * travel * part.travel_x;
    bounds.y -= y / length * travel * part.travel_y;
}

} // namespace

size_t GamepadLayerQuadCapacity(const GamepadSpriteSet& sprites) noexcept {
    size_t count = sprites.parts.size();
    for (const auto& part : sprites.parts) {
        if (part.motion == PadMotion::LeftTrigger || part.motion == PadMotion::RightTrigger) {
            if (count == std::numeric_limits<size_t>::max())
                return count;
            ++count;
        }
    }
    return count;
}

Outcome<std::monostate> PrepareGamepadLayer(const GamepadSpriteSet& sprites, SpriteLayerSnapshot& output) {
    if (!ValidSprites(sprites))
        return InvalidSprites();
    try {
        output.quads.reserve(GamepadLayerQuadCapacity(sprites));
    } catch (const std::exception&) {
        auto error = InvalidSprites();
        error.code = EngineErrorCode::ResourceBudgetExceeded;
        return error;
    }
    output.atlas = sprites.atlas;
    output.quads.clear();
    return std::monostate{};
}

gamepad::GamepadState GamepadDisplayState(gamepad::GamepadState state) noexcept {
    if (state.controls_normalized) {
        for (const auto control : {gamepad::Control::LeftStick, gamepad::Control::RightStick}) {
            auto& value = state.controls[static_cast<size_t>(control)];
            if ((value.status == gamepad::ControlStatus::Active || value.status == gamepad::ControlStatus::Released) &&
                std::hypot(value.x, value.y) <= 0.08F)
                value = {gamepad::ControlStatus::Released, 0, 0};
        }
    }
    state.buttons &= 0x3FFF;
    if (std::hypot(state.left_x, state.left_y) <= 0.08F) {
        state.left_x = 0;
        state.left_y = 0;
    }
    if (std::hypot(state.right_x, state.right_y) <= 0.08F) {
        state.right_x = 0;
        state.right_y = 0;
    }
    return state;
}

void FillGamepadLayer(const GamepadSpriteSet& sprites, const gamepad::GamepadState& input, SpriteContentRect placement,
                      float opacity, SpriteLayerSnapshot& layer) noexcept {
    if (layer.atlas.get() != sprites.atlas.get() || layer.quads.capacity() < GamepadLayerQuadCapacity(sprites))
        return;
    auto state = gamepad::Normalized(input);
    const auto left = gamepad::ReadControl(state, gamepad::Control::LeftStick);
    const auto right = gamepad::ReadControl(state, gamepad::Control::RightStick);
    state.left_x = left.x;
    state.left_y = left.y;
    state.right_x = right.x;
    state.right_y = right.y;
    state.left_trigger = gamepad::ReadControl(state, gamepad::Control::LeftTrigger).x;
    state.right_trigger = gamepad::ReadControl(state, gamepad::Control::RightTrigger).x;
    layer.quads.clear();
    for (const auto& part : sprites.parts) {
        auto bounds = part.bounds;
        if (part.motion == PadMotion::LeftStick)
            MoveStick(bounds, state.left_x, state.left_y, part);
        if (part.motion == PadMotion::RightStick)
            MoveStick(bounds, state.right_x, state.right_y, part);
        const auto control = part.motion == PadMotion::None ? part.control : part.click_control;
        const bool pressed = part.canonical
                                 ? gamepad::ReadControl(state, control).status == gamepad::ControlStatus::Active
                                 : (part.button & state.buttons) != 0 ||
                                       (part.auxiliary == PadAuxiliary::Home && state.home.value_or(false)) ||
                                       (part.auxiliary == PadAuxiliary::Share && state.share.value_or(false)) ||
                                       (part.auxiliary == PadAuxiliary::Touchpad && state.touchpad.value_or(false));
        layer.quads.push_back({pressed ? part.pressed : part.idle, Placed(bounds, placement), opacity});
        const float trigger = part.motion == PadMotion::LeftTrigger    ? state.left_trigger
                              : part.motion == PadMotion::RightTrigger ? state.right_trigger
                                                                       : 0;
        if (trigger > 0) {
            auto source = part.pressed;
            const int fill = static_cast<int>(std::round(source.h * trigger));
            if (fill <= 0)
                continue;
            const float fraction = static_cast<float>(source.h - fill) / source.h;
            source.y += source.h - fill;
            source.h = fill;
            bounds.y += bounds.h * fraction;
            bounds.h *= 1 - fraction;
            layer.quads.push_back({source, Placed(bounds, placement), opacity});
        }
    }
    if (!layer.quads.empty()) {
        size_t index = 0;
        for (const auto& part : sprites.parts) {
            if (part.knockout.w <= 0 || index == layer.quads.front().masks.size())
                continue;
            auto bounds = part.knockout_bounds;
            if (part.motion == PadMotion::LeftStick)
                MoveStick(bounds, state.left_x, state.left_y, part);
            if (part.motion == PadMotion::RightStick)
                MoveStick(bounds, state.right_x, state.right_y, part);
            layer.quads.front().masks[index++] = {part.knockout, Placed(bounds, placement)};
        }
    }
}

GamepadFrameSampler::GamepadFrameSampler(const GamepadSpriteSet& sprites, gamepad::HistoryReadView history)
    : sprites_(sprites), history_(std::move(history)) {
}

Outcome<std::unique_ptr<GamepadFrameSampler>> GamepadFrameSampler::Prepare(const GamepadSpriteSet& sprites,
                                                                           gamepad::HistoryReadView history) {
    if (!ValidSprites(sprites))
        return InvalidSprites();
    try {
        return std::unique_ptr<GamepadFrameSampler>(new GamepadFrameSampler(sprites, std::move(history)));
    } catch (const std::bad_alloc&) {
        auto error = InvalidSprites();
        error.code = EngineErrorCode::ResourceBudgetExceeded;
        return error;
    }
}

Outcome<std::monostate> GamepadFrameSampler::PrepareOutput(SpriteLayerSnapshot& output) {
    return PrepareGamepadLayer(sprites_, output);
}

ReadResult GamepadFrameSampler::Sample(CaptureTime100ns time, const SceneItemProperties& properties,
                                       const WebcamPixelRect& content, SpriteLayerSnapshot& output) noexcept {
    if (!content.IsValid() || output.atlas.get() != sprites_.atlas.get() ||
        output.quads.capacity() < GamepadLayerQuadCapacity(sprites_)) {
        output.quads.clear();
        return ReadResult::NotReady;
    }
    const uint64_t before = history_.SelectionGeneration();
    if (before != selection_generation_ || before == 0)
        previous_.reset();
    gamepad::GamepadReading reading;
    auto result = history_.TryStateAt(time.value, reading);
    const uint64_t after = history_.SelectionGeneration();
    if (before != after || after == 0) {
        previous_.reset();
        result = gamepad::HistoryReadResult::Empty;
    } else if (result == gamepad::HistoryReadResult::Found && reading.capture_100ns <= time.value) {
        previous_ = reading;
    }
    selection_generation_ = after;
    gamepad::GamepadState state;
    if (previous_ && previous_->capture_100ns <= time.value && previous_->connected)
        state = previous_->state;
    state = GamepadDisplayState(std::move(state));
    const auto& p = properties.placement;
    const float scale =
        std::min(p.w * static_cast<float>(content.w) / 480.0F, p.h * static_cast<float>(content.h) / 320.0F);
    const float width = 480 * scale / static_cast<float>(content.w);
    const float height = 320 * scale / static_cast<float>(content.h);
    FillGamepadLayer(sprites_, state, {p.x + (p.w - width) / 2, p.y + (p.h - height) / 2, width, height},
                     properties.opacity, output);
    if (result == gamepad::HistoryReadResult::Busy)
        return ReadResult::Busy;
    return result == gamepad::HistoryReadResult::Found ? ReadResult::Updated : ReadResult::NotReady;
}

} // namespace exosnap::engine
