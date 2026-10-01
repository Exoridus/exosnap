#include <gtest/gtest.h>

#include "diagnostics/AdapterTelemetryTargets.h"
#include "diagnostics/DxgiVideoMemoryProvider.h"
#include "diagnostics/NvidiaGpuTelemetryProvider.h"

#include <array>
#include <vector>

using namespace exosnap::diagnostics;
using exosnap::engine::PipelineAdapterAssignment;
using exosnap::engine::PipelineAdapterIdentity;
using exosnap::engine::PipelineRole;

TEST(GpuTelemetry, MissingReadApiAndUnsupportedFieldsRemainUnavailable) {
    const auto absent = ReadNvmlMeasurements({}, reinterpret_cast<void*>(1), 42);
    EXPECT_FALSE(absent.utilization_percent);
    EXPECT_FALSE(absent.encoder_utilization_percent);
    EXPECT_FALSE(absent.temperature_celsius);
    NvmlReadApi api;
    api.utilization = [](void*, NvmlReadApi::Utilization* value) {
        value->gpu = 99;
        return 0;
    };
    api.temperature = [](void*, unsigned, unsigned* value) {
        *value = 99;
        return 3;
    };
    const auto partial = ReadNvmlMeasurements(api, reinterpret_cast<void*>(1), 43);
    EXPECT_EQ(partial.utilization_percent, 99);
    EXPECT_FALSE(partial.temperature_celsius);
    EXPECT_EQ(partial.metadata.adapter_luid, 43);
    EXPECT_FALSE(partial.metadata.process_id);
    EXPECT_FALSE(ReadNvmlMeasurements(api, nullptr, 43).utilization_percent);
    EXPECT_FALSE(ReadNvmlMeasurements({}, reinterpret_cast<void*>(1), 44).utilization_percent);
}

TEST(GpuTelemetry, AmbiguousAndMissingAdaptersAreNeverGuessed) {
    const std::array<NvidiaAdapterIdentity, 3> identities{{{1, 2}, {3, 4}, {1, 2}}};
    EXPECT_FALSE(MatchNvidiaAdapter({1, 2}, identities));
    EXPECT_FALSE(MatchNvidiaAdapter({9, 2}, identities));
    EXPECT_EQ(MatchNvidiaAdapter({3, 4}, identities), 1u);
}

TEST(VideoMemory, MissingAdapterStaysUnavailableAndHeadroomCannotUnderflow) {
    DxgiVideoMemoryProvider provider;
    const auto reading = provider.Read(0);
    EXPECT_FALSE(reading.local);
    EXPECT_FALSE(reading.nonlocal);
    EXPECT_FALSE(reading.metadata.adapter_luid);
    EXPECT_EQ((VideoMemoryBudget{200, 100, 0, 0}.headroom_bytes()), 0u);
    EXPECT_EQ((VideoMemoryBudget{60, 100, 0, 0}.headroom_bytes()), 40u);
}

namespace {

PipelineAdapterIdentity Identity(int64_t luid, uint32_t vendor_id) {
    PipelineAdapterIdentity identity;
    identity.known = true;
    identity.luid = luid;
    identity.vendor_id = vendor_id;
    return identity;
}

struct CountingGpuProvider final : IGpuTelemetryProvider {
    int reads = 0;
    std::vector<int64_t> adapter_luids;

    GpuTelemetryReading Read(int64_t adapter_luid) override {
        ++reads;
        adapter_luids.push_back(adapter_luid);
        GpuTelemetryReading reading;
        reading.metadata.adapter_luid = adapter_luid;
        return reading;
    }
};

struct CountingMemoryProvider final : IVideoMemoryProvider {
    int reads = 0;
    std::vector<int64_t> adapter_luids;

    VideoMemoryReading Read(int64_t adapter_luid) override {
        ++reads;
        adapter_luids.push_back(adapter_luid);
        VideoMemoryReading reading;
        reading.metadata.adapter_luid = adapter_luid;
        return reading;
    }
};

} // namespace

TEST(AdapterTelemetryTargets, SinglePhysicalAdapterCollapsesThreeRolesToOneTarget) {
    PipelineAdapterAssignment assignment;
    assignment.capture = Identity(7, 0x10DEu);
    assignment.processing = Identity(7, 0x10DEu);
    assignment.encoder = Identity(7, 0x10DEu);

    const auto targets = BuildAdapterTelemetryTargets(assignment);
    ASSERT_EQ(targets.size(), 1u);
    EXPECT_EQ(targets[0].luid, 7);
    EXPECT_TRUE(targets[0].nvidia);
    EXPECT_TRUE(exosnap::engine::HasRole(targets[0].roles, PipelineRole::Capture));
    EXPECT_TRUE(exosnap::engine::HasRole(targets[0].roles, PipelineRole::Processing));
    EXPECT_TRUE(exosnap::engine::HasRole(targets[0].roles, PipelineRole::Encoder));
}

