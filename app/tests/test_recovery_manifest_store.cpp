#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "settings/RecoveryManifestStore.h"

namespace exosnap {
namespace {

// =============================================================================
// Helpers
// =============================================================================

QString UniqueTempStorePath() {
    const QString temp = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    static int s_counter = 0;
    return QDir(temp).filePath(QStringLiteral("exosnap_recovery_test_%1.json").arg(++s_counter));
}

RecoveryManifestEntry MakeEntry(const QString& id = QStringLiteral("test-id"),
                                const QString& artefact = QStringLiteral("/tmp/test.mkv"),
                                const QString& container = QStringLiteral("mkv")) {
    RecoveryManifestEntry e;
    e.id = id;
    e.artefact_path = artefact;
    e.intended_container = container;
    e.final_output_path = artefact;
    e.started_at = QStringLiteral("2026-06-13T00:00:00Z");
    e.finalized = false;
    return e;
}

// =============================================================================
// 1. Missing store returns empty entries
// =============================================================================

TEST(RecoveryManifestStoreTest, MissingStoreReturnsEmpty) {
    const QString path = UniqueTempStorePath();
    if (QFileInfo::exists(path))
        QFile::remove(path);

    RecoveryManifestStore store(path);
    EXPECT_TRUE(store.Entries().isEmpty());
}

// =============================================================================
// 2. Roundtrip — Add then Load
// =============================================================================

TEST(RecoveryManifestStoreTest, Roundtrip) {
    const QString path = UniqueTempStorePath();
    RecoveryManifestStore store(path);

    auto e = MakeEntry(QStringLiteral("abc-123"));
    ASSERT_TRUE(store.Add(e));

    const auto entries = store.Entries();
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].id, QStringLiteral("abc-123"));
    EXPECT_EQ(entries[0].artefact_path, e.artefact_path);
    EXPECT_EQ(entries[0].intended_container, QStringLiteral("mkv"));
    EXPECT_FALSE(entries[0].finalized);

    QFile::remove(path);
}

// =============================================================================
// 3. Remove by id
// =============================================================================

TEST(RecoveryManifestStoreTest, RemoveById) {
    const QString path = UniqueTempStorePath();
    RecoveryManifestStore store(path);

    ASSERT_TRUE(store.Add(MakeEntry(QStringLiteral("id-1"))));
    ASSERT_TRUE(store.Add(MakeEntry(QStringLiteral("id-2"), QStringLiteral("/tmp/b.mkv"))));

    EXPECT_TRUE(store.Remove(QStringLiteral("id-1")));

    const auto entries = store.Entries();
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].id, QStringLiteral("id-2"));

    QFile::remove(path);
}

// =============================================================================
// 4. Remove non-existent id is a no-op (returns true)
// =============================================================================

TEST(RecoveryManifestStoreTest, RemoveNonExistentIsNoOp) {
    const QString path = UniqueTempStorePath();
    RecoveryManifestStore store(path);

    ASSERT_TRUE(store.Add(MakeEntry(QStringLiteral("only"))));
    EXPECT_TRUE(store.Remove(QStringLiteral("does-not-exist")));

    // Entry should still be there.
    EXPECT_EQ(store.Entries().size(), 1);

    QFile::remove(path);
}

// =============================================================================
// 5. UpdateFinalized
// =============================================================================

TEST(RecoveryManifestStoreTest, UpdateFinalized) {
    const QString path = UniqueTempStorePath();
    RecoveryManifestStore store(path);

    ASSERT_TRUE(store.Add(MakeEntry(QStringLiteral("fin-id"))));
    EXPECT_FALSE(store.Entries()[0].finalized);

    EXPECT_TRUE(store.UpdateFinalized(QStringLiteral("fin-id"), true));
    EXPECT_TRUE(store.Entries()[0].finalized);

    EXPECT_TRUE(store.UpdateFinalized(QStringLiteral("fin-id"), false));
    EXPECT_FALSE(store.Entries()[0].finalized);

    QFile::remove(path);
}

// =============================================================================
// 6. Corrupt JSON resets to empty (no crash)
// =============================================================================

TEST(RecoveryManifestStoreTest, CorruptJsonResetsToEmpty) {
    const QString path = UniqueTempStorePath();
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("{ this is: not valid JSON %%%");
    }

    RecoveryManifestStore store(path);
    EXPECT_TRUE(store.Entries().isEmpty());

    QFile::remove(path);
}

// =============================================================================
// 7. Incompatible schema version resets to empty
// =============================================================================

TEST(RecoveryManifestStoreTest, FutureSchemaVersionResetsToEmpty) {
    const QString path = UniqueTempStorePath();
    {
        QJsonObject root;
        root[QStringLiteral("schema_version")] = 999;
        root[QStringLiteral("entries")] = QJsonArray();
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(root).toJson());
    }

    RecoveryManifestStore store(path);
    EXPECT_TRUE(store.Entries().isEmpty());

    QFile::remove(path);
}

// =============================================================================
// 8. EXOSNAP_CONFIG_DIR isolation
// =============================================================================

TEST(RecoveryManifestStoreTest, DefaultConstructorUsesConfigDir) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    qputenv("EXOSNAP_CONFIG_DIR", tmp.path().toUtf8());
    RecoveryManifestStore store;
    const QString expected = QDir(tmp.path()).filePath(QStringLiteral("recovery-manifest.json"));
    EXPECT_EQ(store.StorePath(), expected);
    qunsetenv("EXOSNAP_CONFIG_DIR");
}

// =============================================================================
// 9. MP4 container entry roundtrip
// =============================================================================

TEST(RecoveryManifestStoreTest, Mp4ContainerRoundtrip) {
    const QString path = UniqueTempStorePath();
    RecoveryManifestStore store(path);

    auto e = MakeEntry(QStringLiteral("mp4-id"), QStringLiteral("/tmp/test.mkv.tmp"), QStringLiteral("mp4"));
    e.final_output_path = QStringLiteral("/tmp/test.mp4");
    e.finalized = true;
    ASSERT_TRUE(store.Add(e));

    const auto entries = store.Entries();
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].intended_container, QStringLiteral("mp4"));
    EXPECT_EQ(entries[0].final_output_path, QStringLiteral("/tmp/test.mp4"));
    EXPECT_TRUE(entries[0].finalized);

    QFile::remove(path);
}

} // namespace
} // namespace exosnap
