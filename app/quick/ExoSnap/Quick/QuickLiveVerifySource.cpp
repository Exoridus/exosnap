#include "QuickLiveVerifySource.h"

#include "AboutViewModelAdapter.h"
#include "BlockingSurfaceArbiter.h"
#include "CrashReportAdapter.h"
#include "DeviceAdapter.h"
#include "DiagnosticsAdapter.h"
#include "EditExportAdapter.h"
#include "EditPlayerAdapter.h"
#include "EditSessionAdapter.h"
#include "NotificationEntryModel.h"
#include "NotificationsAdapter.h"
#include "QuickApplication.h"
#include "RecordPreviewAdapter.h"
#include "RecordViewModelAdapter.h"
#include "RecordingErrorAdapter.h"
#include "RecoveryAdapter.h"
#include "SettingsAdapter.h"
#include "SettingsAutomationKeys.h"
#include "ShellAdapter.h"

#include "ExoSnapBuildInfo.h"

#include "diagnostics/NativeWindowFacts.h"
#include "models/AboutInfo.h"
#include "observability/DiagnosticsResultsJson.h"
#include "observability/EnvironmentSnapshot.h"
#include "observability/EventQuery.h"
#include "observability/PipelineSnapshotJson.h"
#include "observability/SessionQuery.h"
#include "observability/SettingsSnapshot.h"
#include "observability/WindowIdentity.h"
#include "services/RecordingCoordinator.h"
#include "services/UpdateService.h"

#include "ui/CodecLabels.h"
#include <control/options.h>
#include <update_handoff/handoff.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonArray>
#include <QMetaObject>
#include <QQuickWindow>
#include <QRect>
#include <QScreen>
#include <QVariant>

#include <optional>
#include <utility>

#include <windows.h>

