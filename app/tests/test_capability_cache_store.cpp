#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "settings/CapabilityCacheStore.h"

namespace exosnap {
namespace {

// =============================================================================
// Helpers
// =============================================================================

QString UniqueTempStorePath() {
    // Unique temp dir per test process (gtest_discover_tests = one process per
    // test); a shared fixed name races under ctest -j.
    static QTemporaryDir s_dir;
    static int s_counter = 0;
    return s_dir.filePath(QStringLiteral("exosnap_capability_cache_test_%1.json").arg(++s_counter));
}

capability::CapabilityCacheKey MakeKey(int64_t luid = 12345, const std::string& driver = "31.0.15.3623",
                                       const std::string& app_version = "0.9.0") {
    capability::CapabilityCacheKey key;
    key.adapter_luid = luid;
    key.driver_version = driver;
    key.app_version = app_version;
    key.schema_version = capability::kCapabilityCacheSchemaVersion;
    return key;
}

capability::RuntimeCapabilitySnapshot MakeSnapshot() {
    capability::RuntimeCapabilitySnapshot snap;
    snap.nvidia.nvenc_dll_present = true;
    snap.nvidia.nvenc_api_version_valid = true;
    snap.nvidia.nvenc_api_version = 0x000D0000u;
    snap.nvidia.adapter_name = "NVIDIA GeForce RTX 9999 (synthetic)";
    snap.nvidia.nvenc_codec_probed = true;
    snap.nvidia.nvenc_av1 = true;
    snap.nvidia.nvenc_hevc = true;
    snap.nvidia.nvenc_h264 = true;
    snap.nvidia.nvenc_yuv444_h264 = false;
    snap.nvidia.nvenc_yuv444_hevc = true;
    snap.nvidia.nvenc_adv_h264 = {2, 1, true, true}; // max_bframes, bframe_ref_mode, lookahead, temporal_aq
    snap.nvidia.nvenc_adv_hevc = {3, 2, true, false};
    snap.nvidia.nvenc_adv_av1 = {0, 0, false, false}; // GPU advertises AV1 but with no B-frame/lookahead support
    snap.mf_webcam.available = true;
    snap.os.build_number = 26100u;
    snap.os.version_string = "10.0.26100";

    capability::DisplayHdrFacts display;
    display.name = "\\\\.\\DISPLAY1";
    display.hdr_active = true;
    display.bits_per_color = 10;
    display.max_luminance_nits = 1000.0f;
    display.min_luminance_nits = 0.01f;
    display.white_point_x = 0.3127f;
    snap.displays.push_back(display);

    return snap;
}

// =============================================================================
// 1. Missing store returns nullopt
// =============================================================================

TEST(CapabilityCacheStoreTest, MissingStoreReturnsNullopt) {
    const QString path = UniqueTempStorePath();
    if (QFileInfo::exists(path))
        QFile::remove(path);

    CapabilityCacheStore store(path);
    EXPECT_FALSE(store.LoadMatching(MakeKey()).has_value());
}

// =============================================================================
// 2. Roundtrip — Save then LoadMatching with the same key returns the snapshot
// =============================================================================

TEST(CapabilityCacheStoreTest, RoundtripWithMatchingKey) {
    const QString path = UniqueTempStorePath();
    CapabilityCacheStore store(path);

    const auto key = MakeKey();
    const auto snapshot = MakeSnapshot();
    ASSERT_TRUE(store.Save(snapshot, key));

    const auto loaded = store.LoadMatching(key);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->nvidia.adapter_name, snapshot.nvidia.adapter_name);
    EXPECT_EQ(loaded->nvidia.nvenc_api_version, snapshot.nvidia.nvenc_api_version);
    EXPECT_TRUE(loaded->nvidia.nvenc_codec_probed);
    EXPECT_TRUE(loaded->nvidia.nvenc_av1);
    EXPECT_FALSE(loaded->nvidia.nvenc_yuv444_h264);
    EXPECT_TRUE(loaded->nvidia.nvenc_yuv444_hevc);
    EXPECT_EQ(loaded->nvidia.nvenc_adv_h264.max_bframes, 2);
    EXPECT_EQ(loaded->nvidia.nvenc_adv_h264.bframe_ref_mode, 1);
    EXPECT_TRUE(loaded->nvidia.nvenc_adv_h264.lookahead);
    EXPECT_TRUE(loaded->nvidia.nvenc_adv_h264.temporal_aq);
    EXPECT_EQ(loaded->nvidia.nvenc_adv_hevc.max_bframes, 3);
    EXPECT_FALSE(loaded->nvidia.nvenc_adv_hevc.temporal_aq);
    EXPECT_EQ(loaded->nvidia.nvenc_adv_av1.max_bframes, 0);
    EXPECT_FALSE(loaded->nvidia.nvenc_adv_av1.lookahead);
    EXPECT_TRUE(loaded->mf_webcam.available);
    EXPECT_EQ(loaded->os.build_number, snapshot.os.build_number);
    ASSERT_EQ(loaded->displays.size(), 1u);
    EXPECT_EQ(loaded->displays[0].name, "\\\\.\\DISPLAY1");
    EXPECT_TRUE(loaded->displays[0].hdr_active);
    EXPECT_EQ(loaded->displays[0].bits_per_color, 10u);
    EXPECT_FLOAT_EQ(loaded->displays[0].max_luminance_nits, 1000.0f);

