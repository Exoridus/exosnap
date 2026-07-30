#pragma once

// update_types.h -- plain data types for the ExoSnap update strand.
//
// No Qt, no WinAPI, no I/O. Pure value types and enums so the logic layer
// (UpdateChecker, ManifestVerifier) can be tested without any runtime.
//
// ADR 0012: Update Security Model (0.4.0 implementation).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace exosnap::update {

// ---------------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------------

enum class UpdateChannel : uint8_t {
    Stable = 0,  // default; latest non-prerelease GitHub release
    Preview = 1, // latest prerelease GitHub release
};

inline const char* ChannelName(UpdateChannel ch) noexcept {
    switch (ch) {
    case UpdateChannel::Stable:
        return "stable";
    case UpdateChannel::Preview:
        return "preview";
    }
    return "stable";
}

// ---------------------------------------------------------------------------
// Semantic version (major.minor.patch)
// ---------------------------------------------------------------------------

struct SemVer {
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t patch = 0;
    // Release-candidate ordinal for a GitHub prerelease tag ("-rcN"). Only
    // meaningful when is_prerelease is true; 0 for a final release.
    // is_prerelease=false, prerelease_number=0 is the aggregate-init default,
    // so every pre-existing `SemVer{X, Y, Z}` call site in the codebase still
    // means "final release X.Y.Z" without being touched.
    bool is_prerelease = false;
    uint32_t prerelease_number = 0;

    [[nodiscard]] bool operator==(const SemVer&) const noexcept = default;
    // SemVer precedence (extends the plain X.Y.Z compare with prerelease
    // ordering, mirroring semver.org's rule that a final release outranks any
    // prerelease of the same X.Y.Z, and prereleases of the same X.Y.Z order by
    // their ordinal): rc1 < rc2 < ... < the final X.Y.Z release.
    [[nodiscard]] bool operator<(const SemVer& o) const noexcept {
        if (major != o.major)
            return major < o.major;
        if (minor != o.minor)
            return minor < o.minor;
        if (patch != o.patch)
            return patch < o.patch;
        if (is_prerelease != o.is_prerelease)
            return is_prerelease; // this is a prerelease, other is final -> this is older
        if (!is_prerelease)
            return false; // both final, same X.Y.Z -> equal, not less
        return prerelease_number < o.prerelease_number;
    }
    [[nodiscard]] bool operator>(const SemVer& o) const noexcept {
        return o < *this;
    }
    [[nodiscard]] bool operator<=(const SemVer& o) const noexcept {
        return !(o < *this);
    }
    [[nodiscard]] bool operator>=(const SemVer& o) const noexcept {
        return !(*this < o);
    }

    [[nodiscard]] std::string ToString() const {
        std::string s = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
        if (is_prerelease)
            s += "-rc" + std::to_string(prerelease_number);
        return s;
    }
};

// Parse "X.Y.Z" -- returns nullopt on malformed input.
[[nodiscard]] std::optional<SemVer> ParseSemVer(std::string_view s) noexcept;

// ---------------------------------------------------------------------------
// Package entry in a manifest
// ---------------------------------------------------------------------------

enum class PackageKind : uint8_t {
    Installer = 0, // NSIS/WiX .exe or .msi
    Portable = 1,  // ZIP -- staged swap
};

struct PackageEntry {
    PackageKind kind = PackageKind::Installer;
    std::string url;
    std::string sha256_hex; // lowercase hex SHA-256 of the downloaded file
};

// ---------------------------------------------------------------------------
// Update manifest (deserialised from JSON + ed25519 envelope)
// ---------------------------------------------------------------------------

struct UpdateManifest {
    SemVer version;
    // The manifest's "version" field verbatim, unparsed ("0.9.0-rc4"). SemVer
    // collapses foreign prerelease labels onto ordinal 0, so the verification
    // reinstall gate (ADR 0055) compares this exact string instead.
    std::string version_raw;
    SemVer minimum_accepted_version;
    std::vector<PackageEntry> packages;
    // The ed25519 signature is detached: it lives in a sibling `.sig` asset and
    // covers the exact manifest bytes, so it is never carried inside this struct.
};

// ---------------------------------------------------------------------------
// Installation mode detection
// ---------------------------------------------------------------------------

enum class InstallMode : uint8_t {
    Installed = 0, // installed via installer -> full update flow
    Portable = 1,  // extracted ZIP -> staged swap via external updater
};

// ---------------------------------------------------------------------------
// Guard reason -- why an update action is currently blocked
// ---------------------------------------------------------------------------

enum class UpdateBlockReason : uint8_t {
    NotBlocked = 0,
    ActiveRecording = 1,
    Finalizing = 2,
    OfficialBuildGateOff = 3,
};

// ---------------------------------------------------------------------------
// Check result
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Release note -- one GitHub release's changelog body, for the What's-new UI.
//
// Defined here (rather than in release_locator.h) because UpdateCheckResult
// carries a vector of these and update_types.h is the base header the locator
// includes — putting it here keeps the include direction acyclic.
// ---------------------------------------------------------------------------

struct ReleaseNote {
    SemVer version;            // parsed from tag_name
    std::string body_markdown; // GitHub release "body" (Markdown)
    std::string html_url;      // release page URL

    [[nodiscard]] bool operator==(const ReleaseNote&) const noexcept = default;
};

struct UpdateCheckResult {
    bool update_available = false;
    // ADR 0055: true when update_available was granted by the verification
    // reinstall rule (the offered version is byte-identical to the running one)
    // rather than by a genuinely newer release.
    bool verification_reinstall = false;
    std::optional<SemVer> available_version;
    std::optional<std::string> releases_page_url;
    std::optional<std::string> error_message;
    bool check_failed = false;

    // Release notes for every version in the gap (current, best], newest first,
    // for the same channel the check ran on. Empty unless an update is available.
    std::vector<ReleaseNote> gap_notes;

    // The full reference list for the channel the check ran on -- every non-draft
    // release, newest first, independent of update_available. Populated on every
    // successful check (unlike gap_notes) because the pre-update "See what's new"
    // link must work even when already up to date.
    std::vector<ReleaseNote> all_channel_notes;
};

// ---------------------------------------------------------------------------
// Verification result for a downloaded package
// ---------------------------------------------------------------------------

enum class VerifyResult : uint8_t {
    Ok = 0,
    ManifestSigInvalid = 1,
    ManifestParseError = 2,
    DowngradeBlocked = 3,
    PackageHashMismatch = 4,
    PackageNotFound = 5,
};

// ---------------------------------------------------------------------------
// UI-facing state snapshot (the seam exposed to the Settings UI)
// ---------------------------------------------------------------------------

struct UpdateState {
    UpdateChannel channel = UpdateChannel::Stable;
    InstallMode install_mode = InstallMode::Portable;
    UpdateBlockReason block_reason = UpdateBlockReason::NotBlocked;

    std::optional<SemVer> available_version;
    bool update_available = false;
    bool checking = false;
    std::string last_error;
    bool pending_restart = false;
};

} // namespace exosnap::update
