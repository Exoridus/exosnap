#include "ControllerModel.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>

namespace exosnap::quick {
namespace {
constexpr uint32_t kGlbMagic = 0x46546C67;
constexpr uint32_t kJsonChunk = 0x4E4F534A;
constexpr uint32_t kBinChunk = 0x004E4942;
constexpr int kFloat = 5126;
constexpr int kUnsignedShort = 5123;
constexpr int kUnsignedInt = 5125;
// Packs are a few hundred kilobytes; this bounds hostile or corrupt allocations.
constexpr qsizetype kMaxGlbBytes = 32 * 1024 * 1024;

uint32_t ReadU32(const QByteArray& bytes, qsizetype offset) {
    uint32_t value = 0;
    std::memcpy(&value, bytes.constData() + offset, sizeof(value));
    return value;
}

std::optional<QVector3D> Vector(const QJsonValue& value, QVector3D fallback) {
    if (value.isUndefined())
        return fallback;
    const auto array = value.toArray();
    if (array.size() != 3 || !array[0].isDouble() || !array[1].isDouble() || !array[2].isDouble())
        return std::nullopt;
    const QVector3D result(static_cast<float>(array[0].toDouble()), static_cast<float>(array[1].toDouble()),
                           static_cast<float>(array[2].toDouble()));
    if (!std::isfinite(result.x()) || !std::isfinite(result.y()) || !std::isfinite(result.z()))
        return std::nullopt;
    return result;
}

float LinearToSrgb(double linear) {
    linear = std::clamp(linear, 0.0, 1.0);
    return static_cast<float>(linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1 / 2.4) - 0.055);
}

struct Accessor {
    const char* data = nullptr;
    qsizetype count = 0;
    qsizetype stride = 0;
    int component = 0;
};

class GlbReader {
  public:
    GlbReader(const QJsonObject& json, const QByteArray& bin, QString& error) : json_(json), bin_(bin), error_(error) {
    }

    std::optional<Accessor> Read(int index, const char* type, int component, const QString& what) {
        const auto accessors = json_.value(QStringLiteral("accessors")).toArray();
        if (index < 0 || index >= accessors.size())
            return Fail(what + QStringLiteral(" accessor is missing"));
        const auto accessor = accessors[index].toObject();
        if (accessor.contains(QStringLiteral("sparse")) || accessor.value(QStringLiteral("normalized")).toBool())
            return Fail(what + QStringLiteral(" accessor uses unsupported sparse or normalized storage"));
        if (accessor.value(QStringLiteral("type")).toString() != QLatin1String(type))
            return Fail(what + QStringLiteral(" accessor has type %1").arg(accessor.value("type").toString()));
        const int actual = accessor.value(QStringLiteral("componentType")).toInt();
        if (component != 0 ? actual != component : actual != kUnsignedShort && actual != kUnsignedInt)
            return Fail(what + QStringLiteral(" accessor has component type %1").arg(actual));
        const qsizetype count = accessor.value(QStringLiteral("count")).toInteger(-1);
        const auto views = json_.value(QStringLiteral("bufferViews")).toArray();
        const int view_index = accessor.value(QStringLiteral("bufferView")).toInt(-1);
        if (count <= 0 || view_index < 0 || view_index >= views.size())
            return Fail(what + QStringLiteral(" accessor has no data"));
        const auto view = views[view_index].toObject();
        if (view.value(QStringLiteral("buffer")).toInt(-1) != 0)
            return Fail(what + QStringLiteral(" references an external buffer"));
        const qsizetype components = QLatin1String(type) == QLatin1String("VEC3") ? 3 : 1;
        const qsizetype component_size = actual == kUnsignedShort ? 2 : 4;
        const qsizetype element = components * component_size;
        const qsizetype stride = view.value(QStringLiteral("byteStride")).toInteger(element);
        const qsizetype view_offset = view.value(QStringLiteral("byteOffset")).toInteger(0);
        const qsizetype view_length = view.value(QStringLiteral("byteLength")).toInteger(-1);
        const qsizetype offset = accessor.value(QStringLiteral("byteOffset")).toInteger(0);
        if (stride < element || view_offset < 0 || offset < 0 || view_length < 0 || offset % component_size != 0 ||
            (view_offset + offset) % component_size != 0 || view_offset > bin_.size() ||
            view_length > bin_.size() - view_offset ||
            count > (std::numeric_limits<qsizetype>::max() - element) / stride ||
            offset + stride * (count - 1) + element > view_length)
            return Fail(what + QStringLiteral(" accessor exceeds its buffer view"));
        return Accessor{bin_.constData() + view_offset + offset, count, stride, actual};
    }

