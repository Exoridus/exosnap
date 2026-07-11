#include <gtest/gtest.h>

#include <capability/capability_builder.h>
#include <capability/capability_cache_key.h>
#include <capability/capability_set.h>
#include <capability/runtime_snapshot.h>

namespace exosnap::capability {
namespace {

// -------------------------------------------------------------------------
// TC-1: BuildCapabilityCacheKey copies every identity/app-version field.
// -------------------------------------------------------------------------
TEST(CapabilityCacheKeyTest, BuildCopiesAllFields) {
    AdapterIdentity identity;
    identity.adapter_luid = 0x1122334455667788LL;
    identity.driver_version = "31.0.15.3623";

    const CapabilityCacheKey key = BuildCapabilityCacheKey(identity, "0.9.0");

    EXPECT_EQ(key.adapter_luid, identity.adapter_luid);
    EXPECT_EQ(key.driver_version, identity.driver_version);
    EXPECT_EQ(key.app_version, "0.9.0");
    EXPECT_EQ(key.schema_version, kCapabilityCacheSchemaVersion);
}

// -------------------------------------------------------------------------
// TC-2: Equality requires every field to match.
// -------------------------------------------------------------------------
TEST(CapabilityCacheKeyTest, EqualityRequiresAllFieldsToMatch) {
    AdapterIdentity identity;
    identity.adapter_luid = 42;
    identity.driver_version = "1.2.3.4";
    const CapabilityCacheKey base = BuildCapabilityCacheKey(identity, "0.9.0");

    CapabilityCacheKey same = base;
    EXPECT_EQ(base, same);

    CapabilityCacheKey different_luid = base;
    different_luid.adapter_luid = 43;
    EXPECT_NE(base, different_luid);

    CapabilityCacheKey different_driver = base;
    different_driver.driver_version = "1.2.3.5";
    EXPECT_NE(base, different_driver);

    CapabilityCacheKey different_app = base;
    different_app.app_version = "0.9.1";
    EXPECT_NE(base, different_app);

    CapabilityCacheKey different_schema = base;
    different_schema.schema_version = kCapabilityCacheSchemaVersion + 1;
    EXPECT_NE(base, different_schema);
}

// -------------------------------------------------------------------------
// TC-3: A GPU with no reported driver version still yields a usable
// (if less precise) key — never throws, never crashes.
// -------------------------------------------------------------------------
TEST(CapabilityCacheKeyTest, MissingDriverVersionStillBuildsAKey) {
    AdapterIdentity identity; // default: luid=0, driver_version empty
    const CapabilityCacheKey key = BuildCapabilityCacheKey(identity, "0.9.0");
    EXPECT_EQ(key.adapter_luid, 0);
    EXPECT_TRUE(key.driver_version.empty());
    EXPECT_EQ(key.app_version, "0.9.0");
}

// -------------------------------------------------------------------------
// TC-4: CapabilitySet::probed defaults to false — the static baseline and any
// set rebuilt directly from a snapshot (the disk-cache warm-start path) must
// never claim to be freshly probed.
// -------------------------------------------------------------------------
TEST(CapabilityCacheKeyTest, CapabilitySetProbedDefaultsFalse) {
    const CapabilitySet baseline = CapabilityBuilder::BuildStaticValidatedBaseline();
    EXPECT_FALSE(baseline.probed);

    RuntimeCapabilitySnapshot snapshot; // synthetic, no hardware touched
    const CapabilitySet from_snapshot = CapabilityBuilder::BuildEffectiveCapabilities(snapshot);
    EXPECT_FALSE(from_snapshot.probed) << "BuildEffectiveCapabilities must never set probed=true on its own — "
                                          "only BuildFromHardwareQuery may (a disk-cache rebuild reuses "
                                          "BuildEffectiveCapabilities on a cached snapshot and must stay false).";
}

} // namespace
} // namespace exosnap::capability
