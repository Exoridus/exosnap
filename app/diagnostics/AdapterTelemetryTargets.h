#pragma once

#include "GpuTelemetryProvider.h"
#include "VideoMemoryProvider.h"

#include <exosnap/engine/encoder_device.h>

#include <cstdint>
#include <vector>

namespace exosnap::diagnostics {

// One physical adapter targeted for telemetry, with the pipeline roles it
// serves. Targets are keyed by physical adapter identity, never by role: an
// adapter that serves three roles is polled once per sampling iteration, not
// three times.
struct AdapterTelemetryTarget {
    int64_t luid = 0;
    uint32_t vendor_id = 0;
    exosnap::engine::PipelineRole roles = exosnap::engine::PipelineRole::None;
    // PCI vendor fact, precomputed so the collection policy never re-derives it.
    bool nvidia = false;

    friend bool operator==(const AdapterTelemetryTarget&, const AdapterTelemetryTarget&) = default;
};

// Pure. Groups the assignment's roles by packed adapter LUID, in first-seen
// order (capture, processing, encoder). Unknown identities produce no target:
// an unresolved role is never polled as adapter 0.
[[nodiscard]] inline std::vector<AdapterTelemetryTarget>
BuildAdapterTelemetryTargets(const exosnap::engine::PipelineAdapterAssignment& assignment) {
    std::vector<AdapterTelemetryTarget> targets;
    const auto add = [&targets](const exosnap::engine::PipelineAdapterIdentity& identity,
                                exosnap::engine::PipelineRole role) {
        if (!identity.known || identity.luid == 0) {
            return;
        }
        for (AdapterTelemetryTarget& target : targets) {
            if (target.luid == identity.luid) {
                target.roles = target.roles | role;
                return;
            }
        }
        AdapterTelemetryTarget target;
        target.luid = identity.luid;
        target.vendor_id = identity.vendor_id;
        target.roles = role;
        target.nvidia = identity.vendor_id == 0x10DEu;
        targets.push_back(target);
    };
    add(assignment.capture, exosnap::engine::PipelineRole::Capture);
    add(assignment.processing, exosnap::engine::PipelineRole::Processing);
    add(assignment.encoder, exosnap::engine::PipelineRole::Encoder);
    return targets;
}

// One unique adapter's telemetry for a sampling iteration. NVML is read only
// for NVIDIA targets; DXGI video memory is read for every target regardless of
// vendor. Both providers are keyed by packed adapter LUID, so provenance rides
// in each reading's metadata rather than in a role label.
struct AdapterTelemetryReading {
    AdapterTelemetryTarget target;
    GpuTelemetryReading gpu;
    VideoMemoryReading memory;
};

[[nodiscard]] inline std::vector<AdapterTelemetryReading>
CollectAdapterTelemetry(const std::vector<AdapterTelemetryTarget>& targets, IGpuTelemetryProvider& gpu,
                        IVideoMemoryProvider& memory) {
    std::vector<AdapterTelemetryReading> readings;
    readings.reserve(targets.size());
    for (const AdapterTelemetryTarget& target : targets) {
        AdapterTelemetryReading reading;
        reading.target = target;
        if (target.nvidia) {
            reading.gpu = gpu.Read(target.luid);
        }
        reading.memory = memory.Read(target.luid);
        readings.push_back(std::move(reading));
    }
    return readings;
}

} // namespace exosnap::diagnostics
