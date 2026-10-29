#include "models/AudioRoutingMigration.h"
#include "models/RecordingPreset.h"
#include "quick/ExoSnap/Quick/AudioRoutingAdapter.h"
#include "settings/RecordingPresetStore.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <limits>

namespace exosnap::quick {
namespace {
using namespace engine;

TrackGraph1Intent ExampleGraph() {
    TrackGraph1Intent graph;
    AudioSourceSpec source;
    source.id = NewId<AudioSourceId>();
    source.name = "Microphone";
    source.binding.follow_default = false;
    source.binding.endpoint_id = "endpoint";
    graph.sources.push_back(source);
    for (uint32_t order = 0; order != 2; ++order) {
        AudioTrackSpec track;
        track.id = NewId<AudioTrackId>();
        track.order = order;
        graph.tracks.push_back(track);
    }
    AudioRouteSpec route;
    route.id = NewId<AudioRouteId>();
    route.source_id = source.id;
    route.track_id = graph.tracks.front().id;
    graph.routes.push_back(route);
    graph.default_monitor_track_ids.push_back(graph.tracks.front().id);
    return graph;
}

AudioEnvironmentSnapshot Environment() {
    AudioEnvironmentSnapshot environment;
    environment.playback_endpoints = {"endpoint"};
    environment.default_playback_endpoint = "endpoint";
    return environment;
}

QString Id(const auto& id) {
    return QString::fromStdString(ToUuid(id));
}

TEST(AudioHarnessIntent, SourceRowsResolveThroughProductPlanWithMergeAndDspSettings) {
    for (const bool merge : {false, true}) {
        capability::AudioUiState audio;
        audio.source_rows = {{AudioSourceKind::Sys, true, false}, {AudioSourceKind::Mic, true, merge}};
        audio.mic_gain_linear = 0.5f;
        audio.mic_hpf_enabled = true;
        audio.mic_hpf_cutoff_hz = 120.f;
        audio.limiter_enabled = false;
        const auto intent = CreateLegacyAudioIntent(audio, NewId<PresetNamespaceId>());
        AudioEnvironmentSnapshot environment;
        environment.default_playback_endpoint = "speaker";
        environment.playback_endpoints = {"speaker"};
        environment.default_microphone_endpoint = "microphone";
        environment.microphone_endpoints = {"microphone"};
        const auto resolved = ResolveAudioIntent(intent, environment, {7});
        ASSERT_TRUE(resolved.succeeded());
        const auto& plan = *resolved.value();
        EXPECT_EQ(plan.revision.value, 7u);
        ASSERT_EQ(plan.sources.size(), 2u);
        ASSERT_TRUE(plan.sources[0].capture_key.has_value());
        ASSERT_TRUE(plan.sources[1].capture_key.has_value());
        EXPECT_EQ(plan.sources[0].capture_key->kind, AudioBindingKind::PlaybackEndpoint);
        EXPECT_EQ(plan.sources[1].capture_key->kind, AudioBindingKind::MicrophoneEndpoint);
        EXPECT_FLOAT_EQ(plan.sources[1].spec.post_dsp_gain_linear, 0.5f);
        EXPECT_TRUE(plan.sources[1].spec.dsp.hpf_enabled);
        EXPECT_FLOAT_EQ(plan.sources[1].spec.dsp.hpf_cutoff_hz, 120.f);
        ASSERT_EQ(plan.tracks.size(), merge ? 1u : 2u);
        EXPECT_EQ(plan.tracks[0].routes.size(), merge ? 2u : 1u);
        EXPECT_EQ(plan.tracks[0].fixed_average_divisor, merge ? 2u : 1u);
        for (size_t i = 0; i < plan.tracks.size(); ++i) {
            EXPECT_EQ(plan.tracks[i].mux_index.value, i);
            EXPECT_FALSE(plan.tracks[i].limiter.enabled);
        }
    }
}

TEST(AudioHarnessIntent, EmptyRowsProduceNoAudioTracks) {
    const auto intent = CreateLegacyAudioIntent({}, NewId<PresetNamespaceId>());
    const auto resolved = ResolveAudioIntent(intent, {});
    ASSERT_TRUE(resolved.succeeded());
    EXPECT_TRUE(resolved.value()->sources.empty());
    EXPECT_TRUE(resolved.value()->tracks.empty());
}

TEST(AudioRoutingAdapter, FailedSaveAndStaleCommandPreserveRevisionAndVisibleCards) {
    auto graph = ExampleGraph();
    AudioRoutingAdapter adapter;
    adapter.configure(
        [](const AudioIntent&, QString& error) {
            error = "disk full";
            return false;
        },
        Environment);
    adapter.refresh(graph);
    const auto before = adapter.tracks();
    const auto revision = adapter.revision();
    EXPECT_FALSE(adapter.addEmptyTrack(revision));
    EXPECT_EQ(adapter.lastError(), "disk full");
    EXPECT_EQ(adapter.tracks(), before);
    EXPECT_EQ(adapter.revision(), revision);
    EXPECT_FALSE(adapter.setTrackName(Id(graph.tracks[0].id), "Renamed", revision + 1));
    EXPECT_EQ(adapter.tracks(), before);
    EXPECT_EQ(adapter.revision(), revision);
}

TEST(AudioRoutingAdapter, EmptyTrackAppearsAndCustomNameSurvivesIdentityReorder) {
    AudioIntent saved;
    AudioRoutingAdapter adapter;
    adapter.configure(
        [&](const AudioIntent& candidate, QString&) {
            saved = candidate;
            return true;
        },
        Environment);
    auto graph = ExampleGraph();
    adapter.refresh(graph);
    ASSERT_TRUE(adapter.addEmptyTrack(adapter.revision()));
    ASSERT_EQ(adapter.tracks().size(), 3);
    const auto empty = adapter.tracks().last().toMap();
    EXPECT_TRUE(empty.value("empty").toBool());
    EXPECT_FALSE(empty.value("recording").toBool());
    EXPECT_EQ(empty.value("name").toString(), "Audio 3");
    const QString track = Id(graph.tracks.front().id);
    ASSERT_TRUE(adapter.setTrackName(track, "My mix", adapter.revision()));
    ASSERT_TRUE(adapter.reorderTrackById(track, 1, adapter.revision()));
    EXPECT_EQ(adapter.tracks()[1].toMap().value("id").toString(), track);
    EXPECT_EQ(adapter.tracks()[1].toMap().value("name").toString(), "My mix");
    EXPECT_EQ(std::get<TrackGraph1Intent>(saved).tracks.size(), 3u);
}

TEST(AudioRoutingAdapter, RemovingTrackAtomicallyRemovesOnlyItsRoutesAndMonitorReferences) {
    auto graph = ExampleGraph();
    graph.routes.push_back({NewId<AudioRouteId>(), graph.sources.front().id, graph.tracks.back().id, -12.f});
    graph.default_monitor_track_ids.push_back(graph.tracks.back().id);
    graph.monitor_selection_origin = MonitorSelectionOrigin::User;
    AudioIntent saved = graph;
    bool save_ok = false;
    AudioRoutingAdapter adapter;
    adapter.configure(
        [&](const AudioIntent& candidate, QString&) {
            if (!save_ok)
                return false;
            saved = candidate;
            return true;
        },
        Environment);
    adapter.refresh(graph);
    const auto revision = adapter.revision();
    const auto before = adapter.tracks();
    EXPECT_FALSE(adapter.removeTrack(Id(graph.tracks.front().id), revision));
    EXPECT_EQ(adapter.tracks(), before);
    EXPECT_EQ(adapter.revision(), revision);
    save_ok = true;
    EXPECT_FALSE(adapter.removeTrack(Id(graph.tracks.front().id), revision + 1));
    EXPECT_FALSE(adapter.removeTrack(Id(NewId<AudioTrackId>()), revision));
    ASSERT_TRUE(adapter.removeTrack(Id(graph.tracks.front().id), revision));
    const auto& remaining = std::get<TrackGraph1Intent>(saved);
    ASSERT_EQ(remaining.tracks.size(), 1u);
    EXPECT_EQ(remaining.tracks.front().id, graph.tracks.back().id);
    EXPECT_EQ(remaining.tracks.front().order, 0u);
    ASSERT_EQ(remaining.routes.size(), 1u);
    EXPECT_EQ(remaining.routes.front().id, graph.routes.back().id);
    EXPECT_EQ(remaining.sources, graph.sources);
    EXPECT_EQ(remaining.default_monitor_track_ids, std::vector{graph.tracks.back().id});
    const auto plan = ResolveAudioIntent(saved, Environment());
    ASSERT_TRUE(plan.succeeded());
    ASSERT_EQ(plan.value()->tracks.size(), 1u);
    EXPECT_EQ(plan.value()->tracks.front().mux_index.value, 0u);
    EXPECT_NEAR(plan.value()->tracks.front().routes.front().gain_linear, GainDbToLinear(-12.f, false), 0.0001f);
    ASSERT_TRUE(adapter.removeTrack(Id(graph.tracks.back().id), adapter.revision()));
    EXPECT_TRUE(adapter.tracks().empty());
    EXPECT_TRUE(std::get<TrackGraph1Intent>(saved).default_monitor_track_ids.empty());
    EXPECT_EQ(adapter.sources().size(), 1);
}

TEST(AudioRoutingAdapter, CompactAssignmentReactivatesLegacySourceWithoutDroppingOtherTrackRoutes) {
    auto graph = ExampleGraph();
    graph.sources.front().participation = false;
    graph.routes.front().enabled = false;
    graph.routes.front().gain_db = -6.f;
    AudioIntent saved;
    AudioRoutingAdapter adapter;
    adapter.configure(
        [&](const AudioIntent& candidate, QString&) {
            saved = candidate;
            return true;
        },
        Environment);
    adapter.refresh(graph);
    const auto source = Id(graph.sources.front().id);
    ASSERT_TRUE(adapter.setTrackSourceAssigned(source, Id(graph.tracks.front().id), true, adapter.revision()));
    auto configured = std::get<TrackGraph1Intent>(saved);
    EXPECT_TRUE(configured.sources.front().participation);
    EXPECT_TRUE(configured.routes.front().enabled);
    EXPECT_EQ(configured.routes.front().id, graph.routes.front().id);
    EXPECT_EQ(configured.routes.front().gain_db, -6.f);
    ASSERT_TRUE(adapter.setTrackSourceAssigned(source, Id(graph.tracks.back().id), true, adapter.revision()));
    auto resolved = ResolveAudioIntent(saved, Environment());
    ASSERT_TRUE(resolved.succeeded());
    ASSERT_EQ(resolved.value()->tracks.size(), 2u);
    EXPECT_EQ(resolved.value()->tracks[0].routes[0].source_id, resolved.value()->tracks[1].routes[0].source_id);
    EXPECT_NE(resolved.value()->tracks[0].routes[0].gain_linear, resolved.value()->tracks[1].routes[0].gain_linear);
    ASSERT_TRUE(adapter.setTrackSourceAssigned(source, Id(graph.tracks.front().id), false, adapter.revision()));
    EXPECT_EQ(std::get<TrackGraph1Intent>(saved).routes.size(), 1u);
    EXPECT_TRUE(std::get<TrackGraph1Intent>(saved).sources.front().participation);
    const auto before = adapter.tracks();
    EXPECT_FALSE(adapter.moveTrackTo(Id(graph.tracks.back().id), 2, adapter.revision()));
    EXPECT_EQ(adapter.tracks(), before);
    ASSERT_TRUE(adapter.moveTrackTo(Id(graph.tracks.back().id), 0, adapter.revision()));
    EXPECT_EQ(adapter.tracks().front().toMap().value("id").toString(), Id(graph.tracks.back().id));
    const auto reordered = ResolveAudioIntent(saved, Environment());
    ASSERT_TRUE(reordered.succeeded());
    EXPECT_EQ(reordered.value()->tracks[0].id, graph.tracks.back().id);
    EXPECT_EQ(reordered.value()->tracks[0].mux_index.value, 0u);
}

TEST(AudioRoutingAdapter, AdditionalKeepsOldRouteAndMoveRemovesAllOtherRoutesAtomically) {
    auto graph = ExampleGraph();
    AudioIntent saved;
    AudioRoutingAdapter adapter;
    adapter.configure(
        [&](const AudioIntent& candidate, QString&) {
            saved = candidate;
            return true;
        },
        Environment);
    adapter.refresh(graph);
    const auto source = Id(graph.sources.front().id);
    const auto target = Id(graph.tracks.back().id);
    ASSERT_TRUE(adapter.assignSource(source, target, AudioRoutingAdapter::Additional, adapter.revision()));
    EXPECT_EQ(std::get<TrackGraph1Intent>(saved).routes.size(), 2u);
    ASSERT_TRUE(adapter.assignSource(source, target, AudioRoutingAdapter::Additional, adapter.revision()));
    EXPECT_EQ(std::get<TrackGraph1Intent>(saved).routes.size(), 2u);
    ASSERT_TRUE(adapter.assignSource(source, target, AudioRoutingAdapter::Move, adapter.revision()));
    const auto& moved = std::get<TrackGraph1Intent>(saved);
    ASSERT_EQ(moved.routes.size(), 1u);
    EXPECT_EQ(moved.routes.front().track_id, graph.tracks.back().id);
}

LegacyRows25Intent Legacy() {
    LegacyRows25Intent legacy;
    legacy.id_namespace = NewId<PresetNamespaceId>();
    LegacyAudioRow row;
    row.row_id = NewId<LegacyRowId>();
    row.source_id = NewId<AudioSourceId>();
    row.anchor_track_id = NewId<AudioTrackId>();
    row.row.kind = AudioSourceKind::SystemOutput;
    row.row.enabled = true;
    legacy.rows.push_back(row);
    return legacy;
}

TEST(AudioRoutingAdapter, LegacyCancelPreservesCapsuleAndConfirmUndoRestoresWholeVariant) {
    const auto legacy = Legacy();
    AudioIntent saved = legacy;
    AudioRoutingAdapter adapter;
    adapter.configure(
        [&](const AudioIntent& candidate, QString&) {
            saved = candidate;
            return true;
        },
        Environment);
    adapter.refresh(legacy);
    const auto revision = adapter.revision();
    EXPECT_FALSE(adapter.addEmptyTrack(revision));
    EXPECT_TRUE(adapter.conversionPending());
    EXPECT_EQ(adapter.revision(), revision);
    ASSERT_TRUE(adapter.cancelLegacyConversion(revision));
    EXPECT_EQ(std::get<LegacyRows25Intent>(saved), legacy);
    EXPECT_FALSE(adapter.conversionPending());
    EXPECT_FALSE(adapter.addEmptyTrack(revision));
    ASSERT_TRUE(adapter.confirmLegacyConversion(revision));
    EXPECT_FALSE(adapter.legacy());
    EXPECT_EQ(std::get<TrackGraph1Intent>(saved).tracks.size(), 2u);
    ASSERT_TRUE(adapter.undo(adapter.revision()));
    EXPECT_TRUE(adapter.legacy());
    EXPECT_EQ(std::get<LegacyRows25Intent>(saved), legacy);
}

TEST(AudioRoutingAdapter, DisabledLegacyTrackRemainsEditableThroughExplicitConversion) {
    auto legacy = Legacy();
    legacy.rows.front().row.enabled = false;
    for (const bool assign : {false, true}) {
        AudioIntent saved = legacy;
        AudioRoutingAdapter adapter;
        adapter.configure(
            [&](const AudioIntent& candidate, QString&) {
                saved = candidate;
                return true;
            },
            Environment);
        adapter.refresh(legacy);
        ASSERT_EQ(adapter.tracks().size(), 1);
        EXPECT_TRUE(adapter.tracks().front().toMap().value("empty").toBool());
        const auto revision = adapter.revision();
        const auto track = Id(legacy.rows.front().anchor_track_id);
        const auto source = Id(legacy.rows.front().source_id);
        EXPECT_FALSE(assign ? adapter.setTrackSourceAssigned(source, track, true, revision)
                            : adapter.removeTrack(track, revision));
        ASSERT_TRUE(adapter.conversionPending());
        EXPECT_EQ(saved, AudioIntent{legacy});
        ASSERT_TRUE(adapter.confirmLegacyConversion(revision));
        const auto& graph = std::get<TrackGraph1Intent>(saved);
        EXPECT_EQ(graph.tracks.size(), assign ? 1u : 0u);
        EXPECT_EQ(graph.routes.size(), assign ? 1u : 0u);
        if (assign) {
            EXPECT_TRUE(graph.sources.front().participation);
            const auto resolved = ResolveAudioIntent(saved, Environment());
            ASSERT_TRUE(resolved.succeeded());
            EXPECT_EQ(resolved.value()->tracks.size(), 1u);
        }
    }
}

TEST(AudioRoutingAdapter, SaveReloadKeepsIdsAndUndoSaveFailureKeepsCurrentState) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    RecordingPresetStore store(directory.filePath("presets.toml"));
    auto config = MakeDefaultPreset().config;
    config.audio_intent = ExampleGraph();
    bool fail = false;
    AudioRoutingAdapter adapter;
    adapter.configure(
        [&](const AudioIntent& candidate, QString& error) {
            if (fail) {
                error = "locked";
                return false;
            }
            auto next = config;
            next.audio_intent = candidate;
            if (!store.Save({}, std::string(kDefaultPresetId), next, &error))
                return false;
            config = std::move(next);
            return true;
        },
        Environment);
    adapter.refresh(*config.audio_intent);
    ASSERT_TRUE(adapter.addEmptyTrack(adapter.revision()));
    const auto source_id = adapter.sources().front().toMap().value("id").toString();
    const auto track_id = adapter.tracks().last().toMap().value("id").toString();
    ASSERT_TRUE(adapter.setTrackSourceAssigned(source_id, track_id, true, adapter.revision()));
    const auto routes = adapter.tracks().last().toMap().value("routes").toList();
    ASSERT_EQ(routes.size(), 1);
    ASSERT_TRUE(adapter.setRouteGain(routes.front().toMap().value("id").toString(), -12.0, adapter.revision()));
    ASSERT_TRUE(adapter.setTrackName(track_id, "Microphone copy", adapter.revision()));
    ASSERT_TRUE(adapter.moveTrackTo(track_id, 0, adapter.revision()));
    ASSERT_TRUE(adapter.setDefaultMonitorSelection({track_id}, adapter.revision()));
    ASSERT_TRUE(adapter.setTrackRecordEnabled(track_id, false, adapter.revision()));
    const auto cards = adapter.tracks();
    const auto loaded = store.Load();
    ASSERT_TRUE(loaded.live);
    ASSERT_TRUE(loaded.live->audio_intent);
    AudioRoutingAdapter reopened;
    reopened.refresh(*loaded.live->audio_intent);
    EXPECT_EQ(reopened.tracks(), cards);
    EXPECT_EQ(reopened.defaultMonitorTrackIds(), QStringList{track_id});
    EXPECT_EQ(*loaded.live->audio_intent, *config.audio_intent);
    fail = true;
    const auto revision = adapter.revision();
    EXPECT_FALSE(adapter.undo(revision));
    EXPECT_EQ(adapter.tracks(), cards);
    EXPECT_EQ(adapter.revision(), revision);
    EXPECT_TRUE(adapter.canUndo());
}

TEST(AudioRoutingAdapter, MetersStaySeparateAndActiveMuteCannotIntroduceSource) {
    auto graph = ExampleGraph();
    AudioRoutingAdapter adapter;
    adapter.configure([](const AudioIntent&, QString&) { return true; }, Environment,
                      [](AudioSourceId, bool) { return true; });
    adapter.refresh(graph);
    const auto resolved = ResolveAudioIntent(graph, Environment());
    ASSERT_TRUE(resolved.succeeded());
    adapter.setActivePlan(*resolved.value());
    SourceMeterSnapshot source;
    source.source_id = graph.sources.front().id;
    source.has_measurement = true;
    source.rms = 0.5f;
    source.peak = 0.75f;
    source.generation.value = 17;
    TrackMeterSnapshot track;
    track.track_id = graph.tracks.front().id;
    track.has_measurement = true;
    track.rms = 0.25f;
    int planChanges = 0;
    int meterChanges = 0;
    QObject::connect(&adapter, &AudioRoutingAdapter::changed, [&] { ++planChanges; });
    QObject::connect(&adapter, &AudioRoutingAdapter::metersChanged, [&] { ++meterChanges; });
    adapter.setMeters({source}, {track});
    EXPECT_EQ(planChanges, 0);
    EXPECT_EQ(meterChanges, 1);
    EXPECT_NEAR(adapter.sourceMeters().value(Id(source.source_id)).toMap().value("rmsDbfs").toDouble(), -6.0206, 0.001);
    EXPECT_NEAR(adapter.trackMeters().value(Id(track.track_id)).toMap().value("rmsDbfs").toDouble(), -12.0412, 0.001);
    EXPECT_NEAR(adapter.sources().front().toMap().value("rmsDbfs").toDouble(), -6.0206, 0.001);
    EXPECT_NEAR(adapter.tracks().front().toMap().value("rmsDbfs").toDouble(), -12.0412, 0.001);
    EXPECT_FALSE(adapter.tracks().back().toMap().value("meterAvailable").toBool());
    const auto revision = adapter.revision();
    EXPECT_FALSE(adapter.setSourceMute(Id(NewId<AudioSourceId>()), true, revision));
    EXPECT_EQ(adapter.revision(), revision);
    ASSERT_TRUE(adapter.setSourceMute(Id(source.source_id), true, revision));
    EXPECT_TRUE(adapter.activeSources().front().toMap().value("muted").toBool());
    EXPECT_FALSE(adapter.sources().front().toMap().value("muted").toBool());
    EXPECT_FALSE(adapter.setSourceMute(Id(source.source_id), false, revision));
}

TEST(AudioRoutingAdapter, FrozenConfigurationStillAllowsMuteOnTheRunningPlan) {
    const auto graph = ExampleGraph();
    AudioRoutingAdapter adapter;
    int saves = 0;
    int live_mutes = 0;
    adapter.configure(
        [&](const AudioIntent&, QString&) {
            ++saves;
            return true;
        },
        Environment,
        [&](AudioSourceId, bool) {
            ++live_mutes;
            return true;
        });
    adapter.refresh(graph, true);
    const auto resolved = ResolveAudioIntent(graph, Environment());
    ASSERT_TRUE(resolved.succeeded());
    adapter.setActivePlan(*resolved.value());
    const auto revision = adapter.revision();
    EXPECT_FALSE(adapter.setTrackName(Id(graph.tracks.front().id), "Changed", revision));
    EXPECT_EQ(saves, 0);
    ASSERT_TRUE(adapter.setSourceMute(Id(graph.sources.front().id), true, revision));
    EXPECT_EQ(live_mutes, 1);
    EXPECT_TRUE(adapter.activeSources().front().toMap().value("muted").toBool());
    EXPECT_FALSE(adapter.sources().front().toMap().value("muted").toBool());
    EXPECT_EQ(saves, 0);
}
TEST(AudioRoutingAdapter, UnknownFutureConfigurationRejectsEditsWithVisibleError) {
    AudioRoutingAdapter adapter;
    adapter.configure([](const AudioIntent&, QString&) { return true; }, Environment);
    adapter.refresh(ExampleGraph(), true, "future audio contract");
    const auto revision = adapter.revision();
    EXPECT_FALSE(adapter.addEmptyTrack(revision));
    EXPECT_FALSE(adapter.lastError().isEmpty());
    EXPECT_EQ(adapter.revision(), revision);
}

TEST(AudioRoutingAdapter, NativeDefaultMonitoringAddsRoutesWithoutRemovingSelectionAndExplicitChoiceIsUser) {
    const auto graph = ExampleGraph();
    AudioIntent saved;
    AudioRoutingAdapter adapter;
    adapter.configure(
        [&](const AudioIntent& candidate, QString&) {
            saved = candidate;
            return true;
        },
        Environment);
    adapter.refresh(graph);
    ASSERT_TRUE(adapter.assignSource(Id(graph.sources.front().id), Id(graph.tracks.back().id),
                                     AudioRoutingAdapter::Additional, adapter.revision()));
    const auto& added = std::get<TrackGraph1Intent>(saved);
    EXPECT_EQ(added.default_monitor_track_ids.size(), 2u);
    EXPECT_TRUE(adapter.monitorConfirmationRequired());
    ASSERT_TRUE(adapter.setDefaultMonitorSelection({Id(graph.tracks.front().id)}, adapter.revision()));
    EXPECT_EQ(std::get<TrackGraph1Intent>(saved).monitor_selection_origin, MonitorSelectionOrigin::User);
    EXPECT_FALSE(adapter.monitorConfirmationRequired());
    EXPECT_FALSE(adapter.warnings().empty());
}

TEST(AudioRoutingAdapter, LegacyNonStructuralEditsKeepCapsuleAndFailedConversionKeepsPendingAction) {
    const auto legacy = Legacy();
    AudioIntent saved = legacy;
    bool fail = false;
    AudioRoutingAdapter adapter;
    adapter.configure(
        [&](const AudioIntent& candidate, QString& error) {
            if (fail) {
                error = "locked";
                return false;
            }
            saved = candidate;
            return true;
        },
        Environment);
    adapter.refresh(legacy);
    ASSERT_TRUE(adapter.setTrackName(Id(legacy.rows.front().anchor_track_id), "Saved name", adapter.revision()));
    ASSERT_TRUE(adapter.setRouteGain(Id(legacy.rows.front().source_id), -9, adapter.revision()));
    EXPECT_TRUE(adapter.legacy());
    EXPECT_EQ(std::get<LegacyRows25Intent>(saved).rows.front().row.gain_db, -9);
    const auto before = saved;
    const auto revision = adapter.revision();
    EXPECT_FALSE(adapter.addEmptyTrack(revision));
    fail = true;
    EXPECT_FALSE(adapter.confirmLegacyConversion(revision));
    EXPECT_TRUE(adapter.conversionPending());
    EXPECT_EQ(saved, before);
    EXPECT_EQ(adapter.revision(), revision);
}

TrackGraph1Intent TwoMicrophones() {
    auto graph = ExampleGraph();
    auto& first = graph.sources.front();
    first.binding.kind = AudioBindingKind::MicrophoneEndpoint;
    first.dsp.hpf_enabled = true;
    first.dsp.hpf_cutoff_hz = 100;
    auto second = first;
    second.id = NewId<AudioSourceId>();
    second.name = first.name;
    graph.sources.push_back(second);
    AudioRouteSpec route;
    route.id = NewId<AudioRouteId>();
    route.source_id = second.id;
    route.track_id = graph.tracks.front().id;
    graph.routes.push_back(route);
    return graph;
}

TEST(AudioRoutingAdapter, SourceDetailsEditExplicitSecondIdentityAndReloadWithoutChangingOtherSourceOrRoutes) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    RecordingPresetStore store(directory.filePath("presets.toml"));
    const auto graph = TwoMicrophones();
    auto config = MakeDefaultPreset().config;
    config.audio_intent = graph;
    AudioRoutingAdapter adapter;
    adapter.configure(
        [&](const AudioIntent& candidate, QString& error) {
            auto next = config;
            next.audio_intent = candidate;
            if (!store.Save({}, std::string(kDefaultPresetId), next, &error))
                return false;
            config = std::move(next);
            return true;
        },
        Environment);
    adapter.refresh(graph);
    ASSERT_TRUE(adapter.setSourceDetails(Id(graph.sources[1].id),
                                         {{"hpfCutoffHz", 220.0},
                                          {"gateEnabled", true},
                                          {"gateThresholdDb", -31.0},
                                          {"agcEnabled", true},
                                          {"agcTargetDb", -14.0},
                                          {"rnnoiseEnabled", true},
                                          {"channelMode", int(MicChannelMode::MonoMix)},
                                          {"gainDb", 6.0},
                                          {"followDefault", true},
                                          {"endpointRole", int(AudioEndpointRole::Communications)}},
                                         adapter.revision()));
    const auto& edited = std::get<TrackGraph1Intent>(*config.audio_intent);
    EXPECT_EQ(edited.sources[0], graph.sources[0]);
    EXPECT_EQ(edited.sources[1].id, graph.sources[1].id);
    EXPECT_EQ(edited.sources[1].dsp.hpf_cutoff_hz, 220);
    EXPECT_EQ(edited.sources[1].dsp.gate_threshold_db, -31);
    EXPECT_EQ(edited.sources[1].dsp.agc_target_db, -14);
    EXPECT_TRUE(edited.sources[1].dsp.rnnoise_enabled);
    EXPECT_EQ(edited.sources[1].channel_mode, MicChannelMode::MonoMix);
    EXPECT_NEAR(edited.sources[1].post_dsp_gain_linear, 1.995262, 0.00001);
    EXPECT_TRUE(edited.sources[1].binding.follow_default);
    EXPECT_EQ(edited.sources[1].binding.endpoint_role, AudioEndpointRole::Communications);
    EXPECT_EQ(edited.routes, graph.routes);
    EXPECT_EQ(edited.tracks, graph.tracks);
    const auto loaded = store.Load();
    ASSERT_TRUE(loaded.live && loaded.live->audio_intent);
    EXPECT_EQ(*loaded.live->audio_intent, *config.audio_intent);
    const auto row = adapter.sources()[1].toMap();
    EXPECT_EQ(row.value("hpfCutoffHz").toDouble(), 220);
    EXPECT_NEAR(row.value("gainDb").toDouble(), 6.0, 0.00001);
    EXPECT_FALSE(row.value("sharedLegacyMicrophone").toBool());
    ASSERT_TRUE(adapter.setSourceDetails(
        Id(graph.sources[1].id), {{"endpointId", "another-microphone"}, {"followDefault", false}}, adapter.revision()));
    const auto& pinned = std::get<TrackGraph1Intent>(*config.audio_intent);
    EXPECT_EQ(pinned.sources[0], graph.sources[0]);
    EXPECT_EQ(pinned.sources[1].binding.endpoint_id, "another-microphone");
    EXPECT_FALSE(pinned.sources[1].binding.follow_default);
    EXPECT_EQ(pinned.routes, graph.routes);
    const auto reloaded = store.Load();
    ASSERT_TRUE(reloaded.live && reloaded.live->audio_intent);
    EXPECT_EQ(*reloaded.live->audio_intent, *config.audio_intent);
}

