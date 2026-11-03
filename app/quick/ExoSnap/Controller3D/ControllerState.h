#pragma once

#include "ControllerMechanics.h"
#include "GamepadArtwork.h"
#include <QObject>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

namespace exosnap::quick {
class GamepadOverlay;

struct ControllerButton {
    gamepad::Control control;
    QVector3D position;
    QVector3D size;
    QVector3D press_axis;
    float travel;
    float hinge_degrees = 0;
    bool round = false;
};

[[nodiscard]] const std::vector<ControllerButton>& DualSenseButtons();

class ControllerState final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the pilot")
    Q_PROPERTY(QVariantList buttons READ buttons NOTIFY poseChanged FINAL)
    Q_PROPERTY(QQuaternion leftRotation READ leftRotation NOTIFY poseChanged FINAL)
    Q_PROPERTY(QQuaternion rightRotation READ rightRotation NOTIFY poseChanged FINAL)
    Q_PROPERTY(float leftClick READ leftClick NOTIFY poseChanged FINAL)
    Q_PROPERTY(float rightClick READ rightClick NOTIFY poseChanged FINAL)
    Q_PROPERTY(bool connected READ connected NOTIFY poseChanged FINAL)

  public:
    explicit ControllerState(const MappedGamepadArtwork& mapping, QObject* parent = nullptr);
    void Sample(const GamepadOverlay& owner, uint64_t frame_time);
    void ApplyReading(const std::optional<gamepad::GamepadReading>& reading, uint64_t frame_time);
    QVariantList buttons() const {
        return buttons_;
    }
    QQuaternion leftRotation() const {
        return left_.rotation;
    }
    QQuaternion rightRotation() const {
        return right_.rotation;
    }
    float leftClick() const {
        return left_.click;
    }
    float rightClick() const {
        return right_.click;
    }
    bool connected() const {
        return connected_;
    }
    uint64_t inputTime() const {
        return input_time_;
    }
    uint64_t frameTime() const {
        return frame_time_;
    }
    uint64_t poseGeneration() const {
        return pose_generation_;
    }

  signals:
    void poseChanged();

  private:
    std::array<bool, static_cast<size_t>(gamepad::Control::Count)> mapped_{};
    StickPose left_;
    StickPose right_;
    QVariantList buttons_;
    bool connected_ = false;
    uint64_t input_time_ = 0;
    uint64_t frame_time_ = 0;
    uint64_t pose_generation_ = 0;
};
} // namespace exosnap::quick
