#include "RecordViewModelAdapter.h"
#include "RecordWebcamFrameProvider.h"

#include "viewmodels/RecordViewModel.h"

#include <gtest/gtest.h>

#include <string>

namespace exosnap::quick {
namespace {

TEST(RecordViewModelAdapterTest, CopiesRepresentativeStateAtConstruction) {
    RecordViewModel source;
    source.state_text = L"Ready";
    source.elapsed_text = L"1:23";
    source.output_size_text = L"42.0 MB";
    source.live_stats_available = true;

    RecordViewModelAdapter adapter(&source);

    EXPECT_EQ(adapter.stateText(), QStringLiteral("No source"));
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

// QCR-605. `changed()` is the NOTIFY of 49 bound properties and synchronize()
// used to fire it unconditionally — at ~8 Hz through a recording, 10 Hz through a
// countdown, and again on every preview frameReady.
TEST(RecordViewModelAdapterTest, ASyncThatMovesNothingDoesNotInvalidateTheBroadSignal) {
    RecordViewModel source;
    source.state_text = L"Ready";
    RecordViewModelAdapter adapter(&source);

    int broad_changes = 0;
    QObject::connect(&adapter, &RecordViewModelAdapter::changed, [&broad_changes]() { ++broad_changes; });

    for (int i = 0; i < 20; ++i)
        adapter.synchronize();

    EXPECT_EQ(broad_changes, 0);
}

// The counters the stats callback moves — frames, bytes, packets — reach the UI
// through their own narrow signals. They must not drag the broad one with them.
TEST(RecordViewModelAdapterTest, LiveStatsAloneDoNotInvalidateTheBroadSignal) {
    RecordViewModel source;
    source.SetState(UiRecordingState::Recording);
    source.live_stats_available = true;
    source.elapsed_text = L"0:01";
    RecordViewModelAdapter adapter(&source);

    int broad_changes = 0;
    QObject::connect(&adapter, &RecordViewModelAdapter::changed, [&broad_changes]() { ++broad_changes; });

    for (int tick = 2; tick < 12; ++tick) {
        source.elapsed_text = std::wstring(L"0:0") + static_cast<wchar_t>(L'0' + (tick % 10));
        source.frames_captured += 60;
        source.video_bytes += 1'000'000;
        source.output_file_bytes += 1'000'000;
        adapter.synchronize();
    }

    EXPECT_EQ(broad_changes, 0);
}

TEST(RecordViewModelAdapterTest, AStateChangeStillInvalidatesTheBroadSignalExactlyOnce) {
    RecordViewModel source;
    source.SetState(UiRecordingState::Ready);
    RecordViewModelAdapter adapter(&source);

    int broad_changes = 0;
    QObject::connect(&adapter, &RecordViewModelAdapter::changed, [&broad_changes]() { ++broad_changes; });

    source.SetState(UiRecordingState::Recording);
    adapter.synchronize();
    EXPECT_EQ(broad_changes, 1);
    EXPECT_TRUE(adapter.recording());

    // A second sync with the same state says nothing new.
    adapter.synchronize();
    EXPECT_EQ(broad_changes, 1);
}

// The three option lists are the expensive half: a QVariantMap of three QStrings
// per monitor and per eligible window, each label parsed out of the target.
TEST(RecordViewModelAdapterTest, TargetOptionsAreRebuiltOnlyWhenTheTargetVectorIsReplaced) {
    RecordViewModel source;
    source.targets = {
        {exosnap::engine::CaptureTarget::Kind::Monitor, 1, "Display 1: 2560x1440 at (0, 0)"},
    };
    source.selected_target_index = 0;
    RecordViewModelAdapter adapter(&source);
    ASSERT_EQ(adapter.targetOptions().size(), 1);

    int option_changes = 0;
    QObject::connect(&adapter, &RecordViewModelAdapter::targetOptionsChanged,
                     [&option_changes]() { ++option_changes; });

    for (int i = 0; i < 20; ++i)
        adapter.synchronize();
    EXPECT_EQ(option_changes, 0);

    // A rescan that replaces the vector bumps the stamp; the lists are rebuilt
    // and, because they differ, republished.
    source.targets.push_back({exosnap::engine::CaptureTarget::Kind::Window, 2, "Editor — project.qml"});
    ++source.targets_revision;
    adapter.synchronize();
    EXPECT_EQ(option_changes, 1);
    EXPECT_EQ(adapter.targetOptions().size(), 2);
    EXPECT_EQ(adapter.windowTargetOptions().size(), 1);

    // A rescan that finds the identical set still must not republish: that signal
    // is what makes the source picker rebuild its list.
    ++source.targets_revision;
    adapter.synchronize();
    EXPECT_EQ(option_changes, 1);
}

TEST(RecordViewModelAdapterTest, ANewSourceRebuildsTheTargetOptionsRegardlessOfItsStamp) {
    RecordViewModel first;
    first.targets = {{exosnap::engine::CaptureTarget::Kind::Monitor, 1, "Display 1: 2560x1440 at (0, 0)"}};
    RecordViewModelAdapter adapter(&first);
    ASSERT_EQ(adapter.targetOptions().size(), 1);

    // Same revision number (both start at 0), different view model.
    RecordViewModel second;
    second.targets = {
        {exosnap::engine::CaptureTarget::Kind::Monitor, 7, "Display 2: 1920x1080 at (2560, 0)"},
        {exosnap::engine::CaptureTarget::Kind::Window, 9, "Terminal"},
    };
    adapter.setSource(&second);

    EXPECT_EQ(adapter.targetOptions().size(), 2);
    EXPECT_EQ(adapter.displayTargetOptions().size(), 1);
    EXPECT_EQ(adapter.windowTargetOptions().size(), 1);
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

// QCR-V03. The switch behind this is exhaustive with no `default:`, so a new
// state cannot silently inherit the permissive answer — MSVC's C4062 refuses the
// build first. This pins the full matrix so the exhaustive rewrite kept every
// existing answer, and so a future enumerator has to be entered here on purpose.
TEST(RecordViewModelAdapterTest, SourceSelectionIsClosedForEveryInFlightState) {
    RecordViewModel source;
    source.targets.push_back({exosnap::engine::CaptureTarget::Kind::Monitor, 1, "Display 1: 1920x1080 at (0, 0)"});
    source.selected_target_index = 0;
    RecordViewModelAdapter adapter(&source);

    // The capture is committed, or an overlay owns the picking.
    for (UiRecordingState state :
         {UiRecordingState::Countdown, UiRecordingState::Preparing, UiRecordingState::RegionSelecting,
          UiRecordingState::Recording, UiRecordingState::Paused, UiRecordingState::ArmedFromRecovery,
          UiRecordingState::Stopping, UiRecordingState::Saving}) {
        source.SetState(state);
        adapter.synchronize();
        EXPECT_FALSE(adapter.canSelectSource()) << "state index " << static_cast<int>(state);
    }

    // Nothing in flight: selectable, because a target exists.
    for (UiRecordingState state : {UiRecordingState::LoadingCapabilities, UiRecordingState::Ready,
                                   UiRecordingState::Blocked, UiRecordingState::Completed, UiRecordingState::Failed}) {
        source.SetState(state);
        adapter.synchronize();
        EXPECT_TRUE(adapter.canSelectSource()) << "state index " << static_cast<int>(state);
    }

    // …and with nothing to pick, no state is selectable.
    source.targets.clear();
    source.selected_target_index = -1;
    for (UiRecordingState state : {UiRecordingState::LoadingCapabilities, UiRecordingState::Ready,
                                   UiRecordingState::Blocked, UiRecordingState::Completed, UiRecordingState::Failed}) {
        source.SetState(state);
        adapter.synchronize();
        EXPECT_FALSE(adapter.canSelectSource()) << "state index " << static_cast<int>(state);
    }
}

TEST(RecordViewModelAdapterTest, MapsRecordingStateActionsAndTone) {
    RecordViewModel source;
    source.targets.push_back({exosnap::engine::CaptureTarget::Kind::Monitor, 1, "Display 1: 1920x1080 at (0, 0)"});
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
    // Paused is a normal state, so it never reports the caution tone: amber is
    // reserved for a real warning.
    EXPECT_EQ(adapter.stateTone(), QStringLiteral("paused"));

    source.SetState(UiRecordingState::Countdown);
    adapter.synchronize();
    EXPECT_EQ(adapter.stateTone(), QStringLiteral("busy"));

    source.SetState(UiRecordingState::Saving);
    adapter.synchronize();
    EXPECT_TRUE(adapter.finalizing());
    EXPECT_FALSE(adapter.canStop());
    EXPECT_EQ(adapter.stateTone(), QStringLiteral("busy"));
}

// The whole point of the tone vocabulary: no normal state may resolve to the
// caution tone. A future state added to the switch inherits this.
TEST(RecordViewModelAdapterTest, NormalStatesNeverReportTheWarningTone) {
    RecordViewModel source;
    source.targets.push_back({exosnap::engine::CaptureTarget::Kind::Monitor, 1, "Display 1: 1920x1080 at (0, 0)"});
    source.selected_target_index = 0;
    RecordViewModelAdapter adapter(&source);

    for (const auto state : {UiRecordingState::Ready, UiRecordingState::Countdown, UiRecordingState::Preparing,
                             UiRecordingState::Recording, UiRecordingState::Paused, UiRecordingState::Stopping,
                             UiRecordingState::Saving, UiRecordingState::RegionSelecting}) {
        source.SetState(state);
        adapter.synchronize();
        EXPECT_NE(adapter.stateTone(), QStringLiteral("warning")) << "state " << static_cast<int>(state);
    }
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
        {exosnap::engine::CaptureTarget::Kind::Monitor, 1, "Display 1: 2560x1440 at (0, 0)"},
        {exosnap::engine::CaptureTarget::Kind::Window, 2, "Editor — project.qml"},
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

TEST(RecordViewModelAdapterTest, PickerRowsExposeStableIdentityPresentationAndSelection) {
    RecordViewModel source;
    source.targets = {
        {exosnap::engine::CaptureTarget::Kind::Monitor, 41, R"(\\.\DISPLAY1)"},
        {exosnap::engine::CaptureTarget::Kind::Window, 73, "Claude Design - Brave"},
    };
    source.selected_target_index = 1;
    source.capture_mode = CaptureMode::Window;
    RecordViewModelAdapter adapter(&source);

    ASSERT_EQ(adapter.targetCount(), 2);
    const QVariantMap display = adapter.targetOptions().at(0).toMap();
    const QVariantMap window = adapter.targetOptions().at(1).toMap();
    EXPECT_EQ(display.value(QStringLiteral("identity")).toString(), QStringLiteral("display:41"));
    EXPECT_EQ(display.value(QStringLiteral("kind")).toString(), QStringLiteral("display"));
    EXPECT_EQ(display.value(QStringLiteral("thumbnailState")).toString(), QStringLiteral("placeholder"));
    EXPECT_FALSE(display.value(QStringLiteral("selected")).toBool());
    EXPECT_EQ(window.value(QStringLiteral("identity")).toString(), QStringLiteral("window:73"));
    EXPECT_EQ(window.value(QStringLiteral("label")).toString(), QStringLiteral("Brave - Claude Design"));
    EXPECT_TRUE(window.value(QStringLiteral("selected")).toBool());
    EXPECT_EQ(adapter.selectedTargetIdentity(), QStringLiteral("window:73"));
    EXPECT_TRUE(adapter.selectedTargetAvailable());
}

TEST(RecordViewModelAdapterTest, PickerFilterMatchesKindAndResolvedLabel) {
    RecordViewModel source;
    source.targets = {
        {exosnap::engine::CaptureTarget::Kind::Monitor, 41, R"(\\.\DISPLAY1)"},
        {exosnap::engine::CaptureTarget::Kind::Window, 73, "Claude Design - Brave"},
        {exosnap::engine::CaptureTarget::Kind::Window, 74, "Task Manager"},
    };
    RecordViewModelAdapter adapter(&source);

    const QVariantList matches = adapter.filteredTargetOptions(QStringLiteral("window"), QStringLiteral("claude"));

    ASSERT_EQ(matches.size(), 1);
    EXPECT_EQ(matches.front().toMap().value(QStringLiteral("identity")).toString(), QStringLiteral("window:73"));
}

TEST(RecordViewModelAdapterTest, CachedStillSurvivesRefreshAndOtherTargetsKeepAPlaceholder) {
    RecordViewModel source;
    source.targets = {
        {exosnap::engine::CaptureTarget::Kind::Window, 73, "Claude Design - Brave"},
        {exosnap::engine::CaptureTarget::Kind::Window, 74, "Task Manager"},
    };
    RecordViewModelAdapter adapter(&source);
    adapter.setTargetStill(QStringLiteral("window:73"), QStringLiteral("image://capture-target/window-73/1"));

    source.targets[0].description = "Renamed document - Brave";
    ++source.targets_revision;
    adapter.synchronize();

    EXPECT_EQ(adapter.targetOptions().at(0).toMap().value(QStringLiteral("thumbnailState")).toString(),
              QStringLiteral("ready"));
    EXPECT_EQ(adapter.targetOptions().at(0).toMap().value(QStringLiteral("thumbnailSource")).toString(),
              QStringLiteral("image://capture-target/window-73/1"));
    EXPECT_EQ(adapter.targetOptions().at(1).toMap().value(QStringLiteral("thumbnailState")).toString(),
              QStringLiteral("placeholder"));
}

TEST(RecordViewModelAdapterTest, VisibleIdentitiesAreForwardedOnceInLayoutOrder) {
    RecordViewModel source;
    source.targets = {
        {exosnap::engine::CaptureTarget::Kind::Monitor, 41, R"(\\.\DISPLAY1)"},
        {exosnap::engine::CaptureTarget::Kind::Window, 73, "Claude Design - Brave"},
    };
    RecordViewModelAdapter adapter(&source);
    QList<QStringList> published;
    QObject::connect(&adapter, &RecordViewModelAdapter::visibleTargetIdentitiesChanged,
                     [&published](const QStringList& identities) { published.push_back(identities); });

    const QStringList visible{QStringLiteral("window:73"), QStringLiteral("display:41")};
    adapter.setVisibleTargetIdentities(visible);
    // The unchanged set is not republished: the still service would restart its
    // round robin from the top and never finish a pass while the user scrolls.
    adapter.setVisibleTargetIdentities(visible);
    adapter.setVisibleTargetIdentities({});

    ASSERT_EQ(published.size(), 2);
    EXPECT_EQ(published.at(0), visible);
    EXPECT_TRUE(published.at(1).isEmpty());
}

TEST(RecordViewModelAdapterTest, AnUnavailableTargetKeepsItsStillAndGoesStale) {
    RecordViewModel source;
    source.targets = {
        {exosnap::engine::CaptureTarget::Kind::Window, 73, "Claude Design - Brave"},
        {exosnap::engine::CaptureTarget::Kind::Window, 74, "Task Manager"},
    };
    RecordViewModelAdapter adapter(&source);
    adapter.setTargetStill(QStringLiteral("window:73"), QStringLiteral("image://capture-target/window-73/1"));

    adapter.setTargetStillUnavailable(QStringLiteral("window:73"));
    // A target that never delivered a still has nothing to lose and stays a
    // placeholder rather than announcing a failure the card cannot show.
    adapter.setTargetStillUnavailable(QStringLiteral("window:74"));

    EXPECT_EQ(adapter.targetOptions().at(0).toMap().value(QStringLiteral("thumbnailState")).toString(),
              QStringLiteral("stale"));
    EXPECT_EQ(adapter.targetOptions().at(0).toMap().value(QStringLiteral("thumbnailSource")).toString(),
              QStringLiteral("image://capture-target/window-73/1"));
    EXPECT_EQ(adapter.targetOptions().at(1).toMap().value(QStringLiteral("thumbnailState")).toString(),
              QStringLiteral("placeholder"));

    adapter.setTargetStill(QStringLiteral("window:73"), QStringLiteral("image://capture-target/window-73/2"));
    EXPECT_EQ(adapter.targetOptions().at(0).toMap().value(QStringLiteral("thumbnailState")).toString(),
              QStringLiteral("ready"));
}

TEST(RecordViewModelAdapterTest, RegionPresetsLeadWithDrawCustomAndCoverTheNamedRatios) {
    RecordViewModel source;
    RecordViewModelAdapter adapter(&source);

    const QVariantList presets = adapter.regionPresetOptions();

    ASSERT_EQ(presets.size(), 5);
    const QVariantMap draw_custom = presets.at(0).toMap();
    EXPECT_EQ(draw_custom.value(QStringLiteral("key")).toString(), QStringLiteral("custom"));
    EXPECT_EQ(draw_custom.value(QStringLiteral("label")).toString(), QStringLiteral("Draw custom"));
    EXPECT_TRUE(draw_custom.value(QStringLiteral("draw")).toBool());

    const QList<QPair<QString, double>> expected{
        {QStringLiteral("16:9"), 16.0 / 9.0},
        {QStringLiteral("9:16"), 9.0 / 16.0},
        {QStringLiteral("1:1"), 1.0},
        {QStringLiteral("4:5"), 4.0 / 5.0},
    };
    for (int i = 0; i < expected.size(); ++i) {
        const QVariantMap preset = presets.at(i + 1).toMap();
        EXPECT_EQ(preset.value(QStringLiteral("key")).toString(), expected.at(i).first) << i;
        EXPECT_DOUBLE_EQ(preset.value(QStringLiteral("aspect")).toDouble(), expected.at(i).second) << i;
        EXPECT_FALSE(preset.value(QStringLiteral("draw")).toBool()) << i;
    }
}

TEST(RecordViewModelAdapterTest, RegionEditingLocksOnlyWhileTheCaptureIsLive) {
    RecordViewModel source;
    source.targets.push_back({exosnap::engine::CaptureTarget::Kind::Monitor, 1, "Display 1: 1920x1080 at (0, 0)"});
    source.selected_target_index = 0;
    RecordViewModelAdapter adapter(&source);

    // The rectangle is the thing the user is still composing, so every state
    // before the capture itself is live leaves it editable -- including the
    // region-selection state whose overlay IS the editor.
    for (const auto state : {UiRecordingState::Ready, UiRecordingState::RegionSelecting, UiRecordingState::Blocked,
                             UiRecordingState::Completed, UiRecordingState::Failed}) {
        source.SetState(state);
        adapter.synchronize();
        EXPECT_FALSE(adapter.regionEditingLocked()) << "state " << static_cast<int>(state);
    }

    // From the countdown onward the capture owns the region.
    for (const auto state :
         {UiRecordingState::Countdown, UiRecordingState::Recording, UiRecordingState::Paused,
          UiRecordingState::ArmedFromRecovery, UiRecordingState::Stopping, UiRecordingState::Saving}) {
        source.SetState(state);
        adapter.synchronize();
        EXPECT_TRUE(adapter.regionEditingLocked()) << "state " << static_cast<int>(state);
    }
}

TEST(RecordViewModelAdapterTest, DisplayRowsCarryTheResolvedRegionLabel) {
    RecordViewModel source;
    source.targets = {
        {exosnap::engine::CaptureTarget::Kind::Monitor, 41, R"(\\.\DISPLAY1)"},
        {exosnap::engine::CaptureTarget::Kind::Window, 73, "Claude Design - Brave"},
    };
    RecordViewModelAdapter adapter(&source);

    const QVariantMap display = adapter.targetOptions().at(0).toMap();
    const QVariantMap window = adapter.targetOptions().at(1).toMap();
    EXPECT_EQ(display.value(QStringLiteral("regionLabel")).toString(), QStringLiteral("Region on Display 1"));
    EXPECT_EQ(window.value(QStringLiteral("regionLabel")).toString(), QString{});
}

TEST(RecordViewModelAdapterTest, RegionPresetRequestRoutesThroughTheNarrowSignal) {
    RecordViewModel source;
    RecordViewModelAdapter adapter(&source);
    QString requested;
    QObject::connect(&adapter, &RecordViewModelAdapter::regionPresetRequested,
                     [&requested](const QString& key) { requested = key; });

    adapter.requestRegionPreset(QStringLiteral("9:16"));

    EXPECT_EQ(requested, QStringLiteral("9:16"));
}

TEST(RecordViewModelAdapterTest, DisappearedSelectionRemainsNamedButUnresolved) {
    RecordViewModel source;
    source.targets = {{exosnap::engine::CaptureTarget::Kind::Window, 73, "Claude Design - Brave"}};
    source.selected_target_index = 0;
    RecordViewModelAdapter adapter(&source);
    ASSERT_EQ(adapter.selectedTargetIdentity(), QStringLiteral("window:73"));

    source.targets.clear();
    source.selected_target_index = -1;
    ++source.targets_revision;
    adapter.synchronize();

    EXPECT_EQ(adapter.selectedTargetIdentity(), QStringLiteral("window:73"));
    EXPECT_FALSE(adapter.selectedTargetAvailable());
    EXPECT_EQ(adapter.targetCount(), 0);
}

TEST(RecordViewModelAdapterTest, RegionSelectionBlocksStartAndNormalizesCrop) {
    RecordViewModel source;
    source.targets.push_back({exosnap::engine::CaptureTarget::Kind::Monitor, 1, "Display 1: 1920x1080 at (0, 0)"});
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
        {exosnap::engine::AudioSourceKind::Sys, true, false},
        {exosnap::engine::AudioSourceKind::App, false, false},
        {exosnap::engine::AudioSourceKind::Mic, true, false},
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

// The remux progress the Widgets shell used to show and the cutover dropped.
// Its whole contract is the distinction between "no fraction measured" and
// "0 %": a bar sitting at zero for the length of the first packet claims
// progress that was never observed.
TEST(RecordViewModelAdapterTest, SavingProgressStartsUnknownAndClampsToUnit) {
    RecordViewModelAdapter adapter;
    EXPECT_DOUBLE_EQ(adapter.savingProgress(), -1.0);

    int notifications = 0;
    QObject::connect(&adapter, &RecordViewModelAdapter::savingProgressChanged, [&notifications]() { ++notifications; });

    // The coordinator's own start marker. It must not read as 0 %.
    adapter.setSavingProgress(-1.0f);
    EXPECT_DOUBLE_EQ(adapter.savingProgress(), -1.0);
    EXPECT_EQ(notifications, 0);

    adapter.setSavingProgress(0.42f);
    // Against the WIDENED float, not the double literal: the coordinator reports
    // a float and 0.42f widens to 0.41999998688697815.
    EXPECT_DOUBLE_EQ(adapter.savingProgress(), static_cast<qreal>(0.42f));
    EXPECT_EQ(notifications, 1);

    // Idempotent: the remuxer reports once per video packet, thousands of times
    // for a short clip, and an unchanged value must not wake the binding.
    adapter.setSavingProgress(0.42f);
    EXPECT_EQ(notifications, 1);

    adapter.setSavingProgress(1.7f);
    EXPECT_DOUBLE_EQ(adapter.savingProgress(), 1.0);

    // Leaving Saving clears it, so the next recording cannot inherit this one's
    // last fraction while its own remux is still silent.
    adapter.setSavingProgress(-1.0f);
    EXPECT_DOUBLE_EQ(adapter.savingProgress(), -1.0);
}

} // namespace
} // namespace exosnap::quick