  private:
    std::nullopt_t Fail(QString message) {
        error_ = std::move(message);
        return std::nullopt;
    }

    const QJsonObject& json_;
    const QByteArray& bin_;
    QString& error_;
};

QVector3D Float3(const Accessor& accessor, qsizetype index) {
    float values[3];
    std::memcpy(values, accessor.data + accessor.stride * index, sizeof(values));
    return {values[0], values[1], values[2]};
}

std::optional<ControllerModelMesh> ReadPrimitive(GlbReader& reader, const QJsonObject& primitive,
                                                 const QJsonArray& materials, const QString& name, QString& error) {
    if (primitive.value(QStringLiteral("mode")).toInt(4) != 4 || primitive.contains(QStringLiteral("targets"))) {
        error = name + QStringLiteral(" is not a static triangle list");
        return std::nullopt;
    }
    const auto attributes = primitive.value(QStringLiteral("attributes")).toObject();
    const auto positions = reader.Read(attributes.value(QStringLiteral("POSITION")).toInt(-1), "VEC3", kFloat,
                                       name + QStringLiteral(" POSITION"));
    if (!positions)
        return std::nullopt;
    const auto normals = reader.Read(attributes.value(QStringLiteral("NORMAL")).toInt(-1), "VEC3", kFloat,
                                     name + QStringLiteral(" NORMAL"));
    if (!normals)
        return std::nullopt;
    if (normals->count != positions->count) {
        error = name + QStringLiteral(" has mismatched POSITION and NORMAL counts");
        return std::nullopt;
    }
    ControllerModelMesh mesh;
    mesh.vertices.resize(positions->count * 6 * static_cast<qsizetype>(sizeof(float)));
    auto* vertex = reinterpret_cast<float*>(mesh.vertices.data());
    mesh.minimum = QVector3D(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max());
    mesh.maximum = -mesh.minimum;
    for (qsizetype index = 0; index < positions->count; ++index) {
        const auto position = Float3(*positions, index);
        const auto normal = Float3(*normals, index);
        for (const float value : {position.x(), position.y(), position.z(), normal.x(), normal.y(), normal.z()}) {
            if (!std::isfinite(value)) {
                error = name + QStringLiteral(" contains a non-finite vertex");
                return std::nullopt;
            }
            *vertex++ = value;
        }
        mesh.minimum = QVector3D(std::min(mesh.minimum.x(), position.x()), std::min(mesh.minimum.y(), position.y()),
                                 std::min(mesh.minimum.z(), position.z()));
        mesh.maximum = QVector3D(std::max(mesh.maximum.x(), position.x()), std::max(mesh.maximum.y(), position.y()),
                                 std::max(mesh.maximum.z(), position.z()));
    }
    std::vector<uint32_t> indices;
    if (primitive.contains(QStringLiteral("indices"))) {
        const auto source = reader.Read(primitive.value(QStringLiteral("indices")).toInt(-1), "SCALAR", 0,
                                        name + QStringLiteral(" indices"));
        if (!source)
            return std::nullopt;
        indices.resize(static_cast<size_t>(source->count));
        for (qsizetype index = 0; index < source->count; ++index) {
            const char* at = source->data + source->stride * index;
            if (source->component == kUnsignedShort) {
                uint16_t value = 0;
                std::memcpy(&value, at, sizeof(value));
                indices[static_cast<size_t>(index)] = value;
            } else {
                std::memcpy(&indices[static_cast<size_t>(index)], at, sizeof(uint32_t));
            }
        }
    } else {
        indices.resize(static_cast<size_t>(positions->count));
        for (size_t index = 0; index < indices.size(); ++index)
            indices[index] = static_cast<uint32_t>(index);
    }
    if (indices.empty() || indices.size() % 3 != 0 || std::any_of(indices.begin(), indices.end(), [&](uint32_t value) {
            return value >= static_cast<uint64_t>(positions->count);
        })) {
        error = name + QStringLiteral(" has invalid triangle indices");
        return std::nullopt;
    }
    mesh.indices = QByteArray(reinterpret_cast<const char*>(indices.data()),
                              static_cast<qsizetype>(indices.size() * sizeof(uint32_t)));
    mesh.color = QColor::fromRgbF(1, 1, 1);
    if (primitive.contains(QStringLiteral("material"))) {
        const int material_index = primitive.value(QStringLiteral("material")).toInt(-1);
        if (material_index < 0 || material_index >= materials.size()) {
            error = name + QStringLiteral(" references a missing material");
            return std::nullopt;
        }
        const auto pbr = materials[material_index].toObject().value(QStringLiteral("pbrMetallicRoughness")).toObject();
        if (pbr.contains(QStringLiteral("baseColorTexture"))) {
            error = name + QStringLiteral(" uses an unsupported material texture");
            return std::nullopt;
        }
        const auto factor = pbr.value(QStringLiteral("baseColorFactor")).toArray();
        if (factor.size() == 4)
            mesh.color = QColor::fromRgbF(LinearToSrgb(factor[0].toDouble()), LinearToSrgb(factor[1].toDouble()),
                                          LinearToSrgb(factor[2].toDouble()),
                                          static_cast<float>(std::clamp(factor[3].toDouble(), 0.0, 1.0)));
    }
    return mesh;
}

std::optional<QJsonObject> ReadJson(const QString& path, QString& error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("%1 is unavailable").arg(path);
        return std::nullopt;
    }
    QJsonParseError parse{};
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject() ||
        document.object().value(QStringLiteral("schemaVersion")).toInt() != 1) {
        error = QStringLiteral("%1 is not a schema 1 object").arg(path);
        return std::nullopt;
    }
    return document.object();
}

