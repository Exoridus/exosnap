#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <atomic>
#include <thread>
#include <vector>

#include "settings/RecoveryManifestStore.h"

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
    return s_dir.filePath(QStringLiteral("exosnap_recovery_test_%1.json").arg(++s_counter));
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

// =============================================================================
// 10. Concurrency (QCR-102)
// =============================================================================
//
// Every mutation is a load → mutate → save sequence. QSaveFile makes only the
// file replacement atomic, so without a store-owned lock two threads load the
// same snapshot and the later save discards the earlier one's change. These
// tests are deterministic in their assertions — the exact interleaving varies,
// but the required end state does not, and the counts below never depend on
// timing.

namespace {

// Runs `body(i)` on `thread_count` threads, released together so the mutations
// genuinely overlap rather than trickling out one at a time.
template <typename Body> void RunConcurrently(int thread_count, Body body) {
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(thread_count));
    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back([&, i] {
            ready.fetch_add(1);
            while (!go.load())
                std::this_thread::yield();
            body(i);
        });
    }
    while (ready.load() < thread_count)
        std::this_thread::yield();
    go.store(true);
    for (auto& t : threads)
        t.join();
}

} // namespace

// Concurrent Add: every entry must survive. A lost update shows up as a missing
// id, which is exactly the crash-recovery artefact that would never be offered.
TEST(RecoveryManifestStoreTest, ConcurrentAdd_NoLostEntries) {
    const QString path = UniqueTempStorePath();
    RecoveryManifestStore store(path);

    constexpr int kThreads = 8;
    constexpr int kPerThread = 12;
    RunConcurrently(kThreads, [&](int t) {
        for (int i = 0; i < kPerThread; ++i) {
            EXPECT_TRUE(store.Add(MakeEntry(QStringLiteral("add-%1-%2").arg(t).arg(i))));
        }
    });

    const auto entries = store.Entries();
    EXPECT_EQ(entries.size(), kThreads * kPerThread);
    QSet<QString> ids;
    for (const auto& e : entries)
        ids.insert(e.id);
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kPerThread; ++i) {
            EXPECT_TRUE(ids.contains(QStringLiteral("add-%1-%2").arg(t).arg(i))) << "lost entry add-" << t << "-" << i;
        }
    }

    QFile::remove(path);
}

// Concurrent Add on one half and Remove on the other. The removed ids must be
// gone and the added ones present — a resurrected deletion is as wrong as a lost
// insertion, and both are what an unsynchronized read-modify-write produces.
TEST(RecoveryManifestStoreTest, ConcurrentAddRemove_NoResurrectedEntries) {
    const QString path = UniqueTempStorePath();
    RecoveryManifestStore store(path);

    constexpr int kPairs = 40;
    // Seed the entries the remover will delete.
    for (int i = 0; i < kPairs; ++i)
        ASSERT_TRUE(store.Add(MakeEntry(QStringLiteral("seed-%1").arg(i))));

    RunConcurrently(2, [&](int role) {
        for (int i = 0; i < kPairs; ++i) {
            if (role == 0)
                EXPECT_TRUE(store.Add(MakeEntry(QStringLiteral("fresh-%1").arg(i))));
            else
                EXPECT_TRUE(store.Remove(QStringLiteral("seed-%1").arg(i)));
        }
    });

    const auto entries = store.Entries();
    QSet<QString> ids;
    for (const auto& e : entries)
        ids.insert(e.id);
    EXPECT_EQ(entries.size(), kPairs); // only the fresh ones remain
    for (int i = 0; i < kPairs; ++i) {
        EXPECT_TRUE(ids.contains(QStringLiteral("fresh-%1").arg(i))) << "lost fresh-" << i;
        EXPECT_FALSE(ids.contains(QStringLiteral("seed-%1").arg(i))) << "resurrected seed-" << i;
    }

    QFile::remove(path);
}

// UpdateFinalized racing Remove over a disjoint id set. Each finalize must stick
// and each removal must hold: the finalize thread's save must not restore an
// entry the remover already dropped, and vice versa.
TEST(RecoveryManifestStoreTest, ConcurrentUpdateFinalizedAndRemove_BothStick) {
    const QString path = UniqueTempStorePath();
    RecoveryManifestStore store(path);

    constexpr int kEach = 30;
    for (int i = 0; i < kEach; ++i) {
        ASSERT_TRUE(store.Add(MakeEntry(QStringLiteral("keep-%1").arg(i))));
        ASSERT_TRUE(store.Add(MakeEntry(QStringLiteral("drop-%1").arg(i))));
    }

    RunConcurrently(2, [&](int role) {
        for (int i = 0; i < kEach; ++i) {
            if (role == 0)
                EXPECT_TRUE(store.UpdateFinalized(QStringLiteral("keep-%1").arg(i), true));
            else
                EXPECT_TRUE(store.Remove(QStringLiteral("drop-%1").arg(i)));
        }
    });

    const auto entries = store.Entries();
    EXPECT_EQ(entries.size(), kEach);
    int finalized = 0;
    for (const auto& e : entries) {
        EXPECT_FALSE(e.id.startsWith(QStringLiteral("drop-"))) << "resurrected " << e.id.toStdString();
        if (e.finalized)
            ++finalized;
    }
    EXPECT_EQ(finalized, kEach) << "a finalize was overwritten by a concurrent save";

    QFile::remove(path);
}

// A reader running against a live mutation stream never observes a torn or
// half-applied manifest: entries are either fully valid or absent, and the file
// stays parseable throughout (the atomic QSaveFile replacement is retained).
TEST(RecoveryManifestStoreTest, ConcurrentLoadDuringMutation_StaysConsistent) {
    const QString path = UniqueTempStorePath();
    RecoveryManifestStore store(path);

    constexpr int kWrites = 120;
    std::atomic<bool> writing{true};
    std::atomic<int> bad_reads{0};

    std::thread reader([&] {
        while (writing.load()) {
            for (const auto& e : store.Entries()) {
                // Every entry the loader hands out is fully populated — a torn
                // read would surface as an empty id or artefact path.
                if (e.id.isEmpty() || e.artefact_path.isEmpty() || e.intended_container.isEmpty())
                    bad_reads.fetch_add(1);
            }
        }
    });

    for (int i = 0; i < kWrites; ++i) {
        ASSERT_TRUE(store.Add(MakeEntry(QStringLiteral("live-%1").arg(i))));
        if (i % 3 == 0)
            ASSERT_TRUE(store.UpdateFinalized(QStringLiteral("live-%1").arg(i), true));
    }
    writing.store(false);
    reader.join();

    EXPECT_EQ(bad_reads.load(), 0);
    EXPECT_EQ(store.Entries().size(), kWrites);

    QFile::remove(path);
}

} // namespace
} // namespace exosnap
