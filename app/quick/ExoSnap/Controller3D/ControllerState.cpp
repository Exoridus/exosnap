#include "ControllerState.h"

#include <QFile>

#include <algorithm>
#include <cmath>
#include <limits>

namespace exosnap::quick {
namespace {
const QColor kDefaultPress(QStringLiteral("#4cb8ff"));

float Amount(gamepad::ControlValue value) {
    return value.status == gamepad::ControlStatus::Active && std::isfinite(value.x) ? std::clamp(value.x, 0.0F, 1.0F)
                                                                                    : 0.0F;
}

bool IsStickAxis(gamepad::Control control) {
    return control == gamepad::Control::LeftStick || control == gamepad::Control::RightStick;
}
} // namespace

int ControllerPartModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant ControllerPartModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount())
        return {};
    const auto& row = rows_[static_cast<size_t>(index.row())];
    switch (role) {
    case Geometry:
        return row.mesh;
    case Position:
        return row.position;
    case Rotation:
        return row.rotation;
    case Scale:
        return row.scale;
    case Color:
        return row.color;
    case PressColor:
        return row.press_color;
    case Active:
        return row.active;
    case Housing:
        return row.housing;
    default:
        return {};
    }
}

QHash<int, QByteArray> ControllerPartModel::roleNames() const {
    return {{Geometry, "mesh"},   {Position, "partPosition"}, {Rotation, "partRotation"}, {Scale, "partScale"},
            {Color, "baseColor"}, {PressColor, "pressColor"}, {Active, "active"},         {Housing, "housing"}};
}

