#include <gtest/gtest.h>

#include "recorder_core/pipeline_health.h"

#include <vector>

namespace {

using namespace recorder_core;

// 60 fps budget = 16.667 ms.
constexpr double kBudget = 1000.0 / 60.0;

StageSignals Duration(StageId id, double avg_ms, bool can_bottleneck = true, double budget_ms = 0.0) {
    StageSignals s;
    s.id = id;
    s.available = true;
    s.is_duration_stage = true;
    s.can_bottleneck = can_bottleneck;
    s.avg_ms = avg_ms;
    s.budget_ms = budget_ms;
    s.fps_ratio = 1.0;
    return s;
}

StageSignals Capture(double fps_ratio, uint32_t recent_drops = 0) {
    StageSignals s;
    s.id = StageId::SourceCapture;
    s.available = true;
    s.is_duration_stage = false;
    s.can_bottleneck = true;
    s.fps_ratio = fps_ratio;
    s.recent_drops = recent_drops;
    return s;
}

StageSignals Queue(uint32_t depth, uint32_t busy_threshold) {
    StageSignals s;
    s.id = StageId::FrameQueue;
    s.available = true;
    s.is_duration_stage = false;
    s.can_bottleneck = false; // never a bottleneck candidate
    s.queue_depth = depth;
    s.queue_busy_threshold = busy_threshold;
    return s;
}

StageHealth HealthOf(const PipelineHealthVerdict& v, StageId id) {
    for (const auto& sv : v.per_stage)
        if (sv.id == id)
            return sv.health;
    ADD_FAILURE() << "stage not found";
    return StageHealth::Healthy;
}

TEST(ResolvePipelineHealth, EmptyInputNoBottleneck) {
    const auto v = ResolvePipelineHealth({}, kBudget);
    EXPECT_TRUE(v.per_stage.empty());
    EXPECT_FALSE(v.has_bottleneck);
}

TEST(ResolvePipelineHealth, AllHealthyNoBottleneck) {
    std::vector<StageSignals> stages = {Capture(0.997),
                                        Queue(1, 8),
                                        Duration(StageId::Compositor, 1.4),
                                        Duration(StageId::Encoder, 2.1),
                                        Duration(StageId::Muxer, 0.8),
                                        Duration(StageId::Disk, 0.8, /*can_bottleneck=*/true, /*budget_ms=*/8.0)};
    const auto v = ResolvePipelineHealth(stages, kBudget);
    EXPECT_FALSE(v.has_bottleneck);
    for (const auto& sv : v.per_stage)
        EXPECT_EQ(sv.health, StageHealth::Healthy) << ToString(sv.health);
}

TEST(ResolvePipelineHealth, DurationOverBudgetIsBottleneck) {
    std::vector<StageSignals> stages = {Duration(StageId::Encoder, kBudget + 5.0)};
    const auto v = ResolvePipelineHealth(stages, kBudget);
    EXPECT_EQ(HealthOf(v, StageId::Encoder), StageHealth::Bottleneck);
    EXPECT_TRUE(v.has_bottleneck);
    EXPECT_EQ(v.bottleneck, StageId::Encoder);
}

TEST(ResolvePipelineHealth, DurationNearBudgetIsBusyNotBottleneck) {
    std::vector<StageSignals> stages = {Duration(StageId::Compositor, kBudget * 0.85)};
    const auto v = ResolvePipelineHealth(stages, kBudget);
    EXPECT_EQ(HealthOf(v, StageId::Compositor), StageHealth::Busy);
    EXPECT_FALSE(v.has_bottleneck);
}

TEST(ResolvePipelineHealth, DropDrivenBottleneck) {
    std::vector<StageSignals> stages = {Duration(StageId::Encoder, 2.0)};
    stages[0].recent_drops = 4; // shedding frames despite low avg
    const auto v = ResolvePipelineHealth(stages, kBudget);
    EXPECT_EQ(HealthOf(v, StageId::Encoder), StageHealth::Bottleneck);
}

TEST(ResolvePipelineHealth, CaptureBelowTargetIsBottleneck) {
    std::vector<StageSignals> stages = {Capture(0.80)};
    const auto v = ResolvePipelineHealth(stages, kBudget);
    EXPECT_EQ(HealthOf(v, StageId::SourceCapture), StageHealth::Bottleneck);
}

TEST(ResolvePipelineHealth, CaptureSlightlyBelowIsBusy) {
    std::vector<StageSignals> stages = {Capture(0.92)};
    const auto v = ResolvePipelineHealth(stages, kBudget);
    EXPECT_EQ(HealthOf(v, StageId::SourceCapture), StageHealth::Busy);
}

TEST(ResolvePipelineHealth, FrameQueueNeverBottleneckEvenWhenDeep) {
    std::vector<StageSignals> stages = {Queue(999, 8)};
    const auto v = ResolvePipelineHealth(stages, kBudget);
    EXPECT_EQ(HealthOf(v, StageId::FrameQueue), StageHealth::Busy);
    EXPECT_FALSE(v.has_bottleneck);
}

TEST(ResolvePipelineHealth, UnavailableStageIsHealthyNeutral) {
    StageSignals s = Duration(StageId::Compositor, kBudget + 99.0);
    s.available = false; // no signal → must not alarm
    const auto v = ResolvePipelineHealth({&s, 1}, kBudget);
    EXPECT_EQ(HealthOf(v, StageId::Compositor), StageHealth::Healthy);
    EXPECT_FALSE(v.has_bottleneck);
}

TEST(ResolvePipelineHealth, MostDownstreamBottleneckWins) {
    std::vector<StageSignals> stages = {Duration(StageId::Compositor, kBudget + 1.0), // bottleneck (upstream)
                                        Duration(StageId::Disk, 99.0, true, 8.0)};    // bottleneck (downstream)
    const auto v = ResolvePipelineHealth(stages, kBudget);
    EXPECT_TRUE(v.has_bottleneck);
    EXPECT_EQ(v.bottleneck, StageId::Disk);
}

} // namespace