TEST(AudioRoutingAdapter, SourceDetailsRejectUnknownMalformedNonFiniteOutOfRangeStaleAndMissingIdentity) {
    const auto graph = TwoMicrophones();
    AudioIntent saved = graph;
    AudioRoutingAdapter adapter;
    adapter.configure(
        [&](const AudioIntent& candidate, QString&) {
            saved = candidate;
            return true;
        },
        Environment);
    adapter.refresh(graph);
    const auto before = adapter.sources();
    const auto revision = adapter.revision();
    const std::vector<QVariantMap> invalid{{{"gainDb", 1.0}, {"unknown", true}},
                                           {{"hpfEnabled", "true"}},
                                           {{"hpfCutoffHz", "80"}},
                                           {{"hpfCutoffHz", 19.0}},
                                           {{"hpfCutoffHz", 1001.0}},
                                           {{"gainDb", std::numeric_limits<double>::infinity()}},
                                           {{"gainDb", 25.0}},
                                           {{"gateThresholdDb", -81.0}},
                                           {{"agcTargetDb", -41.0}},
                                           {{"channelMode", 1.5}},
                                           {{"endpointRole", 3}},
                                           {{"endpointId", 42}},
                                           {{"followDefault", false}, {"endpointId", ""}},
                                           {{"bindingKind", 0}}};
    for (const auto& patch : invalid) {
        EXPECT_FALSE(adapter.setSourceDetails(Id(graph.sources[1].id), patch, revision));
        EXPECT_EQ(saved, AudioIntent{graph});
        EXPECT_EQ(adapter.revision(), revision);
        EXPECT_EQ(adapter.sources(), before);
    }
    EXPECT_FALSE(adapter.setSourceDetails(Id(graph.sources[1].id), {{"gainDb", 2.0}}, revision + 1));
    EXPECT_FALSE(adapter.setSourceDetails(Id(NewId<AudioSourceId>()), {{"gainDb", 2.0}}, revision));
    EXPECT_EQ(saved, AudioIntent{graph});
}