namespace exosnap::quick {
namespace {

using live_verify::AutomationState;

// The UiRecordingState enumerator's own name. A switch rather than a table so
// adding a state without naming it is a compiler warning; the wire has always
// carried the integer, and an integer whose meaning is an enumerator ORDER is
// not a protocol.
QString RecordingStateName(UiRecordingState state) {
    switch (state) {
    case UiRecordingState::LoadingCapabilities:
        return QStringLiteral("LoadingCapabilities");
    case UiRecordingState::Ready:
        return QStringLiteral("Ready");
    case UiRecordingState::Blocked:
        return QStringLiteral("Blocked");
    case UiRecordingState::Countdown:
        return QStringLiteral("Countdown");
    case UiRecordingState::Preparing:
        return QStringLiteral("Preparing");
    case UiRecordingState::RegionSelecting:
        return QStringLiteral("RegionSelecting");
    case UiRecordingState::Recording:
        return QStringLiteral("Recording");
    case UiRecordingState::Paused:
        return QStringLiteral("Paused");
    case UiRecordingState::ArmedFromRecovery:
        return QStringLiteral("ArmedFromRecovery");
    case UiRecordingState::Stopping:
        return QStringLiteral("Stopping");
    case UiRecordingState::Saving:
        return QStringLiteral("Saving");
    case UiRecordingState::Completed:
        return QStringLiteral("Completed");
    case UiRecordingState::Failed:
        return QStringLiteral("Failed");
    }
    return QStringLiteral("Unknown");
}

QString PageName(ShellAdapter::Page page) {
    switch (page) {
    case ShellAdapter::RecordPage:
        return QString::fromLatin1(live_verify::page_name::kRecord);
    case ShellAdapter::SettingsPage:
        return QString::fromLatin1(live_verify::page_name::kSettings);
    case ShellAdapter::DiagnosticsPage:
        return QString::fromLatin1(live_verify::page_name::kDiagnostics);
    case ShellAdapter::LogsPage:
        return QString::fromLatin1(live_verify::page_name::kLogs);
    case ShellAdapter::AboutPage:
        return QString::fromLatin1(live_verify::page_name::kAbout);
    }
    return QString::fromLatin1(live_verify::page_name::kRecord);
}

std::optional<ShellAdapter::Page> PageFromName(const QString& name) {
    if (name == QLatin1String(live_verify::page_name::kRecord))
        return ShellAdapter::RecordPage;
    if (name == QLatin1String(live_verify::page_name::kSettings))
        return ShellAdapter::SettingsPage;
    if (name == QLatin1String(live_verify::page_name::kDiagnostics))
        return ShellAdapter::DiagnosticsPage;
    if (name == QLatin1String(live_verify::page_name::kLogs))
        return ShellAdapter::LogsPage;
    if (name == QLatin1String(live_verify::page_name::kAbout))
        return ShellAdapter::AboutPage;
    return std::nullopt;
}

// The export panel's own lifecycle, by name. A switch rather than a table so
// adding a state without naming it is a compiler warning, and named rather than
// the integer for the same reason the recording state is: an enumerator's ORDER
// is not a protocol.
QString ExportStateName(EditExportAdapter::State state) {
    switch (state) {
    case EditExportAdapter::Options:
        return QStringLiteral("idle");
    case EditExportAdapter::Running:
        return QStringLiteral("exporting");
    case EditExportAdapter::Cancelling:
        return QStringLiteral("cancelling");
    case EditExportAdapter::Done:
        return QStringLiteral("completed");
    case EditExportAdapter::Failed:
        return QStringLiteral("failed");
    }
    return QStringLiteral("idle");
}

QString BlockingSurfaceName(BlockingSurfaceArbiter::Surface surface) {
    switch (surface) {
    case BlockingSurfaceArbiter::Surface::Recovery:
        return QString::fromLatin1(live_verify::blocking_surface_name::kRecovery);
    case BlockingSurfaceArbiter::Surface::Crash:
        return QString::fromLatin1(live_verify::blocking_surface_name::kCrashReport);
    case BlockingSurfaceArbiter::Surface::RecordingError:
        return QString::fromLatin1(live_verify::blocking_surface_name::kRecordingError);
    }
    return {};
}

// The page documents' object names. Constant, and the ONLY strings that ever
// reach findChild(): the wire carries a product surface name, this maps it, and
// a client-supplied string is never looked up.
QString PageObjectName(const QString& surface) {
    if (surface == QLatin1String(live_verify::page_name::kSettings))
        return QStringLiteral("quickSettingsPage");
    if (surface == QLatin1String(live_verify::page_name::kDiagnostics))
        return QStringLiteral("quickDiagnosticsPage");
    if (surface == QLatin1String(live_verify::page_name::kLogs))
        return QStringLiteral("quickLogsPage");
    return {};
}

QString ToQString(const std::wstring& value) {
    return QString::fromStdWString(value);
}

QJsonObject ScreenJson(const QScreen* screen) {
    QJsonObject json;
    if (screen == nullptr)
        return json;
    const QRect geometry = screen->geometry();
    json.insert(QStringLiteral("name"), screen->name());
    json.insert(QStringLiteral("x"), geometry.x());
    json.insert(QStringLiteral("y"), geometry.y());
    json.insert(QStringLiteral("width"), geometry.width());
    json.insert(QStringLiteral("height"), geometry.height());
    json.insert(QStringLiteral("devicePixelRatio"), screen->devicePixelRatio());
    json.insert(QStringLiteral("refreshHz"), screen->refreshRate());
    return json;
}

QJsonObject NativeFactsJson(const diagnostics::NativeWindowFacts& facts) {
    QJsonObject json;
    json.insert(QStringLiteral("valid"), facts.valid);
    json.insert(QStringLiteral("style"), QStringLiteral("0x%1").arg(facts.style, 8, 16, QLatin1Char('0')));
    json.insert(QStringLiteral("exStyle"), QStringLiteral("0x%1").arg(facts.ex_style, 8, 16, QLatin1Char('0')));
    json.insert(QStringLiteral("layered"), facts.layered);
    json.insert(QStringLiteral("transparentForInput"), facts.transparent_for_input);
    json.insert(QStringLiteral("childHwnds"), facts.child_hwnds);
    json.insert(QStringLiteral("nonClientInsetLeft"), facts.inset.left);
    json.insert(QStringLiteral("nonClientInsetTop"), facts.inset.top);
    json.insert(QStringLiteral("nonClientInsetRight"), facts.inset.right);
    json.insert(QStringLiteral("nonClientInsetBottom"), facts.inset.bottom);
    json.insert(QStringLiteral("nativeTitlebar"), facts.inset.top > 0);
    json.insert(QStringLiteral("displayAffinityKnown"), facts.affinity_known);
    json.insert(QStringLiteral("displayAffinity"), static_cast<int>(facts.display_affinity));
    json.insert(QStringLiteral("captureExcluded"),
                facts.affinity_known && facts.display_affinity == 0x00000011 /* WDA_EXCLUDEFROMCAPTURE */);
    json.insert(QStringLiteral("x"), facts.x);
    json.insert(QStringLiteral("y"), facts.y);
    json.insert(QStringLiteral("width"), facts.width);
    json.insert(QStringLiteral("height"), facts.height);
    return json;
}

} // namespace

QuickLiveVerifySource::QuickLiveVerifySource(QuickApplication& application, QQuickWindow* root_window, QObject* parent)
    : QObject(parent), application_(application), root_window_(root_window) {
    // Exactly the signals that can move a field of AutomationState. The list is
    // allowed to be generous -- refreshObservableState() diffs before it
    // advances anything -- but not short: a field whose signal is missing here
    // would change without the revision moving, and a runner waiting on the
    // revision would wait forever.
    const auto refresh = [this]() { refreshObservableState(); };
    if (auto* record = application_.recordViewModelAdapter())
        connect(record, &RecordViewModelAdapter::changed, this, refresh);
    if (auto* session = application_.editSessionAdapter()) {
        connect(session, &EditSessionAdapter::openChanged, this, refresh);
        connect(session, &EditSessionAdapter::clipChanged, this, refresh);
        connect(session, &EditSessionAdapter::exportRunningChanged, this, refresh);
    }
    if (auto* player = application_.editPlayerAdapter()) {
        connect(player, &EditPlayerAdapter::playingChanged, this, refresh);
        connect(player, &EditPlayerAdapter::clipOpenChanged, this, refresh);
    }
    if (auto* shell = application_.shellAdapter()) {
        connect(shell, &ShellAdapter::currentPageChanged, this, refresh);
        connect(shell, &ShellAdapter::editSurfaceVisibleChanged, this, refresh);
        connect(shell, &ShellAdapter::sourcePickerOpenChanged, this, refresh);
    }
    if (auto* notifications = application_.notificationsAdapter()) {
        connect(notifications, &NotificationsAdapter::hubOpenChanged, this, refresh);
        // The entry count and the unread count are product state now, so their
        // signals have to move the revision too -- a client waiting for a
        // notification to arrive would otherwise wait on a clock.
        connect(notifications, &NotificationsAdapter::entriesChanged, this, refresh);
        connect(notifications, &NotificationsAdapter::unreadChanged, this, refresh);
    }
    if (auto* exporter = application_.editExportAdapter())
        connect(exporter, &EditExportAdapter::stateChanged, this, refresh);
    if (auto* diagnostics = application_.diagnosticsAdapter())
        connect(diagnostics, &DiagnosticsAdapter::checkingChanged, this, refresh);
    // The update card's only change notification, and therefore the settle
    // signal for update.check: the card moves to "checking" and then to its
    // answer, and each is a revision the client can wait on instead of sleeping.
    if (auto* settings = application_.settingsAdapter()) {
        connect(settings, &SettingsAdapter::updateStatusChanged, this, refresh);
        // The selected profile and its dirty flag are product state: a
        // settings.set that made the live config drift from its profile is
        // something a client waits on.
        connect(settings, &SettingsAdapter::presetsChanged, this, refresh);
    }
    // The three blocking surfaces. Their own signals, not the arbiter's: the
    // arbiter has no "which one is up" notification, and adding one would be a
    // second place where that fact is decided.
    if (auto* recovery = application_.recoveryAdapter())
        connect(recovery, &RecoveryAdapter::surfaceOpenChanged, this, refresh);
    if (auto* crash = application_.crashReportAdapter())
        connect(crash, &CrashReportAdapter::changed, this, refresh);
    if (auto* recording_error = application_.recordingErrorAdapter())
        connect(recording_error, &RecordingErrorAdapter::changed, this, refresh);

    // Revision 1 is the state at construction. Starting at 0 with an unread
    // state would make the first ui.getState look like a change had happened.
    observed_ = State();
    revision_ = 1;
}

void QuickLiveVerifySource::refreshObservableState() {
    AutomationState current = State();
    if (current == observed_)
        return;
    observed_ = std::move(current);
    ++revision_;
    emit observableStateChanged();
}

std::uint64_t QuickLiveVerifySource::StateRevision() const {
    return revision_;
}

live_verify::AutomationState QuickLiveVerifySource::State() const {
    AutomationState state;

    if (const auto* shell = application_.shellAdapter()) {
        state.page = PageName(static_cast<ShellAdapter::Page>(shell->currentPage()));
        state.edit_visible = shell->editSurfaceVisible();
        state.source_picker_open = shell->sourcePickerOpen();
    }

    if (const auto surface = application_.blockingSurfaces().activeSurface(); surface.has_value())
        state.blocking_surface = BlockingSurfaceName(*surface);

    if (const auto* record = application_.recordViewModelAdapter()) {
        state.recording_state = RecordingStateName(static_cast<UiRecordingState>(record->state()));
        state.selected_source_name = record->sourceName();
        state.selected_source_kind = record->sourceKindText();
        state.can_start = record->canStart();
        state.can_stop = record->canStop();
        state.can_pause = record->canPause();
        state.can_resume = record->canResume();
        state.can_split = record->splitEnabled();
        state.can_capture_frame = record->captureFrameEnabled();
        state.can_select_source = record->canSelectSource();
        state.countdown_active = record->countdownActive();
        // A marker belongs to a running recording. Paused counts: the file is
        // still open and the marker lands at the paused media time.
        state.can_add_marker = record->recording() || record->paused();
    }

    if (const auto* session = application_.editSessionAdapter()) {
        state.edit_session_open = session->open();
        state.edit_export_running = session->exportRunning();
    }
    if (const auto* player = application_.editPlayerAdapter()) {
        state.edit_playback = !player->clipOpen() ? QStringLiteral("none")
                              : player->playing() ? QStringLiteral("playing")
                                                  : QStringLiteral("paused");
    }
    state.can_open_edit = application_.canOpenEditor();

    if (const auto* exporter = application_.editExportAdapter()) {
        state.edit_export_state = ExportStateName(static_cast<EditExportAdapter::State>(exporter->stateValue()));
        state.can_export = exporter->canExport();
    }

    if (auto* notifications = application_.notificationsAdapter()) {
        state.notification_hub_open = notifications->hubOpen();
        state.notification_unread = notifications->unreadCount();
        if (const QAbstractItemModel* model = notifications->model())
            state.notification_count = model->rowCount();
    }

    if (const auto* settings = application_.settingsAdapter()) {
        state.profile_id = settings->selectedPresetId();
        state.profile_name = settings->selectedPresetName();
        state.profile_built_in = settings->presetBuiltIn();
        state.profile_dirty = settings->presetDirty();
    }

    if (const auto* diagnostics = application_.diagnosticsAdapter())
        state.diagnostics_checking = diagnostics->checking();

    if (const auto* recovery = application_.recoveryAdapter())
        state.recovery_candidate_count = recovery->candidateCount();
    if (const auto* failure = application_.recordingErrorAdapter())
        state.recording_error_can_send_report = failure->canSendReport();
    if (const auto* crash = application_.crashReportAdapter())
        state.crash_report_folder_available = crash->crashFolderAvailable();

    // Update. Read from the CARD, not from a second view of the update engine:
    // the card is what the user acts on, and what the precondition for
    // update.apply has to agree with.
    if (auto* settings = application_.settingsAdapter()) {
        state.update_state = settings->updateState();
        state.update_channel = settings->updateChannel();
        state.update_available_version = settings->updateAvailableVersion();
        state.update_available = settings->updateAvailable();
        state.update_action_enabled = settings->updateActionEnabled();
        state.update_checking = settings->updateState() == QLatin1String("checking");
    }
    state.update_current_version = QString::fromLatin1(exosnap::build::kVersion);
    state.update_blocker = application_.updateBlockerReason();

    return state;
}

QJsonObject QuickLiveVerifySource::Identity() const {
    const models::AboutInfo& about = application_.aboutViewModel()->info();
    const QString executable = QCoreApplication::applicationFilePath();
    if (executable_sha256_.isEmpty())
        executable_sha256_ = models::ComputeFileSha256(executable);

    QJsonObject json;
    json.insert(QStringLiteral("productVersion"), about.version);
    json.insert(QStringLiteral("commit"), about.commit_full);
    json.insert(QStringLiteral("buildId"), about.build_id);
    json.insert(QStringLiteral("configuration"), about.configuration);
    json.insert(QStringLiteral("officialBuild"), about.official_build);
    json.insert(QStringLiteral("dirtySourceTree"), about.dirty_source_tree);
    json.insert(QStringLiteral("installMode"), about.install_mode_label);
    json.insert(QStringLiteral("channel"), about.channel);
    json.insert(QStringLiteral("executablePath"), executable);
    json.insert(QStringLiteral("executableSha256"), executable_sha256_);
    json.insert(QStringLiteral("pid"), static_cast<int>(QCoreApplication::applicationPid()));
    json.insert(QStringLiteral("qtVersion"), QString::fromLatin1(qVersion()));
    return json;
}

QJsonObject QuickLiveVerifySource::SystemSnapshot() const {
    QJsonObject json;
    QJsonArray screens;
    for (const QScreen* screen : QGuiApplication::screens())
        screens.append(ScreenJson(screen));
    json.insert(QStringLiteral("screens"), screens);
    json.insert(QStringLiteral("screenCount"), screens.size());
    json.insert(QStringLiteral("primaryScreen"),
                QGuiApplication::primaryScreen() != nullptr ? QGuiApplication::primaryScreen()->name() : QString{});
    json.insert(QStringLiteral("pid"), static_cast<int>(QCoreApplication::applicationPid()));
    return json;
}

QJsonObject QuickLiveVerifySource::AppSnapshot() const {
    QJsonObject json;
    if (auto* settings = application_.settingsAdapter()) {
        json.insert(QStringLiteral("appearanceId"), settings->appearanceId());
        json.insert(QStringLiteral("accentId"), settings->accentId());
    }
    // The shell's own navigation index. Still an integer, because that is what
    // protocol 1 has always answered here and a v1 client must not have its
    // fields change shape underneath it -- but read from the adapter rather than
    // from findChild("quickAppShell") plus a property lookup, so the protocol no
    // longer depends on a QML objectName. Protocol 2 clients read the NAMED page
    // out of ui.getState instead.
    if (const auto* shell = application_.shellAdapter())
        json.insert(QStringLiteral("currentPage"), shell->currentPage());
    if (auto* diagnostics = application_.diagnosticsAdapter())
        json.insert(QStringLiteral("expertMode"), diagnostics->expertMode());
    json.insert(QStringLiteral("windowVisible"), root_window_ != nullptr && root_window_->isVisible());
    return json;
}

QJsonObject QuickLiveVerifySource::WindowSnapshot() const {
    QJsonObject json;
    if (root_window_ == nullptr) {
        json.insert(QStringLiteral("present"), false);
        return json;
    }
    json.insert(QStringLiteral("present"), true);
    json.insert(QStringLiteral("visible"), root_window_->isVisible());
    json.insert(QStringLiteral("exposed"), root_window_->isExposed());
    json.insert(QStringLiteral("screen"), ScreenJson(root_window_->screen()));

    const QRect geometry = root_window_->geometry();
    json.insert(QStringLiteral("x"), geometry.x());
    json.insert(QStringLiteral("y"), geometry.y());
    json.insert(QStringLiteral("width"), geometry.width());
    json.insert(QStringLiteral("height"), geometry.height());
    json.insert(QStringLiteral("devicePixelRatio"), root_window_->devicePixelRatio());

    json.insert(QStringLiteral("native"),
                NativeFactsJson(diagnostics::QueryNativeWindowFacts(reinterpret_cast<void*>(root_window_->winId()))));
    return json;
}

QJsonObject QuickLiveVerifySource::PreviewSnapshot() const {
    QJsonObject json;
    auto* preview = application_.recordPreviewAdapter();
    if (preview == nullptr) {
        json.insert(QStringLiteral("available"), false);
        return json;
    }
    json.insert(QStringLiteral("available"), true);
    json.insert(QStringLiteral("active"), preview->active());
    json.insert(QStringLiteral("sourceAvailable"), preview->sourceAvailable());
    json.insert(QStringLiteral("frameReady"), preview->frameReady());
    json.insert(QStringLiteral("sourceName"), preview->sourceName());
    json.insert(QStringLiteral("statusText"), preview->statusText());
    json.insert(QStringLiteral("errorText"), preview->errorText());
    json.insert(QStringLiteral("presentationRate"), preview->presentationRate());
    json.insert(QStringLiteral("sourceDeliveryRate"), preview->sourceDeliveryRate());
    json.insert(QStringLiteral("consumedFrames"), static_cast<double>(preview->consumedFrames()));

    // The redraw gate's own counters. `owed` is the whole cross-monitor
    // question: a publish that no render pass has followed means the newest
    // frame is in the transport and the screen has not shown it. Reported as
    // structured state rather than left to preview-trace log parsing, because a
    // runner that greps a text log is one log-format change away from silently
    // passing everything.
    if (const auto& scheduler = preview->updateScheduler()) {
        QJsonObject gate;
        gate.insert(QStringLiteral("publishSignals"), static_cast<double>(scheduler->PublishSignals()));
        gate.insert(QStringLiteral("coalescedSignals"), static_cast<double>(scheduler->CoalescedSignals()));
        gate.insert(QStringLiteral("wakeups"), static_cast<double>(scheduler->Wakeups()));
        gate.insert(QStringLiteral("sceneUpdateRequests"), static_cast<double>(scheduler->SceneUpdateRequests()));
        gate.insert(QStringLiteral("renderPasses"), static_cast<double>(scheduler->RenderPasses()));
        gate.insert(QStringLiteral("owed"), scheduler->HasUnrenderedPublish());
        json.insert(QStringLiteral("updateGate"), gate);
    }
    return json;
}

QJsonObject QuickLiveVerifySource::RecordSnapshot() const {
    QJsonObject json;
    auto* record = application_.recordViewModelAdapter();
    if (record == nullptr) {
        json.insert(QStringLiteral("available"), false);
        return json;
    }
    json.insert(QStringLiteral("available"), true);
    json.insert(QStringLiteral("state"), record->state());
    json.insert(QStringLiteral("stateText"), record->stateText());
    json.insert(QStringLiteral("recording"), record->recording());
    json.insert(QStringLiteral("paused"), record->paused());
    json.insert(QStringLiteral("preparing"), record->preparing());
    json.insert(QStringLiteral("finalizing"), record->finalizing());
    json.insert(QStringLiteral("blocked"), record->blocked());
    json.insert(QStringLiteral("failed"), record->failed());
    json.insert(QStringLiteral("countdownActive"), record->countdownActive());
    json.insert(QStringLiteral("canStart"), record->canStart());
    json.insert(QStringLiteral("canStop"), record->canStop());
    json.insert(QStringLiteral("canPause"), record->canPause());
    json.insert(QStringLiteral("canResume"), record->canResume());
    json.insert(QStringLiteral("sourceName"), record->sourceName());
    json.insert(QStringLiteral("sourceKindText"), record->sourceKindText());
    json.insert(QStringLiteral("formatText"), record->formatText());
    json.insert(QStringLiteral("elapsedText"), record->elapsedText());
    json.insert(QStringLiteral("systemAudioEnabled"), record->systemAudioEnabled());
    json.insert(QStringLiteral("appAudioEnabled"), record->appAudioEnabled());
    json.insert(QStringLiteral("microphoneEnabled"), record->microphoneEnabled());
    if (auto* coordinator = application_.recordingCoordinator()) {
        json.insert(QStringLiteral("currentOutputPath"),
                    QString::fromStdWString(coordinator->CurrentOutputPath().wstring()));
    }
    return json;
}

QJsonObject QuickLiveVerifySource::RecordResult() const {
    const RecordViewModel& view_model = application_.recordViewModel();
    QJsonObject json;
    json.insert(QStringLiteral("hasResult"), view_model.HasResult());
    json.insert(QStringLiteral("succeeded"), view_model.last_succeeded);
    json.insert(QStringLiteral("statusText"), ToQString(view_model.result_status_text));
    json.insert(QStringLiteral("outputPath"), ToQString(view_model.result_output_path));
    json.insert(QStringLiteral("editMasterPath"), ToQString(view_model.result_mkv_master_path));
    json.insert(QStringLiteral("markerSidecarPath"), ToQString(view_model.result_marker_sidecar_path));
    json.insert(QStringLiteral("errorPhase"), ToQString(view_model.result_error_phase));
    json.insert(QStringLiteral("errorDetail"), ToQString(view_model.result_error_detail));
    json.insert(QStringLiteral("hresult"), ToQString(view_model.result_hresult_text));
    json.insert(QStringLiteral("outputFileBytes"), static_cast<double>(view_model.result_output_file_bytes));
    json.insert(QStringLiteral("durationSeconds"), view_model.result_duration_seconds);
    json.insert(QStringLiteral("outputWidth"), static_cast<int>(view_model.result_output_width));
    json.insert(QStringLiteral("outputHeight"), static_cast<int>(view_model.result_output_height));
    json.insert(QStringLiteral("frameRateNum"), static_cast<int>(view_model.result_frame_rate_num));
    json.insert(QStringLiteral("frameRateDen"), static_cast<int>(view_model.result_frame_rate_den));
    json.insert(QStringLiteral("cfr"), view_model.result_cfr);
    json.insert(QStringLiteral("container"), ui::containerLabel(view_model.result_container));
    json.insert(QStringLiteral("videoCodec"), ui::videoCodecLabel(view_model.result_video_codec));
    json.insert(QStringLiteral("audioCodec"), ui::audioCodecLabel(view_model.result_audio_codec));
    json.insert(QStringLiteral("markerCount"), static_cast<int>(view_model.result_markers.size()));
    return json;
}

QJsonObject QuickLiveVerifySource::OverlaySnapshot() const {
    // The five capture-excluded overlays are structurally unobservable to every
    // pixel instrument this project has: WDA_EXCLUDEFROMCAPTURE defeats
    // screenshots, screen recording and PrintWindow, and grabWindow() renders the
    // scene graph, which shows correct alpha even when the window composes
    // wrongly on the desktop. What CAN be proven is the native state that decides
    // composition -- which is exactly what this reports. The desktop appearance
    // stays a human gate; this makes the human gate the only remaining one.
    QJsonArray overlays;
    for (QWindow* window : QGuiApplication::topLevelWindows()) {
        if (window == nullptr || !window->objectName().startsWith(QLatin1String("quickOverlay")))
            continue;
        QJsonObject json;
        json.insert(QStringLiteral("objectName"), window->objectName());
        json.insert(QStringLiteral("visible"), window->isVisible());
        json.insert(QStringLiteral("exposed"), window->isExposed());
        if (window->screen() != nullptr)
            json.insert(QStringLiteral("screen"), window->screen()->name());
        // winId() on a hidden window would force the platform window into
        // existence; a snapshot must observe, never create.
        const bool has_handle = window->handle() != nullptr;
        json.insert(QStringLiteral("nativeWindowCreated"), has_handle);
        if (has_handle) {
            json.insert(QStringLiteral("native"),
                        NativeFactsJson(diagnostics::QueryNativeWindowFacts(reinterpret_cast<void*>(window->winId()))));
        }
        overlays.append(json);
    }

    QJsonObject json;
    json.insert(QStringLiteral("overlays"), overlays);
    json.insert(QStringLiteral("count"), overlays.size());
    return json;
}

QJsonObject QuickLiveVerifySource::EditorSnapshot() const {
    QJsonObject json;
    auto* session = application_.editSessionAdapter();
    auto* player = application_.editPlayerAdapter();
    if (session == nullptr) {
        json.insert(QStringLiteral("available"), false);
        return json;
    }
    json.insert(QStringLiteral("available"), true);
    json.insert(QStringLiteral("open"), session->open());
    json.insert(QStringLiteral("clipPath"), session->clipPath());
    json.insert(QStringLiteral("durationMs"), static_cast<double>(session->durationMs()));
    json.insert(QStringLiteral("positionMs"), static_cast<double>(session->positionMs()));
    json.insert(QStringLiteral("trimStartMs"), static_cast<double>(session->trimStartMs()));
    json.insert(QStringLiteral("trimEndMs"), static_cast<double>(session->trimEndMs()));
    json.insert(QStringLiteral("trimmed"), session->trimmed());
    json.insert(QStringLiteral("exportRunning"), session->exportRunning());
    json.insert(QStringLiteral("hasUnsavedEdits"), session->hasUnsavedEdits());
    if (player != nullptr) {
        json.insert(QStringLiteral("playing"), player->playing());
        json.insert(QStringLiteral("clipOpen"), player->clipOpen());
        json.insert(QStringLiteral("placeholderText"), player->placeholderText());
    }
    return json;
}

QJsonObject QuickLiveVerifySource::DiagnosticsSnapshot() const {
    QJsonObject json;
    auto* diagnostics = application_.diagnosticsAdapter();
    if (diagnostics == nullptr) {
        json.insert(QStringLiteral("available"), false);
        return json;
    }
    json.insert(QStringLiteral("available"), true);
    json.insert(QStringLiteral("dataReady"), diagnostics->dataReady());
    json.insert(QStringLiteral("checking"), diagnostics->checking());
    json.insert(QStringLiteral("verdictState"), diagnostics->verdictState());
    json.insert(QStringLiteral("verdictHeadline"), diagnostics->verdictHeadline());
    json.insert(QStringLiteral("blockerCount"), diagnostics->blockerCount());
    json.insert(QStringLiteral("noticeCount"), diagnostics->noticeCount());
    json.insert(QStringLiteral("elevated"), diagnostics->elevated());
    return json;
}

// ---------------------------------------------------------------------------
// Observability surfaces
//
// Each of these is an assembly, never a computation: it collects the models the
// application already owns and hands them to the pure serializer in
// app/observability. Nothing below measures, classifies or reconciles anything.
// ---------------------------------------------------------------------------

QJsonObject QuickLiveVerifySource::PipelineSnapshot() const {
    const auto* adapter = application_.diagnosticsAdapter();
    if (adapter == nullptr) {
        // No diagnostics area at all. Not "an idle pipeline" -- an idle pipeline
        // has a valid:false snapshot, and this does not even have that.
        QJsonObject json;
        json.insert(QStringLiteral("valid"), false);
        json.insert(QStringLiteral("lifecycle"), QStringLiteral("idle"));
        json.insert(QStringLiteral("health"), QStringLiteral("Unavailable"));
        return json;
    }
    // The last snapshot the engine published, verbatim. The controller is a
    // pass-through for it; the 5 Hz delivery already happened and this only reads
    // the latest immutable value, so a poll costs one serialization and never
    // perturbs the recording.
    return observability::PipelineSnapshotToJson(adapter->controller().liveSnapshot());
}

QJsonObject QuickLiveVerifySource::SettingsSnapshot() const {
    const QuickApplication::EffectiveRecordingConfig effective = application_.resolveEffectiveConfig();

    observability::SettingsSnapshotInputs inputs;
    inputs.requested = application_.liveConfig();
    inputs.effective = effective.config;
    inputs.resolution = effective.resolution;
    inputs.capabilities_probed = effective.evaluated;
    inputs.app = application_.appSettings();
    inputs.settingsFilePath = application_.settingsFilePath();

    // The RUNNING level is the encoder's own initialization record, not a third
    // copy of the configuration. It stays valid after a recording ends, which is
    // what makes "what did the last session actually run with" answerable.
    if (const auto* adapter = application_.diagnosticsAdapter()) {
        inputs.running = adapter->controller().liveSnapshot().encoder_init;
        inputs.running_live = adapter->controller().liveRecording();
    }
    return observability::SettingsSnapshotToJson(inputs);
}

QJsonObject QuickLiveVerifySource::DiagnosticsResults() const {
    auto* adapter = application_.diagnosticsAdapter();
    if (adapter == nullptr) {
        QJsonObject json;
        json.insert(QStringLiteral("checked"), false);
        json.insert(QStringLiteral("checking"), false);
        return json;
    }
    const diagnostics::DiagnosticsController& controller = adapter->controller();
    // A self-test that never ran contributes an empty checklist rather than the
    // probe's default-constructed one -- "not executed in this build" must not
    // arrive as a list of zero passing checks.
    const diagnostics::DiagnosticChecklist empty_self_test;
    // `checked` is dataReady(), not "the list is non-empty": a machine with
    // nothing wrong produces an empty checklist, and so does a process that has
    // not probed yet. Those are different answers.
    return observability::DiagnosticsResultsToJson(
        controller.lastChecklist(), controller.selfTestValid() ? controller.selfTestChecklist() : empty_self_test,
        controller.dataReady(), adapter->checking(), controller.elevated());
}

QJsonObject QuickLiveVerifySource::EnvironmentSnapshot() const {
    observability::EnvironmentSnapshotInputs inputs;
    inputs.capabilities = application_.capabilities();
    inputs.elevated = application_.diagnosticsAdapter() != nullptr && application_.diagnosticsAdapter()->elevated();

    if (const auto* devices = application_.deviceAdapter()) {
        // Read only. ensureScanned() is NOT called: an observation that starts a
        // DXGI enumeration and an NVENC session would change what it measures,
        // and `scanned:false` is a perfectly good answer.
        inputs.adapters_scanned = devices->hasScanned();
        inputs.adapters = devices->adapterInfos();
        inputs.adapter_capabilities = devices->adapterCapabilities();
        inputs.active_adapter_index = devices->activeIndex();
    }

    for (const QScreen* screen : QGuiApplication::screens()) {
        if (screen == nullptr)
            continue;
        observability::ScreenFacts facts;
        const QRect geometry = screen->geometry();
        facts.name = screen->name();
        facts.x = geometry.x();
        facts.y = geometry.y();
        facts.width = geometry.width();
        facts.height = geometry.height();
        facts.device_pixel_ratio = screen->devicePixelRatio();
        facts.refresh_hz = screen->refreshRate();
        facts.primary = screen == QGuiApplication::primaryScreen();
        inputs.screens.push_back(std::move(facts));
    }

    const AudioDeviceSnapshot audio = application_.audioDeviceNotifier().currentSnapshot();
    const auto map_endpoints = [](const QVector<recorder_core::AudioInputDeviceInfo>& source) {
        std::vector<observability::AudioEndpointFacts> endpoints;
        endpoints.reserve(static_cast<std::size_t>(source.size()));
        for (const recorder_core::AudioInputDeviceInfo& device : source)
            endpoints.push_back({QString::fromStdString(device.display_name), device.is_default});
        return endpoints;
    };
    inputs.audio_inputs = map_endpoints(audio.inputs);
    inputs.audio_outputs = map_endpoints(audio.outputs);
    inputs.audio_observed = !audio.inputs.isEmpty() || !audio.outputs.isEmpty();

    // PresentMon: this frontend instantiates no provider, so there is nothing to
    // sample. The opt-in and the elevation state are still real and are reported,
    // because they are what a client needs in order to know WHY there is no
    // present measurement rather than merely that there is none.
    inputs.present.opt_in = application_.appSettings().present_diagnostics_optin;
    inputs.present.elevated = inputs.elevated;
    inputs.present.available = false;

    return observability::EnvironmentSnapshotToJson(inputs);
}

QJsonObject QuickLiveVerifySource::WindowsSnapshot() const {
    std::vector<observability::WindowFacts> windows;
    for (QWindow* window : QGuiApplication::topLevelWindows()) {
        if (window == nullptr)
            continue;
        const bool is_root = root_window_ != nullptr && window == static_cast<QWindow*>(root_window_.data());
        const QString object_name = window->objectName();
        // Every top-level window this process owns is reported, including one
        // that matches no known role. A snapshot that filtered by objectName
        // prefix -- which overlay.snapshot does -- cannot show the defect Wave B
        // found, because the window that shared the main window's title was the
        // one the filter dropped.
        observability::WindowFacts facts;
        facts.role = observability::WindowRoleForObjectName(object_name, is_root);
        facts.object_name = object_name;
        facts.title = window->title();
        facts.visible = window->isVisible();
        facts.exposed = window->isExposed();
        // winId() on a window with no platform window would CREATE one. A
        // snapshot observes; it never brings a window into existence.
        facts.native_window_created = window->handle() != nullptr;
        if (window->screen() != nullptr)
            facts.screen = window->screen()->name();
        if (facts.native_window_created) {
            facts.native =
                NativeFactsJson(diagnostics::QueryNativeWindowFacts(reinterpret_cast<void*>(window->winId())));
        }
        windows.push_back(std::move(facts));
    }
    return observability::WindowSnapshotToJson(windows, QCoreApplication::applicationPid());
}

QJsonObject QuickLiveVerifySource::RecentEvents(const QJsonObject& params, QString* error) const {
    const observability::EventQueryFilter filter = observability::ParseEventQueryFilter(params, error);
    if (error != nullptr && !error->isEmpty())
        return {};
    return observability::QueryEvents(filter);
}

QJsonObject QuickLiveVerifySource::SessionReport(const QString& recording_session_id) const {
    return recording_session_id.isEmpty() ? observability::LatestSessionReport()
                                          : observability::SessionReportById(recording_session_id);
}

bool QuickLiveVerifySource::MoveWindowToScreen(const QString& screen_name, QString* error) {
    if (root_window_ == nullptr) {
        *error = QStringLiteral("There is no main window");
        return false;
    }
    QScreen* target = nullptr;
    for (QScreen* screen : QGuiApplication::screens()) {
        if (screen != nullptr && screen->name() == screen_name) {
            target = screen;
            break;
        }
    }
    if (target == nullptr) {
        *error = QStringLiteral("No screen named %1").arg(screen_name);
        return false;
    }
    if (root_window_->screen() == target)
        return true;

    // Move by geometry, not by setScreen(): the window must actually cross the
    // boundary the way a drag does, so the platform emits the same
    // screenChanged/expose sequence the defect under test rides on. setScreen()
    // alone can retarget without a geometry change on some paths.
    const QRect current = root_window_->geometry();
    const QRect available = target->availableGeometry();
    const int width = qMin(current.width(), available.width());
    const int height = qMin(current.height(), available.height());
    const int x = available.x() + ((available.width() - width) / 2);
    const int y = available.y() + ((available.height() - height) / 2);
    root_window_->setGeometry(QRect(x, y, width, height));
    return true;
}

bool QuickLiveVerifySource::SelectRecordTarget(const QString& kind, const QString& title_filter, QString* error) {
    const auto target_kind = kind == QStringLiteral("window") ? recorder_core::CaptureTarget::Kind::Window
                                                              : recorder_core::CaptureTarget::Kind::Monitor;
    if (!application_.selectCaptureTargetForAutomation(target_kind, title_filter)) {
        *error = QStringLiteral("No %1 target matched").arg(kind);
        return false;
    }
    return true;
}

namespace {

// The transport intents share one shape: press the Q_INVOKABLE the button
// presses, and report whether the surface was there to press.
//
// What they no longer do is check canPause()/canStop()/... for themselves. Those
// predicates are in State(), LiveVerifyCommandPolicy reads them to decide both
// "may this run" and "is this in availableActions", and a copy here would be the
// second table the whole precondition design exists to avoid — the one that
// drifts silently and tells a client an action is available that dispatch then
// refuses.
bool RequireRecordSurface(const RecordViewModelAdapter* record, QString* error) {
    if (record != nullptr)
        return true;
    *error = QStringLiteral("The record surface is not available");
    return false;
}

} // namespace

// The one intent whose truthfulness this cut is about.
//
// It presses exactly what the transport button presses — requestStart() emits
// RecordViewModelAdapter::startRequested, which QuickApplication::startRequested()
// answers — and then reads what that call DECIDED. Before this it checked
// canStart() itself and reported success, while the product path went on to
// refuse the start under a blocking surface and only wrote a log line: `ok:true`,
// no state change, no event, and a runner that could discover the lie only as the
// timeout of a polling loop.
//
// The connection is direct (both objects are GUI-thread), so the admission is
// already latched by the time requestStart() returns.
bool QuickLiveVerifySource::RecordStart(QString* error) {
    auto* record = application_.recordViewModelAdapter();
    if (!RequireRecordSurface(record, error))
        return false;
    record->requestStart();
    switch (application_.lastStartAdmission()) {
    case QuickApplication::StartAdmission::Accepted:
    case QuickApplication::StartAdmission::CountdownCancelled:
        return true;
    case QuickApplication::StartAdmission::RefusedByBlockingSurface:
        // The dispatcher re-reads the policy on this, which turns it into
        // `blocked` with the surface named on both sides of requires/actual.
        *error = QStringLiteral("A blocking surface is open; the start was refused");
        return false;
    case QuickApplication::StartAdmission::RefusedByState:
        *error = QStringLiteral("record.start is not available in the current state");
        return false;
    case QuickApplication::StartAdmission::RefusedNoTarget:
        *error = QStringLiteral("There is no capture target to record");
        return false;
    }
    *error = QStringLiteral("The start request produced no decision");
    return false;
}

bool QuickLiveVerifySource::RecordPause(QString* error) {
    auto* record = application_.recordViewModelAdapter();
    if (!RequireRecordSurface(record, error))
        return false;
    record->requestPause();
    return true;
}

bool QuickLiveVerifySource::RecordResume(QString* error) {
    auto* record = application_.recordViewModelAdapter();
    if (!RequireRecordSurface(record, error))
        return false;
    record->requestResume();
    return true;
}

bool QuickLiveVerifySource::RecordStop(QString* error) {
    auto* record = application_.recordViewModelAdapter();
    if (!RequireRecordSurface(record, error))
        return false;
    record->requestStop();
    return true;
}

bool QuickLiveVerifySource::RecordSplit(QString* error) {
    auto* record = application_.recordViewModelAdapter();
    if (!RequireRecordSurface(record, error))
        return false;
    record->requestSplit();
    return true;
}

bool QuickLiveVerifySource::RecordCaptureFrame(QString* error) {
    auto* record = application_.recordViewModelAdapter();
    if (!RequireRecordSurface(record, error))
        return false;
    record->requestCaptureFrame();
    return true;
}

bool QuickLiveVerifySource::RecordAddMarker(QString* error) {
    auto* record = application_.recordViewModelAdapter();
    if (!RequireRecordSurface(record, error))
        return false;
    // The same Q_INVOKABLE the marker hotkey and the transport dock's marker
    // button press. The coordinator's AddMarker() is behind it, and stays there.
    record->requestAddMarker();
    return true;
}

bool QuickLiveVerifySource::RecordCancelCountdown(QString* error) {
    auto* record = application_.recordViewModelAdapter();
    if (!RequireRecordSurface(record, error))
        return false;
    // Pressing the transport during a countdown IS the product's cancel -- there
    // is no separate control, and startRequested() answers it with
    // StartAdmission::CountdownCancelled. Reaching for a private cancel path
    // here would prove something no user executes.
    record->requestStart();
    if (application_.lastStartAdmission() == QuickApplication::StartAdmission::CountdownCancelled)
        return true;
    *error = QStringLiteral("The transport did not treat the request as a countdown cancel");
    return false;
}

// ---------------------------------------------------------------------------
// Settings and profiles
//
// Every write below goes through a SettingsAdapter setter -- the same one the
// QML control is bound to -- so validation, container/codec reconciliation,
// persistence and the propagation into the recording side happen exactly as for
// a user edit. The key table (SettingsAutomationKeys) is what turns that into a
// stable contract instead of a property-name dependency.
// ---------------------------------------------------------------------------

QJsonObject QuickLiveVerifySource::SettingsDescribe() const {
    return settings_automation::DescribeKeys();
}

QJsonObject QuickLiveVerifySource::SettingsGet(const QString& key, QString* error) const {
    const auto* settings = application_.settingsAdapter();
    if (settings == nullptr) {
        if (error != nullptr)
            *error = QStringLiteral("The settings surface is not available");
        return {};
    }
    return settings_automation::ReadKeys(*settings, key, error);
}

bool QuickLiveVerifySource::SettingsSet(const QString& key, const QJsonValue& value, QString* error) {
    auto* settings = application_.settingsAdapter();
    if (settings == nullptr) {
        *error = QStringLiteral("The settings surface is not available");
        return false;
    }
    return settings_automation::WriteKey(*settings, key, value, error);
}

bool QuickLiveVerifySource::SettingsReset(QString* error) {
    auto* settings = application_.settingsAdapter();
    if (settings == nullptr) {
        *error = QStringLiteral("The settings surface is not available");
        return false;
    }
    // The card's own "Reset changes": back to the selected profile, through the
    // same path the button takes.
    settings->resetChanges();
    return true;
}

QJsonObject QuickLiveVerifySource::ProfilesSnapshot() const {
    QJsonObject json;
    const auto* settings = application_.settingsAdapter();
    if (settings == nullptr) {
        json.insert(QStringLiteral("available"), false);
        return json;
    }
    json.insert(QStringLiteral("available"), true);

    QJsonArray profiles;
    for (const QVariant& entry : settings->presetOptions()) {
        const QVariantMap map = entry.toMap();
        const QString id = map.value(QStringLiteral("value")).toString();
        QJsonObject profile;
        profile.insert(QStringLiteral("id"), id);
        profile.insert(QStringLiteral("name"), map.value(QStringLiteral("label")).toString());
        // The shipped profiles are read-only by product rule. Published so a
        // client does not have to learn which ids those are by being refused.
        profile.insert(QStringLiteral("builtIn"), IsBuiltInPresetId(id.toStdString()));
        profile.insert(QStringLiteral("selected"), id == settings->selectedPresetId());
        profiles.append(profile);
    }
    json.insert(QStringLiteral("profiles"), profiles);
    json.insert(QStringLiteral("selectedId"), settings->selectedPresetId());
    json.insert(QStringLiteral("selectedName"), settings->selectedPresetName());
    // Whether the live configuration has drifted from the profile it came from.
    json.insert(QStringLiteral("dirty"), settings->presetDirty());
    return json;
}

bool QuickLiveVerifySource::ProfileSelect(const QString& id, QString* error) {
    auto* settings = application_.settingsAdapter();
    if (settings == nullptr) {
        *error = QStringLiteral("The settings surface is not available");
        return false;
    }
    bool known = false;
    for (const QVariant& entry : settings->presetOptions())
        known = known || entry.toMap().value(QStringLiteral("value")).toString() == id;
    if (!known) {
        *error = QStringLiteral("No profile with id %1").arg(id);
        return false;
    }
    settings->selectPreset(id);
    return true;
}

bool QuickLiveVerifySource::ProfileCreate(const QString& name, QString* error) {
    auto* settings = application_.settingsAdapter();
    if (settings == nullptr) {
        *error = QStringLiteral("The settings surface is not available");
        return false;
    }
    // The adapter owns the name rules (empty, whitespace-only, colliding with an
    // existing profile). Asking it first turns a refusal into an explanation
    // instead of a silently ignored click.
    if (settings->presetNameRejected(name, QString())) {
        *error = QStringLiteral("\"%1\" is not a usable profile name").arg(name);
        return false;
    }
    settings->savePresetAs(name);
    return true;
}

bool QuickLiveVerifySource::ProfileRename(const QString& name, QString* error) {
    auto* settings = application_.settingsAdapter();
    if (settings == nullptr) {
        *error = QStringLiteral("The settings surface is not available");
        return false;
    }
    if (settings->presetNameRejected(name, settings->selectedPresetId())) {
        *error = QStringLiteral("\"%1\" is not a usable profile name").arg(name);
        return false;
    }
    settings->renamePreset(name);
    return true;
}

bool QuickLiveVerifySource::ProfileDelete(QString* error) {
    auto* settings = application_.settingsAdapter();
    if (settings == nullptr) {
        *error = QStringLiteral("The settings surface is not available");
        return false;
    }
    settings->deletePreset();
    return true;
}

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------

int QuickLiveVerifySource::notificationRowForSequence(qint64 sequence) const {
    auto* notifications = application_.notificationsAdapter();
    if (notifications == nullptr)
        return -1;
    const QAbstractItemModel* model = notifications->model();
    if (model == nullptr)
        return -1;
    for (int row = 0; row < model->rowCount(); ++row) {
        if (model->data(model->index(row, 0), NotificationEntryModel::SequenceRole).toLongLong() == sequence)
            return row;
    }
    return -1;
}

QJsonObject QuickLiveVerifySource::NotificationsSnapshot() const {
    QJsonObject json;
    auto* notifications = application_.notificationsAdapter();
    if (notifications == nullptr) {
        json.insert(QStringLiteral("available"), false);
        return json;
    }
    json.insert(QStringLiteral("available"), true);
    json.insert(QStringLiteral("hubOpen"), notifications->hubOpen());
    json.insert(QStringLiteral("unreadCount"), notifications->unreadCount());

    QJsonArray entries;
    if (const QAbstractItemModel* model = notifications->model()) {
        for (int row = 0; row < model->rowCount(); ++row) {
            const QModelIndex index = model->index(row, 0);
            QJsonObject entry;
            // The manager-assigned sequence, which is the hub's own stable
            // identity and the only thing a client should address an entry by.
            entry.insert(QStringLiteral("sequence"),
                         static_cast<double>(model->data(index, NotificationEntryModel::SequenceRole).toLongLong()));
            entry.insert(QStringLiteral("title"), model->data(index, NotificationEntryModel::TitleRole).toString());
            entry.insert(QStringLiteral("body"), model->data(index, NotificationEntryModel::BodyRole).toString());
            entry.insert(QStringLiteral("severity"), model->data(index, NotificationEntryModel::ToneRole).toString());
            entry.insert(QStringLiteral("unread"), model->data(index, NotificationEntryModel::UnreadRole).toBool());
            // Standing entries are the ones whose condition still holds; the hub
            // keeps a permanent record of both kinds, so this is what tells them
            // apart without reading the toast surface.
            const QVariantList actions = model->data(index, NotificationEntryModel::ActionsRole).toList();
            QJsonArray action_names;
            for (const QVariant& action : actions)
                action_names.append(action.toMap().value(QStringLiteral("label")).toString());
            entry.insert(QStringLiteral("actions"), action_names);
            entries.append(entry);
        }
    }
    json.insert(QStringLiteral("entries"), entries);
    json.insert(QStringLiteral("count"), entries.size());
    return json;
}

bool QuickLiveVerifySource::NotificationDismiss(qint64 sequence, QString* error) {
    auto* notifications = application_.notificationsAdapter();
    if (notifications == nullptr) {
        *error = QStringLiteral("The notification hub is not available");
        return false;
    }
    const int row = notificationRowForSequence(sequence);
    if (row < 0) {
        *error = QStringLiteral("No notification with sequence %1").arg(sequence);
        return false;
    }
    notifications->dismissEntry(row);
    return true;
}

bool QuickLiveVerifySource::NotificationInvokeAction(qint64 sequence, const QString& which, QString* error) {
    auto* notifications = application_.notificationsAdapter();
    if (notifications == nullptr) {
        *error = QStringLiteral("The notification hub is not available");
        return false;
    }
    const int row = notificationRowForSequence(sequence);
    if (row < 0) {
        *error = QStringLiteral("No notification with sequence %1").arg(sequence);
        return false;
    }
    const QAbstractItemModel* model = notifications->model();
    const QVariantList actions = model->data(model->index(row, 0), NotificationEntryModel::ActionsRole).toList();
    const int slot = which == QLatin1String("secondary") ? 1 : 0;
    if (slot >= actions.size()) {
        // Named rather than silently no-op: a client that asked for a button the
        // entry does not have has a bug, and a success would hide it.
        *error = QStringLiteral("That notification has no %1 action").arg(which);
        return false;
    }
    notifications->triggerAction(row, actions.at(slot).toMap().value(QStringLiteral("action")).toInt());
    return true;
}

// ---------------------------------------------------------------------------
// Diagnostics, logs and the blocking surfaces
// ---------------------------------------------------------------------------

bool QuickLiveVerifySource::DiagnosticsRun(QString* error) {
    auto* diagnostics = application_.diagnosticsAdapter();
    if (diagnostics == nullptr) {
        *error = QStringLiteral("The diagnostics surface is not available");
        return false;
    }
    // The page's own "Run Check" button. Its worker-thread probe and its
    // `checking` flag are what a client waits on.
    diagnostics->runCheck();
    return true;
}

bool QuickLiveVerifySource::LogsOpen(QString* error) {
    // The Diagnostics "open logs" affordance, which routes through the shell's
    // one navigation edge rather than writing a page index. Deliberately not a
    // file-open: nothing here reveals or reads a log PATH.
    auto* diagnostics = application_.diagnosticsAdapter();
    if (diagnostics == nullptr) {
        *error = QStringLiteral("The diagnostics surface is not available");
        return false;
    }
    diagnostics->openLogs();
    return true;
}

bool QuickLiveVerifySource::RecoveryContinue(int index, QString* error) {
    auto* recovery = application_.recoveryAdapter();
    if (recovery == nullptr) {
        *error = QStringLiteral("The recovery surface is not available");
        return false;
    }
    recovery->continueSession(index);
    return true;
}

bool QuickLiveVerifySource::RecoveryDiscard(int index, QString* error) {
    auto* recovery = application_.recoveryAdapter();
    if (recovery == nullptr) {
        *error = QStringLiteral("The recovery surface is not available");
        return false;
    }
    // The surface's own two-step: arm, then discard. Skipping the arm would
    // exercise a path the user cannot take -- the confirmation IS the product's
    // guard against discarding an interrupted recording by accident.
    recovery->armDiscard(index);
    recovery->discard(index);
    return true;
}

bool QuickLiveVerifySource::RecoveryDismiss(QString* error) {
    auto* recovery = application_.recoveryAdapter();
    if (recovery == nullptr) {
        *error = QStringLiteral("The recovery surface is not available");
        return false;
    }
    recovery->dismiss();
    return true;
}

bool QuickLiveVerifySource::CrashReportSend(QString* error) {
    auto* crash = application_.crashReportAdapter();
    if (crash == nullptr) {
        *error = QStringLiteral("The crash-report surface is not available");
        return false;
    }
    crash->sendReport();
    return true;
}

bool QuickLiveVerifySource::CrashReportDecline(QString* error) {
    auto* crash = application_.crashReportAdapter();
    if (crash == nullptr) {
        *error = QStringLiteral("The crash-report surface is not available");
        return false;
    }
    crash->dontSend();
    return true;
}

bool QuickLiveVerifySource::RecordingErrorDismiss(QString* error) {
    auto* failure = application_.recordingErrorAdapter();
    if (failure == nullptr) {
        *error = QStringLiteral("The failure surface is not available");
        return false;
    }
    failure->dismiss();
    return true;
}

bool QuickLiveVerifySource::RecordingErrorSendReport(QString* error) {
    auto* failure = application_.recordingErrorAdapter();
    if (failure == nullptr) {
        *error = QStringLiteral("The failure surface is not available");
        return false;
    }
    failure->sendReport();
    return true;
}

// --- Shell intents -----------------------------------------------------------

bool QuickLiveVerifySource::Navigate(const QString& page, QString* error) {
    auto* shell = application_.shellAdapter();
    if (shell == nullptr) {
        *error = QStringLiteral("The shell is not available");
        return false;
    }
    const std::optional<ShellAdapter::Page> destination = PageFromName(page);
    if (!destination.has_value()) {
        *error = QStringLiteral("No page named %1").arg(page);
        return false;
    }
    // The one navigation edge (QCR-001). navigateToPageRequested is what the
    // notification actions, the recovery Continue and the Diagnostics jumps
    // already emit; Main routes it into AppShell.navigateTo(), which applies the
    // single navigation guard. Writing `currentPage` here would be exactly the
    // bug QCR-001 removed: a page swapped without the policy ever running.
    //
    // Synchronous: the QML connection is direct, and the destination loaders use
    // Loader.setSource(), which loads synchronously. The resulting page is
    // readable on the next line, which is what lets ui.navigate answer
    // settled:true with no wait at all.
    emit shell->navigateToPageRequested(*destination);
    return true;
}

QObject* QuickLiveVerifySource::pageObjectFor(const QString& surface) const {
    const QString object_name = PageObjectName(surface);
    if (object_name.isEmpty() || root_window_ == nullptr)
        return nullptr;
    return root_window_->findChild<QObject*>(object_name);
}

live_verify::LiveVerifySource::RevealOutcome QuickLiveVerifySource::Reveal(const QString& surface,
                                                                           const QString& target, QString* error) {
    QObject* page = pageObjectFor(surface);
    if (page == nullptr) {
        *error = QStringLiteral("The %1 page is not loaded").arg(surface);
        return RevealOutcome::Failed;
    }
    // -1 no such target, 0 a real target that did not reach the viewport,
    // 1 revealed. The middle case is why this is not a bool: it is an
    // operational failure, and reporting it as "no such target" would send a
    // runner looking for a typo in a name that is correct.
    int outcome = -1;
    if (!QMetaObject::invokeMethod(page, "revealAutomationTarget", Q_RETURN_ARG(int, outcome),
                                   Q_ARG(QString, target))) {
        *error = QStringLiteral("The %1 page exposes no reveal targets").arg(surface);
        return RevealOutcome::Failed;
    }
    if (outcome < 0) {
        *error = QStringLiteral("The %1 page has no automation target named %2").arg(surface, target);
        return RevealOutcome::UnknownTarget;
    }
    if (outcome == 0) {
        *error = QStringLiteral("%1/%2 did not end up in the viewport").arg(surface, target);
        return RevealOutcome::Failed;
    }
    return RevealOutcome::Revealed;
}

bool QuickLiveVerifySource::ScrollHome(const QString& surface, QString* error) {
    QObject* page = pageObjectFor(surface);
    bool moved = false;
    if (page == nullptr || !QMetaObject::invokeMethod(page, "scrollAutomationHome", Q_RETURN_ARG(bool, moved))) {
        *error = QStringLiteral("The %1 page is not loaded").arg(surface);
        return false;
    }
    if (!moved)
        *error = QStringLiteral("The %1 page did not reach its start").arg(surface);
    return moved;
}

bool QuickLiveVerifySource::ScrollEnd(const QString& surface, QString* error) {
    QObject* page = pageObjectFor(surface);
    bool moved = false;
    if (page == nullptr || !QMetaObject::invokeMethod(page, "scrollAutomationEnd", Q_RETURN_ARG(bool, moved))) {
        *error = QStringLiteral("The %1 page is not loaded").arg(surface);
        return false;
    }
    if (!moved)
        *error = QStringLiteral("The %1 page did not reach its end").arg(surface);
    return moved;
}

bool QuickLiveVerifySource::SetSourcePickerOpen(bool open, QString* error) {
    auto* shell = application_.shellAdapter();
    if (shell == nullptr) {
        *error = QStringLiteral("The shell is not available");
        return false;
    }
    if (shell->sourcePickerOpen() == open)
        return true; // Idempotent: already in the state that was asked for.
    // RecordPage owns the picker's Loader, its Popup and the
    // resident-after-first-open contract; this asks it, exactly as the toolbar
    // button does. record.selectTarget bypasses the surface entirely, which is
    // why the picker had never been live-verified at all.
    emit shell->sourcePickerRequested(open);
    return true;
}

bool QuickLiveVerifySource::SetNotificationHubOpen(bool open, QString* error) {
    auto* notifications = application_.notificationsAdapter();
    if (notifications == nullptr) {
        *error = QStringLiteral("The notification hub is not available");
        return false;
    }
    if (open)
        notifications->openHub();
    else
        notifications->closeHub();
    return true;
}

bool QuickLiveVerifySource::ClearNotifications(QString* error) {
    auto* notifications = application_.notificationsAdapter();
    if (notifications == nullptr) {
        *error = QStringLiteral("The notification hub is not available");
        return false;
    }
    // Idempotent by construction: dismissing an empty list is a no-op.
    notifications->dismissAll();
    return true;
}

// --- Edit intents ------------------------------------------------------------
//
// Every one of these calls the Q_INVOKABLE the Edit surface's own controls and
// keys call. Clamping, trim ordering and keyframe snapping live in
// EditSessionAdapter and stay there: a command that clamped for itself would be
// a second trim model, and the check it powers would prove that model rather
// than the product's.

bool QuickLiveVerifySource::EditOpen(QString* error) {
    if (!application_.openEditorForAutomation()) {
        *error = QStringLiteral("There is no completed recording to open in the editor");
        return false;
    }
    return true;
}

bool QuickLiveVerifySource::EditPlayPause(QString* error) {
    auto* player = application_.editPlayerAdapter();
    if (player == nullptr) {
        *error = QStringLiteral("The edit player is not available");
        return false;
    }
    player->togglePlay();
    return true;
}

bool QuickLiveVerifySource::EditSeek(qint64 position_ms, QString* error) {
    auto* session = application_.editSessionAdapter();
    if (session == nullptr) {
        *error = QStringLiteral("The edit session is not available");
        return false;
    }
    session->requestSeek(position_ms);
    return true;
}

bool QuickLiveVerifySource::EditSetTrimIn(qint64 position_ms, QString* error) {
    auto* session = application_.editSessionAdapter();
    if (session == nullptr) {
        *error = QStringLiteral("The edit session is not available");
        return false;
    }
    session->requestTrim(session->clampTrimStartMs(position_ms, session->trimEndMs()), session->trimEndMs());
    return true;
}

bool QuickLiveVerifySource::EditSetTrimOut(qint64 position_ms, QString* error) {
    auto* session = application_.editSessionAdapter();
    if (session == nullptr) {
        *error = QStringLiteral("The edit session is not available");
        return false;
    }
    session->requestTrim(session->trimStartMs(), session->clampTrimEndMs(position_ms, session->trimStartMs()));
    return true;
}

bool QuickLiveVerifySource::EditTimelineHome(QString* error) {
    // The timeline's Home key with its default target: move the playhead to 0
    // and let the adapter clamp. Deliberately not "seek to trimStart" -- that is
    // a different command than the one the keyboard has.
    return EditSeek(0, error);
}

bool QuickLiveVerifySource::EditTimelineEnd(QString* error) {
    auto* session = application_.editSessionAdapter();
    if (session == nullptr) {
        *error = QStringLiteral("The edit session is not available");
        return false;
    }
    return EditSeek(session->durationMs(), error);
}

bool QuickLiveVerifySource::EditClose(QString* error) {
    auto* session = application_.editSessionAdapter();
    if (session == nullptr) {
        *error = QStringLiteral("The edit session is not available");
        return false;
    }
    // QCR-301: close() performs the whole teardown (context, trim, markers,
    // keyframes, position, clip generation) before emitting closeRequested().
    session->close();
    return true;
}

bool QuickLiveVerifySource::ExportStart(QString* error) {
    auto* exporter = application_.editExportAdapter();
    if (exporter == nullptr) {
        *error = QStringLiteral("The export panel is not available");
        return false;
    }
    // The panel's own Export button. Container, destination and overwrite are
    // whatever the panel resolved from the settings and the clip -- this is a
    // product action, not a "write this file here" API.
    exporter->startExport();
    return true;
}

bool QuickLiveVerifySource::ExportCancel(QString* error) {
    auto* exporter = application_.editExportAdapter();
    if (exporter == nullptr) {
        *error = QStringLiteral("The export panel is not available");
        return false;
    }
    exporter->cancel();
    return true;
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

bool QuickLiveVerifySource::UpdateCheck(QString* error) {
    // The card's own manual check. Nothing here reaches past it into
    // UpdateService: a check that skipped the app-layer guard and the loop-guard
    // reset would exercise a path no user has.
    application_.requestUpdateCheck();
    (void)error;
    return true;
}

bool QuickLiveVerifySource::UpdateApply(QString* error) {
    // The card's primary action. Its precondition has already established that
    // the card is offering an update, which is what makes this an APPLY rather
    // than the re-check the same button performs in every other state.
    application_.requestUpdatePrimaryAction();
    const UpdateService* service = application_.updateService();
    if (service == nullptr) {
        *error = QStringLiteral("The update service is not available");
        return false;
    }
    const UpdateService::UpdaterLaunchInfo launch = service->LastUpdaterLaunch();
    if (launch.pid == 0) {
        // LaunchUpdater stages and spawns synchronously, so by here either a
        // child exists or the launch failed -- and a failed staging is exactly
        // the case that must not answer ok.
        *error = QStringLiteral("The updater was not started");
        return false;
    }
    // The child that exists must be THIS operation's child. A previous launch's
    // pid is still on record, so "a pid is set" alone would report success for
    // an apply that was refused (an unprepared or superseded transaction) and
    // never started anything.
    if (launch.update_transaction_id != service->LastPreparedUpdate().update_transaction_id) {
        *error = QStringLiteral("The updater was not started for this update");
        return false;
    }
    return true;
}

QJsonObject QuickLiveVerifySource::UpdaterLaunchSnapshot() const {
    QJsonObject json;
    const UpdateService* service = application_.updateService();
    if (service == nullptr)
        return json;

    const UpdateService::UpdaterLaunchInfo launch = service->LastUpdaterLaunch();
    json.insert(QStringLiteral("pid"), static_cast<double>(launch.pid));
    json.insert(QStringLiteral("stagedExecutable"), launch.staged_exe);
    json.insert(QStringLiteral("targetVersion"),
                launch.target_version.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(launch.target_version));

    // Artifact binding: the hash of the exact staged copy that ran, so a stale
    // build sitting somewhere else cannot be credited with the run. Hashed once
    // per path -- the staging tree is rebuilt per apply, but its bytes do not
    // change while that child lives.
    if (!launch.staged_exe.isEmpty() && launch.staged_exe != staged_updater_path_) {
        staged_updater_path_ = launch.staged_exe;
        staged_updater_sha256_ = models::ComputeFileSha256(launch.staged_exe);
    }
    json.insert(QStringLiteral("stagedExecutableSha256"), staged_updater_sha256_);

    // The endpoint, spelled out rather than left to be derived. Empty unless
    // this process is itself under a control channel, in which case the child
    // got none either -- and a client must be able to see that difference.
    json.insert(QStringLiteral("controlRunId"), launch.automation_run_id.isEmpty()
                                                    ? QJsonValue(QJsonValue::Null)
                                                    : QJsonValue(launch.automation_run_id));
    json.insert(QStringLiteral("controlPipe"),
                launch.automation_run_id.isEmpty()
                    ? QJsonValue(QJsonValue::Null)
                    : QJsonValue(exosnap::control::PipeName(QString::fromLatin1(exosnap::control::role::kUpdater),
                                                            launch.automation_run_id)));

    // The operation and the document that carries it. A check reads the
    // transaction id to correlate this process's update with the child's
    // published state, and the path to inspect exactly what was handed over --
    // neither is derivable from anything else in this payload.
    json.insert(QStringLiteral("updateTransactionId"), launch.update_transaction_id.isEmpty()
                                                           ? QJsonValue(QJsonValue::Null)
                                                           : QJsonValue(launch.update_transaction_id));
    json.insert(QStringLiteral("handoffPath"),
                launch.handoff_path.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(launch.handoff_path));
    json.insert(QStringLiteral("handoffVersion"), launch.handoff_path.isEmpty()
                                                      ? QJsonValue(QJsonValue::Null)
                                                      : QJsonValue(exosnap::update_handoff::kHandoffVersion));
    return json;
}

} // namespace exosnap::quick
