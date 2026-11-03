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

namespace {
constexpr std::array<std::string_view, static_cast<size_t>(Control::Count)> kControlNames{"south",
                                                                                          "east",
                                                                                          "west",
                                                                                          "north",
                                                                                          "back",
                                                                                          "guide",
                                                                                          "start",
                                                                                          "left_stick_click",
                                                                                          "right_stick_click",
                                                                                          "left_shoulder",
                                                                                          "right_shoulder",
                                                                                          "dpad_up",
                                                                                          "dpad_down",
                                                                                          "dpad_left",
                                                                                          "dpad_right",
                                                                                          "misc1",
                                                                                          "right_paddle1",
                                                                                          "left_paddle1",
                                                                                          "right_paddle2",
                                                                                          "left_paddle2",
                                                                                          "touchpad",
                                                                                          "misc2",
                                                                                          "misc3",
                                                                                          "misc4",
                                                                                          "misc5",
                                                                                          "misc6",
                                                                                          "left_stick",
                                                                                          "right_stick",
                                                                                          "left_trigger",
                                                                                          "right_trigger"};

constexpr std::array<uint32_t, 26> kGameInputButtons{
    4,   8,   16, 32,         2,          0,          1,          4096, 8192, 1024,       2048,       64, 128,
    256, 512, 0,  0x10000000, 0x04000000, 0x20000000, 0x08000000, 0,    0,    0x00010000, 0x00020000, 0,  0};

ControlValue Value(float x, float y = 0) {
    return {x != 0 || y != 0 ? ControlStatus::Active : ControlStatus::Released, x, y};
}
} // namespace

std::string_view ControlName(Control control) noexcept {
    const auto index = static_cast<size_t>(control);
    return index < kControlNames.size() ? kControlNames[index] : std::string_view{};
}

Control ControlFromGameInputButton(uint32_t mask) noexcept {
    if (mask != 0)
        for (size_t index = 0; index < kGameInputButtons.size(); ++index)
            if (kGameInputButtons[index] == mask)
                return static_cast<Control>(index);
    return Control::Count;
}

Control ControlFromName(std::string_view name) noexcept {
    for (size_t index = 0; index < kControlNames.size(); ++index)
        if (kControlNames[index] == name)
            return static_cast<Control>(index);
    return Control::Count;
}

void NormalizeGameInputControls(GamepadState& state, uint32_t supported_buttons, bool gamepad_available,
                                bool separately_reported_trigger_clicks) noexcept {
    state.controls = {};
    state.controls_normalized = true;
    for (size_t index = 0; index < kGameInputButtons.size(); ++index) {
        if (!separately_reported_trigger_clicks &&
            (index == static_cast<size_t>(Control::Misc3) || index == static_cast<size_t>(Control::Misc4)))
            continue;
        const auto mask = kGameInputButtons[index];
        if ((mask & supported_buttons) != 0)
            state.controls[index] = gamepad_available ? Value((state.buttons & mask) != 0 ? 1.0F : 0.0F)
                                                      : ControlValue{ControlStatus::Unavailable};
    }
    const auto auxiliary = [&](Control control, std::optional<bool> value) {
        state.controls[static_cast<size_t>(control)] =
            value ? Value(*value ? 1.0F : 0.0F) : ControlValue{ControlStatus::Unavailable};
    };
    auxiliary(Control::Guide, state.home);
    auxiliary(Control::Misc1, state.share);
    auxiliary(Control::Touchpad, state.touchpad);
    const auto analog = [&](Control control, float x, float y = 0) {
        state.controls[static_cast<size_t>(control)] =
            gamepad_available ? Value(x, y) : ControlValue{ControlStatus::Unavailable};
    };
    analog(Control::LeftStick, state.left_x, state.left_y);
    analog(Control::RightStick, state.right_x, state.right_y);
    analog(Control::LeftTrigger, state.left_trigger);
    analog(Control::RightTrigger, state.right_trigger);
}

ControlValue ReadControl(const GamepadState& state, Control control) noexcept {
    const auto index = static_cast<size_t>(control);
    if (index >= state.controls.size())
        return {};
    if (state.controls_normalized)
        return state.controls[index];
    auto legacy = state;
    NormalizeGameInputControls(legacy, 0x3FFF);
    return legacy.controls[index];
}
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
    for (size_t index = 0; index < out.controls.size(); ++index) {
        auto& value = out.controls[index];
        if (value.status == ControlStatus::Unsupported || value.status == ControlStatus::Unavailable) {
            value.x = value.y = 0;
            continue;
        }
        const auto control = static_cast<Control>(index);
        const bool stick = control == Control::LeftStick || control == Control::RightStick;
        value = Value(ClampAxis(value.x, stick ? -1.0F : 0.0F, 1.0F), stick ? ClampAxis(value.y, -1.0F, 1.0F) : 0.0F);
    }
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
    return StateAtLocked(capture_100ns);
}

HistoryReadResult ReadingRing::TryStateAt(uint64_t capture_100ns, GamepadReading& result) const noexcept {
    const std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return HistoryReadResult::Busy;
    const auto reading = StateAtLocked(capture_100ns);
    if (!reading)
        return HistoryReadResult::Empty;
    result = *reading;
    return HistoryReadResult::Found;
}

