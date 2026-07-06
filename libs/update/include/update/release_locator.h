#pragma once
// release_locator.h -- pick the newest qualifying GitHub release for a channel
// and extract its asset URLs; select the manifest package for an install mode.
//
// No Qt, no WinAPI, no I/O. Pure parsing over the GitHub /releases JSON payload
// (the exact array body UpdateChecker fetches) so the selection logic can be
// unit-tested without any runtime.

#include <optional>
#include <string>
#include <string_view>
#include <update/update_types.h>

namespace exosnap::update {

struct ReleaseAssets {
    SemVer version;                // parsed from tag_name "vX.Y.Z"
    std::string manifest_url;      // browser_download_url of "update-manifest.json"
    std::string portable_url;      // asset ending "-portable.zip" ("" if absent)
    std::string installer_url;     // asset ending ".msi" ("" if absent)
    std::string releases_page_url; // html_url
};

// Parse the GitHub /releases JSON array (exact payload UpdateChecker fetches) and pick
// the newest release for `channel` (Stable = non-prerelease, Preview = prerelease) that
// carries an update-manifest.json asset. nullopt when none qualifies.
[[nodiscard]] std::optional<ReleaseAssets> LocateRelease(std::string_view releases_json, UpdateChannel channel);

// Installed -> PackageKind::Installer, Portable -> PackageKind::Portable; nullptr if absent.
[[nodiscard]] const PackageEntry* SelectPackage(const UpdateManifest& m, InstallMode mode);

} // namespace exosnap::update