void ControllerPartModel::Reset(std::vector<Row> rows) {
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

bool ControllerPartModel::Update(const std::vector<Row>& rows) {
    Q_ASSERT(rows.size() == rows_.size());
    const auto same = [](const Row& a, const Row& b) {
        return a.position == b.position && a.rotation == b.rotation && a.scale == b.scale && a.active == b.active &&
               a.press_color == b.press_color;
    };
    if (std::equal(rows.begin(), rows.end(), rows_.begin(), same))
        return false;
    rows_ = rows;
    emit dataChanged(index(0), index(rowCount() - 1), {Position, Rotation, Scale, PressColor, Active});
    return true;
}

ControllerState::ControllerState(std::shared_ptr<const ControllerModel> model, const MappedGamepadArtwork& mapping,
                                 QObject* parent)
    : QObject(parent), model_(std::move(model)), parts_(this) {
    Q_ASSERT(model_);
    press_colors_.fill(kDefaultPress);
    for (const auto& entry : mapping.controls) {
        if (!entry.live || !entry.press_color.isValid())
            continue;
        for (const auto control : {entry.control, entry.click_control})
            if (control != gamepad::Control::Count)
                press_colors_[static_cast<size_t>(control)] = entry.press_color;
    }
    std::vector<bool> housing(model_->nodes.size());
    std::vector<std::vector<gamepad::Control>> highlights(model_->nodes.size());
    for (size_t index = 0; index < model_->nodes.size(); ++index) {
        const auto& node = model_->nodes[index];
        if (node.parent >= 0) {
            housing[index] = housing[static_cast<size_t>(node.parent)];
            highlights[index] = highlights[static_cast<size_t>(node.parent)];
        }
        housing[index] = housing[index] || node.housing;
        for (const auto control : node.controls)
            if (!IsStickAxis(control) &&
                std::find(highlights[index].begin(), highlights[index].end(), control) == highlights[index].end())
                highlights[index].push_back(control);
        for (const int mesh : node.meshes)
            draws_.push_back({static_cast<int>(index), mesh, housing[index], highlights[index]});
    }
    auto rows = PoseRows(std::nullopt);
    QVector3D minimum(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max());
    QVector3D maximum = -minimum;
    std::vector<QVector3D> corners;
    for (size_t index = 0; index < draws_.size(); ++index) {
        const auto& mesh = model_->meshes[static_cast<size_t>(draws_[index].mesh)];
        const auto& row = rows[index];
        for (int corner = 0; corner < 8; ++corner) {
            const QVector3D local((corner & 1) ? mesh.maximum.x() : mesh.minimum.x(),
                                  (corner & 2) ? mesh.maximum.y() : mesh.minimum.y(),
                                  (corner & 4) ? mesh.maximum.z() : mesh.minimum.z());
            const auto world = row.position + row.rotation.rotatedVector(local * row.scale);
            corners.push_back(world);
            minimum = QVector3D(std::min(minimum.x(), world.x()), std::min(minimum.y(), world.y()),
                                std::min(minimum.z(), world.z()));
            maximum = QVector3D(std::max(maximum.x(), world.x()), std::max(maximum.y(), world.y()),
                                std::max(maximum.z(), world.z()));
        }
    }
    center_ = (minimum + maximum) / 2;
    closeup_center_ = center_;
    const auto rest = WorldPoses(std::nullopt);
    for (size_t index = 0; index < model_->nodes.size(); ++index) {
        const auto& motion = model_->nodes[index].motion;
        if (motion && motion->kind == RigMotionKind::Stick && motion->control == gamepad::Control::LeftStick)
            closeup_center_ = rest[index].position;
    }
    const auto up = QQuaternion::fromAxisAndAngle(1, 0, 0, kCameraPitchDegrees).rotatedVector({0, 1, 0});
    float low = std::numeric_limits<float>::max();
    float high = std::numeric_limits<float>::lowest();
    for (const auto& corner : corners) {
        const float projected = QVector3D::dotProduct(corner - center_, up);
        low = std::min(low, projected);
        high = std::max(high, projected);
    }
    fit_width_ = std::max(1e-3F, maximum.x() - minimum.x());
    fit_height_ = std::max(1e-3F, high - low);
    parts_.Reset(std::move(rows));
}

ControllerState::~ControllerState() = default;

gamepad::ControlValue ControllerState::Read(const std::optional<gamepad::GamepadReading>& reading,
                                            gamepad::Control control) const {
    return control == gamepad::Control::Count ? gamepad::ControlValue{}
                                              : ReadPilotControl(reading, control, Supported(control));
}

std::vector<ControllerState::Pose>
ControllerState::WorldPoses(const std::optional<gamepad::GamepadReading>& reading) const {
    const auto read = [&](gamepad::Control control) { return Read(reading, control); };
    std::vector<Pose> world(model_->nodes.size());
    for (size_t index = 0; index < model_->nodes.size(); ++index) {
        const auto& node = model_->nodes[index];
        Pose local{node.translation, node.rotation, node.scale};
        if (node.motion) {
            const auto& motion = *node.motion;
            if (motion.kind == RigMotionKind::Stick) {
                const auto pose = PoseStick(read(motion.control), read(motion.click), motion.max_tilt_degrees,
                                            motion.click_travel, motion.front_sign);
                local.rotation = node.rotation * pose.rotation;
                local.position += local.rotation.rotatedVector({0, -pose.click, 0}) * node.scale;
            } else {
                const float amount = Amount(read(motion.control));
                local.position += motion.translation * amount;
                if (motion.kind == RigMotionKind::Trigger)
                    local.rotation =
                        node.rotation * QQuaternion::fromAxisAndAngle(1, 0, 0, motion.angle_x_degrees * amount);
            }
        }
        if (node.parent < 0) {
            world[index] = local;
        } else {
            const auto& parent = world[static_cast<size_t>(node.parent)];
            world[index] = {parent.position + parent.rotation.rotatedVector(local.position * parent.scale),
                            parent.rotation * local.rotation, parent.scale * local.scale};
        }
    }
    return world;
}

std::vector<ControllerPartModel::Row>
ControllerState::PoseRows(const std::optional<gamepad::GamepadReading>& reading) const {
    const auto world = WorldPoses(reading);
    std::vector<ControllerPartModel::Row> rows;
    rows.reserve(draws_.size());
    for (const auto& draw : draws_) {
        const auto& pose = world[static_cast<size_t>(draw.node)];
        ControllerPartModel::Row row;
        row.mesh = draw.mesh;
        row.node = draw.node;
        row.position = pose.position;
        row.rotation = pose.rotation;
        row.scale = pose.scale;
        row.color = model_->meshes[static_cast<size_t>(draw.mesh)].color;
        row.housing = draw.housing;
        row.press_color = kDefaultPress;
        for (const auto control : draw.highlights) {
            if (Amount(Read(reading, control)) > 0) {
                row.active = true;
                row.press_color = press_colors_[static_cast<size_t>(control)];
                break;
            }
        }
        rows.push_back(row);
    }
    return rows;
}

void ControllerState::ApplyReading(const std::optional<gamepad::GamepadReading>& reading, uint64_t frame_time) {
    const bool connected = reading && reading->connected;
    const bool changed = parts_.Update(PoseRows(reading)) || connected != connected_;
    connected_ = connected;
    input_time_ = reading ? reading->capture_100ns : 0;
    frame_time_ = frame_time;
    if (changed) {
        ++pose_generation_;
        emit poseChanged();
    }
}

MappedGamepadArtwork ControllerPressMapping(const QString& pack, QString& error) {
    const QString path = pack + QStringLiteral("/mapping.json");
    return QFile::exists(path) ? PrepareMappedGamepadArtwork(path, error) : MappedGamepadArtwork{};
}

QString NativeControllerPack(const gamepad::DeviceIdentity& device) {
    // Only models whose rig, mapping and rendering were qualified on hardware are offered.
    if (device.vendor_id == 0x054C && device.product_id == 0x0CE6 && device.layout == gamepad::LayoutKind::DualSense)
        return QStringLiteral(":/gamepad/controllers/dualsense");
    return {};
}

} // namespace exosnap::quick
