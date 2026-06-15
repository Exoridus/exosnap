#pragma once
// manifest_io.h -- parse and verify the ExoSnap update manifest.
//
// The manifest is a JSON envelope:
//   {
//     "version": "1.2.3",
//     "minimum_accepted_version": "1.0.0",
//     "packages": [
//       { "kind": "installer", "url": "https://...", "sha256": "abc..." },
//       { "kind": "portable",  "url": "https://...", "sha256": "def..." }
//     ],
//     "signature": "<hex-encoded 64-byte ed25519 sig over the body>"
//   }
//
// Security contract:
//   1. The ed25519 signature covers the canonical body (all fields EXCEPT
//      "signature" itself), serialised as UTF-8 JSON with no trailing whitespace.
//   2. ed25519_verify() is called with the EMBEDDED public key BEFORE any
//      field is read or acted upon.
//   3. Downgrade is blocked when parsed version < current installed version
//      OR parsed version < minimum_accepted_version.
//
// This module has NO Qt dependency and NO network dependency.

#include <string>
#include <update/update_types.h>
#include <variant>

namespace exosnap::update {

// Embedded ed25519 public key (32 bytes, set at build time via CMake).
// The corresponding private key is NEVER stored in the repository; it lives
// only in the CI secret EXOSNAP_UPDATE_SIGNING_KEY.
//
// This is a placeholder all-zero key for development builds
// (EXOSNAP_OFFICIAL_BUILD=OFF).  The real key is injected by CMake when
// EXOSNAP_OFFICIAL_BUILD=ON via -DEXOSNAP_UPDATE_PUBLIC_KEY_HEX=<64-hex-chars>.
extern const uint8_t kUpdatePublicKey[32];

// Parse a raw manifest JSON blob (the entire HTTP response body).
// Returns UpdateManifest on success, or a string describing the parse error.
// Does NOT perform signature verification — call VerifyManifest() separately.
using ParseResult = std::variant<UpdateManifest, std::string /*error*/>;
[[nodiscard]] ParseResult ParseManifest(std::string_view json) noexcept;

// Verify the manifest signature using the embedded public key.
// Must be called before acting on any field.
// Returns VerifyResult::Ok on success, or a specific failure code.
[[nodiscard]] VerifyResult VerifyManifestSignature(const UpdateManifest& manifest, std::string_view raw_json) noexcept;

// Downgrade guard: returns false if the manifest version is below the current
// installed version OR below minimum_accepted_version.
[[nodiscard]] bool IsDowngrade(const UpdateManifest& manifest, const SemVer& current_version) noexcept;

} // namespace exosnap::update