TEST(AudioRoutingAdapter, FailedSourceDetailsSaveDoesNotPublishParametersOrRevision) {
    const auto graph = TwoMicrophones();
    AudioRoutingAdapter adapter;
    adapter.configure(
        [](const AudioIntent&, QString& error) {
            error = "locked";
            return false;
        },
        Environment);
    adapter.refresh(graph);
    const auto before = adapter.sources();
    const auto revision = adapter.revision();
    EXPECT_FALSE(adapter.setSourceDetails(Id(graph.sources[1].id), {{"gainDb", 3.0}}, revision));
    EXPECT_EQ(adapter.lastError(), "locked");
    EXPECT_EQ(adapter.sources(), before);
    EXPECT_EQ(adapter.revision(), revision);
}

TEST(AudioRoutingAdapter, LegacyMicrophoneDetailsPreserveCapsuleAndExposeSharedScopeForBothRows) {
    auto legacy = Legacy();
    legacy.rows[0].row.kind = AudioSourceKind::Mic;
    auto second = legacy.rows.front();
    second.row_id = NewId<LegacyRowId>();
    second.source_id = NewId<AudioSourceId>();
    second.anchor_track_id = NewId<AudioTrackId>();
    second.row.enabled = false;
    legacy.rows.push_back(second);
    AudioIntent saved = legacy;
    AudioRoutingAdapter adapter;
    adapter.configure(
        [&](const AudioIntent& candidate, QString&) {
            saved = candidate;
            return true;
        },
        Environment);
    adapter.refresh(legacy);
    ASSERT_TRUE(adapter.setSourceDetails(Id(second.source_id),
                                         {{"hpfEnabled", true},
                                          {"hpfCutoffHz", 180.0},
                                          {"gainDb", -3.0},
                                          {"endpointId", "microphone-fixed"},
                                          {"followDefault", false}},
                                         adapter.revision()));
    ASSERT_TRUE(adapter.legacy());
    EXPECT_FALSE(adapter.conversionPending());
    const auto& edited = std::get<LegacyRows25Intent>(saved);
    EXPECT_EQ(edited.rows, legacy.rows);
    EXPECT_EQ(edited.id_namespace, legacy.id_namespace);
    EXPECT_EQ(edited.microphone_dsp.hpf_cutoff_hz, 180);
    EXPECT_EQ(edited.microphone_binding.endpoint_id, "microphone-fixed");
    EXPECT_FALSE(edited.microphone_binding.follow_default);
    EXPECT_NEAR(edited.microphone_gain_linear, 0.7079458, 0.00001);
    for (const auto& source : adapter.sources()) {
        EXPECT_TRUE(source.toMap().value("sharedLegacyMicrophone").toBool());
        EXPECT_EQ(source.toMap().value("hpfCutoffHz").toDouble(), 180);
    }
}