void MarkSupported(ControllerModel& model, gamepad::Control control) {
    if (control != gamepad::Control::Count)
        model.supported[static_cast<size_t>(control)] = true;
}
} // namespace

std::optional<ControllerModel> ParseControllerGlb(const QByteArray& glb, QString& error) {
    if (glb.size() < 28 || glb.size() > kMaxGlbBytes || ReadU32(glb, 0) != kGlbMagic || ReadU32(glb, 4) != 2 ||
        ReadU32(glb, 8) != static_cast<uint32_t>(glb.size())) {
        error = QStringLiteral("Model is not a glTF 2.0 binary file");
        return std::nullopt;
    }
    const qsizetype json_length = ReadU32(glb, 12);
    if (ReadU32(glb, 16) != kJsonChunk || json_length > glb.size() - 28) {
        error = QStringLiteral("Model lacks a JSON chunk");
        return std::nullopt;
    }
    const qsizetype bin_header = 20 + json_length;
    const qsizetype bin_length = ReadU32(glb, bin_header);
    if (ReadU32(glb, bin_header + 4) != kBinChunk || bin_length > glb.size() - bin_header - 8) {
        error = QStringLiteral("Model lacks a binary chunk");
        return std::nullopt;
    }
    QJsonParseError parse{};
    const auto document = QJsonDocument::fromJson(glb.mid(20, json_length), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        error = QStringLiteral("Model JSON is invalid");
        return std::nullopt;
    }
    const auto json = document.object();
    if (json.value(QStringLiteral("asset")).toObject().value(QStringLiteral("version")).toString() !=
            QStringLiteral("2.0") ||
        !json.value(QStringLiteral("extensionsRequired")).toArray().isEmpty() ||
        !json.value(QStringLiteral("skins")).toArray().isEmpty() ||
        !json.value(QStringLiteral("animations")).toArray().isEmpty()) {
        error = QStringLiteral("Model uses unsupported glTF features");
        return std::nullopt;
    }
    const QByteArray bin = glb.mid(bin_header + 8, bin_length);
    GlbReader reader(json, bin, error);
    const auto gltf_nodes = json.value(QStringLiteral("nodes")).toArray();
    const auto gltf_meshes = json.value(QStringLiteral("meshes")).toArray();
    const auto materials = json.value(QStringLiteral("materials")).toArray();
    const auto scenes = json.value(QStringLiteral("scenes")).toArray();
    const int scene = json.value(QStringLiteral("scene")).toInt(0);
    if (scene < 0 || scene >= scenes.size()) {
        error = QStringLiteral("Model has no scene");
        return std::nullopt;
    }
    ControllerModel model;
    std::vector<int> mesh_first(static_cast<size_t>(gltf_meshes.size()), -1);
    std::vector<int> mesh_count(static_cast<size_t>(gltf_meshes.size()), 0);
    std::vector<bool> visited(static_cast<size_t>(gltf_nodes.size()), false);
    std::vector<std::pair<int, int>> pending;
    for (const auto root : scenes[scene].toObject().value(QStringLiteral("nodes")).toArray())
        pending.emplace_back(root.toInt(-1), -1);
    std::reverse(pending.begin(), pending.end());
    while (!pending.empty()) {
        const auto [index, parent] = pending.back();
        pending.pop_back();
        if (index < 0 || index >= gltf_nodes.size() || visited[static_cast<size_t>(index)]) {
            error = QStringLiteral("Model node hierarchy is invalid");
            return std::nullopt;
        }
        visited[static_cast<size_t>(index)] = true;
        const auto source = gltf_nodes[index].toObject();
        const QString name = source.value(QStringLiteral("name")).toString();
        ControllerModelNode node;
        node.name = name;
        node.parent = parent;
        const auto translation = Vector(source.value(QStringLiteral("translation")), {});
        const auto scale = Vector(source.value(QStringLiteral("scale")), {1, 1, 1});
        const auto rotation = source.value(QStringLiteral("rotation")).toArray();
        if (source.contains(QStringLiteral("matrix")) || !translation || !scale ||
            (!rotation.isEmpty() && rotation.size() != 4)) {
            error = QStringLiteral("Model node %1 has an unsupported transform").arg(name);
            return std::nullopt;
        }
        if (std::abs(scale->x() - scale->y()) > 1e-4F || std::abs(scale->x() - scale->z()) > 1e-4F || scale->x() <= 0) {
            error = QStringLiteral("Model node %1 is not uniformly scaled").arg(name);
            return std::nullopt;
        }
        node.translation = *translation;
        node.scale = scale->x();
        if (rotation.size() == 4) {
            node.rotation =
                QQuaternion(static_cast<float>(rotation[3].toDouble()), static_cast<float>(rotation[0].toDouble()),
                            static_cast<float>(rotation[1].toDouble()), static_cast<float>(rotation[2].toDouble()))
                    .normalized();
            if (!std::isfinite(node.rotation.scalar())) {
                error = QStringLiteral("Model node %1 has an invalid rotation").arg(name);
                return std::nullopt;
            }
        }
        if (source.contains(QStringLiteral("mesh"))) {
            const int mesh = source.value(QStringLiteral("mesh")).toInt(-1);
            if (mesh < 0 || mesh >= gltf_meshes.size()) {
                error = QStringLiteral("Model node %1 references a missing mesh").arg(name);
                return std::nullopt;
            }
            auto& first = mesh_first[static_cast<size_t>(mesh)];
            if (first < 0) {
                first = static_cast<int>(model.meshes.size());
                for (const auto primitive :
                     gltf_meshes[mesh].toObject().value(QStringLiteral("primitives")).toArray()) {
                    auto read = ReadPrimitive(reader, primitive.toObject(), materials,
                                              QStringLiteral("Mesh %1").arg(mesh), error);
                    if (!read)
                        return std::nullopt;
                    model.meshes.push_back(std::move(*read));
                }
                mesh_count[static_cast<size_t>(mesh)] = static_cast<int>(model.meshes.size()) - first;
            }
            for (int primitive = 0; primitive < mesh_count[static_cast<size_t>(mesh)]; ++primitive)
                node.meshes.push_back(first + primitive);
        }
        const int self = static_cast<int>(model.nodes.size());
        model.nodes.push_back(std::move(node));
        const auto children = source.value(QStringLiteral("children")).toArray();
        for (auto child = children.size() - 1; child >= 0; --child)
            pending.emplace_back(children[child].toInt(-1), self);
    }
    if (model.meshes.empty()) {
        error = QStringLiteral("Model has no geometry");
        return std::nullopt;
    }
    return model;
}

