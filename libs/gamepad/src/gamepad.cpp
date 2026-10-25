#include "gamepad/gamepad.h"

#include <algorithm>
#include <cmath>

namespace exosnap::gamepad {

namespace {

constexpr uint16_t kVendorMicrosoft = 0x045E;
constexpr uint16_t kVendorSony = 0x054C;
constexpr uint16_t kVendorNintendo = 0x057E;

float ClampAxis(float value, float low, float high) noexcept {
    return std::isnan(value) ? 0.0F : std::clamp(value, low, high);
}

} // namespace

LayoutKind LayoutKindFor(uint16_t vendor_id, uint16_t product_id) noexcept {
    switch (vendor_id) {
    case kVendorSony:
        switch (product_id) {
        case 0x0CE6: // DualSense
        case 0x0DF2: // DualSense Edge
            return LayoutKind::DualSense;
        default:
            return LayoutKind::Generic;
        }
    case kVendorMicrosoft:
        switch (product_id) {
        case 0x0B12: // Xbox Series X|S, USB
        case 0x0B13: // Xbox Series X|S, Bluetooth
            return LayoutKind::XboxSeries;
        case 0x02D1: // Xbox One, original
        case 0x02DD: // Xbox One, 2015 firmware
        case 0x02E3: // Xbox One Elite
        case 0x02EA: // Xbox One S, USB
        case 0x02E0: // Xbox One S, Bluetooth
        case 0x02FD: // Xbox One S, Bluetooth
        case 0x0B20: // Xbox One S, Bluetooth LE
        case 0x02FF: // Xbox One over the GIP driver
        case 0x0B00: // Xbox Elite Series 2, USB
        case 0x0B05: // Xbox Elite Series 2, Bluetooth
        case 0x0B22: // Xbox Elite Series 2, Bluetooth LE
            return LayoutKind::XboxOne;
        default:
            return LayoutKind::Generic;
        }
    case kVendorNintendo:
        return product_id == 0x2009 ? LayoutKind::SwitchPro : LayoutKind::Generic;
    default:
        return LayoutKind::Generic;
    }
}

GamepadState Normalized(const GamepadState& state) noexcept {
    GamepadState out = state;
    out.left_x = ClampAxis(state.left_x, -1.0F, 1.0F);
    out.left_y = ClampAxis(state.left_y, -1.0F, 1.0F);
    out.right_x = ClampAxis(state.right_x, -1.0F, 1.0F);
    out.right_y = ClampAxis(state.right_y, -1.0F, 1.0F);
    out.left_trigger = ClampAxis(state.left_trigger, 0.0F, 1.0F);
    out.right_trigger = ClampAxis(state.right_trigger, 0.0F, 1.0F);
    return out;
}

uint64_t GameInputToCapture100ns(uint64_t gameinput_us, const ClockAnchor& anchor) noexcept {
    if (gameinput_us >= anchor.gameinput_us) {
        return anchor.capture_100ns + (gameinput_us - anchor.gameinput_us) * 10U;
    }
    const uint64_t back = (anchor.gameinput_us - gameinput_us) * 10U;
    return back >= anchor.capture_100ns ? 0U : anchor.capture_100ns - back;
}

ReadingRing::ReadingRing(std::size_t capacity) : slots_(std::max<std::size_t>(capacity, 1)) {
}

void ReadingRing::Push(const GamepadReading& reading) {
    const std::scoped_lock lock(mutex_);
    slots_[next_] = reading;
    next_ = (next_ + 1) % slots_.size();
    count_ = std::min(count_ + 1, slots_.size());
}

std::optional<GamepadReading> ReadingRing::StateAt(uint64_t capture_100ns) const {
    const std::scoped_lock lock(mutex_);
    const GamepadReading* best = nullptr;
    // Oldest to newest in push order, so an equal timestamp resolves to the later push.
    const std::size_t oldest = (next_ + slots_.size() - count_) % slots_.size();
    for (std::size_t i = 0; i < count_; ++i) {
        const GamepadReading& candidate = slots_[(oldest + i) % slots_.size()];
        if (candidate.capture_100ns <= capture_100ns &&
            (best == nullptr || candidate.capture_100ns >= best->capture_100ns)) {
            best = &candidate;
        }
    }
    if (best == nullptr) {
        return std::nullopt;
    }
    return *best;
}

} // namespace exosnap::gamepad
