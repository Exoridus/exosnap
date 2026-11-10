#pragma once

#include <gamepad/gamepad.h>

#include <QByteArray>
#include <QColor>
#include <QQuaternion>
#include <QString>
#include <QStringList>
#include <QVector3D>

#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace exosnap::quick {

enum class RigMotionKind : uint8_t { Press, Trigger, Stick };

/// Mechanics of one rig part, in the coordinate frame of the part's parent.
struct RigMotion {
    RigMotionKind kind = RigMotionKind::Press;
    /// Press and trigger amount, or stick axes.
    gamepad::Control control = gamepad::Control::Count;
    /// Stick click; Count when the model has no click for this stick.
    gamepad::Control click = gamepad::Control::Count;
    /// Press offset at full amount.
    QVector3D translation;
    float angle_x_degrees = 0;
    float max_tilt_degrees = 0;
    float click_travel = 0;
    float front_sign = 1;
};

struct ControllerModelNode {
    QString name;
    /// Parents always precede their children.
    int parent = -1;
    QVector3D translation;
    QQuaternion rotation;
    /// glTF nodes must scale uniformly so that world transforms stay decomposable.
    float scale = 1;
    std::vector<int> meshes;
    std::optional<RigMotion> motion;
    /// Controls whose activity highlights this node and its descendants.
    std::vector<gamepad::Control> controls;
    /// Shell surfaces become transparent in Ghost.
    bool housing = false;
};

/// One glTF primitive: float3 position followed by float3 normal per vertex, uint32 indices.
struct ControllerModelMesh {
    QByteArray vertices;
    QByteArray indices;
    QVector3D minimum;
    QVector3D maximum;
    /// sRGB-encoded base color.
    QColor color;
};

struct ControllerModel {
    QString id;
    std::vector<ControllerModelNode> nodes;
    std::vector<ControllerModelMesh> meshes;
    /// Controls that move or highlight at least one node. Others are never drawn as live.
    std::array<bool, static_cast<size_t>(gamepad::Control::Count)> supported{};
};

/// Loads `<pack>/lab/model.glb`, `<pack>/lab/rig.json` and `<pack>/pack.json`.
/// Returns null with `error` set when the model cannot be drawn faithfully. Rig or
/// pack entries that cannot be bound are reported in `warnings` and left inanimate.
[[nodiscard]] std::shared_ptr<const ControllerModel> LoadControllerModel(const QString& pack_directory,
                                                                         QStringList& warnings, QString& error);

/// Parses the strict glTF 2.0 binary subset the controller packs use: one scene,
/// TRIANGLES with float3 POSITION/NORMAL, unsigned 16/32-bit indices, no
/// extensions, textures or skins.
[[nodiscard]] std::optional<ControllerModel> ParseControllerGlb(const QByteArray& glb, QString& error);

/// Cached per pack directory for the process lifetime; a failed load is retried.
[[nodiscard]] std::shared_ptr<const ControllerModel> SharedControllerModel(const QString& pack_directory,
                                                                           QStringList& warnings, QString& error);

} // namespace exosnap::quick
