#include <gtest/gtest.h>

#include <QAbstractItemModel>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "RecoveryAdapter.h"

// Deterministic coverage of the recovery POLICY: what is offered, what a
// destructive action requires before it runs, and what survives a dismissal.
//
// The one path deliberately not covered here is a successful Finish: it is a
// real repair remux over real media, which recovery_drill_tests already drives
// against synthetic-pipeline artefacts. Everything this adapter decides ABOUT
// that operation — one at a time, cancel is not a failure, a resolved row leaves
// the surface — is reachable without it.

namespace exosnap::quick {
namespace {

QString TempPath(const QString& name) {
    static QTemporaryDir s_dir;
    static int s_counter = 0;
    return s_dir.filePath(QStringLiteral("recovery_%1_%2").arg(++s_counter).arg(name));
}

bool WriteFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write("artefact");
    return true;
}

RecoveryManifestEntry MakeEntry(const QString& id, const QString& artefact, bool finalized = false) {
    RecoveryManifestEntry entry;
    entry.id = id;
    entry.artefact_path = artefact;
    entry.final_output_path = artefact;
    entry.intended_container = QStringLiteral("mkv");
    entry.started_at = QStringLiteral("2026-08-09T21:14:00Z");
    entry.finalized = finalized;
    return entry;
}

class RecoveryAdapterTest : public ::testing::Test {
  protected:
    RecoveryAdapterTest() : store_(TempPath(QStringLiteral("manifest.json"))), service_(store_) {
        adapter_.setService(&service_);
    }

    // Registers a candidate whose artefact really exists on disk, which is what
    // Scan() requires to keep it.
    QString addLiveCandidate(const QString& id, bool finalized = false) {
        const QString artefact = TempPath(QStringLiteral("%1.mkv").arg(id));
        EXPECT_TRUE(WriteFile(artefact));
        store_.Add(MakeEntry(id, artefact, finalized));
        return artefact;
    }

    [[nodiscard]] QVariant role(int row, const char* name) const {
        QAbstractItemModel* model = const_cast<RecoveryAdapter&>(adapter_).model();
        const QHash<int, QByteArray> roles = model->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
            if (it.value() == name)
                return model->data(model->index(row, 0), it.key());
        }
        return {};
    }

    RecoveryManifestStore store_;
    RecoveryService service_;
    RecoveryAdapter adapter_;
};

// ─── Scan ────────────────────────────────────────────────────────────────────

TEST_F(RecoveryAdapterTest, EmptyManifestSurfacesNothing) {
    EXPECT_EQ(adapter_.scan(), 0);
    EXPECT_FALSE(adapter_.hasCandidates());

    // A clean previous session must not leave a surface behind — the failure
    // this guards against is an empty recovery card on every launch.
    adapter_.openSurface();
    EXPECT_FALSE(adapter_.surfaceOpen());
}

TEST_F(RecoveryAdapterTest, EntryWhoseArtefactIsGoneIsDropped) {
    store_.Add(MakeEntry(QStringLiteral("stale"), TempPath(QStringLiteral("never-written.mkv"))));

    EXPECT_EQ(adapter_.scan(), 0);
    EXPECT_FALSE(adapter_.hasCandidates());
    EXPECT_TRUE(store_.Entries().isEmpty()) << "a stale entry must be pruned from the manifest, not just hidden";
}

TEST_F(RecoveryAdapterTest, LiveCandidateIsSurfacedWithPresentationReadyRow) {
    addLiveCandidate(QStringLiteral("crashed"));

    EXPECT_EQ(adapter_.scan(), 1);
    EXPECT_TRUE(adapter_.hasCandidates());
    EXPECT_EQ(adapter_.candidateCount(), 1);

    EXPECT_TRUE(role(0, "displayName").toString().endsWith(QStringLiteral(".mkv")));
    // Size · date · container, all formatted here so QML never parses anything.
    const QString meta = role(0, "meta").toString();
    EXPECT_TRUE(meta.contains(QStringLiteral("2026-08-09")));
    EXPECT_TRUE(meta.contains(QStringLiteral("MKV")));
    EXPECT_TRUE(role(0, "canContinue").toBool());
    EXPECT_FALSE(role(0, "busy").toBool());
    EXPECT_TRUE(role(0, "status").toString().isEmpty());
}

TEST_F(RecoveryAdapterTest, FinalizedCandidateOffersNoContinue) {
    addLiveCandidate(QStringLiteral("stopped"), /*finalized=*/true);
    ASSERT_EQ(adapter_.scan(), 1);

    // A finalized entry is a deliberate stop whose remux failed — there is
    // nothing left to record into it.
    EXPECT_FALSE(role(0, "canContinue").toBool());

    QSignalSpy spy(&adapter_, &RecoveryAdapter::continueRequested);
    adapter_.continueSession(0);
    EXPECT_EQ(spy.count(), 0) << "Continue must be refused for a finalized entry, not merely hidden";
}

