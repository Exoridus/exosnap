#pragma once

// Process-identity guard for PID-keyed audio sources (ADR 0046).
//
// The APP (include-process-tree) and window-target SYS (exclude-process-tree)
// loopback flavors capture audio keyed on a target process id. Windows recycles
// PIDs, so after the target process exits its PID can be reassigned to an
// unrelated process — reactivating a dead PID-keyed loopback could silently grab
// a stranger's audio. Pairing the PID with the process creation time (a FILETIME
// 64-bit value that is effectively unique per process instance) makes the
// identity stable: a reactivation is only allowed when the same PID still names
// the same instance. This decision is pure and testable without a live process.

#include <cstdint>

namespace recorder_core {

// True only when the target process is still alive AND its current creation time
// matches the one captured when the source first initialized. A zero on either
// side (creation time unavailable) or a dead process fails closed — the caller
// keeps the source's contribution silent rather than reacquire a possibly
// recycled PID.
[[nodiscard]] inline bool ProcessIdentityMatches(uint64_t captured_creation_time, uint64_t current_creation_time,
                                                 bool current_process_alive) noexcept {
    if (!current_process_alive) {
        return false;
    }
    if (captured_creation_time == 0 || current_creation_time == 0) {
        return false;
    }
    return captured_creation_time == current_creation_time;
}

} // namespace recorder_core
