#include "benchmark/BenchmarkEffectiveConfig.h"

#include <array>
#include <exosnap/engine/recorder_session.h>
#include <exosnap/engine/visual_scene_runtime.h>
#include <gtest/gtest.h>

namespace exosnap::benchmark {
namespace {

TEST(BenchmarkEffectiveConfig, AudioCountUsesResolvedTracksAndHonorsDisabledAudio) {
    engine::RecorderConfig config;
    config.record_audio = true;
    config.resolved_audio_plan.emplace();
    config.resolved_audio_plan->tracks.resize(8);
    EXPECT_TRUE(DescribeEffectiveConfig(config).fields.contains(QStringLiteral("audio_track_count=8")));
    config.record_audio = false;
    EXPECT_TRUE(DescribeEffectiveConfig(config).fields.contains(QStringLiteral("audio_track_count=0")));
}

TEST(BenchmarkEffectiveConfig, LegacyAudioRetainsImplicitSingleTrackAndExplicitPlan) {
    engine::RecorderConfig config;
    config.record_audio = true;
    EXPECT_TRUE(DescribeEffectiveConfig(config).fields.contains(QStringLiteral("audio_track_count=1")));
    config.audio_track_plan.tracks.resize(2);
    EXPECT_TRUE(DescribeEffectiveConfig(config).fields.contains(QStringLiteral("audio_track_count=2")));
}

TEST(BenchmarkEffectiveConfig, AbsentAndEmptyScenesAreAvailableButUnpublishedSceneIsUnavailable) {
    engine::RecorderConfig config;
    EXPECT_TRUE(DescribeEffectiveConfig(config).available);
    config.visual_scene = std::make_shared<engine::VisualSceneRuntime>(engine::VisualBudgetPolicy{});
    const auto unpublished = DescribeEffectiveConfig(config);
    EXPECT_FALSE(unpublished.available);
    EXPECT_TRUE(unpublished.fingerprint.isEmpty());
    engine::PreparedSceneDescription scene;
    scene.revision = engine::ConfigRevision{1};
    ASSERT_TRUE(config.visual_scene->PublishScene(scene).succeeded());
    const auto empty = DescribeEffectiveConfig(config);
    EXPECT_TRUE(empty.available);
    EXPECT_FALSE(empty.fingerprint.isEmpty());
    EXPECT_TRUE(empty.fields.contains(QStringLiteral("visual_item_count=0")));
    EXPECT_EQ(empty.fingerprint, DescribeEffectiveConfig(config).fingerprint);
}

TEST(BenchmarkEffectiveConfig, SampledFactsIgnoreRuntimeAndSourceIdentityButTrackPresentation) {
    using namespace engine;
    std::array<RecorderConfig, 2> configs;
    std::array<PreparedSceneDescription, 2> scenes;
    for (size_t index = 0; index < configs.size(); ++index) {
        auto& runtime = configs[index].visual_scene;
        runtime = std::make_shared<VisualSceneRuntime>(VisualBudgetPolicy{});
        const VisualAddress address{NewId<VisualSourceId>(), VisualRuntimeGeneration{index + 1}};
        const SourceToken token{address, SourceRequestRevision{index + 1}};
        auto writer = runtime->CreateSource(address);
        ASSERT_TRUE(writer.succeeded());
        runtime->SetRequest(token, true, HoldPolicy::ClearAccepted);
        ASSERT_TRUE(writer.value()->PrepareImage(token, {2, 2}).succeeded());
        auto& scene = scenes[index];
        scene.revision = ConfigRevision{index + 1};
        scene.binding_sources = {address};
        PreparedSceneItem item;
        item.id = NewId<SceneItemId>();
        item.source = address;
        item.properties.placement = {0.1F, 0.2F, 0.25F, 0.25F};
        scene.items.push_back(item);
        ASSERT_TRUE(runtime->PublishScene(scene).succeeded());
    }
    const auto first = DescribeEffectiveConfig(configs[0]);
    const auto equivalent = DescribeEffectiveConfig(configs[1]);
    ASSERT_TRUE(first.available);
    ASSERT_TRUE(equivalent.available);
    EXPECT_EQ(first.fields, equivalent.fields);
    EXPECT_EQ(first.fingerprint, equivalent.fingerprint);

    scenes[0].revision = ConfigRevision{7};
    ASSERT_TRUE(configs[0].visual_scene->PublishScene(scenes[0]).succeeded());
    EXPECT_EQ(first.fingerprint, DescribeEffectiveConfig(configs[0]).fingerprint);
    scenes[0].items[0].properties.opacity = 0.5F;
    ASSERT_TRUE(configs[0].visual_scene->PublishScene(scenes[0]).succeeded());
    const auto changed = DescribeEffectiveConfig(configs[0]);
    ASSERT_TRUE(changed.available);
    EXPECT_NE(first.fingerprint, changed.fingerprint);
}
} // namespace
} // namespace exosnap::benchmark
