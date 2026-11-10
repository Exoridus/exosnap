#include "ControllerMechanics.h"
#include "ControllerModel.h"
#include "ControllerState.h"
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <gtest/gtest.h>
#include <limits>
#include <memory>

using namespace exosnap::quick;
using namespace exosnap::gamepad;

namespace {
ControlValue Active(float x, float y = 0) {
    return {ControlStatus::Active, x, y};
}

const QString kPacks = QStringLiteral(EXOSNAP_GAMEPAD_ASSET_DIR "/controllers/");

QGuiApplication& App() {
    static int argc = 1;
    static char name[] = "controller3d-tests";
    static char* argv[]{name, nullptr};
    static QGuiApplication application(argc, argv);
    return application;
}

void Append(QByteArray& bytes, uint32_t value) {
    bytes.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

QByteArray Glb(const QJsonObject& json, const QByteArray& bin) {
    auto text = QJsonDocument(json).toJson(QJsonDocument::Compact);
    while (text.size() % 4 != 0)
        text.append(' ');
    auto payload = bin;
    while (payload.size() % 4 != 0)
        payload.append('\0');
    QByteArray glb;
    Append(glb, 0x46546C67);
    Append(glb, 2);
    Append(glb, static_cast<uint32_t>(28 + text.size() + payload.size()));
    Append(glb, static_cast<uint32_t>(text.size()));
    Append(glb, 0x4E4F534A);
    glb += text;
    Append(glb, static_cast<uint32_t>(payload.size()));
    Append(glb, 0x004E4942);
    glb += payload;
    return glb;
}

// One triangle: float3 positions and normals, then three uint16 indices.
struct Triangle {
    QJsonObject json;
    QByteArray bin;
};

Triangle MakeTriangle() {
    const float positions[]{0, 0, 0, 1, 0, 0, 0, 1, 0};
    const float normals[]{0, 0, 1, 0, 0, 1, 0, 0, 1};
    const uint16_t indices[]{0, 1, 2, 0};
    Triangle triangle;
    triangle.bin.append(reinterpret_cast<const char*>(positions), sizeof(positions));
    triangle.bin.append(reinterpret_cast<const char*>(normals), sizeof(normals));
    triangle.bin.append(reinterpret_cast<const char*>(indices), sizeof(indices));
    const auto json = QStringLiteral(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
        "nodes":[{"name":"root","children":[1]},{"name":"part","translation":[0,1,0],"mesh":0}],
        "meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1},"indices":2,"material":0}]}],
        "materials":[{"pbrMetallicRoughness":{"baseColorFactor":[1,0,0,1]}}],
        "buffers":[{"byteLength":80}],
        "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":36},
                       {"buffer":0,"byteOffset":72,"byteLength":6}],
        "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
                     {"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},
                     {"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"}]})");
    triangle.json = QJsonDocument::fromJson(json.toUtf8()).object();
    return triangle;
}

std::shared_ptr<const ControllerModel> LoadPack(const QString& id) {
    QStringList warnings;
    QString error;
    auto model = LoadControllerModel(kPacks + id, warnings, error);
    EXPECT_TRUE(model) << id.toStdString() << ": " << error.toStdString();
    EXPECT_TRUE(warnings.isEmpty()) << id.toStdString() << ": " << warnings.join(QStringLiteral("; ")).toStdString();
    return model;
}

std::vector<ControllerPartModel::Row> RowsFor(const ControllerState& state, const QString& node) {
    std::vector<ControllerPartModel::Row> result;
    const auto& nodes = state.Model().nodes;
    for (const auto& row : state.Parts().Rows())
        for (int index = row.node; index >= 0; index = nodes[static_cast<size_t>(index)].parent)
            if (nodes[static_cast<size_t>(index)].name == node) {
                result.push_back(row);
                break;
            }
    return result;
}

GamepadReading ConnectedDualSense() {
    GamepadReading reading;
    reading.capture_100ns = 10;
    reading.device.vendor_id = 0x054C;
    reading.device.product_id = 0x0CE6;
    reading.device.layout = LayoutKind::DualSense;
    reading.state.controls_normalized = true;
    for (auto& control : reading.state.controls)
        control = {ControlStatus::Released, 0, 0};
    return reading;
}
} // namespace