TEST(AdapterTelemetryTargets, TwoAdaptersCarryDisjointRoleSets) {
    PipelineAdapterAssignment assignment;
    assignment.capture = Identity(1, 0x10DEu);
    assignment.processing = Identity(2, 0x8086u);
    assignment.encoder = Identity(2, 0x8086u);

    const auto targets = BuildAdapterTelemetryTargets(assignment);
    ASSERT_EQ(targets.size(), 2u);
    EXPECT_EQ(targets[0].luid, 1);
    EXPECT_TRUE(targets[0].nvidia);
    EXPECT_TRUE(exosnap::engine::HasRole(targets[0].roles, PipelineRole::Capture));
    EXPECT_FALSE(exosnap::engine::HasRole(targets[0].roles, PipelineRole::Processing));
    EXPECT_FALSE(exosnap::engine::HasRole(targets[0].roles, PipelineRole::Encoder));
    EXPECT_EQ(targets[1].luid, 2);
    EXPECT_FALSE(targets[1].nvidia);
    EXPECT_FALSE(exosnap::engine::HasRole(targets[1].roles, PipelineRole::Capture));
    EXPECT_TRUE(exosnap::engine::HasRole(targets[1].roles, PipelineRole::Processing));
    EXPECT_TRUE(exosnap::engine::HasRole(targets[1].roles, PipelineRole::Encoder));
}

TEST(AdapterTelemetryTargets, ThreeDistinctAdaptersKeepThreeRoleAttributedTargets) {
    PipelineAdapterAssignment assignment;
    assignment.capture = Identity(1, 0x10DEu);
    assignment.processing = Identity(2, 0x8086u);
    assignment.encoder = Identity(3, 0x1002u);

    const auto targets = BuildAdapterTelemetryTargets(assignment);
    ASSERT_EQ(targets.size(), 3u);
    EXPECT_EQ(targets[0].roles, PipelineRole::Capture);
    EXPECT_EQ(targets[1].roles, PipelineRole::Processing);
    EXPECT_EQ(targets[2].roles, PipelineRole::Encoder);
}

TEST(AdapterTelemetryTargets, UnknownRolesAreNeverPolledAsAdapterZero) {
    const auto targets = BuildAdapterTelemetryTargets(PipelineAdapterAssignment{});
    EXPECT_TRUE(targets.empty());
}

TEST(AdapterTelemetryCollection, NvmlRunsOncePerUniqueNvidiaTargetAndNeverForOthers) {
    PipelineAdapterAssignment assignment;
    assignment.capture = Identity(1, 0x10DEu);
    assignment.processing = Identity(2, 0x8086u);
    assignment.encoder = Identity(2, 0x8086u);
    const auto targets = BuildAdapterTelemetryTargets(assignment);

    CountingGpuProvider gpu;
    CountingMemoryProvider memory;
    const auto readings = CollectAdapterTelemetry(targets, gpu, memory);

    ASSERT_EQ(readings.size(), 2u);
    EXPECT_EQ(gpu.reads, 1);
    EXPECT_EQ(gpu.adapter_luids, (std::vector<int64_t>{1}));
    EXPECT_EQ(memory.reads, 2);
    EXPECT_EQ(memory.adapter_luids, (std::vector<int64_t>{1, 2}));
    EXPECT_EQ(readings[0].gpu.metadata.adapter_luid, 1);
    EXPECT_FALSE(readings[1].gpu.metadata.adapter_luid.has_value());
    EXPECT_EQ(readings[1].memory.metadata.adapter_luid, 2);
}

TEST(AdapterTelemetryCollection, SingleAdapterWithThreeRolesIsPolledOncePerProvider) {
    PipelineAdapterAssignment assignment;
    assignment.capture = Identity(7, 0x10DEu);
    assignment.processing = Identity(7, 0x10DEu);
    assignment.encoder = Identity(7, 0x10DEu);
    const auto targets = BuildAdapterTelemetryTargets(assignment);

    CountingGpuProvider gpu;
    CountingMemoryProvider memory;
    const auto readings = CollectAdapterTelemetry(targets, gpu, memory);

    ASSERT_EQ(readings.size(), 1u);
    EXPECT_EQ(gpu.reads, 1);
    EXPECT_EQ(memory.reads, 1);
    EXPECT_TRUE(exosnap::engine::HasRole(readings[0].target.roles, PipelineRole::Encoder));
}

TEST(GpuTelemetry, FreshnessRequiresCurrentAdapterAndMonotonicReceipt) {
    MeasurementMetadata m;
    const auto now = std::chrono::steady_clock::now();
    EXPECT_FALSE(m.FreshForAdapter(42, now));
    m.adapter_luid = 42;
    m.observed_at = now;
    EXPECT_TRUE(m.FreshForAdapter(42, now));
    EXPECT_FALSE(m.FreshForAdapter(43, now));
    EXPECT_FALSE(m.FreshForAdapter(42, now + std::chrono::seconds(16)));
    EXPECT_FALSE(m.FreshForAdapter(42, now - std::chrono::seconds(1)));
}
