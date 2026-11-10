#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace exosnap::engine {

// Cumulative CPU publication and host read facts across all sources/consumers
// for one runtime lifetime. Independent atomic loads are not a transaction.
struct VisualRuntimeCounters {
    uint64_t accepted_publications = 0;
    uint64_t dropped_publications = 0;
    uint64_t busy_reads = 0;
    // Logical producer/shared/consumer texture reservations, including retired
    // generations still pinned. Excludes Qt internal textures and driver overhead.
    std::optional<uint64_t> reserved_gpu_bytes;
};

// Recording-session attempts by the concrete gamepad history sampler, counted
// per visible scene item. Busy and missing reads can still render neutral/held input.
struct VisualSamplerCounters {
    uint64_t history_samples = 0;
    uint64_t history_busy_reads = 0;
    uint64_t history_missing_reads = 0;
};

// Cumulative timing for one native producer generation. No samples means
// unavailable. GPU spans include CPU submission gaps, not isolated GPU busy time.
struct VisualStageTiming {
    uint64_t samples = 0;
    double latest_ms = 0;
    double total_ms = 0;
    double maximum_ms = 0;

    void Add(double milliseconds) noexcept {
        ++samples;
        latest_ms = milliseconds;
        total_ms += milliseconds;
        if (milliseconds > maximum_ms)
            maximum_ms = milliseconds;
    }
};

// Sampled on the GUI owner thread at the diagnostics cadence. A new native
// producer resets all counts even when its source request token is unchanged.
struct NativeGamepadDiagnostics {
    uint64_t generation = 0;
    uint64_t latest_sample_100ns = 0;
    VisualStageTiming render_cpu;
    VisualStageTiming render_gpu_span;
    VisualStageTiming publish_cpu;
    VisualStageTiming publish_gpu_span;
    uint64_t accepted_publications = 0;
    uint64_t no_free_slot_publications = 0;
};

// Copied on the app control thread from the current accepted scene and provider
// status. Missing token/status fields are unknown, not dormant or generation zero.
struct VisualSourceDiagnostics {
    std::string source_id;
    std::string type_id;
    std::vector<std::string> item_ids;
    std::optional<uint64_t> runtime_generation;
    std::optional<uint64_t> request_revision;
    std::optional<std::string> provider_phase;
    std::optional<uint32_t> provider_error_code;
    std::optional<NativeGamepadDiagnostics> native_gamepad;
};

} // namespace exosnap::engine
