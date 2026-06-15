// ed25519_verify.cpp -- thin wrapper around Monocypher's RFC-8032 ed25519.
//
// The CI signs manifests with pynacl (standard RFC-8032 / SHA-512 ed25519).
// Monocypher ships two variants:
//   - crypto_eddsa_check   (monocypher.c)          uses BLAKE2b  -- NOT RFC 8032
//   - crypto_ed25519_check (monocypher-ed25519.c)  uses SHA-512  -- RFC 8032 ✓
//
// We MUST use crypto_ed25519_check.  The BLAKE2b variant would reject every
// signature produced by pynacl / libsodium.
//
// monocypher-ed25519.h already has proper extern "C" guards for C++ callers.

#include <monocypher-ed25519.h>
#include <update/ed25519_verify.h>

namespace exosnap::update {

bool ed25519_verify(const uint8_t sig[64], const uint8_t* msg, size_t msg_len, const uint8_t pub_key[32]) noexcept {
    // crypto_ed25519_check returns 0 on success, -1 on failure.
    return crypto_ed25519_check(sig, pub_key, msg, msg_len) == 0;
}

} // namespace exosnap::update