TEST(AudioRoutingAdapter, AddedSourcesHaveUniqueIdsStayUnassignedAndUndoRestoresIdentitySet) {
    auto graph = ExampleGraph();
    graph.default_monitor_track_ids.clear();
    AudioIntent saved = graph;
    AudioRoutingAdapter adapter;
    adapter.configure(
        [&](const AudioIntent& candidate, QString&) {
            saved = candidate;
            return true;
        },
        Environment);
    adapter.refresh(graph);
    ASSERT_TRUE(adapter.addSource(int(AudioBindingKind::MicrophoneEndpoint), "Same name", adapter.revision()));
    ASSERT_TRUE(adapter.addSource(int(AudioBindingKind::MicrophoneEndpoint), "Same name", adapter.revision()));
    const auto& edited = std::get<TrackGraph1Intent>(saved);
    ASSERT_EQ(edited.sources.size(), 3u);
    EXPECT_NE(edited.sources[1].id, edited.sources[2].id);
    EXPECT_EQ(edited.sources[1].binding.kind, AudioBindingKind::MicrophoneEndpoint);
    EXPECT_TRUE(edited.sources[1].binding.follow_default);
    EXPECT_EQ(edited.sources[1].binding.endpoint_role, AudioEndpointRole::Multimedia);
    EXPECT_EQ(edited.routes, graph.routes);
    EXPECT_EQ(edited.tracks, graph.tracks);
    EXPECT_TRUE(edited.default_monitor_track_ids.empty());
    EXPECT_TRUE(adapter.sources()[1].toMap().value("assignedTrackIds").toStringList().empty());
    ASSERT_TRUE(adapter.undo(adapter.revision()));
    EXPECT_EQ(std::get<TrackGraph1Intent>(saved).sources.size(), 2u);
    ASSERT_TRUE(adapter.undo(adapter.revision()));
    EXPECT_EQ(saved, AudioIntent{graph});
}

