#pragma once

#include <cstdint>

namespace exosnap::update {

// Ed25519 release verification key. Development builds carry an invalid curve point.
extern const uint8_t kUpdatePublicKey[32];

} // namespace exosnap::update
