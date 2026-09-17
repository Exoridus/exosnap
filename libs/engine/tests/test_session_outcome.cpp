// Outcome attribution for a session that never captured a video frame.
//
// REGRESSION: such a session recorded no failure of its own, so the first
// consumer to miss something it needed -- the mux, waiting for codec headers --
// became the reported cause. A recording that captured nothing was reported as
// `phase=Mux "Codec private data not available at mux start"`, which names a
// worker that was only ever downstream of the real problem.

#include <gtest/gtest.h>

#include "session_outcome.h"

using exosnap::engine::ApplyMissingCaptureOutcome;
using exosnap::engine::CaptureBackend;
using exosnap::engine::CaptureTarget;
using exosnap::engine::ClassifyMissingCapture;
using exosnap::engine::EffectiveCaptureBackend;
using exosnap::engine::ErrorPhase;
using exosnap::engine::MissingCaptureCause;
using exosnap::engine::RecorderConfig;
using exosnap::engine::RecorderResult;
using exosnap::engine::ResolveCaptureBackend;

namespace {

RecorderResult SucceedingResult() {
    RecorderResult result;
    result.succeeded = true;
    result.error_code = 0;
    result.error_phase = ErrorPhase::None;
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// ClassifyMissingCapture
// ---------------------------------------------------------------------------

TEST(ClassifyMissingCapture, ACapturedFrameLeavesTheOutcomeAlone) {
    EXPECT_EQ(ClassifyMissingCapture(/*has_recorded_cause=*/false, /*frames_captured=*/1,
                                     /*stopped_before_start=*/false),
              MissingCaptureCause::None);
}

// A single captured frame is enough: the pipeline demonstrably ran, and whatever
// went wrong afterwards belongs to the worker that recorded it.
TEST(ClassifyMissingCapture, ACapturedFrameLeavesTheOutcomeAloneEvenAfterAPreStop) {
    EXPECT_EQ(ClassifyMissingCapture(/*has_recorded_cause=*/false, /*frames_captured=*/1,
                                     /*stopped_before_start=*/true),
              MissingCaptureCause::None);
}

// A worker that named a cause keeps it -- this rule fills a silence, it does not
// overwrite a diagnosis.
TEST(ClassifyMissingCapture, ARecordedCauseIsNotOverwritten) {
    EXPECT_EQ(ClassifyMissingCapture(/*has_recorded_cause=*/true, /*frames_captured=*/0,
                                     /*stopped_before_start=*/false),
              MissingCaptureCause::None);
}

TEST(ClassifyMissingCapture, NoFrameAndNoCauseIsACaptureFault) {
    EXPECT_EQ(ClassifyMissingCapture(/*has_recorded_cause=*/false, /*frames_captured=*/0,
                                     /*stopped_before_start=*/false),
              MissingCaptureCause::NoFramesDelivered);
}

TEST(ClassifyMissingCapture, AStopThatBeatTheCaptureIsNotACaptureFault) {
    EXPECT_EQ(ClassifyMissingCapture(/*has_recorded_cause=*/false, /*frames_captured=*/0,
                                     /*stopped_before_start=*/true),
              MissingCaptureCause::StoppedBeforeCapture);
}

// ---------------------------------------------------------------------------
// ApplyMissingCaptureOutcome
// ---------------------------------------------------------------------------

TEST(ApplyMissingCaptureOutcome, NoneChangesNothing) {
    RecorderResult result = SucceedingResult();

    ApplyMissingCaptureOutcome(result, MissingCaptureCause::None, EffectiveCaptureBackend::DxgiOutputDuplication);

    EXPECT_TRUE(result.succeeded);
    EXPECT_EQ(result.error_phase, ErrorPhase::None);
    EXPECT_TRUE(result.error_detail.empty());
}

TEST(ApplyMissingCaptureOutcome, ACaptureFaultNamesTheCaptureAndItsBackend) {
    RecorderResult result = SucceedingResult();

    ApplyMissingCaptureOutcome(result, MissingCaptureCause::NoFramesDelivered,
                               EffectiveCaptureBackend::DxgiOutputDuplication);

    EXPECT_FALSE(result.succeeded);
    EXPECT_EQ(result.error_phase, ErrorPhase::VideoCapture) << "the mux must never be named for a capture fault";
    EXPECT_NE(result.error_detail.find("DXGI desktop duplication"), std::string::npos);
}

TEST(ApplyMissingCaptureOutcome, AWindowTargetNamesItsOwnBackend) {
    RecorderResult result = SucceedingResult();

    ApplyMissingCaptureOutcome(result, MissingCaptureCause::NoFramesDelivered,
                               EffectiveCaptureBackend::WindowsGraphicsCapture);

    EXPECT_NE(result.error_detail.find("Windows Graphics Capture"), std::string::npos);
}

TEST(ApplyMissingCaptureOutcome, AMonitorRecordedWithWgcIsNotCalledADxgiFault) {
    // The defect. The backend name came from the TARGET KIND, so a monitor was
    // always "DXGI desktop duplication" -- including when the session explicitly
    // selected Windows Graphics Capture for it. The reader was sent to the
    // diagnostics of a backend that never ran.
    RecorderConfig config;
    config.target.kind = CaptureTarget::Kind::Monitor;
    config.capture_backend = CaptureBackend::WindowsGraphicsCapture;
    ASSERT_EQ(ResolveCaptureBackend(config), EffectiveCaptureBackend::WindowsGraphicsCapture);

    RecorderResult result = SucceedingResult();
    ApplyMissingCaptureOutcome(result, MissingCaptureCause::NoFramesDelivered, ResolveCaptureBackend(config));

    EXPECT_NE(result.error_detail.find("Windows Graphics Capture"), std::string::npos);
    EXPECT_EQ(result.error_detail.find("DXGI"), std::string::npos)
        << "a backend that never ran must not be named: " << result.error_detail;
}

TEST(ApplyMissingCaptureOutcome, AMonitorOnTheDefaultBackendIsStillDxgi) {
    // The control: the default path must keep naming DXGI.
    RecorderConfig config;
    config.target.kind = CaptureTarget::Kind::Monitor;
    config.capture_backend = CaptureBackend::Default;
    ASSERT_EQ(ResolveCaptureBackend(config), EffectiveCaptureBackend::DxgiOutputDuplication);

    RecorderResult result = SucceedingResult();
    ApplyMissingCaptureOutcome(result, MissingCaptureCause::NoFramesDelivered, ResolveCaptureBackend(config));
    EXPECT_NE(result.error_detail.find("DXGI desktop duplication"), std::string::npos);
}

// The user's own stop is not a capture fault, and must not read like one.
TEST(ApplyMissingCaptureOutcome, AStopBeforeCaptureIsReportedAsAnAbortedPreparation) {
    RecorderResult result = SucceedingResult();

    ApplyMissingCaptureOutcome(result, MissingCaptureCause::StoppedBeforeCapture,
                               EffectiveCaptureBackend::DxgiOutputDuplication);

    EXPECT_FALSE(result.succeeded);
    EXPECT_EQ(result.error_phase, ErrorPhase::Prepare);
    EXPECT_EQ(result.error_code, E_ABORT);
    EXPECT_EQ(result.error_detail.find("capture delivered no frames"), std::string::npos)
        << "a stop the user asked for must not be reported as a capture fault";
}
