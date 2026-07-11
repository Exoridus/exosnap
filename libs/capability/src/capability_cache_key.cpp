#include <capability/capability_cache_key.h>

namespace exosnap::capability {

CapabilityCacheKey BuildCapabilityCacheKey(const AdapterIdentity& identity, std::string_view app_version) {
    CapabilityCacheKey key;
    key.adapter_luid = identity.adapter_luid;
    key.driver_version = identity.driver_version;
    key.app_version = std::string(app_version);
    key.schema_version = kCapabilityCacheSchemaVersion;
    return key;
}

} // namespace exosnap::capability