TEST(AudioRoutingAdapter, CreateAndAssignPublishesOnceAndFailedSaveLeavesNoDefinition) {
    const auto graph = ExampleGraph();
    AudioIntent saved = graph;
    bool reject = true;
    int saves = 0;
    AudioRoutingAdapter adapter;
    adapter.configure(
        [&](const AudioIntent& candidate, QString& error) {
            ++saves;
            if (reject) {
                error = "locked";
                return false;
            }
            saved = candidate;
            return true;
        },
        Environment);
    adapter.refresh(graph);
    const auto create = [&](const QString& track, qulonglong revision) {
        bool result = false;
        const bool invoked = QMetaObject::invokeMethod(&adapter, "addSourceToTrack", Q_RETURN_ARG(bool, result),
                                                       Q_ARG(int, 0), Q_ARG(QString, QStringLiteral("New system")),
                                                       Q_ARG(QString, track), Q_ARG(qulonglong, revision));
        EXPECT_TRUE(invoked);
        return result;
    };
    const auto revision = adapter.revision();
    EXPECT_FALSE(create(Id(graph.tracks[0].id), revision));
    EXPECT_EQ(saves, 1);
    EXPECT_EQ(saved, AudioIntent{graph});
    EXPECT_EQ(adapter.revision(), revision);
    reject = false;
    ASSERT_TRUE(create(Id(graph.tracks[0].id), revision));
    EXPECT_EQ(saves, 2);
    const auto& edited = std::get<TrackGraph1Intent>(saved);
    ASSERT_EQ(edited.sources.size(), graph.sources.size() + 1);
    ASSERT_EQ(edited.routes.size(), graph.routes.size() + 1);
    EXPECT_EQ(edited.routes.back().source_id, edited.sources.back().id);
    EXPECT_EQ(edited.routes.back().track_id, graph.tracks[0].id);
    const auto after = saved;
    EXPECT_FALSE(create(Id(NewId<AudioTrackId>()), adapter.revision()));
    EXPECT_EQ(saved, after);
    EXPECT_EQ(saves, 2);
    EXPECT_FALSE(create(Id(graph.tracks[0].id), revision));
    EXPECT_EQ(saved, after);
}

