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
};

// Recording-session attempts by the concrete gamepad history sampler, counted
// per visible scene item. Busy and missing reads can still render neutral/held input.
struct VisualSamplerCounters {
    uint64_t history_samples = 0;
    uint64_t history_busy_reads = 0;
    uint64_t history_missing_reads = 0;
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
};

} // namespace exosnap::engine
