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
#include <vector>

namespace exosnap::update {

struct ReleaseAssets {
    SemVer version;                // parsed from tag_name "vX.Y.Z"
    std::string version_tag;       // tag_name verbatim, leading "v" stripped ("0.9.0-rc4").
                                   // Kept unparsed because SemVer collapses foreign
                                   // prerelease labels onto ordinal 0; the verification
                                   // reinstall gate needs the exact string.
    std::string manifest_url;      // browser_download_url of "update-manifest.json"
    std::string signature_url;     // browser_download_url of "update-manifest.json.sig"
    std::string portable_url;      // asset ending "-portable.zip" ("" if absent)
    std::string installer_url;     // asset ending ".msi" ("" if absent)
    std::string releases_page_url; // html_url
};

// Parse the GitHub /releases JSON array (exact payload UpdateChecker fetches) and pick
// the newest release for `channel` (Stable = non-prerelease, Preview = prerelease) that
// carries BOTH an update-manifest.json asset AND its update-manifest.json.sig detached
// signature. A release missing either cannot be verified and does not qualify. nullopt
// when none qualifies.
//
// If `parse_error` is non-null, it is set to a non-empty message when the JSON body
// could not be parsed or had an unexpected shape (i.e. LocateRelease could not read
// it at all). It is left untouched when the JSON is well-formed but simply contains
// no qualifying release for `channel`.
[[nodiscard]] std::optional<ReleaseAssets> LocateRelease(std::string_view releases_json, UpdateChannel channel,
                                                         std::string* parse_error = nullptr);

// Collect the release notes for every non-draft release whose version lies in
// the half-open/closed range (`above`, `up_to`] for `channel`, newest first.
//
// Parses the exact GitHub /releases JSON array UpdateChecker fetches — the same
// payload LocateRelease reads — so no extra network call is needed to populate
// the What's-new overlay. A release's Markdown changelog is its "body" field and
// its page link is "html_url".
//
// Channel rule (mirrors the product's channel semantics):
//   * Stable  -> prereleases are excluded.
//   * Preview -> prereleases are included (alongside stable releases).
// Drafts are always skipped. Releases whose tag does not parse to a SemVer, or
// that fall outside the range, are skipped. On a malformed JSON body the result
// is an empty vector (the What's-new UI simply shows nothing).
[[nodiscard]] std::vector<ReleaseNote> CollectReleaseNotes(std::string_view releases_json, const SemVer& above,
                                                           const SemVer& up_to, UpdateChannel channel);

// Collect the release notes for every non-draft release on `channel`, newest first,
// independent of any install/target gap -- the full reference list, not a window.
// Same channel rule as CollectReleaseNotes (Stable excludes prereleases, Preview
// includes them) and the same already-fetched JSON body (no extra network call).
// Bounded by whatever the fetch's per_page returns (currently 30, unpaginated).
[[nodiscard]] std::vector<ReleaseNote> CollectAllReleaseNotesForChannel(std::string_view releases_json,
                                                                        UpdateChannel channel);

// Installed -> PackageKind::Installer, Portable -> PackageKind::Portable; nullptr if absent.
[[nodiscard]] const PackageEntry* SelectPackage(const UpdateManifest& m, InstallMode mode);

} // namespace exosnap::update