// ─── Surface lifecycle ───────────────────────────────────────────────────────

TEST_F(RecoveryAdapterTest, DismissKeepsCandidatesForTheNextLaunch) {
    addLiveCandidate(QStringLiteral("later"));
    ASSERT_EQ(adapter_.scan(), 1);
    adapter_.openSurface();
    ASSERT_TRUE(adapter_.surfaceOpen());

    adapter_.dismiss();

    EXPECT_FALSE(adapter_.surfaceOpen());
    EXPECT_TRUE(adapter_.hasCandidates()) << "\"Decide later\" resolves nothing";
    EXPECT_EQ(store_.Entries().size(), 1);

    // And the notification action can raise the same surface again.
    adapter_.openSurface();
    EXPECT_TRUE(adapter_.surfaceOpen());
}

// ─── Discard ─────────────────────────────────────────────────────────────────

TEST_F(RecoveryAdapterTest, DiscardWithoutArmingIsRefused) {
    const QString artefact = addLiveCandidate(QStringLiteral("armed"));
    ASSERT_EQ(adapter_.scan(), 1);

    adapter_.discard(0);

    EXPECT_EQ(adapter_.candidateCount(), 1);
    EXPECT_TRUE(QFile::exists(artefact)) << "an unconfirmed delete must not touch the artefact";
    EXPECT_EQ(store_.Entries().size(), 1);
}

TEST_F(RecoveryAdapterTest, ArmedDiscardDeletesArtefactAndEntry) {
    const QString artefact = addLiveCandidate(QStringLiteral("doomed"));
    ASSERT_EQ(adapter_.scan(), 1);
    adapter_.openSurface();

    adapter_.armDiscard(0);
    EXPECT_TRUE(role(0, "armedDiscard").toBool());

    adapter_.discard(0);

    EXPECT_EQ(adapter_.candidateCount(), 0);
    EXPECT_FALSE(QFile::exists(artefact));
    EXPECT_TRUE(store_.Entries().isEmpty());
    EXPECT_FALSE(adapter_.surfaceOpen()) << "the surface closes once the last candidate is resolved";
}

TEST_F(RecoveryAdapterTest, ArmingASecondRowDisarmsTheFirst) {
    addLiveCandidate(QStringLiteral("one"));
    addLiveCandidate(QStringLiteral("two"));
    ASSERT_EQ(adapter_.scan(), 2);

    adapter_.armDiscard(0);
    adapter_.armDiscard(1);

    EXPECT_FALSE(role(0, "armedDiscard").toBool());
    EXPECT_TRUE(role(1, "armedDiscard").toBool());

    // The disarmed row must not delete on a stray second click.
    adapter_.discard(0);
    EXPECT_EQ(adapter_.candidateCount(), 2);
}

// ─── Continue ────────────────────────────────────────────────────────────────

TEST_F(RecoveryAdapterTest, ContinueHandsTheEntryToTheCompositionRootAndLowersTheSurface) {
    addLiveCandidate(QStringLiteral("resume"));
    ASSERT_EQ(adapter_.scan(), 1);
    adapter_.openSurface();

    QSignalSpy spy(&adapter_, &RecoveryAdapter::continueRequested);
    adapter_.continueSession(0);

    ASSERT_EQ(spy.count(), 1);
    EXPECT_FALSE(adapter_.surfaceOpen()) << "arming lands on Record; a scrim over it would hide the session";
    // The candidate stays: arming does not resolve the manifest entry, the
    // coordinator does when the resumed session completes.
    EXPECT_EQ(adapter_.candidateCount(), 1);
}

TEST_F(RecoveryAdapterTest, OutOfRangeIndicesAreIgnored) {
    addLiveCandidate(QStringLiteral("only"));
    ASSERT_EQ(adapter_.scan(), 1);

    // A stale delegate must never be able to act on a row that no longer exists.
    adapter_.finish(7);
    adapter_.armDiscard(-1);
    adapter_.discard(7);
    adapter_.continueSession(7);

    EXPECT_EQ(adapter_.candidateCount(), 1);
    EXPECT_FALSE(adapter_.busy());
}

TEST_F(RecoveryAdapterTest, WithoutAServiceEveryVerbIsInert) {
    RecoveryAdapter unwired;

    EXPECT_EQ(unwired.scan(), 0);
    EXPECT_FALSE(unwired.hasCandidates());
    unwired.openSurface();
    unwired.finish(0);
    unwired.discard(0);
    EXPECT_FALSE(unwired.surfaceOpen());
}

} // namespace
} // namespace exosnap::quick
