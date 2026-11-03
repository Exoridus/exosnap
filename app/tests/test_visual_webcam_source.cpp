#include "extensions/WebcamModule.h"

#include <gtest/gtest.h>

namespace exosnap {
namespace {

constexpr uint8_t Consumer(VisualConsumer consumer) {
    return static_cast<uint8_t>(consumer);
}

class WebcamSourceTest : public testing::Test {
  protected:
    engine::SourceToken token{{engine::VisualSourceId{{1}}, engine::VisualRuntimeGeneration{1}},
                              engine::SourceRequestRevision{1}};
    int starts = 0, updates = 0, stops = 0;
    std::vector<engine::SourceToken> verification_requests;
    std::string resolved_device = "camera";
    bool verification = false, quiescent = true, session_owns_device = false;
    WebcamSettings last;
    std::function<void(SourceStatus)> completion;
    WebcamControlHooks Hooks() {
        return {
            [this](const toml::table& table) -> engine::Outcome<WebcamSettings> {
                WebcamSettings value;
                value.enabled = true;
                value.device_id = resolved_device;
                value.width = static_cast<int>(table["width"].value_or(int64_t{1280}));
                return value;
            },
            [this](engine::SourceToken, WebcamSettings settings, engine::VisualPublicationWriter,
                   std::function<void(SourceStatus)> done) {
                ++starts;
                last = std::move(settings);
                completion = std::move(done);
                quiescent = false;
            },
            [this](engine::SourceToken, WebcamSettings settings, engine::VisualPublicationWriter) {
                ++updates;
                last = std::move(settings);
            },
            [this] { ++stops; },
            [this] { return quiescent; },
            [this] { return verification; },
            [this] { return session_owns_device; },
            [this](engine::SourceToken token, engine::VisualPublicationWriter, std::function<void(SourceStatus)> done) {
                verification_requests.push_back(token);
                completion = std::move(done);
            }};
    }
    SourceRequest Request(uint8_t demand = Consumer(VisualConsumer::RecordPreview)) const {
        return {token, engine::ConfigRevision{1}, {demand}, {}};
    }
    SourceCreateRequest Creation() const {
        return {token, {}, {}, {}};
    }
};

TEST_F(WebcamSourceTest, NoCameraAtDiscoveryOrConstruction) {
    CapabilityRegistry registry;
    int enumeration_calls = 0;
    ASSERT_TRUE(RegisterWebcamModule(registry, Hooks(), [&](const VisualSourceRecord&) {
                    ++enumeration_calls;
                    return ChoiceSnapshot{};
                }).succeeded());
    ASSERT_TRUE(registry.Seal().succeeded());
    auto source = registry.FindVisualSource("org.exosnap.webcam/camera")->create(Creation());
    EXPECT_TRUE(source.succeeded());
    EXPECT_EQ(starts, 0);
    EXPECT_EQ(stops, 0);
    EXPECT_EQ(enumeration_calls, 0);
}

TEST_F(WebcamSourceTest, InertFactoryIsQuiescentEvenWhenItsSharedBackendHasOtherWork) {
    quiescent = false;
    WebcamSource source(Creation(), Hooks());
    EXPECT_TRUE(source.IsQuiescent());
    source.RequestStop();
    EXPECT_TRUE(source.IsQuiescent());
    EXPECT_EQ(stops, 0);
}

TEST_F(WebcamSourceTest, OneCameraForAllDemandAndRepeatedApply) {
    WebcamSource source(Creation(), Hooks());
    auto request = Request();
    ASSERT_TRUE(source.Apply(request).succeeded());
    ASSERT_TRUE(source.Apply(request).succeeded());
    request.demand.consumers = 7;
    ASSERT_TRUE(source.Apply(request).succeeded());
    EXPECT_EQ(starts, 1);
    EXPECT_EQ(updates, 0);
    EXPECT_EQ(stops, 0);
}

TEST_F(WebcamSourceTest, LiveControlsRetainAcceptedPixelsButDeviceAndModeChangesDoNot) {
    WebcamSource source(Creation(), Hooks());
    auto request = Request();
    EXPECT_FALSE(source.RetainsAccepted(request.validated_settings));
    ASSERT_TRUE(source.Apply(request).succeeded());
    auto controls = request.validated_settings;
    controls.insert("controls", toml::table{{"brightness", toml::table{{"auto", false}, {"value", 10}}}});
    EXPECT_TRUE(source.RetainsAccepted(controls));
    auto device = controls;
    device.insert("device_id", "other camera");
    EXPECT_FALSE(source.RetainsAccepted(device));
    auto mode = controls;
    mode.insert("width", 640);
    EXPECT_FALSE(source.RetainsAccepted(mode));
    source.RequestStop();
    EXPECT_FALSE(source.RetainsAccepted(controls));
}

TEST_F(WebcamSourceTest, ClosingSettingsKeepsRecordingCapture) {
    WebcamSource source(Creation(), Hooks());
    auto request = Request(Consumer(VisualConsumer::Recording) | Consumer(VisualConsumer::SettingsPreview));
    ASSERT_TRUE(source.Apply(request).succeeded());
    request.demand.consumers = Consumer(VisualConsumer::Recording);
    ASSERT_TRUE(source.Apply(request).succeeded());
    EXPECT_EQ(starts, 1);
    EXPECT_EQ(stops, 0);
}

TEST_F(WebcamSourceTest, VerificationSourceNeverOpensHardware) {
    verification = true;
    WebcamSource source(Creation(), Hooks());
    ASSERT_TRUE(source.Apply(Request(7)).succeeded());
    EXPECT_EQ(starts, 0);
    ASSERT_EQ(verification_requests.size(), 1u);
    ASSERT_TRUE(source.Apply(Request(7)).succeeded());
    EXPECT_EQ(verification_requests.size(), 1u);
    auto request = Request(7);
    request.token.request.value++;
    ASSERT_TRUE(source.Apply(request).succeeded());
    EXPECT_EQ(verification_requests.size(), 2u);
    EXPECT_FALSE(source.Apply(Request(7)).succeeded());
    EXPECT_EQ(verification_requests.size(), 2u);
}

TEST_F(WebcamSourceTest, ModeChangesRemainNextSession) {
    session_owns_device = true;
    WebcamSource source(Creation(), Hooks());
    auto request = Request(Consumer(VisualConsumer::Recording));
    ASSERT_TRUE(source.Apply(request).succeeded());
    request.token.request.value++;
    request.configuration.value++;
    request.validated_settings.insert("width", 640);
    ASSERT_TRUE(source.Apply(request).succeeded());
    EXPECT_EQ(starts, 1);
    EXPECT_EQ(last.width, 1280);
    session_owns_device = false;
    request.demand.consumers = Consumer(VisualConsumer::RecordPreview);
    EXPECT_FALSE(source.Apply(request).succeeded());
    request.token.request.value++;
    ASSERT_TRUE(source.Apply(request).succeeded());
    EXPECT_EQ(starts, 2);
    EXPECT_EQ(last.width, 640);
}

TEST_F(WebcamSourceTest, DisableEditAndReenableKeepsTheSessionMode) {
    WebcamSource source(Creation(), Hooks());
    auto request = Request(Consumer(VisualConsumer::Recording));
    session_owns_device = true;
    ASSERT_TRUE(source.Apply(request).succeeded());
    request.demand.consumers = 0;
    ASSERT_TRUE(source.Apply(request).succeeded());
    request.token.request.value++;
    request.validated_settings.insert("width", 640);
    ASSERT_TRUE(source.Apply(request).succeeded());
    request.token.request.value++;
    request.demand.consumers = Consumer(VisualConsumer::Recording);
    ASSERT_TRUE(source.Apply(request).succeeded());
    EXPECT_EQ(starts, 2);
    EXPECT_EQ(last.width, 1280);
    session_owns_device = false;
    EXPECT_FALSE(source.Apply(request).succeeded());
    request.token.request.value++;
    ASSERT_TRUE(source.Apply(request).succeeded());
    EXPECT_EQ(starts, 3);
    EXPECT_EQ(last.width, 640);
}

TEST_F(WebcamSourceTest, CachedAutomaticDeviceChangeRequiresANewToken) {
    WebcamSource source(Creation(), Hooks());
    auto request = Request();
    ASSERT_TRUE(source.Apply(request).succeeded());
    resolved_device = "other camera";
    EXPECT_FALSE(source.Apply(request).succeeded());
    EXPECT_EQ(starts, 1);
    request.token.request.value++;
    ASSERT_TRUE(source.Apply(request).succeeded());
    EXPECT_EQ(starts, 2);
}
TEST_F(WebcamSourceTest, OldRequestCannotRestartSource) {
    WebcamSource source(Creation(), Hooks());
    auto old = Request();
    auto current = old;
    current.token.request.value++;
    ASSERT_TRUE(source.Apply(current).succeeded());
    EXPECT_FALSE(source.Apply(old).succeeded());
    source.RequestStop();
    EXPECT_FALSE(source.Apply(current).succeeded());
    EXPECT_EQ(starts, 1);
    EXPECT_EQ(stops, 1);
    EXPECT_FALSE(source.IsQuiescent());
    quiescent = true;
    EXPECT_TRUE(source.IsQuiescent());
}

TEST_F(WebcamSourceTest, ChangedSettingsCannotReuseAToken) {
    WebcamSource source(Creation(), Hooks());
    auto request = Request();
    ASSERT_TRUE(source.Apply(request).succeeded());
    request.validated_settings.insert("width", 640);
    EXPECT_FALSE(source.Apply(request).succeeded());
    EXPECT_EQ(starts, 1);
}

TEST_F(WebcamSourceTest, ZeroDemandNeedsANewTokenBeforeReopening) {
    WebcamSource source(Creation(), Hooks());
    auto request = Request();
    ASSERT_TRUE(source.Apply(request).succeeded());
    request.demand.consumers = 0;
    ASSERT_TRUE(source.Apply(request).succeeded());
    request.demand.consumers = 1;
    EXPECT_FALSE(source.Apply(request).succeeded());
    request.token.request.value++;
    ASSERT_TRUE(source.Apply(request).succeeded());
    EXPECT_EQ(starts, 2);
}

} // namespace
} // namespace exosnap
