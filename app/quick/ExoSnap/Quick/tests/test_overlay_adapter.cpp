#include "OverlayAdapter.h"
#include "models/OverlayContentPolicy.h"
#include "viewmodels/RecordViewModel.h"

#include <gtest/gtest.h>

namespace exosnap::quick {
namespace {

using models::DiagnosticsOverlayContent;
using models::DiagnosticsOverlayPreset;
using models::RecordingOverlayContent;
using models::RecordingOverlayPreset;
using models::RecordingOverlayState;
using models::RecordingOverlayStateInputs;

// ---------------------------------------------------------------------------
// State resolution
// ---------------------------------------------------------------------------

TEST(OverlayContentPolicy, IdleResolvesToHidden) {
    EXPECT_EQ(models::ResolveRecordingOverlayState({}), RecordingOverlayState::Hidden);
}

TEST(OverlayContentPolicy, RecordingWithoutDropsIsRecording) {
    RecordingOverlayStateInputs inputs;
    inputs.recording = true;
    inputs.live_stats_available = true;
    EXPECT_EQ(models::ResolveRecordingOverlayState(inputs), RecordingOverlayState::Recording);
}

TEST(OverlayContentPolicy, MeasuredDropsRaiseWarning) {
    RecordingOverlayStateInputs inputs;
    inputs.recording = true;
    inputs.live_stats_available = true;
    inputs.dropped_frames = 1;
    EXPECT_EQ(models::ResolveRecordingOverlayState(inputs), RecordingOverlayState::Warning);
}

// The count is only meaningful once the engine has reported stats. Reading a
// stale or initial value would let the HUD claim a problem it never measured.
TEST(OverlayContentPolicy, DropsWithoutLiveStatsDoNotWarn) {
    RecordingOverlayStateInputs inputs;
    inputs.recording = true;
    inputs.live_stats_available = false;
    inputs.dropped_frames = 12;
    EXPECT_EQ(models::ResolveRecordingOverlayState(inputs), RecordingOverlayState::Recording);
}

// Paused outranks Warning: the HUD's whole reason to exist is stopping a user
// from believing a held capture is running.
TEST(OverlayContentPolicy, PausedOutranksWarning) {
    RecordingOverlayStateInputs inputs;
    inputs.paused = true;
    inputs.live_stats_available = true;
    inputs.dropped_frames = 7;
    EXPECT_EQ(models::ResolveRecordingOverlayState(inputs), RecordingOverlayState::Paused);
}

// A failed capture takes the HUD off the screen rather than turning it into an
// error pill: the recording-error surface is what tells the user what happened,
// and a pill still sitting over the recorded screen would read as "still going".
TEST(OverlayContentPolicy, FailureHidesTheHudEntirely) {
    RecordingOverlayStateInputs inputs;
    inputs.recording = true;
    inputs.paused = true;
    inputs.failed = true;
    EXPECT_EQ(models::ResolveRecordingOverlayState(inputs), RecordingOverlayState::Hidden);
}

// ---------------------------------------------------------------------------
// Content resolution
// ---------------------------------------------------------------------------

TEST(OverlayContentPolicy, MinimalIgnoresCustomTokens) {
    const RecordingOverlayContent content =
        models::ResolveRecordingOverlayContent(RecordingOverlayPreset::Minimal, QStringLiteral("elapsed,size,source"));
    EXPECT_TRUE(content.elapsed);
    EXPECT_FALSE(content.output_size);
    EXPECT_FALSE(content.source_name);
}

TEST(OverlayContentPolicy, HealthOmitsFpsAndSize) {
    const DiagnosticsOverlayContent content =
        models::ResolveDiagnosticsOverlayContent(DiagnosticsOverlayPreset::Health, QString());
    EXPECT_FALSE(content.fps);
    EXPECT_TRUE(content.drop);
    EXPECT_TRUE(content.drift);
    EXPECT_FALSE(content.size);
    EXPECT_TRUE(content.muted_sources);
}

TEST(OverlayContentPolicy, TechnicalCarriesEveryToken) {
    const DiagnosticsOverlayContent content =
        models::ResolveDiagnosticsOverlayContent(DiagnosticsOverlayPreset::Technical, QString());
    EXPECT_TRUE(content.fps);
    EXPECT_TRUE(content.drop);
    EXPECT_TRUE(content.drift);
    EXPECT_TRUE(content.size);
    EXPECT_TRUE(content.muted_sources);
}

TEST(OverlayContentPolicy, CustomReadsTheTokenList) {
    const DiagnosticsOverlayContent content =
        models::ResolveDiagnosticsOverlayContent(DiagnosticsOverlayPreset::Custom, QStringLiteral("fps, size"));
    EXPECT_TRUE(content.fps);
    EXPECT_FALSE(content.drop);
    EXPECT_FALSE(content.drift);
    EXPECT_TRUE(content.size);
    EXPECT_FALSE(content.muted_sources);
}

// A settings file is user-editable, and a typo must degrade to a working HUD
// rather than to an unparseable one.
TEST(OverlayContentPolicy, UnknownTokensAreIgnored) {
    const DiagnosticsOverlayContent content =
        models::ResolveDiagnosticsOverlayContent(DiagnosticsOverlayPreset::Custom, QStringLiteral("drop,bogus,,drift"));
    EXPECT_TRUE(content.drop);
    EXPECT_TRUE(content.drift);
    EXPECT_FALSE(content.fps);
}

TEST(OverlayContentPolicy, UnknownPresetFallsBackToTheShippedDefault) {
    EXPECT_EQ(models::DiagnosticsOverlayPresetFromToken(QStringLiteral("nonsense")), DiagnosticsOverlayPreset::Health);
    EXPECT_EQ(models::RecordingOverlayPresetFromToken(QStringLiteral("nonsense")), RecordingOverlayPreset::Minimal);
}

TEST(OverlayContentPolicy, ContentRoundTripsThroughItsTokens) {
    DiagnosticsOverlayContent original;
    original.fps = true;
    original.drop = false;
    original.drift = true;
    original.size = true;
    original.muted_sources = false;

    const DiagnosticsOverlayContent restored = models::ResolveDiagnosticsOverlayContent(
        DiagnosticsOverlayPreset::Custom, models::TokensForDiagnosticsOverlayContent(original));
    EXPECT_EQ(restored, original);
}

TEST(OverlayContentPolicy, EverythingUntickedIsEmpty) {
    const DiagnosticsOverlayContent content =
        models::ResolveDiagnosticsOverlayContent(DiagnosticsOverlayPreset::Custom, QString());
    EXPECT_TRUE(content.IsEmpty());
}

// ---------------------------------------------------------------------------
// Adapter gating
// ---------------------------------------------------------------------------

class OverlayAdapterTest : public ::testing::Test {
  protected:
    void SetUp() override {
        settings_.show_recording_overlay = true;
        settings_.show_diagnostics_overlay = true;
        settings_.show_quick_controls = true;
        adapter_.setSource(&model_);
        adapter_.setAppSettings(settings_);
    }

