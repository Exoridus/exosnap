#include <gtest/gtest.h>

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "BlockingSurfaceArbiter.h"
#include "CrashReportAdapter.h"
#include "RecoveryAdapter.h"

// QCR-403. The state matrix for the two startup-owned modal surfaces.
//
// Both are raised from the composition root and both can be asked for while the
// other is already up. The z-order between their loaders decides what is drawn
// on top and nothing else — two active loaders means the covered one still owns
// focusable controls — so which one may be up is decided here instead.
//
// The reverse direction is the one that used to be missing: the crash prompt
// deferred behind an open recovery surface, but the standing "Recover last
// session?" notification raised recovery with no check at all, and its desktop
// toast is a separate always-on-top window that stays clickable while a modal
// scrim covers the shell.

namespace exosnap::quick {
namespace {

QString TempPath(const QString& name) {
    static QTemporaryDir s_dir;
    static int s_counter = 0;
    return s_dir.filePath(QStringLiteral("arbiter_%1_%2").arg(++s_counter).arg(name));
}

RecoveryCandidate MakeCandidate() {
    RecoveryCandidate candidate;
    candidate.entry.id = QStringLiteral("session");
    candidate.entry.artefact_path = TempPath(QStringLiteral("session.mkv"));
    candidate.entry.final_output_path = candidate.entry.artefact_path;
    candidate.entry.intended_container = QStringLiteral("mkv");
    candidate.entry.started_at = QStringLiteral("2026-08-09T21:14:00Z");
    candidate.artefact_size_bytes = 1024;
    return candidate;
}

class BlockingSurfaceArbiterTest : public ::testing::Test {
  protected:
    BlockingSurfaceArbiterTest() {
        // Seeded rather than scanned: this fixture is about arbitration, and the
        // adapter refuses to open a surface with no candidates at all.
        recovery_.seedCandidatesForVisualHarness({MakeCandidate()});
        arbiter_.setSurfaces(&recovery_, &crash_);
    }

    void presentCrash() {
        CrashReportContext context;
        context.version = QStringLiteral("0.9.0");
        crash_.present(context, /*crash_folder_available=*/false);
    }

    RecoveryAdapter recovery_;
    CrashReportAdapter crash_;
    BlockingSurfaceArbiter arbiter_;
};

// ─── One at a time ───────────────────────────────────────────────────────────

TEST_F(BlockingSurfaceArbiterTest, RecoveryAloneIsRaisedImmediately) {
    arbiter_.requestRecovery();

    EXPECT_TRUE(recovery_.surfaceOpen());
    EXPECT_FALSE(arbiter_.recoveryQueued());
}

TEST_F(BlockingSurfaceArbiterTest, CrashAloneIsRaisedImmediately) {
    QSignalSpy raised(&arbiter_, &BlockingSurfaceArbiter::crashSurfaceRequested);

    arbiter_.requestCrash();

    EXPECT_EQ(raised.count(), 1);
    EXPECT_FALSE(arbiter_.crashQueued());
}

TEST_F(BlockingSurfaceArbiterTest, CrashDefersBehindOpenRecovery) {
    arbiter_.requestRecovery();
    QSignalSpy raised(&arbiter_, &BlockingSurfaceArbiter::crashSurfaceRequested);

    arbiter_.requestCrash();

    EXPECT_EQ(raised.count(), 0);
    EXPECT_TRUE(arbiter_.crashQueued());
    EXPECT_TRUE(recovery_.surfaceOpen()) << "the request must not disturb the surface that is already up";
}

TEST_F(BlockingSurfaceArbiterTest, RecoveryDefersBehindActiveCrash) {
    presentCrash();

    arbiter_.requestRecovery();

    EXPECT_FALSE(recovery_.surfaceOpen()) << "two active modal surfaces is exactly what this must not produce";
    EXPECT_TRUE(arbiter_.recoveryQueued());
    EXPECT_TRUE(crash_.active());
}

// ─── The queued surface comes due ────────────────────────────────────────────

TEST_F(BlockingSurfaceArbiterTest, DismissingRecoveryRaisesTheQueuedCrashPrompt) {
    arbiter_.requestRecovery();
    arbiter_.requestCrash();
    QSignalSpy raised(&arbiter_, &BlockingSurfaceArbiter::crashSurfaceRequested);

    recovery_.dismiss();

    EXPECT_EQ(raised.count(), 1);
    EXPECT_FALSE(arbiter_.crashQueued());
}

TEST_F(BlockingSurfaceArbiterTest, AnsweringTheCrashPromptRaisesTheQueuedRecoverySurface) {
    presentCrash();
    arbiter_.requestRecovery();

    crash_.dontSend();

    EXPECT_FALSE(crash_.active());
    EXPECT_TRUE(recovery_.surfaceOpen());
    EXPECT_FALSE(arbiter_.recoveryQueued());
}

TEST_F(BlockingSurfaceArbiterTest, DismissingTheCrashPromptStillRaisesQueuedRecovery) {
    presentCrash();
    arbiter_.requestRecovery();

    // Dismiss commits no consent decision, but it does lower the surface — so
    // the recovery offer behind it is due either way.
    crash_.dismiss();

    EXPECT_TRUE(recovery_.surfaceOpen());
}

// ─── Single shot ─────────────────────────────────────────────────────────────

TEST_F(BlockingSurfaceArbiterTest, AQueuedCrashPromptIsRaisedOnlyOnce) {
    arbiter_.requestRecovery();
    arbiter_.requestCrash();
    QSignalSpy raised(&arbiter_, &BlockingSurfaceArbiter::crashSurfaceRequested);

    recovery_.dismiss();
    // The standing notification can raise and lower recovery any number of
    // times afterwards; a crash prompt the user has already answered must not
    // come back with it.
    arbiter_.requestRecovery();
    recovery_.dismiss();

    EXPECT_EQ(raised.count(), 1);
}

TEST_F(BlockingSurfaceArbiterTest, RepeatedRecoveryRequestsWhileCrashIsUpQueueOnce) {
    presentCrash();

    arbiter_.requestRecovery();
    arbiter_.requestRecovery();
    arbiter_.requestRecovery();
    crash_.dismiss();

    EXPECT_TRUE(recovery_.surfaceOpen());
    EXPECT_FALSE(arbiter_.recoveryQueued());
}

// ─── Nothing is lost ─────────────────────────────────────────────────────────

TEST_F(BlockingSurfaceArbiterTest, ClosingOneSurfaceWithNothingQueuedChangesNothing) {
    QSignalSpy raised(&arbiter_, &BlockingSurfaceArbiter::crashSurfaceRequested);

    arbiter_.requestRecovery();
    recovery_.dismiss();

    EXPECT_EQ(raised.count(), 0);
    EXPECT_FALSE(recovery_.surfaceOpen());
}

TEST_F(BlockingSurfaceArbiterTest, WithoutSurfacesEveryRequestIsInert) {
    BlockingSurfaceArbiter bare;
    QSignalSpy raised(&bare, &BlockingSurfaceArbiter::crashSurfaceRequested);

    bare.requestRecovery();
    bare.requestCrash();

    // A frontend with no recovery/crash wiring stays inert rather than raising a
    // surface that has nothing behind it. requestCrash() is the one that can
    // still fire: the composition root, not the adapter, owns that surface.
    EXPECT_EQ(raised.count(), 1);
    EXPECT_FALSE(bare.recoveryQueued());
}

} // namespace
} // namespace exosnap::quick
