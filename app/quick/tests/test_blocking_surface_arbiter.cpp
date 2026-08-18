#include <gtest/gtest.h>

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "BlockingSurfaceArbiter.h"
#include "CrashReportAdapter.h"
#include "RecordingErrorAdapter.h"
#include "RecoveryAdapter.h"

// QCR-403 / QCR-415. The state matrix for the three modal blocking surfaces.
//
// All three are raised from the composition root and any of them can be asked
// for while another is already up. The z-order between their loaders decides
// what is drawn on top and nothing else — two active loaders means the covered
// one still owns focusable controls — so which one may be up is decided here
// instead.
//
// The reverse direction was the one missing in QCR-403: the crash prompt
// deferred behind an open recovery surface, but the standing "Recover last
// session?" notification raised recovery with no check at all, and its desktop
// toast is a separate always-on-top window that stays clickable while a modal
// scrim covers the shell.
//
// QCR-415 added the third: the recording-error surface was raised straight from
// the result callback with no reference to the other two. Its path is reachable
// because the start hotkey is deliberately desktop-wide and a scrim inside the
// shell does not reach it.

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

models::RecordingFailureReport MakeFailureReport(const QString& detail) {
    models::RecordingFailureReport report;
    report.title = QStringLiteral("Recording stopped unexpectedly");
    report.summary = QStringLiteral("The encoder reported an error.");
    report.phase = QStringLiteral("Encode");
    report.detail = detail;
    return report;
}

class BlockingSurfaceArbiterTest : public ::testing::Test {
  protected:
    BlockingSurfaceArbiterTest() {
        // Seeded rather than scanned: this fixture is about arbitration, and the
        // adapter refuses to open a surface with no candidates at all.
        recovery_.seedCandidatesForVisualHarness({MakeCandidate()});
        arbiter_.setSurfaces(&recovery_, &crash_, &recording_error_);
        // The composition root owns the report the error surface shows, so the
        // arbiter only asks for it. This stands in for that half; the crash
        // surface is presented explicitly per case, exactly as before.
        QObject::connect(&arbiter_, &BlockingSurfaceArbiter::recordingErrorSurfaceRequested, &recording_error_,
                         [this]() { recording_error_.present(pending_report_, /*can_send_report=*/false); });
    }

    void presentCrash() {
        CrashReportContext context;
        context.version = QStringLiteral("0.9.0");
        crash_.present(context, /*crash_folder_available=*/false);
    }

    void requestFailure(const QString& detail) {
        pending_report_ = MakeFailureReport(detail);
        arbiter_.requestRecordingError();
    }

