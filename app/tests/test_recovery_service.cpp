#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "services/RecoveryService.h"
#include "settings/RecoveryManifestStore.h"

namespace exosnap {
namespace {

// =============================================================================
// Helpers
// =============================================================================

QString UniqueTempPath(const QString& suffix = QStringLiteral(".json")) {
    const QString temp = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    static int s_counter = 0;
    return QDir(temp).filePath(QStringLiteral("exosnap_svc_test_%1%2").arg(++s_counter).arg(suffix));
}

// Write minimal valid bytes to path so QFileInfo::exists returns true.
bool CreateDummyFile(const QString& path, const QByteArray& content = QByteArray("dummy")) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(content);
    return true;
}

RecoveryManifestEntry MakeEntry(const QString& id, const QString& artefact,
                                const QString& container = QStringLiteral("mkv"), bool finalized = false) {
    RecoveryManifestEntry e;
    e.id = id;
    e.artefact_path = artefact;
    e.intended_container = container;
    e.final_output_path = artefact;
    e.started_at = QStringLiteral("2026-06-13T00:00:00Z");
    e.finalized = finalized;
    return e;
}

// =============================================================================
// 1. Scan removes orphaned entries (artefact no longer exists)
// =============================================================================

TEST(RecoveryServiceTest, ScanRemovesOrphanedEntries) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString store_path = QDir(tmp.path()).filePath(QStringLiteral("manifest.json"));
    RecoveryManifestStore store(store_path);
    RecoveryService service(store);

    // Entry whose artefact exists.
    const QString real_artefact = QDir(tmp.path()).filePath(QStringLiteral("real.mkv"));
    ASSERT_TRUE(CreateDummyFile(real_artefact));
    store.Add(MakeEntry(QStringLiteral("id-real"), real_artefact));

    // Entry whose artefact is gone.
    store.Add(MakeEntry(QStringLiteral("id-orphan"), QStringLiteral("/nonexistent/path.mkv")));

    const auto candidates = service.Scan();
    ASSERT_EQ(candidates.size(), 1);
    EXPECT_EQ(candidates[0].entry.id, QStringLiteral("id-real"));

    // Orphan must be removed from the manifest.
    EXPECT_EQ(store.Entries().size(), 1);
}

// =============================================================================
// 2. KeepAsMkv with finalized=true → rename
// =============================================================================

TEST(RecoveryServiceTest, KeepAsMkvFinalizedRenames) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString store_path = QDir(tmp.path()).filePath(QStringLiteral("manifest.json"));
    RecoveryManifestStore store(store_path);
    RecoveryService service(store);

    // Artefact: a .mkv.tmp (finalized engine output)
    const QString artefact = QDir(tmp.path()).filePath(QStringLiteral("recording.mkv.tmp"));
    ASSERT_TRUE(CreateDummyFile(artefact));

    const auto e = MakeEntry(QStringLiteral("keep-id"), artefact, QStringLiteral("mkv"), /*finalized=*/true);
    store.Add(e);

    const auto result = service.KeepAsMkv(e);
    EXPECT_TRUE(result.success) << result.message;

    // Artefact should be renamed (gone from tmp path).
    EXPECT_FALSE(QFileInfo::exists(artefact));

    // Entry should be removed.
    EXPECT_TRUE(store.Entries().isEmpty());

    // The renamed file should exist (same dir, .mkv extension).
    const QString expected = QDir(tmp.path()).filePath(QStringLiteral("recording.mkv"));
    EXPECT_TRUE(QFileInfo::exists(expected));
}

// =============================================================================
// 3. Discard deletes the artefact and removes the entry
// =============================================================================

TEST(RecoveryServiceTest, DiscardDeletesArtefactAndEntry) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString store_path = QDir(tmp.path()).filePath(QStringLiteral("manifest.json"));
    RecoveryManifestStore store(store_path);
    RecoveryService service(store);

    const QString artefact = QDir(tmp.path()).filePath(QStringLiteral("discard.mkv"));
    ASSERT_TRUE(CreateDummyFile(artefact));
    const auto e = MakeEntry(QStringLiteral("discard-id"), artefact);
    store.Add(e);

    const auto result = service.Discard(e);
    EXPECT_TRUE(result.success) << result.message;
    EXPECT_FALSE(QFileInfo::exists(artefact));
    EXPECT_TRUE(store.Entries().isEmpty());
}

// =============================================================================
// 4. Collision handling in KeepAsMkv rename
// =============================================================================

TEST(RecoveryServiceTest, KeepAsMkvHandlesCollision) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString store_path = QDir(tmp.path()).filePath(QStringLiteral("manifest.json"));
    RecoveryManifestStore store(store_path);
    RecoveryService service(store);

    const QString artefact = QDir(tmp.path()).filePath(QStringLiteral("recording.mkv.tmp"));
    ASSERT_TRUE(CreateDummyFile(artefact));

    // Pre-create the "natural" target to force collision resolution.
    const QString natural = QDir(tmp.path()).filePath(QStringLiteral("recording.mkv"));
    ASSERT_TRUE(CreateDummyFile(natural));

    const auto e = MakeEntry(QStringLiteral("col-id"), artefact, QStringLiteral("mkv"), /*finalized=*/true);
    store.Add(e);

    const auto result = service.KeepAsMkv(e);
    EXPECT_TRUE(result.success) << result.message;

    // Natural target is still there (untouched), artefact was renamed elsewhere.
    EXPECT_TRUE(QFileInfo::exists(natural));
    EXPECT_FALSE(QFileInfo::exists(artefact));

    // The renamed file must have "(2)" suffix.
    const QString collided = QDir(tmp.path()).filePath(QStringLiteral("recording (2).mkv"));
    EXPECT_TRUE(QFileInfo::exists(collided));
}

// =============================================================================
// 5. Scan returns size metadata
// =============================================================================

TEST(RecoveryServiceTest, ScanReturnsSizeMetadata) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString store_path = QDir(tmp.path()).filePath(QStringLiteral("manifest.json"));
    RecoveryManifestStore store(store_path);
    RecoveryService service(store);

    const QString artefact = QDir(tmp.path()).filePath(QStringLiteral("sized.mkv"));
    const QByteArray content(1024, 'X');
    ASSERT_TRUE(CreateDummyFile(artefact, content));
    store.Add(MakeEntry(QStringLiteral("size-id"), artefact));

    const auto candidates = service.Scan();
    ASSERT_EQ(candidates.size(), 1);
    EXPECT_EQ(candidates[0].artefact_size_bytes, 1024);
}

} // namespace
} // namespace exosnap
