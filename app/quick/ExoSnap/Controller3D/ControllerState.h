#pragma once

#include "ControllerMechanics.h"
#include "ControllerModel.h"
#include "GamepadArtwork.h"

#include <QAbstractListModel>
#include <QColor>
#include <QObject>
#include <QQuaternion>
#include <QVector3D>
#include <QtQmlIntegration/qqmlintegration.h>

#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace exosnap::quick {

/// One drawable glTF primitive with its current pose. Rows never change count.
class ControllerPartModel final : public QAbstractListModel {
    Q_OBJECT
  public:
    enum Role { Geometry = Qt::UserRole + 1, Position, Rotation, Scale, Color, PressColor, Active, Housing };
    struct Row {
        /// Index into ControllerModel::meshes.
        int mesh = -1;
        /// Index into ControllerModel::nodes.
        int node = -1;
        QVector3D position;
        QQuaternion rotation;
        float scale = 1;
        QColor color;
        QColor press_color;
        bool active = false;
        bool housing = false;
    };
    using QAbstractListModel::QAbstractListModel;
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    void Reset(std::vector<Row> rows);
    // Emits one dataChanged for the whole range when any pose changed.
    bool Update(const std::vector<Row>& rows);
    [[nodiscard]] const std::vector<Row>& Rows() const noexcept {
        return rows_;
    }

  private:
    std::vector<Row> rows_;
};

class ControllerState final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the native renderer")
    Q_PROPERTY(QAbstractItemModel* parts READ parts CONSTANT FINAL)
    Q_PROPERTY(QVector3D center READ center CONSTANT FINAL)
    Q_PROPERTY(QVector3D closeupCenter READ closeupCenter CONSTANT FINAL)
    Q_PROPERTY(float fitWidth READ fitWidth CONSTANT FINAL)
    Q_PROPERTY(float fitHeight READ fitHeight CONSTANT FINAL)
    Q_PROPERTY(float cameraPitch READ cameraPitch CONSTANT FINAL)
    Q_PROPERTY(bool connected READ connected NOTIFY poseChanged FINAL)

  public:
    /// Scene units per model unit; the camera clip range is chosen for this scale.
    static constexpr float kSceneScale = 100;
    Q_PROPERTY(float sceneScale READ sceneScale CONSTANT FINAL)
    static float sceneScale() noexcept {
        return kSceneScale;
    }
    // Matches the Controller Lab reference view: front-facing, slightly from above.
    static constexpr float kCameraPitchDegrees = -14.3F;

    ControllerState(std::shared_ptr<const ControllerModel> model, const MappedGamepadArtwork& mapping,
                    QObject* parent = nullptr);
    ~ControllerState() override;
    void ApplyReading(const std::optional<gamepad::GamepadReading>& reading, uint64_t frame_time);

    QAbstractItemModel* parts() noexcept {
        return &parts_;
    }
    [[nodiscard]] const ControllerPartModel& Parts() const noexcept {
        return parts_;
    }
    QVector3D center() const noexcept {
        return center_;
    }
    QVector3D closeupCenter() const noexcept {
        return closeup_center_;
    }
    float fitWidth() const noexcept {
        return fit_width_;
    }
    float fitHeight() const noexcept {
        return fit_height_;
    }
    static float cameraPitch() noexcept {
        return kCameraPitchDegrees;
    }
    bool connected() const noexcept {
        return connected_;
    }
    [[nodiscard]] const ControllerModel& Model() const noexcept {
        return *model_;
    }
    [[nodiscard]] bool Supported(gamepad::Control control) const noexcept {
        return model_->supported[static_cast<size_t>(control)];
    }
    uint64_t inputTime() const noexcept {
        return input_time_;
    }
    uint64_t frameTime() const noexcept {
        return frame_time_;
    }
    uint64_t poseGeneration() const noexcept {
        return pose_generation_;
    }

  signals:
    void poseChanged();

  private:
    struct Draw {
        int node = 0;
        int mesh = 0;
        bool housing = false;
        std::vector<gamepad::Control> highlights;
    };
    struct Pose {
        QVector3D position;
        QQuaternion rotation;
        float scale = 1;
    };
    [[nodiscard]] gamepad::ControlValue Read(const std::optional<gamepad::GamepadReading>&, gamepad::Control) const;
    [[nodiscard]] std::vector<Pose> WorldPoses(const std::optional<gamepad::GamepadReading>& reading) const;
    std::vector<ControllerPartModel::Row> PoseRows(const std::optional<gamepad::GamepadReading>& reading) const;

    std::shared_ptr<const ControllerModel> model_;
    std::vector<Draw> draws_;
    std::array<QColor, static_cast<size_t>(gamepad::Control::Count)> press_colors_;
    ControllerPartModel parts_;
    QVector3D center_;
    QVector3D closeup_center_;
    float fit_width_ = 1;
    float fit_height_ = 1;
    bool connected_ = false;
    uint64_t input_time_ = 0;
    uint64_t frame_time_ = 0;
    uint64_t pose_generation_ = 0;
};

/// 2D mapping whose press colors the 3D view shares. Packs without 2D artwork
/// use the default highlight.
[[nodiscard]] MappedGamepadArtwork ControllerPressMapping(const QString& pack, QString& error);

/// Pack directory of the 3D model qualified for `device`, or empty when none is.
[[nodiscard]] QString NativeControllerPack(const gamepad::DeviceIdentity& device);

} // namespace exosnap::quick