    RecoveryAdapter recovery_;
    CrashReportAdapter crash_;
    RecordingErrorAdapter recording_error_;
    BlockingSurfaceArbiter arbiter_;
    models::RecordingFailureReport pending_report_;
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

// ─── The admission fact the start path reads ─────────────────────────────────

TEST_F(BlockingSurfaceArbiterTest, AnySurfaceUpReportsWhatTheStartPathAsks) {
    EXPECT_FALSE(arbiter_.anySurfaceUp());

    arbiter_.requestRecovery();
    EXPECT_TRUE(arbiter_.anySurfaceUp());
    recovery_.dismiss();
    EXPECT_FALSE(arbiter_.anySurfaceUp());

    presentCrash();
    EXPECT_TRUE(arbiter_.anySurfaceUp());
    crash_.dismiss();
    EXPECT_FALSE(arbiter_.anySurfaceUp());

    requestFailure(QStringLiteral("failure"));
    EXPECT_TRUE(arbiter_.anySurfaceUp());
    recording_error_.dismiss();
    EXPECT_FALSE(arbiter_.anySurfaceUp());
}

// A merely QUEUED surface is not on screen, so it must not gate the start path —
// only what actually covers the transport does.
TEST_F(BlockingSurfaceArbiterTest, AQueuedSurfaceAloneDoesNotCountAsUp) {
    presentCrash();
    arbiter_.requestRecovery();
    ASSERT_TRUE(arbiter_.recoveryQueued());

    crash_.dismiss();
    // Recovery took the screen when the crash prompt came down, so this is still
    // true — but by being UP, not by having been queued.
    EXPECT_TRUE(arbiter_.anySurfaceUp());
    recovery_.dismiss();
    EXPECT_FALSE(arbiter_.anySurfaceUp());
}

// ─── QCR-415: the third surface ──────────────────────────────────────────────

TEST_F(BlockingSurfaceArbiterTest, RecordingErrorAloneIsRaisedImmediately) {
    requestFailure(QStringLiteral("first"));

    EXPECT_TRUE(recording_error_.active());
    EXPECT_FALSE(arbiter_.recordingErrorQueued());
}

TEST_F(BlockingSurfaceArbiterTest, RecordingErrorDefersBehindOpenRecovery) {
    arbiter_.requestRecovery();

    requestFailure(QStringLiteral("while recovery is up"));

    EXPECT_FALSE(recording_error_.active()) << "two active modal surfaces is exactly what this must not produce";
    EXPECT_TRUE(arbiter_.recordingErrorQueued());
    EXPECT_TRUE(recovery_.surfaceOpen()) << "the surface already up keeps precedence";
}

TEST_F(BlockingSurfaceArbiterTest, RecordingErrorDefersBehindActiveCrash) {
    presentCrash();

    requestFailure(QStringLiteral("while the crash prompt is up"));

    EXPECT_FALSE(recording_error_.active());
    EXPECT_TRUE(arbiter_.recordingErrorQueued());
    EXPECT_TRUE(crash_.active());
}

TEST_F(BlockingSurfaceArbiterTest, RecoveryDefersBehindTheRecordingErrorSurface) {
    requestFailure(QStringLiteral("failure first"));

    arbiter_.requestRecovery();

    EXPECT_FALSE(recovery_.surfaceOpen());
    EXPECT_TRUE(arbiter_.recoveryQueued());
    EXPECT_TRUE(recording_error_.active());
}

TEST_F(BlockingSurfaceArbiterTest, CrashDefersBehindTheRecordingErrorSurface) {
    requestFailure(QStringLiteral("failure first"));
    QSignalSpy raised(&arbiter_, &BlockingSurfaceArbiter::crashSurfaceRequested);

    arbiter_.requestCrash();

    EXPECT_EQ(raised.count(), 0);
    EXPECT_TRUE(arbiter_.crashQueued());
    EXPECT_TRUE(recording_error_.active());
}

TEST_F(BlockingSurfaceArbiterTest, DismissingRecoveryRaisesTheQueuedRecordingError) {
    arbiter_.requestRecovery();
    requestFailure(QStringLiteral("queued behind recovery"));

    recovery_.dismiss();

    EXPECT_TRUE(recording_error_.active()) << "a failure report that disappears cannot be got back";
    EXPECT_EQ(recording_error_.report().detail, QStringLiteral("queued behind recovery"));
    EXPECT_FALSE(arbiter_.recordingErrorQueued());
}

TEST_F(BlockingSurfaceArbiterTest, DismissingTheRecordingErrorRaisesTheQueuedRecovery) {
    requestFailure(QStringLiteral("failure first"));
    arbiter_.requestRecovery();

    recording_error_.dismiss();

    EXPECT_FALSE(recording_error_.active());
    EXPECT_TRUE(recovery_.surfaceOpen());
    EXPECT_FALSE(arbiter_.recoveryQueued());
}

// ─── FIFO, and single-shot ───────────────────────────────────────────────────

TEST_F(BlockingSurfaceArbiterTest, TwoQueuedSurfacesComeUpOneAtATimeInRequestOrder) {
    presentCrash();
    arbiter_.requestRecovery();                           // queued first
    requestFailure(QStringLiteral("queued behind both")); // queued second

    crash_.dismiss();
    EXPECT_TRUE(recovery_.surfaceOpen());
    EXPECT_FALSE(recording_error_.active()) << "still exactly one blocking surface";
    EXPECT_TRUE(arbiter_.recordingErrorQueued());

    recovery_.dismiss();
    EXPECT_TRUE(recording_error_.active());
    EXPECT_FALSE(arbiter_.recordingErrorQueued());
}

TEST_F(BlockingSurfaceArbiterTest, ASecondFailureWhileQueuedReplacesTheReportAndQueuesOnce) {
    presentCrash();
    requestFailure(QStringLiteral("first failure"));
    requestFailure(QStringLiteral("second failure"));

    crash_.dismiss();

    // The newest attempt is the one the user just made — the surface's own rule,
    // and it has to hold whether the report was raised or queued.
    EXPECT_TRUE(recording_error_.active());
    EXPECT_EQ(recording_error_.report().detail, QStringLiteral("second failure"));
    EXPECT_FALSE(arbiter_.recordingErrorQueued());
}

TEST_F(BlockingSurfaceArbiterTest, ASecondFailureWhileTheSurfaceIsUpReplacesItInPlace) {
    requestFailure(QStringLiteral("first failure"));

    requestFailure(QStringLiteral("second failure"));

    EXPECT_TRUE(recording_error_.active());
    EXPECT_EQ(recording_error_.report().detail, QStringLiteral("second failure"));
    EXPECT_FALSE(arbiter_.recordingErrorQueued()) << "a surface never queues behind itself";
}

TEST_F(BlockingSurfaceArbiterTest, AQueuedRecordingErrorIsRaisedOnlyOnce) {
    arbiter_.requestRecovery();
    requestFailure(QStringLiteral("once"));

    recovery_.dismiss();
    recording_error_.dismiss();
    // Recovery can come and go any number of times afterwards; a failure the
    // user has already read must not come back with it.
    arbiter_.requestRecovery();
    recovery_.dismiss();

    EXPECT_FALSE(recording_error_.active());
}

// ─── surfacesCleared: for a surface that waits rather than competes ───────────
//
// The post-update "What's new" overlay asks nothing and blocks no recording, so
// it is deliberately not a fourth arbitrated Surface — joining the queue would
// put a changelog into the precedence rules and into anySurfaceUp(), which is the
// recording-admission edge. It only has to wait for the screen, which is what
// this signal reports.

TEST_F(BlockingSurfaceArbiterTest, SurfacesClearedFiresWhenTheLastSurfaceComesDown) {
    QSignalSpy cleared(&arbiter_, &BlockingSurfaceArbiter::surfacesCleared);
    arbiter_.requestRecovery();
    ASSERT_TRUE(recovery_.surfaceOpen());
    EXPECT_EQ(cleared.count(), 0) << "nothing has come down yet";

    recovery_.dismiss();

    EXPECT_EQ(cleared.count(), 1);
}

TEST_F(BlockingSurfaceArbiterTest, SurfacesClearedIsATransitionAndNotAState) {
    QSignalSpy cleared(&arbiter_, &BlockingSurfaceArbiter::surfacesCleared);
    // The exact input this class observes, on a screen where nothing is up:
    // `changed()` is an aggregate notification and fires for reasons that have
    // nothing to do with a surface coming down. A listener that treated every one
    // of them as "the screen is free now" would raise its own surface at an
    // arbitrary moment during startup.
    emit crash_.changed();
    emit recording_error_.changed();

    EXPECT_EQ(cleared.count(), 0);

    arbiter_.requestRecovery();
    recovery_.dismiss();
    ASSERT_EQ(cleared.count(), 1);

    // And it does not repeat: still nothing up, still nothing coming down.
    emit crash_.changed();

    EXPECT_EQ(cleared.count(), 1);
}

TEST_F(BlockingSurfaceArbiterTest, SurfacesClearedWaitsForTheQueueBehindTheSurface) {
    QSignalSpy cleared(&arbiter_, &BlockingSurfaceArbiter::surfacesCleared);
    arbiter_.requestRecovery();
    requestFailure(QStringLiteral("queued behind recovery"));
    ASSERT_TRUE(arbiter_.recordingErrorQueued());

    recovery_.dismiss();
    // The failure report took the screen the moment recovery left it, so the
    // screen was never free.
    ASSERT_TRUE(recording_error_.active());
    EXPECT_EQ(cleared.count(), 0);

    recording_error_.dismiss();

    EXPECT_EQ(cleared.count(), 1);
}

} // namespace
} // namespace exosnap::quick
