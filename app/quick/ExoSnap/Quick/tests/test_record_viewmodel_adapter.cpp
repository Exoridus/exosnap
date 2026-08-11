#include "RecordViewModelAdapter.h"
#include "RecordWebcamFrameProvider.h"

#include "viewmodels/RecordViewModel.h"

#include <gtest/gtest.h>

namespace exosnap::quick {
namespace {

TEST(RecordViewModelAdapterTest, CopiesRepresentativeStateAtConstruction) {
    RecordViewModel source;
    source.state_text = L"Ready";
    source.elapsed_text = L"1:23";
    source.output_size_text = L"42.0 MB";
    source.live_stats_available = true;

    RecordViewModelAdapter adapter(&source);

    EXPECT_EQ(adapter.stateText(), QStringLiteral("Ready"));
    EXPECT_EQ(adapter.elapsedText(), QStringLiteral("00:01:23"));
    EXPECT_EQ(adapter.outputSizeText(), QStringLiteral("42.0 MB"));
    EXPECT_TRUE(adapter.liveStatsAvailable());
}

TEST(RecordViewModelAdapterTest, SynchronizeEmitsOnlyForChangedProperties) {
    RecordViewModel source;
    RecordViewModelAdapter adapter(&source);

    int state_changes = 0;
    int elapsed_changes = 0;
    int output_size_changes = 0;
    int availability_changes = 0;
    QObject::connect(&adapter, &RecordViewModelAdapter::stateTextChanged, [&state_changes]() { ++state_changes; });
    QObject::connect(&adapter, &RecordViewModelAdapter::elapsedTextChanged,
                     [&elapsed_changes]() { ++elapsed_changes; });
    QObject::connect(&adapter, &RecordViewModelAdapter::outputSizeTextChanged,
                     [&output_size_changes]() { ++output_size_changes; });
    QObject::connect(&adapter, &RecordViewModelAdapter::liveStatsAvailableChanged,
                     [&availability_changes]() { ++availability_changes; });

    source.elapsed_text = L"0:01";
    source.live_stats_available = true;
    adapter.synchronize();

    EXPECT_EQ(state_changes, 0);
    EXPECT_EQ(elapsed_changes, 1);
    EXPECT_EQ(output_size_changes, 0);
    EXPECT_EQ(availability_changes, 1);
    EXPECT_EQ(adapter.elapsedText(), QStringLiteral("00:00:01"));
    EXPECT_TRUE(adapter.liveStatsAvailable());

    adapter.synchronize();

    EXPECT_EQ(elapsed_changes, 1);
    EXPECT_EQ(availability_changes, 1);
}

TEST(RecordViewModelAdapterTest, DetachingSourceClearsTheBoundarySnapshot) {
    RecordViewModel source;
    source.state_text = L"Recording";
    source.elapsed_text = L"2:34";
    source.output_size_text = L"128.0 MB";
    source.live_stats_available = true;
    RecordViewModelAdapter adapter(&source);

    adapter.setSource(nullptr);

    EXPECT_TRUE(adapter.stateText().isEmpty());
    EXPECT_EQ(adapter.elapsedText(), QStringLiteral("00:00:00"));
    EXPECT_TRUE(adapter.outputSizeText().isEmpty());
    EXPECT_FALSE(adapter.liveStatsAvailable());
}

TEST(RecordViewModelAdapterTest, MapsRecordingStateActionsAndTone) {
    RecordViewModel source;
    source.targets.push_back({recorder_core::CaptureTarget::Kind::Monitor, 1, "Display 1: 1920x1080 at (0, 0)"});
    source.selected_target_index = 0;
    RecordViewModelAdapter adapter(&source);

    source.SetState(UiRecordingState::Ready);
    adapter.synchronize();
    EXPECT_TRUE(adapter.canStart());
    EXPECT_TRUE(adapter.canSelectSource());
    EXPECT_EQ(adapter.stateTone(), QStringLiteral("neutral"));

    source.SetState(UiRecordingState::Recording);
    adapter.synchronize();
    EXPECT_TRUE(adapter.recording());
    EXPECT_TRUE(adapter.canPause());
    EXPECT_TRUE(adapter.canStop());
    EXPECT_FALSE(adapter.canSelectSource());
    EXPECT_EQ(adapter.stateTone(), QStringLiteral("recording"));

    source.SetState(UiRecordingState::Paused);
    adapter.synchronize();
    EXPECT_TRUE(adapter.paused());
    EXPECT_TRUE(adapter.canResume());
    EXPECT_EQ(adapter.stateTone(), QStringLiteral("warning"));

    source.SetState(UiRecordingState::Saving);
    adapter.synchronize();
    EXPECT_TRUE(adapter.finalizing());
    EXPECT_FALSE(adapter.canStop());
}

TEST(RecordViewModelAdapterTest, MapsBlockedAndFailedStatesTextually) {
    RecordViewModel source;
    RecordViewModelAdapter adapter(&source);

    source.SetState(UiRecordingState::Blocked);
    source.capability_status_text = L"No supported hardware encoder";
    adapter.synchronize();
    EXPECT_TRUE(adapter.blocked());
    EXPECT_EQ(adapter.stateText(), QStringLiteral("Blocked"));
    EXPECT_EQ(adapter.stateTone(), QStringLiteral("error"));
    EXPECT_EQ(adapter.capabilityText(), QStringLiteral("No supported hardware encoder"));

    source.SetState(UiRecordingState::Failed);
    adapter.synchronize();
    EXPECT_TRUE(adapter.failed());
    EXPECT_EQ(adapter.stateTone(), QStringLiteral("error"));
}

TEST(RecordViewModelAdapterTest, BuildsTypedDisplayAndWindowTargetOptions) {
    RecordViewModel source;
    source.targets = {
        {recorder_core::CaptureTarget::Kind::Monitor, 1, "Display 1: 2560x1440 at (0, 0)"},
        {recorder_core::CaptureTarget::Kind::Window, 2, "Editor — project.qml"},
    };
    source.selected_target_index = 1;
    source.capture_mode = CaptureMode::Window;
    RecordViewModelAdapter adapter(&source);

    ASSERT_EQ(adapter.targetOptions().size(), 2);
    EXPECT_EQ(adapter.targetOptions().at(0).toMap().value(QStringLiteral("kind")).toString(),
              QStringLiteral("display"));
    EXPECT_EQ(adapter.targetOptions().at(1).toMap().value(QStringLiteral("kind")).toString(), QStringLiteral("window"));
    EXPECT_EQ(adapter.sourceKindText(), QStringLiteral("WINDOW"));
    EXPECT_FALSE(adapter.sourceName().isEmpty());
}

TEST(RecordViewModelAdapterTest, RegionSelectionBlocksStartAndNormalizesCrop) {
    RecordViewModel source;
    source.targets.push_back({recorder_core::CaptureTarget::Kind::Monitor, 1, "Display 1: 1920x1080 at (0, 0)"});
    source.selected_target_index = 0;
    source.capture_mode = CaptureMode::Region;
    source.SetState(UiRecordingState::Ready);
    RecordViewModelAdapter adapter(&source);

    adapter.setRegionState(QRectF(-0.2, 0.1, 0.8, 1.2), true);
    EXPECT_TRUE(adapter.regionSelectionNeeded());
    EXPECT_FALSE(adapter.canStart());
    EXPECT_EQ(adapter.sourceKindText(), QStringLiteral("REGION"));
    EXPECT_EQ(adapter.normalizedSourceRect(), QRectF(0.0, 0.1, 0.6, 0.9));

    source.has_region = true;
    source.region = {100, 120, 800, 450};
    adapter.setRegionState(QRectF(0.1, 0.2, 0.5, 0.5), false);
    EXPECT_TRUE(adapter.canStart());
    EXPECT_EQ(adapter.sourceDetailText(), QStringLiteral("800 × 450 at 100, 120"));
}

TEST(RecordViewModelAdapterTest, MapsAudioAndDeviceAvailabilityWithoutResolvingPolicy) {
    RecordViewModel source;
    source.audio_ui_state.target_kind = capability::CaptureTargetKind::Window;
    source.audio_ui_state.source_rows = {
        {recorder_core::AudioSourceKind::Sys, true, false},
        {recorder_core::AudioSourceKind::App, false, false},
        {recorder_core::AudioSourceKind::Mic, true, false},
    };
    RecordViewModelAdapter adapter(&source);
    adapter.setDeviceState(false, true, true, QStringLiteral("Device is busy"));

    EXPECT_TRUE(adapter.systemAudioEnabled());
    EXPECT_FALSE(adapter.appAudioEnabled());
    EXPECT_TRUE(adapter.microphoneEnabled());
    EXPECT_TRUE(adapter.appAudioVisible());
    EXPECT_FALSE(adapter.microphoneAvailable());
    EXPECT_TRUE(adapter.webcamAvailable());
    EXPECT_TRUE(adapter.webcamEnabled());
    EXPECT_TRUE(adapter.webcamError());
    EXPECT_EQ(adapter.webcamErrorText(), QStringLiteral("Device is busy"));
}

TEST(RecordWebcamFrameProviderTest, AppliesConfiguredChromaAtRequestedPreviewSize) {
    RecordWebcamFrameProvider provider;
    QImage green(2, 2, QImage::Format_RGBA8888);
    green.fill(QColor(0, 255, 0));
    provider.submitFrame(green);
    provider.setChromaKey(true, 0, 255, 0, 0.01f, 0.01f, 0.0f);

    QSize source_size;
    const QImage keyed = provider.requestImage(QStringLiteral("frame"), &source_size, QSize(1, 1));
    ASSERT_EQ(keyed.size(), QSize(1, 1));
    EXPECT_EQ(source_size, QSize(2, 2));
    EXPECT_EQ(keyed.pixelColor(0, 0).alpha(), 0);

    QImage red(2, 2, QImage::Format_RGBA8888);
    red.fill(QColor(255, 0, 0));
    provider.submitFrame(red);
    const QImage foreground = provider.requestImage(QStringLiteral("frame"), nullptr, QSize(1, 1));
    EXPECT_EQ(foreground.pixelColor(0, 0).alpha(), 255);
}

TEST(RecordWebcamFrameProviderTest, NeverUpscalesProviderWorkPastSourceResolution) {
    RecordWebcamFrameProvider provider;
    QImage frame(320, 180, QImage::Format_RGBA8888);
    frame.fill(Qt::red);
    provider.submitFrame(frame);

    QSize source_size;
    const QImage requested = provider.requestImage(QStringLiteral("frame"), &source_size, QSize(1920, 1080));
    EXPECT_EQ(source_size, QSize(320, 180));
    EXPECT_EQ(requested.size(), QSize(320, 180));
}

TEST(RecordViewModelAdapterTest, ClampsMetersAndGatesSessionActions) {
    RecordViewModel source;
    RecordViewModelAdapter adapter(&source);
    adapter.setMeters(-0.5, 0.4, 1.5);
    EXPECT_DOUBLE_EQ(adapter.systemMeter(), 0.0);
    EXPECT_DOUBLE_EQ(adapter.appMeter(), 0.4);
    EXPECT_DOUBLE_EQ(adapter.microphoneMeter(), 1.0);

    source.SetState(UiRecordingState::Ready);
    adapter.synchronize();
    EXPECT_FALSE(adapter.captureFrameEnabled());
    adapter.setPreviewFrameReady(true);
    adapter.synchronize();
    EXPECT_TRUE(adapter.captureFrameEnabled());

    source.SetState(UiRecordingState::Recording);
    adapter.setSplitEnabled(true);
    adapter.synchronize();
    EXPECT_TRUE(adapter.captureFrameEnabled());
    EXPECT_TRUE(adapter.splitEnabled());
}

TEST(RecordViewModelAdapterTest, FormatsLiveDiagnosticsAtAdapterCadence) {
    RecordViewModel source;
    source.live_stats_available = true;
    source.elapsed_seconds = 2.0;
    source.video_bytes = 2'000'000;
    source.audio_bytes = 500'000;
    source.dropped_frames = 3;
    source.av_drift_available = true;
    source.av_drift_ms = -4.6;
    RecordViewModelAdapter adapter(&source);

    EXPECT_EQ(adapter.bitrateText(), QStringLiteral("10.0 Mb/s"));
    EXPECT_EQ(adapter.droppedFramesText(), QStringLiteral("3"));
    EXPECT_EQ(adapter.driftText(), QStringLiteral("-5 ms"));
}

TEST(RecordViewModelAdapterTest, RoutesNarrowCommandsWithoutServiceExposure) {
    RecordViewModel source;
    RecordViewModelAdapter adapter(&source);
    int starts = 0;
    int selected_index = -1;
    int selected_mode = -1;
    QString toggled_key;
    QRectF selected_region;
    QRectF webcam_overlay;
    QObject::connect(&adapter, &RecordViewModelAdapter::startRequested, [&starts]() { ++starts; });
    QObject::connect(&adapter, &RecordViewModelAdapter::selectTargetRequested,
                     [&selected_index, &selected_mode](int index, int mode) {
                         selected_index = index;
                         selected_mode = mode;
                     });
    QObject::connect(&adapter, &RecordViewModelAdapter::toggleSourceRequested,
                     [&toggled_key](const QString& key) { toggled_key = key; });
    QObject::connect(&adapter, &RecordViewModelAdapter::selectRegionRequested,
                     [&selected_region](const QRectF& rect) { selected_region = rect; });
    QObject::connect(&adapter, &RecordViewModelAdapter::webcamOverlayRectRequested,
                     [&webcam_overlay](const QRectF& rect) { webcam_overlay = rect; });

    adapter.requestStart();
    adapter.requestSelectTarget(4, static_cast<int>(CaptureMode::Window));
    adapter.requestToggleSource(QStringLiteral("microphone"));
    adapter.requestSelectRegion(QRectF(0.1, 0.2, 0.3, 0.4));
    adapter.requestWebcamOverlayRect(QRectF(0.2, 0.3, 0.25, 0.25));

    EXPECT_EQ(starts, 1);
    EXPECT_EQ(selected_index, 4);
    EXPECT_EQ(selected_mode, static_cast<int>(CaptureMode::Window));
    EXPECT_EQ(toggled_key, QStringLiteral("microphone"));
    EXPECT_EQ(selected_region, QRectF(0.1, 0.2, 0.3, 0.4));
    EXPECT_EQ(webcam_overlay, QRectF(0.2, 0.3, 0.25, 0.25));
}

} // namespace
} // namespace exosnap::quick
