#pragma once

#include <cstdint>

namespace exosnap::diagnostics {

// ──────────────────────────────────────────────────────────────────────────────
// Disk-space thresholds for the low-disk guard (LOW-DISK-GUARD-R1).
//
// Two tiers:
//
//   kWarnFreeBytes  — rec.005 Notice.  Soft advisory; recording is still
//                     allowed.  Fires when free space drops below this value
//                     but is still above kHardStopFreeBytes.
//
//   kHardStopFreeBytes — rec.007 Blocker.  Recording is blocked at start and
//                        a running recording is stopped gracefully when free
//                        space falls to or below this value.
//
// Rationale for chosen values:
//
//   kWarnFreeBytes = 2 GB
//     A typical 60 fps H.264 recording at the default quality preset produces
//     roughly 150–300 MB/min.  2 GB gives ~7–13 minutes of warning headroom.
//     The user has time to react before the hard stop.
//
//   kHardStopFreeBytes = 500 MB
//     Enough to safely finalize an MKV file (close the final Cluster, write
//     Cues + SeekHead + Duration — typically only a few KB) with margin.
//     For MP4 sessions this value is added to the remux reserve (see below);
//     the effective threshold is therefore higher and the constant itself
//     remains conservative.
//
// Remux reserve (ADR-0014):
//   When MP4 output is selected, the MKV transient file and the MP4 output
//   file coexist on disk during the remux-on-stop phase.  The low-disk guard
//   must add the current transient-MKV size to the hard-stop threshold so
//   there is still enough space to complete the remux after stopping.
//   Helper: ComputeHardStopThreshold().
// ──────────────────────────────────────────────────────────────────────────────

// 2 GB soft warning threshold.
inline constexpr uint64_t kWarnFreeBytes = 2ULL * 1024 * 1024 * 1024;

// 500 MB hard-stop threshold (MKV-only baseline; add remux reserve for MP4).
inline constexpr uint64_t kHardStopFreeBytes = 500ULL * 1024 * 1024;

// Compute the effective hard-stop threshold for a session.
//
// For MKV-only sessions (remux_reserve_bytes == 0) this returns kHardStopFreeBytes.
// For MP4 sessions the caller passes the current transient MKV file size
// (obtained from the engine or via std::filesystem::file_size at poll time).
// The guard will trigger when free space falls to or below the returned value.
[[nodiscard]] inline uint64_t ComputeHardStopThreshold(uint64_t remux_reserve_bytes) noexcept {
    return kHardStopFreeBytes + remux_reserve_bytes;
}

} // namespace exosnap::diagnostics
