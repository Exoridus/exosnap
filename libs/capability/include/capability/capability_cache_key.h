#pragma once

#include "runtime_snapshot.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace exosnap::capability {

// Bumped whenever the on-disk cache payload shape changes. A stored key with a
// different schema_version is always treated as a mismatch — no migration
// (pre-v1.0 policy): the cache is discarded and silently rewritten by the next
// real probe.
inline constexpr int kCapabilityCacheSchemaVersion = 3;

// Identifies whether a persisted RuntimeCapabilitySnapshot cache entry is
// still valid for the CURRENT adapter/driver/app build. A mismatch on any
// field means the cache is stale (GPU swapped, driver updated, app upgraded,
// or the on-disk schema changed) and must be discarded, never merged.
//
// This key gates ONLY the disk-cache warm-start path (see
// app/settings/CapabilityCacheStore.h in the application layer); it has no
// bearing on the real probe (CapabilityBuilder::BuildFromHardwareQuery),
// which always runs on every launch regardless of cache state.
struct CapabilityCacheKey {
    int64_t adapter_luid = 0;
    std::string driver_version;
    std::string app_version;
    int schema_version = kCapabilityCacheSchemaVersion;

    bool operator==(const CapabilityCacheKey&) const = default;
};

// Builds the cache key for the CURRENT system from a cheap adapter-identity
// read (CapabilityBuilder::QueryAdapterIdentity) and the running app's
// version string. Pure function — no probing, no I/O.
CapabilityCacheKey BuildCapabilityCacheKey(const AdapterIdentity& identity, std::string_view app_version);

} // namespace exosnap::capability
