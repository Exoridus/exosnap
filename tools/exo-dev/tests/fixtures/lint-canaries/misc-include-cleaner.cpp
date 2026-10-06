#include "include-cleaner-bridge.h"

CanaryToken Take(CanaryToken token) {
    return std::move(token);
}