    void publish() {
        adapter_.setAppSettings(settings_);
        adapter_.synchronize();
    }

    RecordViewModel model_;
    PersistedAppSettings settings_;
    OverlayAdapter adapter_;
};

TEST_F(OverlayAdapterTest, NothingIsActiveWhileIdle) {
    model_.SetState(UiRecordingState::Ready);
    publish();
    EXPECT_FALSE(adapter_.recordingOverlayActive());
    EXPECT_FALSE(adapter_.diagnosticsOverlayActive());
    EXPECT_FALSE(adapter_.countdownOverlayActive());
    EXPECT_FALSE(adapter_.quickControlsActive());
}

TEST_F(OverlayAdapterTest, RecordingActivatesTheEnabledSurfaces) {
    model_.SetState(UiRecordingState::Recording);
    publish();
    EXPECT_TRUE(adapter_.recordingOverlayActive());
    EXPECT_TRUE(adapter_.diagnosticsOverlayActive());
    EXPECT_TRUE(adapter_.quickControlsActive());
    EXPECT_EQ(adapter_.recordingState(), OverlayAdapter::Recording);
}

// The regression the whole session is about: these were persisted preferences
// that no surface read.
TEST_F(OverlayAdapterTest, DisablingTheSettingTakesTheSurfaceOffScreen) {
    model_.SetState(UiRecordingState::Recording);
    publish();
    ASSERT_TRUE(adapter_.recordingOverlayActive());

    settings_.show_recording_overlay = false;
    publish();
    EXPECT_FALSE(adapter_.recordingOverlayActive());
    // The diagnostics HUD has its own gate and must not follow.
    EXPECT_TRUE(adapter_.diagnosticsOverlayActive());
}

TEST_F(OverlayAdapterTest, AnEmptyDiagnosticsSetKeepsTheWindowOff) {
    model_.SetState(UiRecordingState::Recording);
    settings_.diagnostics_overlay_preset = models::TokenFor(DiagnosticsOverlayPreset::Custom);
    settings_.diagnostics_overlay_custom_elements = QString();
    publish();
    EXPECT_FALSE(adapter_.diagnosticsOverlayActive());
    // The recording HUD is unaffected: its content is a separate set.
    EXPECT_TRUE(adapter_.recordingOverlayActive());
}

// A failed capture has no HUD state at all: the recording-error surface is what
// reports the failure, and a click-through pill would otherwise sit on the
// desktop next to it saying the same thing with no way to dismiss it.
TEST_F(OverlayAdapterTest, FailureResolvesToHiddenAndDoesNotActivateTheWindow) {
    model_.SetState(UiRecordingState::Failed);
    publish();
    EXPECT_EQ(adapter_.recordingState(), OverlayAdapter::Hidden);
    EXPECT_FALSE(adapter_.recordingOverlayActive());
}

TEST_F(OverlayAdapterTest, CountdownFollowsTheRecordingOverlaySetting) {
    model_.SetState(UiRecordingState::Countdown);
    publish();
    EXPECT_TRUE(adapter_.countdownOverlayActive());
    // A countdown is not a capture yet, so the metric HUD stays off.
    EXPECT_FALSE(adapter_.diagnosticsOverlayActive());

    settings_.show_recording_overlay = false;
    publish();
    EXPECT_FALSE(adapter_.countdownOverlayActive());
}

// ArmedFromRecovery is a paused session with a slice pending, not a stopped one.
TEST_F(OverlayAdapterTest, ArmedFromRecoveryReadsAsPaused) {
    model_.SetState(UiRecordingState::ArmedFromRecovery);
    publish();
    EXPECT_EQ(adapter_.recordingState(), OverlayAdapter::Paused);
    EXPECT_TRUE(adapter_.recordingOverlayActive());
}

TEST_F(OverlayAdapterTest, MeasuredDropsSwitchTheLiveHudToWarning) {
    model_.SetState(UiRecordingState::Recording);
    model_.live_stats_available = true;
    model_.dropped_frames = 4;
    publish();
    EXPECT_EQ(adapter_.recordingState(), OverlayAdapter::Warning);
    // Still on screen: a warning is a state of the running capture, not a reason
    // to take the indicator away.
    EXPECT_TRUE(adapter_.recordingOverlayActive());
}

// No monitor target means no rectangle, and the QML falls back to its own
// screen. An empty rect must not be reported as a zero-origin monitor.
TEST_F(OverlayAdapterTest, WindowTargetsYieldNoMonitorGeometry) {
    recorder_core::CaptureTarget window_target;
    window_target.kind = recorder_core::CaptureTarget::Kind::Window;
    window_target.native_id = 0x1234;
    model_.targets.push_back(window_target);
    model_.selected_target_index = 0;
    model_.SetState(UiRecordingState::Recording);
    publish();
    EXPECT_TRUE(adapter_.recordedMonitorGeometry().isEmpty());
}

} // namespace
} // namespace exosnap::quick