TEST(AudioRoutingAdapter, AutomaticPlaybackSelectionExposesEffectiveRecordedTracks) {
    auto graph = ExampleGraph();
    graph.default_monitor_track_ids.clear();
    auto second = graph.routes.front();
    second.id = NewId<AudioRouteId>();
    second.track_id = graph.tracks.back().id;
    graph.routes.push_back(second);
    AudioRoutingAdapter adapter;
    adapter.configure([](const AudioIntent&, QString&) { return true; }, Environment);
    adapter.refresh(graph);
    EXPECT_TRUE(adapter.defaultMonitorTrackIds().empty());
    EXPECT_EQ(adapter.property("effectiveMonitorTrackIds").toStringList(),
              (QStringList{Id(graph.tracks[0].id), Id(graph.tracks[1].id)}));
    EXPECT_TRUE(adapter.monitorConfirmationRequired());
    ASSERT_TRUE(adapter.setDefaultMonitorSelection({Id(graph.tracks[1].id)}, adapter.revision()));
    EXPECT_EQ(adapter.property("effectiveMonitorTrackIds").toStringList(), (QStringList{Id(graph.tracks[1].id)}));
    EXPECT_EQ(adapter.monitorSelectionOrigin(), "user");
    EXPECT_FALSE(adapter.monitorConfirmationRequired());
}

