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
#include <optional>
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

// Raw releases fetch seam (shared with the standalone updater process): HTTPS
// GET of the first releases page for `base_url` ("?per_page=30" is appended;
// "&" when the URL already carries a query). `base_url` must be an https URL
// of the CheckParams::api_base_url shape; "host:port" is honoured for dev
// servers. Returns the response body, or nullopt with `out_error` set.
//
// Deliberately NOT gated by EXOSNAP_OFFICIAL_BUILD: the gate is an update-check
// *policy* enforced by CheckForUpdate; the updater process only exists once the
// app (which is gated) has handed off, and dev runs override via --base-url.
[[nodiscard]] std::optional<std::string> FetchReleasesJson(const std::string& base_url,
                                                           std::string& out_error) noexcept;

// Compile-time gate: false when EXOSNAP_OFFICIAL_BUILD is not defined.
[[nodiscard]] constexpr bool IsUpdateCheckEnabled() noexcept {
#ifdef EXOSNAP_OFFICIAL_BUILD
    return true;
#else
    return false;
#endif
}

} // namespace exosnap::update
