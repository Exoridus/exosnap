#include "ControllerMechanics.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace exosnap::quick {
namespace {
bool Available(gamepad::ControlValue value) {
    return value.status == gamepad::ControlStatus::Released || value.status == gamepad::ControlStatus::Active;
}
} // namespace

bool ValidStickDimensions(const StickDimensions& d) noexcept {
    const std::array dimensions{d.cap_radius,   d.cap_height,  d.ring_inner_radius, d.ring_thickness, d.shaft_height,
                                d.shaft_radius, d.boot_radius, d.well_radius,       d.well_height,    d.well_depth};
    if (!std::all_of(dimensions.begin(), dimensions.end(), [](float v) { return std::isfinite(v) && v > 0 && v < 10; }))
        return false;
    return d.ring_inner_radius < .8F * d.cap_radius && d.ring_thickness < d.cap_height &&
           d.shaft_radius < d.ring_inner_radius && d.shaft_radius < d.boot_radius && d.boot_radius < d.well_radius &&
           std::sqrt(d.boot_radius * d.boot_radius - d.shaft_radius * d.shaft_radius) < d.shaft_height &&
           d.well_height < d.shaft_height && d.well_depth < d.well_height &&
           d.cap_height - d.ring_thickness - .24F * d.cap_height > .35F * d.cap_height;
}

StickPose PoseStick(gamepad::ControlValue axes, gamepad::ControlValue click) noexcept {
    StickPose pose;
    if (Available(click) && std::isfinite(click.x) && click.x > 0)
        pose.click = .008F;
    if (!Available(axes) || !std::isfinite(axes.x) || !std::isfinite(axes.y))
        return pose;
    const double radius = std::hypot(static_cast<double>(axes.x), static_cast<double>(axes.y));
    if (radius <= .05F)
        return pose;
    pose.amount = static_cast<float>(std::clamp((radius - .05) / .90, 0.0, 1.0));
    // Native +Y input points toward -Z on the shell. Click is applied below this rotation.
    pose.rotation = QQuaternion::fromAxisAndAngle(
        QVector3D(static_cast<float>(-axes.y / radius), 0, static_cast<float>(-axes.x / radius)), 14 * pose.amount);
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
