// Test runner for the QML contracts of the Record source picker and the
// transport dock.
//
// A separate executable from record_controls_qml_tests for the same reason the
// edit runner exists: both take a REQUIRED, C++-typed adapter
// (RecordViewModelAdapter, QML_UNCREATABLE like every application-provided
// adapter), so neither can be stubbed from QML. The runner owns one real adapter
// over a seeded RecordViewModel and exposes it, plus a seed entry point that
// rebuilds the target list through the same revision bump a rescan uses.

#include "AudioRoutingAdapter.h"
#include "CaptureExclusion.h"
#include "RecordViewModelAdapter.h"
#include "RecordingClipAdapter.h"
#include "RecordingWorkspaceState.h"
#include "SettingsAdapter.h"

#include "viewmodels/RecordViewModel.h"

#include <QCoreApplication>
#include <QObject>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQuickTest>

#include <cstdint>
#include <memory>

using exosnap::RecordViewModel;
using exosnap::quick::RecordingClipAdapter;
using exosnap::quick::RecordViewModelAdapter;

class RecordPickerTestDriver final : public QObject {
    Q_OBJECT

    Q_PROPERTY(exosnap::quick::RecordViewModelAdapter* adapter READ adapter CONSTANT)
    Q_PROPERTY(exosnap::quick::RecordingClipAdapter* clipAdapter READ clipAdapter CONSTANT)
    Q_PROPERTY(exosnap::quick::RecordingWorkspaceState* workspaceState READ workspaceState CONSTANT)
    Q_PROPERTY(exosnap::quick::SettingsAdapter* settings READ settings CONSTANT)

  public:
    explicit RecordPickerTestDriver(QObject* parent = nullptr) : QObject(parent) {
        adapter_.setTargetConfirmation([this](const QString& identity, int mode) {
            for (int index = 0; index < static_cast<int>(source_.targets.size()); ++index) {
                const auto& target = source_.targets[static_cast<size_t>(index)];
                if (adapter_.targetIdentity(target) == identity &&
                    (mode == static_cast<int>(exosnap::CaptureMode::Window)) ==
                        (target.kind == exosnap::engine::CaptureTarget::Kind::Window)) {
                    adapter_.requestSelectTarget(index, mode);
                    return true;
                }
            }
            return false;
        });
        settings_.setAudioRouting(&routing_);
        settings_.setConfig(exosnap::MakeDefaultPreset().config);
        seedTargets(2, {QStringLiteral("Claude Design - Brave"), QStringLiteral("Task Manager")});
    }

    [[nodiscard]] RecordViewModelAdapter* adapter() {
        return &adapter_;
    }

    [[nodiscard]] RecordingClipAdapter* clipAdapter() {
        return &clip_adapter_;
    }
    exosnap::quick::RecordingWorkspaceState* workspaceState() {
        return &workspace_state_;
    }
    exosnap::quick::SettingsAdapter* settings() {
        return &settings_;
    }

    Q_INVOKABLE void resetPickerState() {
        adapter_.setHiddenWindowRules({});
        adapter_.setShowHiddenWindows(false);
        ++seed_generation_;
    }
    Q_INVOKABLE void forceCaptureExclusionFailure() {
        capture_exclusion_attempts_ = 0;
        exosnap::quick::CaptureExclusion::SetAffinityFunctionForTest([this](void*, quint32) {
            ++capture_exclusion_attempts_;
            return false;
        });
    }
    Q_INVOKABLE int captureExclusionAttempts() const {
        return capture_exclusion_attempts_;
    }
    Q_INVOKABLE void resetCaptureExclusion() {
        exosnap::quick::CaptureExclusion::ResetAffinityFunctionForTest();
    }
    Q_INVOKABLE void prepareRegionSelection() {
        source_.state = exosnap::UiRecordingState::RegionSelecting;
        adapter_.setSource(&source_);
        adapter_.setRegionState(QRectF(0, 0, 1, 1), true);
    }
    void updateWindowIdentities(const QVariantList& rows = {}) {
        QHash<quintptr, QString> identities;
        QHash<quintptr, QString> applications;
        for (const auto& target : source_.targets) {
            if (target.kind != exosnap::engine::CaptureTarget::Kind::Window)
                continue;
            quint64 process = 10;
            quint64 creation = 20;
            quint64 generation = seed_generation_;
            for (const auto& value : rows) {
                const auto row = value.toMap();
                if (row.value(QStringLiteral("id")).toULongLong() != target.native_id)
                    continue;
                process = row.value(QStringLiteral("pid"), 10).toULongLong();
                creation = row.value(QStringLiteral("creation"), 20).toULongLong();
                generation = row.value(QStringLiteral("generation"), seed_generation_).toULongLong();
            }
            identities.insert(
                target.native_id,
                QStringLiteral("window:%1:%2:%3:%4").arg(target.native_id).arg(process).arg(creation).arg(generation));
            applications.insert(target.native_id, QStringLiteral("application:%1").arg(process));
        }
        adapter_.setWindowIdentities(std::move(identities), std::move(applications));
    }
    Q_INVOKABLE void seedTargets(int display_count, const QStringList& window_labels) {
        source_.targets.clear();
        for (int i = 0; i < display_count; ++i) {
            source_.targets.push_back({exosnap::engine::CaptureTarget::Kind::Monitor, static_cast<std::uint64_t>(i + 1),
                                       "\\\\.\\DISPLAY" + std::to_string(i + 1)});
        }
        std::uint64_t window_id = 100;
        for (const QString& label : window_labels) {
            source_.targets.push_back({exosnap::engine::CaptureTarget::Kind::Window, window_id++, label.toStdString()});
        }
        source_.selected_target_index = 0;
        source_.capture_mode = exosnap::CaptureMode::Monitor;
        ++source_.targets_revision;
        updateWindowIdentities();
        adapter_.setSource(&source_);
    }

