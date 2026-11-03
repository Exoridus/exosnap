#include "ControllerGeometry.h"
#include "ControllerMechanics.h"
#include "ControllerState.h"
#include "GamepadOverlay.h"
#include <QCoreApplication>

#include <cmath>
#include <cstring>
#include <gtest/gtest.h>
#include <limits>

using namespace exosnap::quick;
using namespace exosnap::gamepad;

namespace {
ControlValue Active(float x, float y = 0) {
    return {ControlStatus::Active, x, y};
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

TEST(Controller3D, InvalidDimensionsAreRejectedBeforeAllocation) {
    const StickDimensions defaults;
    EXPECT_TRUE(ValidStickDimensions(defaults));
    for (const float bad :
         {0.0F, -1.0F, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        auto dimensions = defaults;
        dimensions.cap_radius = bad;
        EXPECT_FALSE(ValidStickDimensions(dimensions));
    }
    auto dimensions = defaults;
    dimensions.shaft_height = .10F;
    EXPECT_FALSE(ValidStickDimensions(dimensions));
    dimensions = defaults;
    dimensions.ring_thickness = .03F;
    EXPECT_FALSE(ValidStickDimensions(dimensions));
    dimensions = defaults;
    dimensions.boot_radius = .17F;
    EXPECT_FALSE(ValidStickDimensions(dimensions));
    dimensions = defaults;
    dimensions.ring_inner_radius = .115F;
    EXPECT_FALSE(ValidStickDimensions(dimensions));
    dimensions.shaft_radius = .10F;
    dimensions.ring_inner_radius = .11F;
    EXPECT_FALSE(ValidStickDimensions(dimensions));
}

TEST(Controller3D, GeometryIsFiniteIndexedAndUnchangedByHeldPoses) {
    ControllerGeometry geometry;
    for (const auto part : {ControllerGeometry::Stem, ControllerGeometry::Rim, ControllerGeometry::Cap,
                            ControllerGeometry::Well, ControllerGeometry::Shell}) {
        geometry.setPart(part);
        ASSERT_GT(geometry.vertexData().size(), 0);
        ASSERT_GT(geometry.indexData().size(), 0);
        const auto vertex_data = geometry.vertexData();
        for (qsizetype offset = 0; offset < vertex_data.size(); offset += sizeof(float)) {
            float coordinate = 0;
            std::memcpy(&coordinate, vertex_data.constData() + offset, sizeof(float));
            ASSERT_TRUE(std::isfinite(coordinate));
        }
        const auto index_data = geometry.indexData();
        for (qsizetype offset = 0; offset < index_data.size(); offset += sizeof(quint32)) {
            quint32 index = 0;
            std::memcpy(&index, index_data.constData() + offset, sizeof(quint32));
            ASSERT_LT(index, vertex_data.size() / geometry.stride());
        }
        EXPECT_GT(geometry.boundsMax().y(), geometry.boundsMin().y());
        const auto generation = geometry.generation();
        const auto bytes = geometry.vertexData();
        for (int frame = 0; frame < 3600; ++frame) {
            const auto pose = PoseStick(Active(1, 1), Active(1));
            EXPECT_GT(pose.amount, 0);
            geometry.setPart(part);
            EXPECT_TRUE(geometry.SetDimensions({}));
        }
        EXPECT_EQ(geometry.generation(), generation);
        EXPECT_EQ(geometry.vertexData(), bytes);
        auto invalid = StickDimensions{};
        invalid.cap_radius = std::numeric_limits<float>::quiet_NaN();
        EXPECT_FALSE(geometry.SetDimensions(invalid));
        EXPECT_EQ(geometry.vertexData(), bytes);
    }
}

TEST(Controller3D, ConcaveShellCapsHaveOutwardNondegenerateTriangles) {
    ControllerGeometry geometry;
    geometry.setPart(ControllerGeometry::Shell);
    const auto vertices = geometry.vertexData();
    const auto indices = geometry.indexData();
    for (qsizetype offset = 0; offset < indices.size(); offset += 3 * sizeof(quint32)) {
        std::array<QVector3D, 3> triangle;
        for (int i = 0; i < 3; ++i) {
            quint32 index = 0;
            std::memcpy(&index, indices.constData() + offset + i * sizeof(quint32), sizeof(index));
            std::memcpy(&triangle[static_cast<size_t>(i)], vertices.constData() + index * geometry.stride(),
                        sizeof(QVector3D));
        }
        const auto normal = QVector3D::crossProduct(triangle[1] - triangle[0], triangle[2] - triangle[0]);
        if (triangle[0].y() == .23F && triangle[1].y() == .23F && triangle[2].y() == .23F)
            EXPECT_GT(normal.y(), 0);
        if (triangle[0].y() == -.08F && triangle[1].y() == -.08F && triangle[2].y() == -.08F)
            EXPECT_LT(normal.y(), 0);
    }
}

TEST(Controller3D, SharedOwnerConsumerClearsHeldPoseAndHonorsNullBindings) {
    int argc = 1;
    char name[] = "controller3d-tests";
    char* argv[]{name, nullptr};
    QCoreApplication application(argc, argv);
    MappedGamepadArtwork mapping;
    mapping.controls.insert("left", {Control::LeftStick, Control::LeftStickClick, true});
    mapping.controls.insert("right", {Control::RightStick, Control::RightStickClick, false});
    ControllerState state(mapping);
    GamepadReading reading;
    reading.capture_100ns = 10;
    reading.state.left_x = .06F;
    reading.state.right_x = 1;
    reading.state.controls_normalized = true;
    reading.state.controls[static_cast<size_t>(Control::LeftStick)] = Active(.06F);
    reading.state.controls[static_cast<size_t>(Control::LeftStickClick)] = Active(1);
    reading.state.controls[static_cast<size_t>(Control::RightStick)] = Active(1);
    state.ApplyReading(reading, 20);
    EXPECT_FALSE(state.leftRotation().isIdentity());
    EXPECT_TRUE(state.rightRotation().isIdentity());
    EXPECT_FLOAT_EQ(state.leftClick(), .008F);
    EXPECT_EQ(state.inputTime(), 10);
    EXPECT_EQ(state.frameTime(), 20);
    const auto generation = state.poseGeneration();
    state.ApplyReading(reading, 30);
    EXPECT_EQ(state.poseGeneration(), generation);
    GamepadOverlay owner;
    state.Sample(owner, 40);
    EXPECT_TRUE(state.leftRotation().isIdentity());
    EXPECT_FLOAT_EQ(state.leftClick(), 0);
    EXPECT_FALSE(state.connected());
}

TEST(Controller3D, FrontButtonsRemainAboveShellThroughoutPressAndShouldersHaveLocalAxes) {
    ASSERT_GE(DualSenseButtons().size(), 16);
    for (const auto& button : DualSenseButtons()) {
        if (button.control == Control::Back || button.control == Control::Start) {
            for (int step = 0; step <= 100; ++step) {
                const float visible_surface =
                    button.position.y() + button.size.y() / 2 + button.press_axis.y() * button.travel * step / 100;
                EXPECT_GT(visible_surface - .23F, .02F);
            }
        }
        if (button.control == Control::LeftShoulder || button.control == Control::RightShoulder) {
            EXPECT_NEAR(button.press_axis.length(), 1, .000001F);
            EXPECT_GT(button.press_axis.z(), 0);
            EXPECT_LT(button.travel, .014F);
        }
    }
}
