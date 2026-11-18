#pragma once

#include <audioclient.h>
#include <cstdint>

namespace exosnap::engine {

// Uncertain timing cannot anchor a clock, even when the packet carries usable PCM.
[[nodiscard]] inline bool HasReliableWasapiTimestamp(uint32_t flags, uint64_t qpc_position) noexcept {
    return qpc_position != 0 && (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) == 0;
}

} // namespace exosnap::engine