TEST(AudioRoutingAdapter, SourceCreationChoicesReflectSupportWithoutOfferingIndependentPicker) {
    auto environment = Environment();
    AudioRoutingAdapter adapter;
    adapter.configure([](const AudioIntent&, QString&) { return true; }, [&] { return environment; });
    adapter.refresh(ExampleGraph());
    auto options = adapter.property("sourceCreationOptions").toList();
    ASSERT_EQ(options.size(), 2);
    EXPECT_EQ(options[0].toMap().value("kind").toInt(), 0);
    EXPECT_EQ(options[1].toMap().value("kind").toInt(), 1);
    environment.process_loopback_supported = true;
    options = adapter.property("sourceCreationOptions").toList();
    ASSERT_EQ(options.size(), 3);
    EXPECT_EQ(options[2].toMap().value("kind").toInt(), 2);
    EXPECT_FALSE(options[2].toMap().value("selectable").toBool());
    EXPECT_FALSE(options[2].toMap().value("reason").toString().isEmpty());
    environment.window_target = true;
    environment.capture_target_process = ProcessInstanceIdentity{23, 1000, "chosen-app"};
    environment.confirmed_processes = {*environment.capture_target_process};
    EXPECT_TRUE(adapter.property("sourceCreationOptions").toList()[2].toMap().value("selectable").toBool());
}

TEST(AudioRoutingAdapter, NumericSourceDefaultsComeFromEngineDefaults) {
    AudioRoutingAdapter adapter;
    const auto defaults = adapter.property("sourceDetailDefaults").toMap();
    const MicDspConfig dsp;
    EXPECT_EQ(defaults.value("gainDb").toDouble(), 0.0);
    EXPECT_EQ(defaults.value("hpfCutoffHz").toDouble(), dsp.hpf_cutoff_hz);
    EXPECT_EQ(defaults.value("gateThresholdDb").toDouble(), dsp.gate_threshold_db);
    EXPECT_EQ(defaults.value("agcTargetDb").toDouble(), dsp.agc_target_db);
}

TEST(AudioRoutingAdapter, AddingProcessSourceRequiresExplicitConfirmedCurrentWindowIdentity) {
    auto snapshot = Environment();
    const auto graph = ExampleGraph();
    AudioIntent saved = graph;
    AudioRoutingAdapter adapter;
    adapter.configure(
        [&](const AudioIntent& candidate, QString&) {
            saved = candidate;
            return true;
        },
        [&]() { return snapshot; });
    adapter.refresh(graph);
    const auto revision = adapter.revision();
    EXPECT_FALSE(adapter.addSource(int(AudioBindingKind::CaptureTargetProcess), "App", revision));
    snapshot.window_target = true;
    snapshot.process_loopback_supported = true;
    snapshot.capture_target_process = ProcessInstanceIdentity{23, 1000, "chosen-app"};
    EXPECT_FALSE(adapter.addSource(int(AudioBindingKind::IndependentProcess), "App", revision));
    snapshot.confirmed_processes = {{23, 999, "chosen-app"}};
    EXPECT_FALSE(adapter.addSource(int(AudioBindingKind::IndependentProcess), "App", revision));
    snapshot.confirmed_processes = {*snapshot.capture_target_process};
    ASSERT_TRUE(adapter.addSource(int(AudioBindingKind::IndependentProcess), "App", revision));
    const auto& source = std::get<TrackGraph1Intent>(saved).sources.back();
    EXPECT_EQ(source.binding.process, snapshot.capture_target_process);
    EXPECT_EQ(source.binding.kind, AudioBindingKind::IndependentProcess);
    EXPECT_EQ(std::get<TrackGraph1Intent>(saved).routes, graph.routes);
}

TEST(AudioRoutingAdapter, LegacySourceAddCancelKeepsCapsuleAndConversionFreezesSelectedProcessIdentity) {
    const auto legacy = Legacy();
    auto snapshot = Environment();
    snapshot.window_target = true;
    snapshot.process_loopback_supported = true;
    const ProcessInstanceIdentity selected{23, 1000, "chosen-app"};
    snapshot.capture_target_process = selected;
    snapshot.confirmed_processes = {selected};
    AudioIntent saved = legacy;
    AudioRoutingAdapter adapter;
    adapter.configure(
        [&](const AudioIntent& candidate, QString&) {
            saved = candidate;
            return true;
        },
        [&]() { return snapshot; });
    adapter.refresh(legacy);
    const auto revision = adapter.revision();
    EXPECT_FALSE(adapter.addSource(int(AudioBindingKind::IndependentProcess), "App", revision));
    ASSERT_TRUE(adapter.conversionPending());
    ASSERT_TRUE(adapter.cancelLegacyConversion(revision));
    EXPECT_EQ(saved, AudioIntent{legacy});
    EXPECT_FALSE(adapter.addSource(int(AudioBindingKind::IndependentProcess), "App", revision));
    snapshot.capture_target_process = ProcessInstanceIdentity{23, 2000, "replacement-app"};
    snapshot.confirmed_processes = {*snapshot.capture_target_process};
    ASSERT_TRUE(adapter.confirmLegacyConversion(revision));
    const auto& graph = std::get<TrackGraph1Intent>(saved);
    ASSERT_EQ(graph.sources.size(), 2u);
    EXPECT_EQ(graph.sources.back().binding.process, std::optional{selected});
    EXPECT_EQ(graph.routes.size(), 1u);
    EXPECT_NE(graph.sources.front().id, graph.sources.back().id);
    ASSERT_TRUE(adapter.undo(adapter.revision()));
    EXPECT_EQ(saved, AudioIntent{legacy});
}

