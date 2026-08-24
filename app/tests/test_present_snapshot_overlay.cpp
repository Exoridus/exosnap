// Wave D: the bridge that puts the app-layer present measurement onto the engine's
// live snapshot.
//
// This mapping is the whole reason `pipeline.snapshot` reported `presentMode: null`
// on every machine: engine declares the fields, only the app layer can fill
// them, and until Wave D nobody did. The tests below pin both halves of the contract
// -- what a real measurement produces, and the three distinct ways of having no
// measurement, none of which may fabricate one.

#include <gtest/gtest.h>

#include "diagnostics/PresentSnapshotOverlay.h"

namespace {

using exosnap::diagnostics::ApplyPresentSample;
using exosnap::diagnostics::PresentMode;
using exosnap::diagnostics::PresentSample;
using exosnap::diagnostics::ToSnapshotPresentMode;

exosnap::engine::CaptureDiagnostics FreshCapture() {
    return exosnap::engine::CaptureDiagnostics{};
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
    ApplyPresentSample(capture, std::nullopt);
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
    PresentSample sample;
    sample.available = false;
    sample.mode = PresentMode::Composed;
    sample.tearing = true;
    ApplyPresentSample(capture, sample);
    EXPECT_EQ(capture.present_mode_availability, exosnap::engine::MetricAvailability::Unavailable);
    EXPECT_EQ(capture.source_present_mode, exosnap::engine::PresentMode::Unknown);
    EXPECT_FALSE(capture.source_tearing);
}

// The session is open and draining but no present has been decoded yet. "Unknown"
// is the absence of a verdict, not a verdict of unknown presentation.
TEST(PresentSnapshotOverlayTest, OpenSessionWithoutAPresentStaysUnavailable) {
    exosnap::engine::CaptureDiagnostics capture = FreshCapture();
    PresentSample sample;
    sample.available = true;
    sample.mode = PresentMode::Unknown;
    ApplyPresentSample(capture, sample);
    EXPECT_EQ(capture.present_mode_availability, exosnap::engine::MetricAvailability::Unavailable);
    EXPECT_EQ(capture.source_present_mode, exosnap::engine::PresentMode::Unknown);
}

TEST(PresentSnapshotOverlayTest, ClassifiedPresentBecomesAnAvailableMeasurement) {
    exosnap::engine::CaptureDiagnostics capture = FreshCapture();
    PresentSample sample;
    sample.available = true;
    sample.mode = PresentMode::IndependentFlip;
    sample.tearing = true;
    sample.present_count = 412;
    ApplyPresentSample(capture, sample);
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
    PresentSample sample;
    sample.available = true;
    sample.mode = PresentMode::ExclusiveFullscreen;
    ApplyPresentSample(capture, sample);

    const bool present_fse = capture.present_mode_availability == exosnap::engine::MetricAvailability::Available &&
                             capture.source_present_mode == exosnap::engine::PresentMode::ExclusiveFullscreen;
    EXPECT_TRUE(present_fse);
}

// Tearing is a property of the sample, never a default. A measured non-tearing
// present must be reported as measured-and-false, not as unavailable.
TEST(PresentSnapshotOverlayTest, MeasuredAbsenceOfTearingIsStillAMeasurement) {
    exosnap::engine::CaptureDiagnostics capture = FreshCapture();
    PresentSample sample;
    sample.available = true;
    sample.mode = PresentMode::Composed;
    sample.tearing = false;
    ApplyPresentSample(capture, sample);
    EXPECT_EQ(capture.present_mode_availability, exosnap::engine::MetricAvailability::Available);
    EXPECT_FALSE(capture.source_tearing);
}

} // namespace