TEST(Controller3D, RadialResponseDoesNotApplyThe2DDeadzone) {
    EXPECT_FLOAT_EQ(PoseStick(Active(.05F), {}).amount, 0);
    EXPECT_NEAR(PoseStick(Active(.06F), {}).amount, 1.0F / 90, .000001F);
    EXPECT_NEAR(PoseStick(Active(.50F), {}).amount, .5F, .000001F);
    EXPECT_FLOAT_EQ(PoseStick(Active(.95F), {}).amount, 1);
    EXPECT_FLOAT_EQ(PoseStick(Active(1, 1), {}).amount, 1);
}

TEST(Controller3D, CardinalAndDiagonalHaveEqualTiltAndNativeYPointsForward) {
    const auto cardinal = PoseStick(Active(1), {}).rotation.rotatedVector({0, 1, 0});
    const auto diagonal = PoseStick(Active(1, 1), {}).rotation.rotatedVector({0, 1, 0});
    const auto up = PoseStick(Active(0, 1), {}).rotation.rotatedVector({0, 1, 0});
    EXPECT_NEAR(cardinal.y(), .9702957F, .000001F);
    EXPECT_NEAR(cardinal.x(), .2419219F, .000001F);
    EXPECT_NEAR(diagonal.y(), cardinal.y(), .000001F);
    EXPECT_NEAR(diagonal.x(), -diagonal.z(), .000001F);
    EXPECT_LT(up.z(), 0);
}

TEST(Controller3D, ClickIsIndependentAndTravelsAlongTiltedLocalAxis) {
    const auto pose = PoseStick(Active(1), Active(1));
    EXPECT_FLOAT_EQ(pose.click, .008F);
    const auto travel = pose.rotation.rotatedVector({0, -pose.click, 0});
    EXPECT_LT(travel.x(), 0);
    EXPECT_NEAR(travel.length(), .008F, .000001F);
    EXPECT_TRUE(PoseStick({}, Active(1)).rotation.isIdentity());
}

TEST(Controller3D, InvalidAndUnavailableValuesDriveNeutralTransforms) {
    for (const auto status : {ControlStatus::Unsupported, ControlStatus::Unavailable}) {
        const auto pose = PoseStick({status, 1, 1}, {status, 1, 0});
        EXPECT_TRUE(pose.rotation.isIdentity());
        EXPECT_EQ(pose.click, 0);
    }
    EXPECT_TRUE(PoseStick(Active(std::numeric_limits<float>::quiet_NaN()), {}).rotation.isIdentity());
    EXPECT_TRUE(PoseStick(Active(0, std::numeric_limits<float>::infinity()), {}).rotation.isIdentity());
    EXPECT_TRUE(std::isfinite(PoseStick(Active(std::numeric_limits<float>::max(), 1), {}).rotation.scalar()));
}

TEST(Controller3D, DisconnectAndNullMappingClearHeldControlsWithoutErasingStatus) {
    GamepadReading reading;
    reading.state.controls_normalized = true;
    reading.state.controls[static_cast<size_t>(Control::LeftStick)] = Active(.06F, .02F);
    auto value = ReadPilotControl(reading, Control::LeftStick, true);
    EXPECT_FLOAT_EQ(value.x, .06F);
    EXPECT_FLOAT_EQ(value.y, .02F);
    EXPECT_GT(PoseStick(value, {}).amount, 0);
    EXPECT_EQ(ReadPilotControl(reading, Control::LeftStick, false).status, ControlStatus::Unsupported);
    reading.connected = false;
    value = ReadPilotControl(reading, Control::LeftStick, true);
    EXPECT_EQ(value.status, ControlStatus::Unavailable);
    EXPECT_EQ(value.x, 0);
    EXPECT_EQ(ReadPilotControl(std::nullopt, Control::LeftStick, true).status, ControlStatus::Unavailable);
}

