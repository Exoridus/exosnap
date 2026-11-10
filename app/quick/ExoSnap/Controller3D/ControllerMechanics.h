#pragma once

#include <QQuaternion>
#include <QVector3D>
#include <gamepad/gamepad.h>
#include <optional>

namespace exosnap::quick {

struct StickPose {
    QQuaternion rotation;
    float click = 0;
    float amount = 0;
};

/// Applies a 5% inner deadzone and 5% outer saturation, independently of the 2D
/// display filter. `front_sign` flips the forward tilt for models authored facing +Z.
[[nodiscard]] StickPose PoseStick(gamepad::ControlValue axes, gamepad::ControlValue click, float max_tilt_degrees = 14,
                                  float click_travel = .008F, float front_sign = 1) noexcept;
[[nodiscard]] gamepad::ControlValue ReadPilotControl(const std::optional<gamepad::GamepadReading>& reading,
                                                     gamepad::Control control, bool mapped) noexcept;

} // namespace exosnap::quick