    // A rescan that keeps the native handles of the windows the caller names,
    // so a test can prove the pending selection follows its identity through an
    // insert or a reorder instead of the index it happened to have.
    Q_INVOKABLE void setRecordingContext(bool active) {
        settings_.setControlsLocked(active);
    }

    Q_INVOKABLE void seedLegacyAudio() {
        routing_.setActivePlan(std::nullopt);
        routing_.refresh(exosnap::engine::LegacyRows25Intent{});
    }

    Q_INVOKABLE void seedAudioSources(int count, const QString& prefix) {
        using namespace exosnap::engine;
        TrackGraph1Intent graph;
        AudioTrackSpec track;
        track.id = NewId<AudioTrackId>();
        graph.tracks.push_back(track);
        for (int index = 0; index < count; ++index) {
            AudioSourceSpec source;
            source.id = NewId<AudioSourceId>();
            source.name = (prefix + QString::number(index)).toStdString();
            source.binding.follow_default = false;
            source.binding.endpoint_id = "endpoint";
            graph.sources.push_back(source);
            AudioRouteSpec route;
            route.id = NewId<AudioRouteId>();
            route.source_id = source.id;
            route.track_id = track.id;
            graph.routes.push_back(route);
        }
        AudioEnvironmentSnapshot environment;
        environment.playback_endpoints = {"endpoint"};
        environment.default_playback_endpoint = "endpoint";
        routing_.configure([](const AudioIntent&, QString&) { return true; }, [environment] { return environment; },
                           [](AudioSourceId, bool) { return true; });
        routing_.setActivePlan(std::nullopt);
        routing_.refresh(graph);
        if (count > 0) {
            const auto resolved = ResolveAudioIntent(graph, environment);
            if (resolved.succeeded())
                routing_.setActivePlan(*resolved.value());
        }
    }
    Q_INVOKABLE void seedWindowTargets(const QVariantList& windows) {
        source_.targets.clear();
        for (const QVariant& value : windows) {
            const QVariantMap row = value.toMap();
            source_.targets.push_back({exosnap::engine::CaptureTarget::Kind::Window,
                                       row.value(QStringLiteral("id")).toULongLong(),
                                       row.value(QStringLiteral("label")).toString().toStdString()});
        }
        if (source_.selected_target_index >= static_cast<int>(source_.targets.size()))
            source_.selected_target_index = source_.targets.empty() ? -1 : 0;
        ++source_.targets_revision;
        updateWindowIdentities(windows);
        adapter_.setSource(&source_);
    }

    // "ready", "recording", "paused", "completed" (a successful run), "failed",
    // or anything else for the view model's initial LoadingCapabilities state.
    // The finished states carry the run's duration, so a test can tell whether
    // the transport still shows it.
    Q_INVOKABLE void setRecordingState(const QString& state) {
        if (state == QLatin1String("ready"))
            source_.state = exosnap::UiRecordingState::Ready;
        else if (state == QLatin1String("recording"))
            source_.state = exosnap::UiRecordingState::Recording;
        else if (state == QLatin1String("paused"))
            source_.state = exosnap::UiRecordingState::Paused;
        else if (state == QLatin1String("completed"))
            source_.state = exosnap::UiRecordingState::Completed;
        else if (state == QLatin1String("failed"))
            source_.state = exosnap::UiRecordingState::Failed;
        else
            source_.state = exosnap::UiRecordingState::LoadingCapabilities;
        source_.last_succeeded = state == QLatin1String("completed");
        source_.elapsed_text = state == QLatin1String("paused")      ? L"12:34"
                               : state == QLatin1String("completed") ? L"1:23"
                               : state == QLatin1String("failed")    ? L"0:42"
                                                                     : L"";
        updateWindowIdentities();
        adapter_.setSource(&source_);
    }

    // The still service is a C++ collaborator the QML test does not have, so
    // the two edges it drives are reachable from the test instead.
    Q_INVOKABLE void deliverStill(const QString& identity, const QString& source) {
        adapter_.setTargetStill(identity, source);
    }

    Q_INVOKABLE void failStill(const QString& identity) {
        adapter_.setTargetStillUnavailable(identity);
    }

  private:
    int capture_exclusion_attempts_ = 0;
    quint64 seed_generation_ = 1;
    RecordViewModel source_;
    RecordViewModelAdapter adapter_;
    RecordingClipAdapter clip_adapter_;
    exosnap::quick::RecordingWorkspaceState workspace_state_;
    exosnap::quick::SettingsAdapter settings_;
    exosnap::quick::AudioRoutingAdapter routing_;
};

class Setup final : public QObject {
    Q_OBJECT

  public slots:
    void applicationAvailable() {
        QCoreApplication::setOrganizationName(QStringLiteral("ExoSnap"));
        QCoreApplication::setOrganizationDomain(QStringLiteral("exosnap.example"));
        QCoreApplication::setApplicationName(QStringLiteral("record-source-picker-qml-tests"));
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        driver_ = std::make_unique<RecordPickerTestDriver>();
    }

    void qmlEngineAvailable(QQmlEngine* engine) {
        engine->rootContext()->setContextProperty(QStringLiteral("recordDriver"), driver_.get());
    }

  private:
    std::unique_ptr<RecordPickerTestDriver> driver_;
};

QUICK_TEST_MAIN_WITH_SETUP(record_source_picker, Setup)

#include "record_picker_qml_test_main.moc"
