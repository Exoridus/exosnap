#pragma once
// update_checker.h -- GitHub Releases API polling for ExoSnap updates.
//
// Rules (from ADR-0012):
//   - Public API only; NO authentication token in the client.
//   - Stable channel  = latest non-prerelease GitHub release.
//   - Preview channel = latest prerelease.
//   - Recording guard: returns UpdateBlockReason::ActiveRecording /
//     Finalizing when a recording is in progress.
//   - EXOSNAP_OFFICIAL_BUILD gate: when the compile-time symbol is absent,
//     IsUpdateCheckEnabled() returns false and CheckForUpdate() returns a
//     blocked result without making any network request.

#include <functional>
#include <string>
#include <update/update_types.h>

namespace exosnap::update {

// Callback that supplies the current recording block reason.
// Called synchronously before any network request is issued.
using RecordingGuardFn = std::function<UpdateBlockReason()>;

struct CheckParams {
    SemVer current_version;
    UpdateChannel channel = UpdateChannel::Stable;
    RecordingGuardFn recording_guard{}; // may be nullptr (no guard)
    // Optional: override the API base URL for testing
    std::string api_base_url = "https://api.github.com/repos/Exoridus/exosnap/releases";
};

// Synchronous blocking call; intended to be run on a background thread.
// Never throws; always returns a populated UpdateCheckResult.
[[nodiscard]] UpdateCheckResult CheckForUpdate(const CheckParams& params) noexcept;

// Compile-time gate: false when EXOSNAP_OFFICIAL_BUILD is not defined.
[[nodiscard]] constexpr bool IsUpdateCheckEnabled() noexcept {
#ifdef EXOSNAP_OFFICIAL_BUILD
    return true;
#else
    return false;
#endif
}

} // namespace exosnap::update
