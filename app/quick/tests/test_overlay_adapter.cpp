#include "OverlayAdapter.h"
#include "models/OverlayContentPolicy.h"
#include "services/ScreenPresentation.h"
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
    EXPECT_FALSE(content.drift);
    EXPECT_FALSE(content.size);
    EXPECT_TRUE(content.muted_sources);
    EXPECT_TRUE(content.health);
}

TEST(OverlayContentPolicy, TechnicalCarriesEveryToken) {
    const DiagnosticsOverlayContent content =
        models::ResolveDiagnosticsOverlayContent(DiagnosticsOverlayPreset::Technical, QString());
    EXPECT_TRUE(content.fps);
    EXPECT_TRUE(content.drop);
    EXPECT_TRUE(content.drift);
    EXPECT_TRUE(content.size);
    EXPECT_TRUE(content.muted_sources);
    EXPECT_TRUE(content.health);
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

TEST(OverlayContentPolicy, LegacyCustomTokensRetainTheirMeaningWithoutAddingHealth) {
    const auto content =
        models::ResolveDiagnosticsOverlayContent(DiagnosticsOverlayPreset::Custom, QStringLiteral("drop,drift,muted"));
    EXPECT_TRUE(content.drop);
    EXPECT_TRUE(content.drift);
    EXPECT_TRUE(content.muted_sources);
    EXPECT_FALSE(content.health);
}

TEST(OverlayContentPolicy, HealthTokenIsSelectableAndNotEmptyOnItsOwn) {
    const auto content =
        models::ResolveDiagnosticsOverlayContent(DiagnosticsOverlayPreset::Custom, QStringLiteral("health,unknown"));
    EXPECT_TRUE(content.health);
    EXPECT_FALSE(content.IsEmpty());
    EXPECT_EQ(models::TokensForDiagnosticsOverlayContent(content), QStringLiteral("health"));
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
        // A real HWND -> monitor lookup is environment state (see
        // ScreenPresentation.h's WindowMonitorFunction); every test here
        // gets a deterministic one instead of whatever monitor the test
        // machine happens to have.
        exosnap::SetWindowMonitorFunctionForTest([](std::uintptr_t hwnd) { return hwnd; });
    }

    void TearDown() override {
        exosnap::ResetWindowMonitorFunctionForTest();
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

// No selected target means no rectangle, and the QML falls back to its own
// screen. An empty rect must not be reported as a zero-origin monitor.
TEST_F(OverlayAdapterTest, NoSelectedTargetYieldsNoMonitorGeometry) {
    model_.selected_target_index = -1;
    model_.SetState(UiRecordingState::Recording);
    publish();
    EXPECT_TRUE(adapter_.recordedMonitorGeometry().isEmpty());
}

// ---------------------------------------------------------------------------
// Monitor geometry invalidation (QCR-414)
// ---------------------------------------------------------------------------

// The rectangle is cached per HMONITOR so a 4 Hz synchronize() does not query
// Win32 on every tick. A resolution switch, a scale change or a rearranged
// desktop all keep that handle and change the rectangle — which is why the
// cache needs someone to tell it, and why these tests drive the query through a
// seam rather than through a real display.
class OverlayMonitorGeometryTest : public OverlayAdapterTest {
  protected:
    void SetUp() override {
        OverlayAdapterTest::SetUp();

        exosnap::engine::CaptureTarget monitor;
        monitor.kind = exosnap::engine::CaptureTarget::Kind::Monitor;
        monitor.native_id = 0xABCD;
        model_.targets.push_back(monitor);
        model_.selected_target_index = 0;
        model_.SetState(UiRecordingState::Recording);

        adapter_.setPresentationProviderForTesting([this](std::uintptr_t) {
            ++queries_;
            return presentation_;
        });
        presentation_ = MakePresentation(0, 0, 2560, 1440);
    }

    static ScreenPresentation MakePresentation(int x, int y, int width, int height) {
        ScreenPresentation meta;
        meta.available = true;
        meta.width = width;
        meta.height = height;
        meta.origin_x = x;
        meta.origin_y = y;
        return meta;
    }

    ScreenPresentation presentation_;
    int queries_ = 0;
};

TEST_F(OverlayMonitorGeometryTest, ResolvesTheRecordedMonitorRectangle) {
    publish();
    EXPECT_EQ(adapter_.recordedMonitorGeometry(), QRect(0, 0, 2560, 1440));
}

// A Window target's overlays anchor to its hosting monitor, not to whatever
// screen Qt happens to place a new top-level window on -- previously empty,
// which sent the overlays to that ambient fallback instead.
TEST_F(OverlayAdapterTest, WindowTargetGeometryResolvesTheHostingMonitor) {
    exosnap::engine::CaptureTarget window_target;
    window_target.kind = exosnap::engine::CaptureTarget::Kind::Window;
    window_target.native_id = 0x9999;
    model_.targets.push_back(window_target);
    model_.selected_target_index = 0;
    model_.SetState(UiRecordingState::Recording);

    exosnap::SetWindowMonitorFunctionForTest([](std::uintptr_t hwnd) {
        EXPECT_EQ(hwnd, 0x9999u);
        return std::uintptr_t{0xABCD};
    });
    adapter_.setPresentationProviderForTesting([](std::uintptr_t monitor) {
        EXPECT_EQ(monitor, 0xABCDu);
        ScreenPresentation meta;
        meta.available = true;
        meta.width = 1920;
        meta.height = 1080;
        meta.origin_x = 2560;
        meta.origin_y = 0;
        return meta;
    });

    publish();
    EXPECT_EQ(adapter_.recordedMonitorGeometry(), QRect(2560, 0, 1920, 1080));
}

// A window that never resolves to a monitor (e.g. it closed between target
// selection and this query) yields no rectangle rather than a stale or
// half-resolved one.
TEST_F(OverlayAdapterTest, WindowTargetWithUnresolvableMonitorYieldsNoGeometry) {
    exosnap::engine::CaptureTarget window_target;
    window_target.kind = exosnap::engine::CaptureTarget::Kind::Window;
    window_target.native_id = 0x9999;
    model_.targets.push_back(window_target);
    model_.selected_target_index = 0;
    model_.SetState(UiRecordingState::Recording);

    exosnap::SetWindowMonitorFunctionForTest([](std::uintptr_t) { return std::uintptr_t{0}; });

    publish();
    EXPECT_TRUE(adapter_.recordedMonitorGeometry().isEmpty());
}

TEST_F(OverlayMonitorGeometryTest, RepeatedSynchronizeDoesNotRequery) {
    publish();
    const int after_first = queries_;

    publish();
    publish();
    publish();

    EXPECT_EQ(queries_, after_first) << "the same-monitor fast path is what keeps a 4 Hz tick cheap";
}

TEST_F(OverlayMonitorGeometryTest, SameMonitorAtANewResolutionIsStaleUntilInvalidated) {
    publish();
    ASSERT_EQ(adapter_.recordedMonitorGeometry(), QRect(0, 0, 2560, 1440));

    // The display switched mode. Same handle, same target, different rectangle.
    presentation_ = MakePresentation(0, 0, 1920, 1080);
    publish();
    EXPECT_EQ(adapter_.recordedMonitorGeometry(), QRect(0, 0, 2560, 1440))
        << "nothing has reported the change yet, so the cache is entitled to its answer";

    adapter_.invalidateMonitorGeometry();
    publish();
    EXPECT_EQ(adapter_.recordedMonitorGeometry(), QRect(0, 0, 1920, 1080));
}

// A display rearranged to the right of another keeps its size and moves its
// origin, which is exactly as wrong for a frameless overlay as a size change.
TEST_F(OverlayMonitorGeometryTest, InvalidationCatchesAMovedOrigin) {
    publish();
    ASSERT_EQ(adapter_.recordedMonitorGeometry().topLeft(), QPoint(0, 0));

    presentation_ = MakePresentation(1920, 0, 2560, 1440);
    adapter_.invalidateMonitorGeometry();
    publish();

    EXPECT_EQ(adapter_.recordedMonitorGeometry(), QRect(1920, 0, 2560, 1440));
}

TEST_F(OverlayMonitorGeometryTest, AnInvalidatedMonitorThatVanishedReportsNoRectangle) {
    publish();
    ASSERT_FALSE(adapter_.recordedMonitorGeometry().isEmpty());

    // The display was unplugged: the handle is still what the target names, and
    // the query no longer answers for it.
    presentation_ = ScreenPresentation{};
    adapter_.invalidateMonitorGeometry();
    publish();

    EXPECT_TRUE(adapter_.recordedMonitorGeometry().isEmpty())
        << "an empty rect is what makes the overlays fall back to their own screen";
}

// ---------------------------------------------------------------------------
// Source geometry: what the recording/diagnostics pill actually binds to.
// ---------------------------------------------------------------------------
//
// recordedMonitorGeometry and recordedSourceGeometry agree in Monitor mode and
// disagree everywhere else -- that disagreement is exactly the geometry defect
// this property exists to fix, so every test here asserts on the SOURCE
// rectangle, not the monitor one.

TEST_F(OverlayMonitorGeometryTest, MonitorModeSourceGeometryMatchesTheMonitorRectangle) {
    publish();
    EXPECT_EQ(adapter_.recordedSourceGeometry(), adapter_.recordedMonitorGeometry());
}

TEST_F(OverlayAdapterTest, WindowModeSourceGeometryUsesTheLiveWindowRectangleNotTheMonitor) {
    exosnap::engine::CaptureTarget window_target;
    window_target.kind = exosnap::engine::CaptureTarget::Kind::Window;
    window_target.native_id = 0x9999;
    model_.targets.push_back(window_target);
    model_.selected_target_index = 0;
    model_.SetState(UiRecordingState::Recording);

    adapter_.setPresentationProviderForTesting([](std::uintptr_t) {
        ScreenPresentation meta;
        meta.available = true;
        meta.width = 2560;
        meta.height = 1440;
        return meta;
    });
    adapter_.setWindowRectProviderForTesting([](std::uintptr_t hwnd) {
        EXPECT_EQ(hwnd, 0x9999u);
        WindowScreenRect rect;
        rect.available = true;
        rect.x = 100;
        rect.y = 40;
        rect.width = 800;
        rect.height = 600;
        return rect;
    });

    publish();
    EXPECT_EQ(adapter_.recordedSourceGeometry(), QRect(100, 40, 800, 600));
    // The pill must not fall back to the full monitor rectangle just because
    // one happened to resolve.
    EXPECT_NE(adapter_.recordedSourceGeometry(), adapter_.recordedMonitorGeometry());
}

// A moved or resized window must keep the pill attached even though its
// hosting monitor (and therefore the cached monitor rectangle) has not
// changed -- unlike recordedMonitorGeometry, this is not gated behind the
// same-HMONITOR fast path.
TEST_F(OverlayAdapterTest, WindowModeSourceGeometryFollowsTheWindowEveryTick) {
    exosnap::engine::CaptureTarget window_target;
    window_target.kind = exosnap::engine::CaptureTarget::Kind::Window;
    window_target.native_id = 0x9999;
    model_.targets.push_back(window_target);
    model_.selected_target_index = 0;
    model_.SetState(UiRecordingState::Recording);

    adapter_.setPresentationProviderForTesting([](std::uintptr_t) {
        ScreenPresentation meta;
        meta.available = true;
        meta.width = 2560;
        meta.height = 1440;
        return meta;
    });

    QRect window_rect(100, 40, 800, 600);
    adapter_.setWindowRectProviderForTesting([&window_rect](std::uintptr_t) {
        WindowScreenRect rect;
        rect.available = true;
        rect.x = window_rect.x();
        rect.y = window_rect.y();
        rect.width = window_rect.width();
        rect.height = window_rect.height();
        return rect;
    });

    publish();
    ASSERT_EQ(adapter_.recordedSourceGeometry(), window_rect);

    // No invalidateMonitorGeometry() call here on purpose: the window moved,
    // the monitor did not.
    window_rect.moveTo(300, 200);
    publish();
    EXPECT_EQ(adapter_.recordedSourceGeometry(), window_rect);
}

// A Window target whose HWND cannot be queried (closed, or otherwise
// unresolvable) yields no source rectangle rather than a stale one or a
// fallback to some other monitor.
TEST_F(OverlayAdapterTest, WindowModeSourceGeometryIsEmptyWhenTheRectIsUnresolvable) {
    exosnap::engine::CaptureTarget window_target;
    window_target.kind = exosnap::engine::CaptureTarget::Kind::Window;
    window_target.native_id = 0x9999;
    model_.targets.push_back(window_target);
    model_.selected_target_index = 0;
    model_.SetState(UiRecordingState::Recording);

    adapter_.setWindowRectProviderForTesting([](std::uintptr_t) { return WindowScreenRect{}; });

    publish();
    EXPECT_TRUE(adapter_.recordedSourceGeometry().isEmpty());
}

// Region mode is a crop layered on top of a Monitor target: the source
// geometry must be the region rect, not the full monitor rectangle it was
// cropped from.
TEST_F(OverlayMonitorGeometryTest, RegionModeSourceGeometryUsesTheRegionRectangle) {
    model_.capture_mode = exosnap::CaptureMode::Region;
    model_.has_region = true;
    model_.region = exosnap::engine::CaptureRegion{200, 100, 640, 360};

    publish();
    EXPECT_EQ(adapter_.recordedSourceGeometry(), QRect(200, 100, 640, 360));
    EXPECT_NE(adapter_.recordedSourceGeometry(), adapter_.recordedMonitorGeometry());
}

// has_region alone must not outrank the active capture mode: a region left
// over from a previous session in a different mode must not silently redirect
// the pill away from what is actually being recorded now.
TEST_F(OverlayMonitorGeometryTest, AStaleRegionIsIgnoredOutsideRegionMode) {
    model_.capture_mode = exosnap::CaptureMode::Monitor;
    model_.has_region = true;
    model_.region = exosnap::engine::CaptureRegion{200, 100, 640, 360};

    publish();
    EXPECT_EQ(adapter_.recordedSourceGeometry(), adapter_.recordedMonitorGeometry());
}

} // namespace
} // namespace exosnap::quick