TEST(Controller3D, GlbReaderAcceptsTheStrictSubsetAndConvertsLinearColor) {
    QString error;
    const auto triangle = MakeTriangle();
    const auto model = ParseControllerGlb(Glb(triangle.json, triangle.bin), error);
    ASSERT_TRUE(model) << error.toStdString();
    ASSERT_EQ(model->nodes.size(), 2u);
    EXPECT_EQ(model->nodes[1].parent, 0);
    EXPECT_EQ(model->nodes[1].translation, QVector3D(0, 1, 0));
    ASSERT_EQ(model->meshes.size(), 1u);
    EXPECT_EQ(model->meshes[0].indices.size(), 3 * static_cast<qsizetype>(sizeof(uint32_t)));
    EXPECT_EQ(model->meshes[0].maximum, QVector3D(1, 1, 0));
    EXPECT_EQ(model->meshes[0].color, QColor::fromRgbF(1, 0, 0));
}

TEST(Controller3D, GlbReaderRejectsCorruptOrUnsupportedModels) {
    const auto triangle = MakeTriangle();
    const auto rejects = [&](const QByteArray& glb, const char* what) {
        QString error;
        EXPECT_FALSE(ParseControllerGlb(glb, error)) << what;
        EXPECT_FALSE(error.isEmpty()) << what;
    };
    const auto valid = Glb(triangle.json, triangle.bin);
    rejects(valid.left(valid.size() - 4), "truncated");
    rejects(QByteArray(valid).replace(0, 4, "glTX"), "magic");
    const auto edit = [&](auto change) {
        auto json = triangle.json;
        change(json);
        return Glb(json, triangle.bin);
    };
    rejects(edit([](QJsonObject& json) { json["extensionsRequired"] = QJsonArray{"KHR_draco_mesh_compression"}; }),
            "required extension");
    rejects(edit([](QJsonObject& json) {
                auto accessors = json["accessors"].toArray();
                auto positions = accessors[0].toObject();
                positions["count"] = 4;
                accessors[0] = positions;
                json["accessors"] = accessors;
            }),
            "accessor beyond view");
    rejects(edit([](QJsonObject& json) {
                auto nodes = json["nodes"].toArray();
                auto part = nodes[1].toObject();
                part["scale"] = QJsonArray{1, 2, 1};
                nodes[1] = part;
                json["nodes"] = nodes;
            }),
            "non-uniform scale");
    rejects(edit([](QJsonObject& json) {
                auto nodes = json["nodes"].toArray();
                auto part = nodes[1].toObject();
                part["children"] = QJsonArray{0};
                nodes[1] = part;
                json["nodes"] = nodes;
            }),
            "cycle");
    auto bin = triangle.bin;
    const uint16_t out_of_range = 7;
    std::memcpy(bin.data() + 72, &out_of_range, sizeof(out_of_range));
    rejects(Glb(triangle.json, bin), "index beyond vertices");
}

TEST(Controller3D, EveryControllerLabPackBindsItsRigToCanonicalControls) {
    for (const auto* id : {"dualsense", "dualshock4", "gamecube", "generic", "joycon_grip", "joycon_left",
                           "joycon_right", "switch_pro", "wavebird", "xbox_360", "xbox_one"}) {
        SCOPED_TRACE(id);
        const auto model = LoadPack(QString::fromLatin1(id));
        ASSERT_TRUE(model);
        size_t motions = 0;
        for (const auto& node : model->nodes)
            if (node.motion) {
                ++motions;
                EXPECT_NE(node.motion->control, Control::Count);
            }
        EXPECT_GE(motions, 6u);
        EXPECT_TRUE(
            std::any_of(model->nodes.begin(), model->nodes.end(), [](const auto& node) { return node.housing; }));
        EXPECT_TRUE(model->supported[static_cast<size_t>(Control::LeftStick)] ||
                    model->supported[static_cast<size_t>(Control::RightStick)]);
    }
}

