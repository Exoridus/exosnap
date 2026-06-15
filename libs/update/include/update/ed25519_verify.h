#pragma once
// ed25519_verify.h -- verify-only ed25519 (RFC 8032) for the ExoSnap updater.
//
// On Windows 10 1607+ BCryptVerifySignature implements ed25519 natively via
// the DSA_ECDSA algorithm with BCRYPT_ECDSA_P256_ALG_HANDLE -- but ed25519
// is NOT Edwards; it uses the Bernstein curve25519 / ed25519 algorithm which
// maps to BCRYPT_EDDSA_ALGORITHM on Windows 11 22H2+ (KB5030310) or a
// preview feature on older builds.
//
// Because ExoSnap targets Windows 10+ broadly (not Windows 11 exclusively)
// we vendor a minimal, self-contained, public-domain ed25519 verify
// implementation (derived from SUPERCOP/ref10 by Bernstein, Lange, Schwabe).
// Only verify is exposed; no signing, no key generation.
//
// Thread-safe (no global state, no dynamic allocation).

#include <cstddef>
#include <cstdint>

namespace exosnap::update {

// Verify an ed25519 signature (RFC 8032).
//   sig[64]    -- the 64-byte raw signature (R || S)
//   msg        -- pointer to the message
//   msg_len    -- length of the message in bytes
//   pub_key[32]-- the 32-byte public key
//
// Returns true iff the signature is valid for the message under pub_key.
[[nodiscard]] bool ed25519_verify(const uint8_t sig[64], const uint8_t* msg, size_t msg_len,
                                  const uint8_t pub_key[32]) noexcept;

} // namespace exosnap::update
