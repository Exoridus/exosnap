#include <update/release_key.h>

#include <cstdint>

namespace exosnap::update {

#ifdef EXOSNAP_UPDATE_PUBLIC_KEY_BYTES
const uint8_t kUpdatePublicKey[32] = {EXOSNAP_UPDATE_PUBLIC_KEY_BYTES};
#else
// y = 2 is not a valid Ed25519 curve point.
const uint8_t kUpdatePublicKey[32] = {2};
#endif

} // namespace exosnap::update