TEST(Controller3D, DualSenseRigAnimatesOnlyItsOwnControlsAndClearsOnDisconnect) {
    App();
    const QString pack = kPacks + QStringLiteral("dualsense");
    const auto model = LoadPack(QStringLiteral("dualsense"));
    ASSERT_TRUE(model);
    QString error;
    const auto mapping = ControllerPressMapping(pack, error);
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    ControllerState state(model, mapping);
    EXPECT_FALSE(state.Supported(Control::Misc2));
    EXPECT_GT(state.fitWidth(), state.fitHeight());
    const auto rest_button = RowsFor(state, QStringLiteral("part__a_button"));
    const auto rest_trigger = RowsFor(state, QStringLiteral("part__left_trigger"));
    const auto rest_cap = RowsFor(state, QStringLiteral("part__left_cap"));
    ASSERT_FALSE(rest_button.empty());
    ASSERT_FALSE(rest_trigger.empty());
    ASSERT_FALSE(rest_cap.empty());

    auto reading = ConnectedDualSense();
    reading.state.controls[static_cast<size_t>(Control::South)] = Active(1);
    reading.state.controls[static_cast<size_t>(Control::LeftTrigger)] = Active(1);
    reading.state.controls[static_cast<size_t>(Control::LeftStick)] = Active(1, 0);
    reading.state.controls[static_cast<size_t>(Control::LeftStickClick)] = Active(1);
    reading.state.controls[static_cast<size_t>(Control::Misc2)] = Active(1);
    state.ApplyReading(reading, 20);
    EXPECT_TRUE(state.connected());
    const auto button = RowsFor(state, QStringLiteral("part__a_button")).front();
    EXPECT_TRUE(button.active);
    EXPECT_EQ(button.press_color, QColor(QStringLiteral("#4cb8ff")));
    EXPECT_NEAR((button.position - rest_button.front().position).length(), 0.008F * 1.95F, 1e-4F);
    const auto trigger = RowsFor(state, QStringLiteral("part__left_trigger")).front();
    EXPECT_TRUE(trigger.active);
    EXPECT_NEAR(std::abs(QQuaternion::dotProduct(trigger.rotation, rest_trigger.front().rotation)),
                std::cos(16.0F * 3.14159265F / 360), 1e-4F);
    const auto cap = RowsFor(state, QStringLiteral("part__left_cap")).front();
    EXPECT_TRUE(cap.active);
    EXPECT_FALSE(qFuzzyCompare(cap.rotation, rest_cap.front().rotation));
    EXPECT_FALSE(RowsFor(state, QStringLiteral("part__right_cap")).front().active);
    for (const auto& row : RowsFor(state, QStringLiteral("part__top_shell")))
        EXPECT_FALSE(row.active);
    const auto generation = state.poseGeneration();
    state.ApplyReading(reading, 30);
    EXPECT_EQ(state.poseGeneration(), generation);

    reading.connected = false;
    state.ApplyReading(reading, 40);
    EXPECT_FALSE(state.connected());
    EXPECT_EQ(RowsFor(state, QStringLiteral("part__a_button")).front().position, rest_button.front().position);
    EXPECT_EQ(RowsFor(state, QStringLiteral("part__left_cap")).front().rotation, rest_cap.front().rotation);
    EXPECT_TRUE(std::none_of(state.Parts().Rows().begin(), state.Parts().Rows().end(),
                             [](const auto& row) { return row.active; }));
}

TEST(Controller3D, OnlyQualifiedDevicesReceiveANativeModel) {
    DeviceIdentity device;
    device.vendor_id = 0x054C;
    device.product_id = 0x0CE6;
    device.layout = LayoutKind::DualSense;
    EXPECT_EQ(NativeControllerPack(device), QStringLiteral(":/gamepad/controllers/dualsense"));
    device.product_id = 0x0DF2;
    EXPECT_TRUE(NativeControllerPack(device).isEmpty());
    device.vendor_id = 0x045E;
    device.product_id = 0x0B12;
    device.layout = LayoutKind::XboxSeries;
    EXPECT_TRUE(NativeControllerPack(device).isEmpty());
}