std::optional<GamepadReading> ReadingRing::StateAtLocked(uint64_t capture_100ns) const noexcept {
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

void DeviceHistory::Push(GamepadReading reading) {
    const std::scoped_lock lock(mutex_);
    const auto selected_before = manual_ ? manual_ : automatic_;
    auto& entry = devices_[reading.device.id];
    const bool selected_connection_changed = selected_before == reading.device.id &&
                                             reading.capture_100ns >= entry.latest_100ns &&
                                             entry.connected != reading.connected;
    if (!reading.connected) {
        const auto previous = entry.history.StateAt(reading.capture_100ns);
        reading.state = {};
        reading.state.controls_normalized = true;
        for (size_t index = 0; index < reading.state.controls.size(); ++index) {
            const bool unsupported = previous && previous->state.controls_normalized &&
                                     previous->state.controls[index].status == ControlStatus::Unsupported;
            reading.state.controls[index].status =
                unsupported ? ControlStatus::Unsupported : ControlStatus::Unavailable;
        }
    }
    entry.history.Push(reading);
    if (reading.capture_100ns >= entry.latest_100ns) {
        entry.identity = reading.device;
        entry.latest_100ns = reading.capture_100ns;
        entry.connected = reading.connected;
        const auto& state = reading.state;
        const bool active = (state.buttons & 0x3FFFU) != 0 || std::hypot(state.left_x, state.left_y) > 0.20F ||
                            std::hypot(state.right_x, state.right_y) > 0.20F || state.left_trigger > 0.1F ||
                            state.right_trigger > 0.1F;
        if (!reading.connected && automatic_ == reading.device.id) {
            automatic_engaged_ = false;
            automatic_since_ = reading.capture_100ns;
        }
        if (reading.connected && reading.capture_100ns >= automatic_since_ &&
            (!automatic_ || !devices_.at(*automatic_).connected || (!automatic_engaged_ && active))) {
            automatic_ = reading.device.id;
            automatic_since_ = reading.capture_100ns;
            automatic_engaged_ = active;
        }
    }
    if (selected_before != (manual_ ? manual_ : automatic_) || selected_connection_changed)
        AdvanceSelectionGeneration();
}

void DeviceHistory::Select(std::optional<DeviceId> device) {
    const std::scoped_lock lock(mutex_);
    const auto previous = manual_ ? manual_ : automatic_;
    manual_ = device;
    if (previous != (manual_ ? manual_ : automatic_))
        AdvanceSelectionGeneration();
}

void DeviceHistory::AdvanceSelectionGeneration() noexcept {
    const auto generation = selection_generation_.load(std::memory_order_relaxed);
    if (generation != 0)
        selection_generation_.store(generation == UINT64_MAX ? 0 : generation + 1, std::memory_order_release);
}

uint64_t DeviceHistory::SelectionGeneration() const noexcept {
    return selection_generation_.load(std::memory_order_acquire);
}

HistoryReadResult DeviceHistory::TryStateAt(uint64_t capture_100ns, GamepadReading& result) const noexcept {
    const std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return HistoryReadResult::Busy;
    if (SelectionGeneration() == 0)
        return HistoryReadResult::Empty;
    const auto selected = manual_ ? manual_ : automatic_;
    if (!selected)
        return HistoryReadResult::Empty;
    const auto found = devices_.find(*selected);
    return found == devices_.end() ? HistoryReadResult::Empty : found->second.history.TryStateAt(capture_100ns, result);
}

HistoryReadView::HistoryReadView(std::shared_ptr<const DeviceHistory> history) noexcept : history_(std::move(history)) {
}

HistoryReadResult HistoryReadView::TryStateAt(uint64_t capture_100ns, GamepadReading& reading) const noexcept {
    return history_ ? history_->TryStateAt(capture_100ns, reading) : HistoryReadResult::Empty;
}

uint64_t HistoryReadView::SelectionGeneration() const noexcept {
    return history_ ? history_->SelectionGeneration() : 0;
}

std::optional<GamepadReading> DeviceHistory::StateFor(const DeviceId& device, uint64_t timestamp) const {
    const std::scoped_lock lock(mutex_);
    const auto found = devices_.find(device);
    return found == devices_.end() ? std::nullopt : found->second.history.StateAt(timestamp);
}

std::vector<DeviceIdentity> DeviceHistory::Devices() const {
    const std::scoped_lock lock(mutex_);
    std::vector<DeviceIdentity> result;
    for (const auto& [id, entry] : devices_) {
        if (entry.connected) {
            result.push_back(entry.identity);
        }
    }
    return result;
}

std::optional<GamepadReading> DeviceHistory::StateAt(uint64_t capture_100ns) const {
    const std::scoped_lock lock(mutex_);
    const auto selected = manual_ ? manual_ : automatic_;
    if (!selected) {
        return std::nullopt;
    }
    const auto found = devices_.find(*selected);
    return found == devices_.end() ? std::nullopt : found->second.history.StateAt(capture_100ns);
}

} // namespace exosnap::gamepad
