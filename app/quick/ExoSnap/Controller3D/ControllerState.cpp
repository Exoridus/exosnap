#include "ControllerState.h"
#include "GamepadOverlay.h"

#include <algorithm>
#include <cmath>

namespace exosnap::quick {
const std::vector<ControllerButton>& DualSenseButtons() {
    using gamepad::Control;
    static const std::vector<ControllerButton> buttons{
        {Control::South, {.64F, .28F, -.03F}, {.115F, .06F, .115F}, {0, -1, 0}, .009F, 0, true},
        {Control::East, {.77F, .28F, -.16F}, {.115F, .06F, .115F}, {0, -1, 0}, .009F, 0, true},
        {Control::West, {.51F, .28F, -.16F}, {.115F, .06F, .115F}, {0, -1, 0}, .009F, 0, true},
        {Control::North, {.64F, .28F, -.29F}, {.115F, .06F, .115F}, {0, -1, 0}, .009F, 0, true},
        {Control::DpadUp, {-.64F, .28F, -.27F}, {.09F, .06F, .12F}, {0, -1, 0}, .009F},
        {Control::DpadDown, {-.64F, .28F, -.05F}, {.09F, .06F, .12F}, {0, -1, 0}, .009F},
        {Control::DpadLeft, {-.75F, .28F, -.16F}, {.12F, .06F, .09F}, {0, -1, 0}, .009F},
        {Control::DpadRight, {-.53F, .28F, -.16F}, {.12F, .06F, .09F}, {0, -1, 0}, .009F},
        {Control::Back, {-.36F, .29F, -.34F}, {.055F, .045F, .10F}, {0, -1, 0}, .008F},
        {Control::Start, {.36F, .29F, -.34F}, {.055F, .045F, .10F}, {0, -1, 0}, .008F},
        {Control::Guide, {0, .28F, .10F}, {.065F, .045F, .06F}, {0, -1, 0}, .008F},
        {Control::Misc1, {0, .28F, .19F}, {.09F, .035F, .025F}, {0, -1, 0}, .004F},
        {Control::Touchpad, {0, .265F, -.32F}, {.54F, .045F, .29F}, {0, -1, 0}, .006F},
        {Control::LeftShoulder, {-.62F, .30F, -.49F}, {.30F, .075F, .12F}, {0, -.8F, .6F}, .012F},
        {Control::RightShoulder, {.62F, .30F, -.49F}, {.30F, .075F, .12F}, {0, -.8F, .6F}, .012F},
        {Control::LeftTrigger, {-.62F, .12F, -.53F}, {.28F, .10F, .20F}, {0, 0, 0}, 0, 16},
        {Control::RightTrigger, {.62F, .12F, -.53F}, {.28F, .10F, .20F}, {0, 0, 0}, 0, 16}};
    return buttons;
}

ControllerState::ControllerState(const MappedGamepadArtwork& mapping, QObject* parent) : QObject(parent) {
    for (const auto& entry : mapping.controls) {
        if (entry.live && entry.control != gamepad::Control::Count)
            mapped_[static_cast<size_t>(entry.control)] = true;
        if (entry.live && entry.click_control != gamepad::Control::Count)
            mapped_[static_cast<size_t>(entry.click_control)] = true;
    }
    ApplyReading(std::nullopt, 0);
}

void ControllerState::Sample(const GamepadOverlay& owner, uint64_t frame_time) {
    ApplyReading(owner.ReadingAt(frame_time), frame_time);
}

void ControllerState::ApplyReading(const std::optional<gamepad::GamepadReading>& reading, uint64_t frame_time) {
    using gamepad::Control;
    const auto read = [&](Control control) {
        return ReadPilotControl(reading, control, mapped_[static_cast<size_t>(control)]);
    };
    const auto left = PoseStick(read(Control::LeftStick), read(Control::LeftStickClick));
    const auto right = PoseStick(read(Control::RightStick), read(Control::RightStickClick));
    QVariantList buttons;
    for (const auto& button : DualSenseButtons()) {
        const auto value = read(button.control);
        const float amount = value.status == gamepad::ControlStatus::Active && std::isfinite(value.x)
                                 ? std::clamp(value.x, 0.0F, 1.0F)
                                 : 0;
        buttons.push_back(
            QVariantMap{{QStringLiteral("control"), QString::fromUtf8(gamepad::ControlName(button.control))},
                        {QStringLiteral("position"),
                         QVariant::fromValue(button.position + button.press_axis * (amount * button.travel))},
                        {QStringLiteral("size"), QVariant::fromValue(button.size / 100)},
                        {QStringLiteral("rotation"),
                         QVariant::fromValue(QQuaternion::fromAxisAndAngle(1, 0, 0, amount * button.hinge_degrees))},
                        {QStringLiteral("hinged"), button.hinge_degrees > 0},
                        {QStringLiteral("round"), button.round},
                        {QStringLiteral("amount"), amount},
                        {QStringLiteral("status"), static_cast<int>(value.status)}});
    }
    const bool connected = reading && reading->connected;
    const bool changed = buttons != buttons_ || left.rotation != left_.rotation || left.click != left_.click ||
                         right.rotation != right_.rotation || right.click != right_.click || connected != connected_;
    buttons_ = std::move(buttons);
    left_ = left;
    right_ = right;
    connected_ = connected;
    input_time_ = reading ? reading->capture_100ns : 0;
    frame_time_ = frame_time;
    if (changed) {
        ++pose_generation_;
        emit poseChanged();
    }
}
} // namespace exosnap::quick
