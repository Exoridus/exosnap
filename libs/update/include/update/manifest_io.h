#pragma once
// manifest_io.h -- parse and verify the ExoSnap update manifest.
//
// The manifest is a plain JSON document with NO signature field:
//   {
//     "version": "1.2.3",
//     "minimum_accepted_version": "1.0.0",
//     "packages": [
//       { "kind": "installer", "url": "https://...", "sha256": "abc..." },
//       { "kind": "portable",  "url": "https://...", "sha256": "def..." }
//     ]
//   }
//
// The ed25519 signature is DETACHED (minisign-style): the release pipeline ships
// a sibling asset `update-manifest.json.sig` holding the 128-hex-char ed25519
// signature over the EXACT bytes of `update-manifest.json`. The client verifies
// the signature against the received manifest bytes verbatim — it never
// re-serialises the JSON, so signer and verifier can never disagree on a
// canonical form (the previous embedded-signature scheme broke because the
// Python signer and the C++ verifier emitted object keys in different orders).
//
// Security contract:
//   1. ed25519_verify() covers the raw manifest bytes as received and runs with
//      the EMBEDDED public key BEFORE any field is parsed or acted upon.
//   2. Downgrade is blocked when parsed version < current installed version
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
// Does NOT perform signature verification — call VerifyManifestSignature() first.
using ParseResult = std::variant<UpdateManifest, std::string /*error*/>;
[[nodiscard]] ParseResult ParseManifest(std::string_view json) noexcept;

// Verify the detached ed25519 signature over the exact manifest bytes as
// received, using the embedded public key. `signature_hex` is the content of the
// sibling `.sig` asset (128 hex chars). Must be called BEFORE any field is read.
// Returns VerifyResult::Ok on success, or a specific failure code.
[[nodiscard]] VerifyResult VerifyManifestSignature(std::string_view raw_manifest_json,
                                                   std::string_view signature_hex) noexcept;

// Test seam: verify against a caller-supplied public key instead of the embedded
// one (the embedded key is an invalid all-zero placeholder in dev builds and
// rejects every signature, so a real round-trip cannot be exercised otherwise).
[[nodiscard]] VerifyResult VerifyManifestSignature(std::string_view raw_manifest_json, std::string_view signature_hex,
                                                   const uint8_t pub_key[32]) noexcept;

// Downgrade guard: returns false if the manifest version is below the current
// installed version OR below minimum_accepted_version.
[[nodiscard]] bool IsDowngrade(const UpdateManifest& manifest, const SemVer& current_version) noexcept;

} // namespace exosnap::update
