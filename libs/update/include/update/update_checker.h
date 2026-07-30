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

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <update/release_locator.h>
#include <update/update_types.h>

namespace exosnap::update {

// Callback that supplies the current recording block reason.
// Called synchronously before any network request is issued.
using RecordingGuardFn = std::function<UpdateBlockReason()>;

struct CheckParams {
    SemVer current_version;
    // The EXACT version string of the running build ("0.9.0-rc4"), unparsed.
    // Only the verification-reinstall gate reads it: SemVer equality is too
    // coarse there (every foreign prerelease label parses to ordinal 0, so
    // "0.9.0-beta1" and "0.9.0-alpha7" compare equal), so that gate demands
    // string equality against the release tag. Empty disables the gate.
    std::string current_version_raw;
    UpdateChannel channel = UpdateChannel::Stable;
    RecordingGuardFn recording_guard{}; // may be nullptr (no guard)
    // Optional: override the API base URL for testing
    std::string api_base_url = "https://api.github.com/repos/Exoridus/exosnap/releases";
    // ADR 0055 — verification reinstall. Non-persistent, opted into per app run
    // via the --verify-update-reinstall CLI flag. When true, a release whose tag
    // is byte-identical to current_version_raw is additionally offered, so the
    // full production update path (download -> signature -> hash -> swap) can be
    // exercised against the running build. NEVER relaxes the ordering rule: an
    // older release stays invisible in this mode too.
    bool allow_same_version_reinstall = false;
};

// What a located release may be offered as.
enum class UpdateOffer : uint8_t {
    None = 0,                  // not offered (same or older version)
    Update = 1,                // a genuinely newer release
    VerificationReinstall = 2, // the identical version, verification mode only
};

// Pure decision behind CheckForUpdate's "do we offer this release?" step.
// `release_version_raw` is the release tag with any leading "v" stripped.
//   * release_version > current                  -> Update (always, any mode)
//   * verification mode AND the two version
//     strings are non-empty and byte-identical   -> VerificationReinstall
//   * anything else                              -> None
[[nodiscard]] UpdateOffer DecideOffer(const SemVer& release_version, std::string_view release_version_raw,
                                      const CheckParams& params) noexcept;

// Assembles the check result once a release has been located (or not) for the channel
// -- the offer decision, gap notes, and the full-channel reference list. Pulled out of
// CheckForUpdate so this logic is testable without the network fetch or the
// EXOSNAP_OFFICIAL_BUILD gate. `releases_json` is the same raw body LocateRelease and
// CollectReleaseNotes/CollectAllReleaseNotesForChannel read; `release` is
// LocateRelease's result for that body and params.channel (nullopt if none qualified).
[[nodiscard]] UpdateCheckResult BuildCheckResult(std::string_view releases_json,
                                                 const std::optional<ReleaseAssets>& release,
                                                 const CheckParams& params) noexcept;

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
