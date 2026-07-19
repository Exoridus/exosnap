#pragma once

// Pure, allocation-free math for turning D3D11 timestamp-query results into a
// millisecond stage duration. Deliberately free of any D3D11 dependency so the
// tick->ms conversion and the disjoint / wrap / not-calibrated handling are
// exhaustively unit-testable without a live GPU (see test_gpu_timestamp.cpp).
// The thin device-facing wrapper that actually issues the queries lives in
// gpu_timestamp_profiler.h and defers every judgement to this header.

#include <cstdint>
#include <optional>

namespace recorder_core {

// The result of the D3D11_QUERY_TIMESTAMP_DISJOINT query that brackets a frame's
// timestamp pairs. `frequency` is GPU timestamp ticks per second; `disjoint`
// true means the GPU clock changed rate or was interrupted during the covered
// span, so any timestamps inside it are meaningless.
struct GpuTimestampDisjoint {
    uint64_t frequency = 0;
    bool disjoint = true;
};

// Convert a begin/end GPU timestamp tick pair into milliseconds, given the
// disjoint result that covers them. Returns nullopt (meaning: drop this sample,
// it is not trustworthy) when:
//   - the covering period was disjoint (clock interrupted / re-clocked),
//   - the frequency is 0 (the disjoint query never calibrated),
//   - end precedes begin (counter wrap or a mis-ordered/incomplete pair).
// Never throws; never blocks. A dropped sample is a normal, expected condition,
// not an error.
[[nodiscard]] inline std::optional<double> ResolveGpuSpanMs(uint64_t begin_ticks, uint64_t end_ticks,
                                                            const GpuTimestampDisjoint& dj) noexcept {
    if (dj.disjoint || dj.frequency == 0 || end_ticks < begin_ticks) {
        return std::nullopt;
    }
    const uint64_t delta = end_ticks - begin_ticks;
    return (static_cast<double>(delta) / static_cast<double>(dj.frequency)) * 1000.0;
}

} // namespace recorder_core
