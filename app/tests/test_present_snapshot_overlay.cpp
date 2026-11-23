#include <gtest/gtest.h>

#include "diagnostics/PresentProvider.h"
#include "diagnostics/PresentSnapshotOverlay.h"

#include <chrono>
#include <exosnap/engine/pipeline_diagnostics.h>
#include <optional>

namespace {

using exosnap::diagnostics::ApplyPresentSample;
using exosnap::diagnostics::PresentMode;
using exosnap::diagnostics::PresentSample;
using exosnap::diagnostics::ToSnapshotPresentMode;

constexpr auto kNow = std::chrono::steady_clock::time_point{std::chrono::minutes(2)};
constexpr unsigned long kTargetPid = 123;

exosnap::engine::CaptureDiagnostics FreshCapture() {
    return exosnap::engine::CaptureDiagnostics{};
}

PresentSample MeasuredPresent(PresentMode mode) {
    PresentSample sample;
    sample.available = true;
    sample.mode = mode;
    sample.metadata.process_id = kTargetPid;
    sample.metadata.observed_at = kNow - std::chrono::seconds(1);
    return sample;
}

TEST(PresentSnapshotOverlayTest, EveryModeMapsOntoItsSnapshotCounterpart) {
    EXPECT_EQ(ToSnapshotPresentMode(PresentMode::Unknown), exosnap::engine::PresentMode::Unknown);
    EXPECT_EQ(ToSnapshotPresentMode(PresentMode::Composed), exosnap::engine::PresentMode::Composed);
    EXPECT_EQ(ToSnapshotPresentMode(PresentMode::IndependentFlip), exosnap::engine::PresentMode::IndependentFlip);
    EXPECT_EQ(ToSnapshotPresentMode(PresentMode::ExclusiveFullscreen),
              exosnap::engine::PresentMode::ExclusiveFullscreen);
}

// The default state of a snapshot the engine produced: nothing measured.
TEST(PresentSnapshotOverlayTest, NoProviderLeavesTheSnapshotUnavailable) {
    exosnap::engine::CaptureDiagnostics capture = FreshCapture();
    ApplyPresentSample(capture, std::nullopt, kTargetPid, kNow);
    EXPECT_EQ(capture.present_mode_availability, exosnap::engine::MetricAvailability::Unavailable);
    EXPECT_EQ(capture.source_present_mode, exosnap::engine::PresentMode::Unknown);
    EXPECT_FALSE(capture.source_tearing);
}

// Opt-in off, or not elevated, or the ETW session refused to open. The provider
// still answers -- with available == false. Publishing its zeroed mode as a
// measurement would report "Composed, no tearing" on a machine that measured
// nothing at all.
TEST(PresentSnapshotOverlayTest, UnavailableSampleIsNotAMeasurement) {
    exosnap::engine::CaptureDiagnostics capture = FreshCapture();
    PresentSample sample = MeasuredPresent(PresentMode::Composed);
    sample.available = false;
    sample.tearing = true;
    ApplyPresentSample(capture, sample, kTargetPid, kNow);
    EXPECT_EQ(capture.present_mode_availability, exosnap::engine::MetricAvailability::Unavailable);
    EXPECT_EQ(capture.source_present_mode, exosnap::engine::PresentMode::Unknown);
    EXPECT_FALSE(capture.source_tearing);
}

// The session is open and draining but no present has been decoded yet. "Unknown"
// is the absence of a verdict, not a verdict of unknown presentation.
TEST(PresentSnapshotOverlayTest, OpenSessionWithoutAPresentStaysUnavailable) {
    exosnap::engine::CaptureDiagnostics capture = FreshCapture();
    const PresentSample sample = MeasuredPresent(PresentMode::Unknown);
    ApplyPresentSample(capture, sample, kTargetPid, kNow);
    EXPECT_EQ(capture.present_mode_availability, exosnap::engine::MetricAvailability::Unavailable);
    EXPECT_EQ(capture.source_present_mode, exosnap::engine::PresentMode::Unknown);
}

TEST(PresentSnapshotOverlayTest, ClassifiedPresentBecomesAnAvailableMeasurement) {
    exosnap::engine::CaptureDiagnostics capture = FreshCapture();
    PresentSample sample = MeasuredPresent(PresentMode::IndependentFlip);
    sample.tearing = true;
    sample.present_count = 412;
    ApplyPresentSample(capture, sample, kTargetPid, kNow);
    EXPECT_EQ(capture.present_mode_availability, exosnap::engine::MetricAvailability::Available);
    EXPECT_EQ(capture.source_present_mode, exosnap::engine::PresentMode::IndependentFlip);
    EXPECT_TRUE(capture.source_tearing);
}

// The one classification the capture-stall path acts on: an exclusive-fullscreen
// present is what lets a confirmed WGC stall be explained as FSE rather than
// reported as an unexplained stall. Without this overlay that branch was
// unreachable, so the explanation could never appear.
TEST(PresentSnapshotOverlayTest, ExclusiveFullscreenReachesTheStallClassifier) {
    exosnap::engine::CaptureDiagnostics capture = FreshCapture();
    const PresentSample sample = MeasuredPresent(PresentMode::ExclusiveFullscreen);
    ApplyPresentSample(capture, sample, kTargetPid, kNow);

    const bool present_fse = capture.present_mode_availability == exosnap::engine::MetricAvailability::Available &&
                             capture.source_present_mode == exosnap::engine::PresentMode::ExclusiveFullscreen;
    EXPECT_TRUE(present_fse);
}

TEST(PresentSnapshotOverlayTest, ExpiredExclusiveFullscreenDoesNotReachTheStallClassifier) {
    exosnap::engine::CaptureDiagnostics capture = FreshCapture();
    PresentSample sample = MeasuredPresent(PresentMode::ExclusiveFullscreen);
    sample.tearing = true;
    sample.metadata.observed_at = kNow - std::chrono::minutes(1);
    ApplyPresentSample(capture, sample, kTargetPid, kNow);

    EXPECT_EQ(capture.present_mode_availability, exosnap::engine::MetricAvailability::Unavailable);
    EXPECT_EQ(capture.source_present_mode, exosnap::engine::PresentMode::Unknown);
    EXPECT_FALSE(capture.source_tearing);
}

TEST(PresentSnapshotOverlayTest, ExclusiveFullscreenRequiresAMatchingProcess) {
    exosnap::engine::CaptureDiagnostics capture = FreshCapture();
    PresentSample sample = MeasuredPresent(PresentMode::ExclusiveFullscreen);

    ApplyPresentSample(capture, sample, 456, kNow);
    EXPECT_EQ(capture.present_mode_availability, exosnap::engine::MetricAvailability::Unavailable);

    sample.metadata.process_id.reset();
    ApplyPresentSample(capture, sample, 0, kNow);
    EXPECT_EQ(capture.present_mode_availability, exosnap::engine::MetricAvailability::Unavailable);
}

// Tearing is a property of the sample, never a default. A measured non-tearing
// present must be reported as measured-and-false, not as unavailable.
TEST(PresentSnapshotOverlayTest, MeasuredAbsenceOfTearingIsStillAMeasurement) {
    exosnap::engine::CaptureDiagnostics capture = FreshCapture();
    const PresentSample sample = MeasuredPresent(PresentMode::Composed);
    ApplyPresentSample(capture, sample, kTargetPid, kNow);
    EXPECT_EQ(capture.present_mode_availability, exosnap::engine::MetricAvailability::Available);
    EXPECT_FALSE(capture.source_tearing);
}

} // namespace