std::shared_ptr<const ControllerModel> LoadControllerModel(const QString& pack_directory, QStringList& warnings,
                                                           QString& error) {
    QFile glb_file(pack_directory + QStringLiteral("/lab/model.glb"));
    if (!glb_file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("%1 has no 3D model").arg(pack_directory);
        return {};
    }
    auto model = ParseControllerGlb(glb_file.read(kMaxGlbBytes + 1), error);
    if (!model)
        return {};
    const auto rig = ReadJson(pack_directory + QStringLiteral("/lab/rig.json"), error);
    const auto pack = rig ? ReadJson(pack_directory + QStringLiteral("/pack.json"), error) : std::nullopt;
    if (!rig || !pack)
        return {};
    model->id = rig->value(QStringLiteral("id")).toString();
    QHash<QString, int> by_name;
    for (size_t index = 0; index < model->nodes.size(); ++index) {
        const auto& name = model->nodes[index].name;
        if (!name.isEmpty() && by_name.contains(name)) {
            error = QStringLiteral("Model node name %1 is ambiguous").arg(name);
            return {};
        }
        by_name.insert(name, static_cast<int>(index));
    }
    if (!by_name.contains(rig->value(QStringLiteral("rootNode")).toString())) {
        error = QStringLiteral("Rig root node is missing from the model");
        return {};
    }
    QHash<QString, gamepad::Control> canonical;
    for (const auto entry : pack->value(QStringLiteral("controls")).toArray()) {
        const auto binding = entry.toObject();
        const QString source = binding.value(QStringLiteral("source")).toString();
        const QString name = binding.value(QStringLiteral("control")).toString();
        const auto control = gamepad::ControlFromName(name.toStdString());
        if (control == gamepad::Control::Count) {
            warnings.push_back(QStringLiteral("Pack control %1 is not a canonical control").arg(name));
            continue;
        }
        canonical.insert(source, control);
        for (const auto node_name : binding.value(QStringLiteral("nodes")).toArray()) {
            const auto found = by_name.find(node_name.toString());
            if (found == by_name.end()) {
                error = QStringLiteral("Pack control %1 references missing node %2").arg(name, node_name.toString());
                return {};
            }
            auto& controls = model->nodes[static_cast<size_t>(*found)].controls;
            if (std::find(controls.begin(), controls.end(), control) == controls.end())
                controls.push_back(control);
            MarkSupported(*model, control);
        }
    }
    const auto bound = [&](const QString& source) { return canonical.value(source, gamepad::Control::Count); };
    for (const auto entry : rig->value(QStringLiteral("parts")).toArray()) {
        const auto part = entry.toObject();
        const QString node_name = part.value(QStringLiteral("node")).toString();
        const auto found = by_name.find(node_name);
        if (found == by_name.end()) {
            error = QStringLiteral("Rig part %1 is missing from the model").arg(node_name);
            return {};
        }
        auto& node = model->nodes[static_cast<size_t>(*found)];
        node.housing = part.value(QStringLiteral("role")).toString() == QStringLiteral("housing");
        const auto motion = part.value(QStringLiteral("motion")).toObject();
        if (motion.isEmpty())
            continue;
        if (node.motion) {
            error = QStringLiteral("Rig part %1 declares two motions").arg(node_name);
            return {};
        }
        const QString kind = motion.value(QStringLiteral("kind")).toString();
        RigMotion rig_motion;
        if (kind == QStringLiteral("press") || kind == QStringLiteral("trigger")) {
            rig_motion.kind = kind == QStringLiteral("press") ? RigMotionKind::Press : RigMotionKind::Trigger;
            rig_motion.control = bound(motion.value(QStringLiteral("control")).toString());
            const auto translation = Vector(motion.value(QStringLiteral("translation")), {});
            if (!translation) {
                error = QStringLiteral("Rig part %1 has an invalid press offset").arg(node_name);
                return {};
            }
            rig_motion.translation = *translation;
            rig_motion.angle_x_degrees = static_cast<float>(motion.value(QStringLiteral("angleXDegrees")).toDouble());
        } else if (kind == QStringLiteral("stick")) {
            rig_motion.kind = RigMotionKind::Stick;
            const QString axis = motion.value(QStringLiteral("axis")).toString();
            rig_motion.control = bound(axis);
            rig_motion.click = bound(axis + QStringLiteral("_click"));
            rig_motion.max_tilt_degrees = static_cast<float>(motion.value(QStringLiteral("maxTiltDegrees")).toDouble());
            rig_motion.click_travel = static_cast<float>(motion.value(QStringLiteral("clickTravel")).toDouble());
            rig_motion.front_sign = motion.value(QStringLiteral("frontSign")).toDouble(1) < 0 ? -1.0F : 1.0F;
        } else {
            warnings.push_back(QStringLiteral("Rig part %1 has unknown motion %2").arg(node_name, kind));
            continue;
        }
        if (!std::isfinite(rig_motion.angle_x_degrees) || std::abs(rig_motion.angle_x_degrees) > 90 ||
            !std::isfinite(rig_motion.max_tilt_degrees) || rig_motion.max_tilt_degrees < 0 ||
            rig_motion.max_tilt_degrees > 45 || !std::isfinite(rig_motion.click_travel) ||
            std::abs(rig_motion.click_travel) > 0.1F) {
            error = QStringLiteral("Rig part %1 has out-of-range mechanics").arg(node_name);
            return {};
        }
        if (rig_motion.control == gamepad::Control::Count) {
            warnings.push_back(QStringLiteral("Rig part %1 has no canonical control binding").arg(node_name));
            continue;
        }
        MarkSupported(*model, rig_motion.control);
        MarkSupported(*model, rig_motion.click);
        node.motion = rig_motion;
    }
    return std::make_shared<const ControllerModel>(std::move(*model));
}

std::shared_ptr<const ControllerModel> SharedControllerModel(const QString& pack_directory, QStringList& warnings,
                                                             QString& error) {
    static QMutex mutex;
    static std::map<QString, std::pair<std::shared_ptr<const ControllerModel>, QStringList>> cache;
    const QMutexLocker lock(&mutex);
    if (const auto found = cache.find(pack_directory); found != cache.end()) {
        warnings += found->second.second;
        return found->second.first;
    }
    QStringList loaded_warnings;
    auto model = LoadControllerModel(pack_directory, loaded_warnings, error);
    if (model)
        cache.emplace(pack_directory, std::pair{model, loaded_warnings});
    warnings += loaded_warnings;
    return model;
}

} // namespace exosnap::quick