    QFile::remove(path);
}

// =============================================================================
// 3. Key mismatch (any field) discards the cache
// =============================================================================

TEST(CapabilityCacheStoreTest, AdapterLuidMismatchDiscardsCache) {
    const QString path = UniqueTempStorePath();
    CapabilityCacheStore store(path);
    ASSERT_TRUE(store.Save(MakeSnapshot(), MakeKey(/*luid=*/1)));
    EXPECT_FALSE(store.LoadMatching(MakeKey(/*luid=*/2)).has_value());
    QFile::remove(path);
}

TEST(CapabilityCacheStoreTest, DriverVersionMismatchDiscardsCache) {
    const QString path = UniqueTempStorePath();
    CapabilityCacheStore store(path);
    ASSERT_TRUE(store.Save(MakeSnapshot(), MakeKey(12345, "31.0.15.3623")));
    EXPECT_FALSE(store.LoadMatching(MakeKey(12345, "31.0.15.9999")).has_value());
    QFile::remove(path);
}

TEST(CapabilityCacheStoreTest, AppVersionMismatchDiscardsCache) {
    const QString path = UniqueTempStorePath();
    CapabilityCacheStore store(path);
    ASSERT_TRUE(store.Save(MakeSnapshot(), MakeKey(12345, "31.0.15.3623", "0.9.0")));
    EXPECT_FALSE(store.LoadMatching(MakeKey(12345, "31.0.15.3623", "0.9.1")).has_value());
    QFile::remove(path);
}

TEST(CapabilityCacheStoreTest, SchemaVersionMismatchDiscardsCache) {
    const QString path = UniqueTempStorePath();
    CapabilityCacheStore store(path);
    ASSERT_TRUE(store.Save(MakeSnapshot(), MakeKey()));

    auto future_key = MakeKey();
    future_key.schema_version = capability::kCapabilityCacheSchemaVersion + 1;
    EXPECT_FALSE(store.LoadMatching(future_key).has_value());
    QFile::remove(path);
}

// =============================================================================
// 4. Corrupt JSON discards without crash
// =============================================================================

TEST(CapabilityCacheStoreTest, CorruptJsonDiscardsWithoutCrash) {
    const QString path = UniqueTempStorePath();
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("{ this is: not valid JSON %%%");
    }

    CapabilityCacheStore store(path);
    EXPECT_FALSE(store.LoadMatching(MakeKey()).has_value());

    QFile::remove(path);
}

// =============================================================================
// 5. A Save() after a discarded/mismatched cache overwrites it (no migration)
// =============================================================================

TEST(CapabilityCacheStoreTest, SaveOverwritesAMismatchedCache) {
    const QString path = UniqueTempStorePath();
    CapabilityCacheStore store(path);

    ASSERT_TRUE(store.Save(MakeSnapshot(), MakeKey(/*luid=*/1)));
    EXPECT_FALSE(store.LoadMatching(MakeKey(/*luid=*/2)).has_value());

    // Simulates the real probe completing and rewriting the cache for the
    // current (new) adapter.
    ASSERT_TRUE(store.Save(MakeSnapshot(), MakeKey(/*luid=*/2)));
    EXPECT_TRUE(store.LoadMatching(MakeKey(/*luid=*/2)).has_value());

    QFile::remove(path);
}

// =============================================================================
// 6. EXOSNAP_CONFIG_DIR isolation
// =============================================================================

TEST(CapabilityCacheStoreTest, DefaultConstructorUsesConfigDir) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    qputenv("EXOSNAP_CONFIG_DIR", tmp.path().toUtf8());
    CapabilityCacheStore store;
    const QString expected = QDir(tmp.path()).filePath(QStringLiteral("capability-cache.json"));
    EXPECT_EQ(store.StorePath(), expected);
    qunsetenv("EXOSNAP_CONFIG_DIR");
}

} // namespace
} // namespace exosnap