TEST(AudioRoutingAdapter, SourceAddRejectsInvalidNamesKindsBudgetsStaleRevisionAndFailedSave) {
    const auto graph = ExampleGraph();
    bool reject = false;
    AudioIntent saved = graph;
    AudioRoutingAdapter adapter;
    adapter.configure(
        [&](const AudioIntent& candidate, QString& error) {
            if (reject) {
                error = "locked";
                return false;
            }
            saved = candidate;
            return true;
        },
        Environment);
    adapter.refresh(graph);
    const auto revision = adapter.revision();
    EXPECT_FALSE(adapter.addSource(int(AudioBindingKind::LegacyExcludeTarget), "Source", revision));
    EXPECT_FALSE(adapter.addSource(0, " ", revision));
    EXPECT_FALSE(adapter.addSource(0, QString(129, QLatin1Char('x')), revision));
    EXPECT_FALSE(adapter.addSource(0, "Bad\nname", revision));
    EXPECT_FALSE(adapter.addSource(0, "Source", revision + 1));
    reject = true;
    EXPECT_FALSE(adapter.addSource(0, "Source", revision));
    EXPECT_EQ(adapter.revision(), revision);
    EXPECT_EQ(saved, AudioIntent{graph});
    reject = false;
    for (size_t index = 1; index < kMaxAudioSources; ++index)
        ASSERT_TRUE(adapter.addSource(0, "Source", adapter.revision()));
    const auto full = saved;
    const auto full_revision = adapter.revision();
    EXPECT_FALSE(adapter.addSource(0, "Too many", full_revision));
    EXPECT_EQ(saved, full);
    EXPECT_EQ(adapter.revision(), full_revision);
}

TEST(AudioRoutingAdapter, ResourceDiagnosticsDeduplicatesOwnersAndNeverUsesNextDraftOrPrivateBindings) {
    AudioRoutingAdapter adapter;
    ResolvedAudioPlan active;
    ResolvedAudioSource source;
    source.spec.id = NewId<AudioSourceId>();
    source.spec.name = "private source title";
    source.spec.binding.endpoint_id = "private endpoint path";
    active.sources.push_back(source);
    active.sources.push_back(source);
    ResolvedRoutingTrack track;
    track.id = NewId<AudioTrackId>();
    active.tracks.push_back(track);
    active.tracks.push_back(track);
    adapter.setActivePlan(active);
    PhysicalMeterSnapshot physical;
    physical.clock_id = NewId<PhysicalClockId>();
    physical.reader_count = 2;
    physical.available = true;
    physical.processing_time_available = true;
    physical.processing_ms = 12.25;
    SourceMeterSnapshot meter;
    meter.source_id = source.spec.id;
    meter.clock_id = physical.clock_id;
    meter.has_measurement = true;
    meter.rms = 0;
    meter.peak = 0;
    meter.processing_time_available = true;
    meter.processing_ms = 4.5;
    TrackMeterSnapshot output;
    output.track_id = track.id;
    output.has_measurement = true;
    adapter.setMeters({meter}, {output}, {physical, physical});
    const auto resources = adapter.resourceDiagnostics();
    EXPECT_EQ(resources.value("physical").toList().size(), 1);
    EXPECT_EQ(resources.value("sources").toList().size(), 1);
    EXPECT_EQ(resources.value("tracks").toList().size(), 1);
    const auto logical = resources.value("sources").toList().first().toMap();
    EXPECT_EQ(logical.value("identity").toString(), Id(source.spec.id));
    const auto metrics = logical.value("metrics").toList();
    bool measured_silence = false;
    for (const auto& value : metrics) {
        const auto item = value.toMap();
        if (item.value("label").toString() == AudioRoutingAdapter::tr("Source RMS (dBFS)")) {
            EXPECT_TRUE(item.value("available").toBool());
            EXPECT_TRUE(item.value("value").toString().contains("inf"));
            measured_silence = true;
        }
    }
    EXPECT_TRUE(measured_silence);
    auto next = ExampleGraph();
    adapter.refresh(next, true);
    EXPECT_EQ(adapter.resourceDiagnostics(), resources);
    const auto serialized = QJsonDocument(QJsonObject::fromVariantMap(resources)).toJson();
    EXPECT_FALSE(serialized.contains("private source title"));
    EXPECT_FALSE(serialized.contains("private endpoint path"));
    adapter.setActivePlan(std::nullopt);
    EXPECT_TRUE(adapter.resourceDiagnostics().value("sources").toList().isEmpty());
    EXPECT_TRUE(adapter.resourceDiagnostics().value("physical").toList().isEmpty());
}

TEST(AudioRoutingAdapter, ResourceDiagnosticsDistinguishesNoSampleAndUnmeasuredCostFromZero) {
    AudioRoutingAdapter adapter;
    ResolvedAudioPlan active;
    ResolvedAudioSource source;
    source.spec.id = NewId<AudioSourceId>();
    active.sources.push_back(source);
    adapter.setActivePlan(active);
    auto resources = adapter.resourceDiagnostics();
    const auto absent = resources.value("sources").toList().first().toMap().value("metrics").toList();
    for (const auto& value : absent) {
        const auto metric = value.toMap();
        EXPECT_FALSE(metric.value("available").toBool());
        EXPECT_EQ(metric.value("value").toString(), AudioRoutingAdapter::tr("n/a"));
        EXPECT_FALSE(metric.value("reason").toString().isEmpty());
    }
    SourceMeterSnapshot silent;
    silent.source_id = source.spec.id;
    silent.has_measurement = true;
    adapter.setMeters({silent}, {});
    const auto measured =
        adapter.resourceDiagnostics().value("sources").toList().first().toMap().value("metrics").toList();
    for (const auto& value : measured) {
        const auto metric = value.toMap();
        if (metric.value("label").toString() == AudioRoutingAdapter::tr("Source DSP elapsed (ms)"))
            EXPECT_FALSE(metric.value("available").toBool());
        if (metric.value("label").toString() == AudioRoutingAdapter::tr("Gap frames")) {
            EXPECT_TRUE(metric.value("available").toBool());
            EXPECT_EQ(metric.value("value").toString(), "0");
        }
    }
}
} // namespace
} // namespace exosnap::quick
