#pragma once

#include <cstdint>

namespace exosnap::engine {

// Identifies one recording from the moment a caller decides to record until the
// matching Record() call returns, so Stop() can name the recording it means.
using RecordRequestId = uint64_t;

// Matches every recording. For a caller that never overlaps a stop with the
// preparation of a different recording.
inline constexpr RecordRequestId kUnscopedRecordRequest = 0;

// Mints an id that no other call returns. Process-wide and thread-safe.
[[nodiscard]] RecordRequestId NextRecordRequestId() noexcept;

} // namespace exosnap::engine
