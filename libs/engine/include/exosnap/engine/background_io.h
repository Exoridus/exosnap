#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

namespace exosnap::engine {

// Admission precedes each physical background transfer. The cancellation flag
// remains owned by the synchronous caller. Recording payload I/O is exempt.
using BackgroundIoAdmission = std::function<bool(uint64_t, const std::atomic<bool>*)>;

} // namespace exosnap::engine
