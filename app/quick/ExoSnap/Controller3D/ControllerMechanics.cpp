#include "ControllerMechanics.h"

#include <algorithm>
#include <cmath>

namespace exosnap::quick {
namespace {
bool Available(gamepad::ControlValue value) {
    return value.status == gamepad::ControlStatus::Released || value.status == gamepad::ControlStatus::Active;
}
} // namespace

StickPose PoseStick(gamepad::ControlValue axes, gamepad::ControlValue click, float max_tilt_degrees, float click_travel,
                    float front_sign) noexcept {
    StickPose pose;
    if (Available(click) && std::isfinite(click.x) && click.x > 0)
        pose.click = click_travel;
    if (!Available(axes) || !std::isfinite(axes.x) || !std::isfinite(axes.y))
        return pose;
    const double radius = std::hypot(static_cast<double>(axes.x), static_cast<double>(axes.y));
    if (radius <= .05F)
        return pose;
    pose.amount = static_cast<float>(std::clamp((radius - .05) / .90, 0.0, 1.0));
    // Native +Y input points toward -Z on the shell. Click is applied below this rotation.
    pose.rotation = QQuaternion::fromAxisAndAngle(
        QVector3D(static_cast<float>(-front_sign * axes.y / radius), 0, static_cast<float>(-axes.x / radius)),
        max_tilt_degrees * pose.amount);
    return pose;
}

gamepad::ControlValue ReadPilotControl(const std::optional<gamepad::GamepadReading>& reading, gamepad::Control control,
                                       bool mapped) noexcept {
    if (!mapped)
        return {};
    if (!reading || !reading->connected)
        return {gamepad::ControlStatus::Unavailable, 0, 0};
    return gamepad::ReadControl(reading->state, control);
}
} // namespace exosnap::quick
