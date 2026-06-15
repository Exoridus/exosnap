#pragma once

// update_types.h -- plain data types for the ExoSnap update strand.
//
// No Qt, no WinAPI, no I/O. Pure value types and enums so the logic layer
// (UpdateChecker, ManifestVerifier) can be tested without any runtime.
//
// ADR 0012: Update Security Model (0.4.0 implementation).

#include <array>
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

    [[nodiscard]] bool operator==(const SemVer&) const noexcept = default;
    [[nodiscard]] bool operator<(const SemVer& o) const noexcept {
        if (major != o.major)
            return major < o.major;
        if (minor != o.minor)
            return minor < o.minor;
        return patch < o.patch;
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
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }
};

// Parse "X.Y.Z" -- returns nullopt on malformed input.
[[nodiscard]] std::optional<SemVer> ParseSemVer(std::string_view s) noexcept;

// ---------------------------------------------------------------------------
// Package entry in a manifest
// ---------------------------------------------------------------------------

enum class PackageKind : uint8_t {
    Installer = 0, // NSIS/WiX .exe or .msi
    Portable = 1,  // ZIP -- notify-only
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
    SemVer minimum_accepted_version;
    std::vector<PackageEntry> packages;
    std::array<uint8_t, 64> signature{}; // raw ed25519 signature bytes
};

// ---------------------------------------------------------------------------
// Installation mode detection
// ---------------------------------------------------------------------------

enum class InstallMode : uint8_t {
    Installed = 0, // installed via installer -> full update flow
    Portable = 1,  // extracted ZIP -> notify-only
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

struct UpdateCheckResult {
    bool update_available = false;
    std::optional<SemVer> available_version;
    std::optional<std::string> releases_page_url;
    std::optional<std::string> error_message;
    bool check_failed = false;
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
