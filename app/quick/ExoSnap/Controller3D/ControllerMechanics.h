#pragma once

#include <QQuaternion>
#include <QVector3D>
#include <gamepad/gamepad.h>
#include <optional>

namespace exosnap::quick {

struct StickDimensions {
    float cap_radius = .116F;
    float cap_height = .048F;
    float ring_inner_radius = .081F;
    float ring_thickness = .006F;
    float shaft_height = .176F;
    float shaft_radius = .039F;
    float boot_radius = .149F;
    float well_radius = .16F;
    float well_height = .10F;
    float well_depth = .025F;
    bool operator==(const StickDimensions&) const = default;
};

struct StickPose {
    QQuaternion rotation;
    float click = 0;
    float amount = 0;
};

[[nodiscard]] bool ValidStickDimensions(const StickDimensions& dimensions) noexcept;
[[nodiscard]] StickPose PoseStick(gamepad::ControlValue axes, gamepad::ControlValue click) noexcept;
[[nodiscard]] gamepad::ControlValue ReadPilotControl(const std::optional<gamepad::GamepadReading>& reading,
                                                     gamepad::Control control, bool mapped) noexcept;

} // namespace exosnap::quick
