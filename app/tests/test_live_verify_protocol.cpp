// Pure protocol + allowlist coverage for the Live Verify control channel.
//
// Everything here runs without a pipe, a window or a GPU, which is the point:
// the rejection surface (malformed JSON, unsupported protocol version, missing
// handshake, wrong run id, unknown command, bad parameters, an unmet
// precondition, a refusing intent) is the security boundary, and a boundary that
// can only be exercised against a live application is a boundary nobody
// exercises.

#include "live_verify/LiveVerifyAutomationState.h"
#include "live_verify/LiveVerifyCommandPolicy.h"
#include "live_verify/LiveVerifyDispatcher.h"
#include "live_verify/LiveVerifyOptions.h"
#include "live_verify/LiveVerifyProtocol.h"
#include "live_verify/LiveVerifySource.h"
#include "models/RecordingCapabilities.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

using namespace exosnap::live_verify;

namespace {

QByteArray Line(const QString& json) {
    return json.toUtf8();
}

QJsonObject ErrorOf(const QJsonObject& response) {
    return response.value(QStringLiteral("error")).toObject();
}

QString ErrorCode(const QJsonObject& response) {
    return ErrorOf(response).value(QStringLiteral("code")).toString();
}

bool Ok(const QJsonObject& response) {
    return response.value(QStringLiteral("ok")).toBool();
}

// A state in which every precondition holds. Tests that are about routing rather
// than about policy start from it, so a refusal in one of them is a real
// routing defect and not an unmet gate.
AutomationState PermissiveState() {
    AutomationState state;
    state.page = QString::fromLatin1(page_name::kRecord);
    state.recording_state = QStringLiteral("Recording");
    state.can_start = true;
    state.can_stop = true;
    state.can_pause = true;
    state.can_resume = true;
    state.can_split = true;
    state.can_request_clip = true;
    state.can_capture_frame = true;
    state.can_select_source = true;
    state.can_open_edit = true;
    state.can_add_marker = true;
    state.countdown_active = true;
    state.edit_session_open = true;
    state.edit_visible = true;
    state.can_export = true;
    // A workspace with something of every kind selected and a step of history
    // either way, so each timeline command meets its own precondition.
    state.edit_workspace_revision = 7;
    state.edit_media_count = 1;
    state.edit_clip_count = 2;
    state.edit_track_count = 2;
    state.edit_selected_clip = 1;
    state.edit_selected_transition = 1;
    state.edit_gap_selected = true;
    state.edit_can_undo = true;
    state.edit_can_redo = true;
    state.edit_monitoring_track_count = 1;
    state.edit_media_panel_can_pin = true;
    state.edit_details_panel_can_pin = true;
    // One entry in the hub, so the notification commands have something to act
    // on. Deliberately NOT an export in flight: export.cancel needs one and
    // edit.close refuses under one, so that flag is set per command below.
    state.notification_count = 1;
    state.profile_id = QStringLiteral("preset.default");
    // An offered update with nothing in the way, so the update commands are
    // reachable by default and a test that wants them refused says so.
    state.update_state = QStringLiteral("available");
    state.update_channel = QStringLiteral("stable");
    state.update_current_version = QStringLiteral("0.9.0-test");
    state.update_available_version = QStringLiteral("0.9.1");
    state.update_available = true;
    state.update_action_enabled = true;
    return state;
}

// Records what was asked of it and answers with markers, so a dispatcher that
// routed a command to the wrong member is a failing assertion rather than a
// plausible-looking snapshot.
class FakeSource final : public LiveVerifySource {
  public:
    QStringList calls;
    bool allow_intents = true;
    AutomationState state = PermissiveState();
    std::uint64_t revision = 1;
    RevealOutcome reveal_outcome = RevealOutcome::Revealed;
    bool scroll_lands = true;

    [[nodiscard]] QJsonObject Identity() const override {
        return QJsonObject{{QStringLiteral("productVersion"), QStringLiteral("0.9.0-test")}};
    }
    [[nodiscard]] AutomationState State() const override {
        return state;
    }
    [[nodiscard]] std::uint64_t StateRevision() const override {
        return revision;
    }
    [[nodiscard]] QJsonObject SystemSnapshot() const override {
        return Marker(QStringLiteral("system"));
    }
    [[nodiscard]] QJsonObject AppSnapshot() const override {
        return Marker(QStringLiteral("app"));
    }
    [[nodiscard]] QJsonObject WindowSnapshot() const override {
        return Marker(QStringLiteral("window"));
    }
    [[nodiscard]] QJsonObject PreviewSnapshot() const override {
        return Marker(QStringLiteral("preview"));
    }
    [[nodiscard]] QJsonObject RecordSnapshot() const override {
        return Marker(QStringLiteral("record"));
    }
    [[nodiscard]] QJsonObject RecordResult() const override {
        return Marker(QStringLiteral("record.result"));
    }
    [[nodiscard]] QJsonObject OverlaySnapshot() const override {
        return Marker(QStringLiteral("overlay"));
    }
    [[nodiscard]] QJsonObject EditorSnapshot() const override {
        QJsonObject json = Marker(QStringLiteral("editor"));
        json.insert(QStringLiteral("export"), export_options);
        return json;
    }
    [[nodiscard]] QJsonObject DiagnosticsSnapshot() const override {
        return Marker(QStringLiteral("diagnostics"));
    }

    // Observability surfaces. Markers for the same reason as the snapshots above:
    // what is under test here is the ROUTING -- that pipeline.snapshot reaches
    // PipelineSnapshot() and nothing else -- not the payloads, which have their
    // own fixture-driven tests.
    [[nodiscard]] QJsonObject PipelineSnapshot() const override {
        return Marker(QStringLiteral("pipeline"));
    }
    [[nodiscard]] QJsonObject SettingsSnapshot() const override {
        return Marker(QStringLiteral("settings"));
    }
    [[nodiscard]] QJsonObject DiagnosticsResults() const override {
        return Marker(QStringLiteral("diagnostics.results"));
    }
    [[nodiscard]] QJsonObject EnvironmentSnapshot() const override {
        return Marker(QStringLiteral("environment"));
    }
    [[nodiscard]] QJsonObject WindowsSnapshot() const override {
        return Marker(QStringLiteral("windows"));
    }
    [[nodiscard]] QJsonObject RecentEvents(const QJsonObject& params, QString* error) const override {
        if (params.value(QStringLiteral("severity")).toString() == QLatin1String("nonsense")) {
            *error = QStringLiteral("Unknown severity \"nonsense\"");
            return {};
        }
        QJsonObject json = Marker(QStringLiteral("events"));
        json.insert(QStringLiteral("max"), params.value(QStringLiteral("max")).toDouble());
        return json;
    }
    [[nodiscard]] QJsonObject SessionReport(const QString& recording_session_id) const override {
        QJsonObject json = Marker(QStringLiteral("session"));
        json.insert(QStringLiteral("requestedId"), recording_session_id);
        return json;
    }

    // --- Settings, profiles, notifications ----------------------------------
    QJsonObject last_overlay_fields;
    QJsonObject applied_overlay;
    quint64 overlay_sequence = 0;
    QString last_settings_key;
    QJsonValue last_settings_value;

    [[nodiscard]] QJsonObject SettingsDescribe() const override {
        return Marker(QStringLiteral("settings.describe"));
    }
    [[nodiscard]] QJsonObject SettingsGet(const QString& key, QString* error) const override {
        if (key == QLatin1String("nope")) {
            *error = QStringLiteral("No settings key named \"nope\"");
            return {};
        }
        QJsonObject json = Marker(QStringLiteral("settings.get"));
        json.insert(QStringLiteral("requestedKey"), key);
        return json;
    }
    bool SettingsSet(const QString& key, const QJsonValue& value, QString* error) override {
        calls.append(QStringLiteral("settings.set:%1").arg(key));
        last_settings_key = key;
        last_settings_value = value;
        return Outcome(error);
    }
    [[nodiscard]] QJsonObject AudioRoutingSnapshot() const override {
        return Marker(QStringLiteral("audio.routing.snapshot"));
    }
    bool AddAudioTrack(quint64 expected_revision, QString* error) override {
        calls.append(QStringLiteral("audio.routing.addTrack:%1").arg(expected_revision));
        return Outcome(error);
    }
    bool AssignAudioSource(const QString& source_id, const QString& track_id, int mode, quint64 expected_revision,
                           QString* error) override {
        calls.append(QStringLiteral("audio.routing.assignSource:%1:%2:%3:%4")
                         .arg(source_id)
                         .arg(track_id)
                         .arg(mode)
                         .arg(expected_revision));
        return Outcome(error);
    }
    bool SetAudioTrackRecordEnabled(const QString& track_id, bool enabled, quint64 expected_revision,
                                    QString* error) override {
        calls.append(QStringLiteral("audio.routing.setTrackRecordEnabled:%1:%2:%3")
                         .arg(track_id)
                         .arg(enabled)
                         .arg(expected_revision));
        return Outcome(error);
    }
    [[nodiscard]] QJsonObject ClipSnapshot() const override {
        return Marker(QStringLiteral("clip.snapshot"));
    }
    bool ClipRequest(const QString&, QString* error) override {
        calls.append(QStringLiteral("clip.request"));
        return Outcome(error);
    }
    bool ClipConfirm(const QString&, const QString&, QString* error) override {
        calls.append(QStringLiteral("clip.confirm"));
        return Outcome(error);
    }
    bool ClipOpen(const QString&, QString* error) override {
        return Outcome(error);
    }
    bool WebcamOverlaySet(const QJsonObject& fields, QJsonObject* applied, QString* error) override {
        calls.append(QStringLiteral("webcam.overlay.set"));
        last_overlay_fields = fields;
        if (!Outcome(error))
            return false;
        // What a real session answers: the state read back after the store, which
        // is not the request when the engine clamped it.
        applied->insert(QStringLiteral("appliedSequence"), static_cast<qint64>(++overlay_sequence));
        applied->insert(QStringLiteral("appliedQpc100ns"), static_cast<qint64>(1234567));
        applied->insert(QStringLiteral("applied"), applied_overlay);
        return true;
    }
    bool SettingsReset(QString* error) override {
        calls.append(QStringLiteral("settings.reset"));
        return Outcome(error);
    }

    [[nodiscard]] QJsonObject ProfilesSnapshot() const override {
        return Marker(QStringLiteral("profiles"));
    }
    bool ProfileSelect(const QString& id, QString* error) override {
        calls.append(QStringLiteral("profiles.select:%1").arg(id));
        if (allow_intents)
            state.profile_id = id;
        return Outcome(error);
    }
    bool ProfileCreate(const QString& name, QString* error) override {
        calls.append(QStringLiteral("profiles.create:%1").arg(name));
        return Outcome(error);
    }
    bool ProfileRename(const QString& name, QString* error) override {
        calls.append(QStringLiteral("profiles.rename:%1").arg(name));
        return Outcome(error);
    }
    bool ProfileDelete(QString* error) override {
        calls.append(QStringLiteral("profiles.delete"));
        return Outcome(error);
    }

    [[nodiscard]] QJsonObject NotificationsSnapshot() const override {
        return Marker(QStringLiteral("notifications"));
    }
    bool NotificationRaise(const QString& type_name, const QString& title, const QString& body,
                           const QString& action_name, const QString& action_payload, qint64* out_sequence,
                           QString* error) override {
        calls.append(QStringLiteral("notification.raise:%1/%2/%3/%4/%5")
                         .arg(type_name, title, body, action_name, action_payload));
        if (out_sequence != nullptr)
            *out_sequence = 4242;
        return Outcome(error);
    }
    bool NotificationDismiss(qint64 sequence, QString* error) override {
        calls.append(QStringLiteral("notification.dismiss:%1").arg(sequence));
        return Outcome(error);
    }
    bool NotificationInvokeAction(qint64 sequence, const QString& which, QString* error) override {
        calls.append(QStringLiteral("notification.invokeAction:%1/%2").arg(sequence).arg(which));
        return Outcome(error);
    }

    bool DiagnosticsRun(QString* error) override {
        calls.append(QStringLiteral("diagnostics.run"));
        return Outcome(error);
    }
    bool DiagnosticsSetInDepth(bool enabled, QString* error) override {
        calls.append(QStringLiteral("diagnostics.setInDepth:%1").arg(enabled ? 1 : 0));
        return Outcome(error);
    }
    bool LogsOpen(QString* error) override {
        calls.append(QStringLiteral("logs.open"));
        return Outcome(error);
    }
    bool AppQuit(QString* error) override {
        calls.append(QStringLiteral("app.quit"));
        return Outcome(error);
    }

    bool RecoveryContinue(int index, QString* error) override {
        calls.append(QStringLiteral("recovery.continue:%1").arg(index));
        return Outcome(error);
    }
    bool RecoveryDiscard(int index, QString* error) override {
        calls.append(QStringLiteral("recovery.discard:%1").arg(index));
        return Outcome(error);
    }
    bool RecoveryDismiss(QString* error) override {
        calls.append(QStringLiteral("recovery.dismiss"));
        if (allow_intents)
            state.blocking_surface.clear();
        return Outcome(error);
    }
    bool CrashReportSend(QString* error) override {
        calls.append(QStringLiteral("crashReport.send"));
        if (allow_intents)
            state.blocking_surface.clear();
        return Outcome(error);
    }
    bool CrashReportDecline(QString* error) override {
        calls.append(QStringLiteral("crashReport.decline"));
        if (allow_intents)
            state.blocking_surface.clear();
        return Outcome(error);
    }
    bool RecordingErrorDismiss(QString* error) override {
        calls.append(QStringLiteral("recordingError.dismiss"));
        if (allow_intents)
            state.blocking_surface.clear();
        return Outcome(error);
    }
    bool RecordingErrorSendReport(QString* error) override {
        calls.append(QStringLiteral("recordingError.sendReport"));
        if (allow_intents)
            state.blocking_surface.clear();
        return Outcome(error);
    }

    bool ExportStart(QString* error) override {
        calls.append(QStringLiteral("export.start"));
        return Outcome(error);
    }
    bool ExportCancel(QString* error) override {
        calls.append(QStringLiteral("export.cancel"));
        return Outcome(error);
    }
    bool ExportSetOptions(const QJsonObject& options, QString* error) override {
        calls.append(QStringLiteral("export.setOptions"));
        if (!Outcome(error))
            return false;
        // The panel refuses values it does not know; the fake accepts what the
        // test allows, so a refusal is visible as a value that did not change.
        for (const QString& key : options.keys())
            if (!refused_export_keys.contains(key))
                export_options.insert(key, options.value(key));
        return true;
    }
    bool ExportConfirm(QString* error) override {
        calls.append(QStringLiteral("export.confirm"));
        return Outcome(error);
    }
    bool ExportDiscard(QString* error) override {
        calls.append(QStringLiteral("export.discard"));
        if (allow_intents && discard_returns_to_idle)
            state.edit_export_state = QStringLiteral("idle");
        return Outcome(error);
    }
    bool ExportReset(QString* error) override {
        calls.append(QStringLiteral("export.reset"));
        if (allow_intents)
            state.edit_export_state = QStringLiteral("idle");
        return Outcome(error);
    }

    // --- Edit workspace ---------------------------------------------------------
    // A model in miniature: an intent either names something that exists, or it
    // does not; a delivered edit is accepted or refused by `model_accepts`, and
    // an accepted one advances the workspace revision exactly once.
    bool open_probes_first = false;
    bool open_has_effect = true;
    bool targets_exist = true;
    bool model_accepts = true;
    bool panels_follow = true;
    bool discard_returns_to_idle = true;
    QStringList refused_export_keys;
    QJsonObject export_options;
    EditOutcome edit_outcome;

    [[nodiscard]] EditOutcome LastEditOutcome() const override {
        return edit_outcome;
    }
    EditIntent EditImportMedia(const QString& path, bool append, qint64 at_ms, QString* error) override {
        calls.append(QStringLiteral("edit.importMedia:%1/%2/%3").arg(path).arg(append).arg(at_ms));
        return Target(error);
    }
    EditIntent EditAppendRecording(const QString& path, qint64 at_ms, QString* error) override {
        calls.append(QStringLiteral("edit.appendRecording:%1/%2").arg(path).arg(at_ms));
        return Target(error);
    }
    EditIntent EditAppendMedia(quint64 media_id, qint64 at_ms, QString* error) override {
        calls.append(QStringLiteral("edit.appendMedia:%1/%2").arg(media_id).arg(at_ms));
        return ModelEdit(error);
    }
    EditIntent EditSelectClip(quint64 clip_id, QString* error) override {
        calls.append(QStringLiteral("edit.selectClip:%1").arg(clip_id));
        const EditIntent intent = Target(error);
        if (intent == EditIntent::Delivered)
            state.edit_selected_clip = clip_id;
        return intent;
    }
    EditIntent EditSelectTransition(quint64 outgoing_clip_id, QString* error) override {
        calls.append(QStringLiteral("edit.selectTransition:%1").arg(outgoing_clip_id));
        const EditIntent intent = Target(error);
        if (intent == EditIntent::Delivered)
            state.edit_selected_transition = outgoing_clip_id;
        return intent;
    }
    EditIntent EditSelectGap(quint64 track_id, qint64 at_ms, QString* error) override {
        calls.append(QStringLiteral("edit.selectGap:%1/%2").arg(track_id).arg(at_ms));
        const EditIntent intent = Target(error);
        if (intent == EditIntent::Delivered)
            state.edit_gap_selected = true;
        return intent;
    }
    EditIntent EditMoveClip(quint64 clip_id, qint64 at_ms, bool snap, QString* error) override {
        calls.append(QStringLiteral("edit.moveClip:%1/%2/%3").arg(clip_id).arg(at_ms).arg(snap));
        return ModelEdit(error);
    }
    EditIntent EditTrimClip(quint64 clip_id, qint64 in_ms, qint64 out_ms, QString* error) override {
        calls.append(QStringLiteral("edit.trimClip:%1/%2/%3").arg(clip_id).arg(in_ms).arg(out_ms));
        return ModelEdit(error);
    }
    EditIntent EditSplit(QString* error) override {
        calls.append(QStringLiteral("edit.split"));
        return ModelEdit(error);
    }
    EditIntent EditDelete(bool ripple, QString* error) override {
        calls.append(QStringLiteral("edit.delete:%1").arg(ripple));
        return ModelEdit(error);
    }
    EditIntent EditCloseGap(QString* error) override {
        calls.append(QStringLiteral("edit.closeGap"));
        return ModelEdit(error);
    }
    EditIntent EditDeleteTrack(quint64 track_id, bool confirmed, QString* error) override {
        calls.append(QStringLiteral("edit.deleteTrack:%1/%2").arg(track_id).arg(confirmed));
        return ModelEdit(error);
    }
    EditIntent EditUndo(QString* error) override {
        calls.append(QStringLiteral("edit.undo"));
        return ModelEdit(error);
    }
    EditIntent EditRedo(QString* error) override {
        calls.append(QStringLiteral("edit.redo"));
        return ModelEdit(error);
    }
    EditIntent EditApplyCrossfade(quint64 clip_id, qint64 duration_ms, QString* error) override {
        calls.append(QStringLiteral("edit.applyCrossfade:%1/%2").arg(clip_id).arg(duration_ms));
        return ModelEdit(error);
    }
    EditIntent EditRemoveTransition(QString* error) override {
        calls.append(QStringLiteral("edit.removeTransition"));
        return ModelEdit(error);
    }
    EditIntent EditSetJunctionWindow(qint64 start_ms, qint64 end_ms, QString* error) override {
        calls.append(QStringLiteral("edit.setJunctionWindow:%1/%2").arg(start_ms).arg(end_ms));
        return ModelEdit(error);
    }
    EditIntent EditSetJunctionCurve(const QString& curve, double bias, QString* error) override {
        calls.append(QStringLiteral("edit.setJunctionCurve:%1/%2").arg(curve).arg(bias));
        return ModelEdit(error);
    }
    EditIntent EditSetJunctionAudioLaw(const QString& law, QString* error) override {
        calls.append(QStringLiteral("edit.setJunctionAudioLaw:%1").arg(law));
        return ModelEdit(error);
    }
    EditIntent EditSetJunctionCut(qint64 at_ms, QString* error) override {
        calls.append(QStringLiteral("edit.setJunctionCut:%1").arg(at_ms));
        return ModelEdit(error);
    }
    EditIntent EditSetClipFade(quint64 clip_id, bool incoming, qint64 duration_ms, QString* error) override {
        calls.append(QStringLiteral("edit.setClipFade:%1/%2/%3").arg(clip_id).arg(incoming).arg(duration_ms));
        return ModelEdit(error);
    }
    EditIntent EditSetFixedTimelineEnd(bool fixed, qint64 end_ms, QString* error) override {
        calls.append(QStringLiteral("edit.setFixedTimelineEnd:%1/%2").arg(fixed).arg(end_ms));
        return ModelEdit(error);
    }
    EditIntent EditSetOption(const QString& option, bool enabled, QString* error) override {
        calls.append(QStringLiteral("edit.setOption:%1/%2").arg(option).arg(enabled));
        const EditIntent intent = Target(error);
        if (intent != EditIntent::Delivered)
            return intent;
        if (option == QLatin1String("snapping"))
            state.edit_snapping = enabled;
        else if (option == QLatin1String("ripple"))
            state.edit_ripple = enabled;
        else if (option == QLatin1String("autoCrossfade"))
            state.edit_auto_crossfade = enabled;
        else
            state.edit_linked_selection = enabled;
        return intent;
    }
    EditIntent EditSetSelectionBoundary(bool out, QString* error) override {
        calls.append(QStringLiteral("edit.setSelectionBoundary:%1").arg(out));
        return Target(error);
    }
    EditIntent EditSetPlaying(bool playing, QString* error) override {
        calls.append(QStringLiteral("edit.setPlaying:%1").arg(playing));
        return Target(error);
    }
    EditIntent EditFrameStep(int direction, QString* error) override {
        calls.append(QStringLiteral("edit.frameStep:%1").arg(direction));
        return Target(error);
    }
    EditIntent EditSetVolume(double volume, QString* error) override {
        calls.append(QStringLiteral("edit.setVolume:%1").arg(volume));
        return Target(error);
    }
    EditIntent EditConfirmMonitoring(const QStringList& track_ids, bool overlap_confirmed, QString* error) override {
        calls.append(QStringLiteral("edit.confirmMonitoring:%1/%2")
                         .arg(track_ids.join(QLatin1Char(',')))
                         .arg(overlap_confirmed));
        return Target(error);
    }
    EditIntent EditSetPanel(const QString& panel, bool open, bool pinned, QString* error) override {
        calls.append(QStringLiteral("edit.setPanel:%1/%2/%3").arg(panel).arg(open).arg(pinned));
        const EditIntent intent = Target(error);
        if (intent == EditIntent::Delivered && panels_follow) {
            const QString value = !open    ? QStringLiteral("closed")
                                  : pinned ? QStringLiteral("pinned")
                                           : QStringLiteral("open");
            (panel == QLatin1String("media") ? state.edit_media_panel : state.edit_details_panel) = value;
        }
        return intent;
    }
    EditIntent EditSetSourceTab(const QString& tab, QString* error) override {
        calls.append(QStringLiteral("edit.setSourceTab:%1").arg(tab));
        const EditIntent intent = Target(error);
        if (intent == EditIntent::Delivered && panels_follow)
            state.edit_source_tab = tab;
        return intent;
    }

    bool MoveWindowToScreen(const QString& screen_name, QString* error) override {
        calls.append(QStringLiteral("moveToScreen:%1").arg(screen_name));
        return Outcome(error);
    }
    bool SelectRecordTarget(const QString& kind, const QString& title_filter, QString* error) override {
        calls.append(QStringLiteral("selectTarget:%1/%2").arg(kind, title_filter));
        return Outcome(error);
    }
    bool SelectRecordRegion(const QString& display_device, int x, int y, int width, int height,
                            QString* error) override {
        calls.append(
            QStringLiteral("selectRegion:%1/%2,%3 %4x%5").arg(display_device).arg(x).arg(y).arg(width).arg(height));
        return Outcome(error);
    }
    bool OpenRegionSelector(const QString& display_device, QString* error) override {
        calls.append(QStringLiteral("openRegionSelector:%1").arg(display_device));
        return Outcome(error);
    }
    bool RecordStart(QString* error) override {
        calls.append(QStringLiteral("start"));
        return Outcome(error);
    }
    bool RecordPause(QString* error) override {
        calls.append(QStringLiteral("pause"));
        return Outcome(error);
    }
    bool RecordResume(QString* error) override {
        calls.append(QStringLiteral("resume"));
        return Outcome(error);
    }
    bool RecordStop(QString* error) override {
        calls.append(QStringLiteral("stop"));
        return Outcome(error);
    }
    bool RecordSplit(QString* error) override {
        calls.append(QStringLiteral("split"));
        return Outcome(error);
    }
    bool RecordCaptureFrame(QString* error) override {
        calls.append(QStringLiteral("captureFrame"));
        return Outcome(error);
    }
    bool RecordAddMarker(QString* error) override {
        calls.append(QStringLiteral("addMarker"));
        return Outcome(error);
    }
    bool RecordCancelCountdown(QString* error) override {
        calls.append(QStringLiteral("cancelCountdown"));
        if (allow_intents)
            state.countdown_active = false;
        return Outcome(error);
    }

    bool Navigate(const QString& page, QString* error) override {
        calls.append(QStringLiteral("navigate:%1").arg(page));
        if (!Outcome(error))
            return false;
        // The real shell reaches the destination synchronously; the fake mirrors
        // that so the dispatcher's postcondition check has something to observe.
        // The legacy "logs" request is normalized to Diagnostics with the logs
        // subview, and the fake mirrors that too.
        if (page == QLatin1String(page_name::kLogs)) {
            state.page = QString::fromLatin1(page_name::kDiagnostics);
            state.diagnostics_section = QString::fromLatin1(diagnostics_section_name::kLogs);
        } else {
            state.page = page;
        }
        ++revision;
        return true;
    }
    [[nodiscard]] RevealOutcome Reveal(const QString& surface, const QString& target, QString* error) override {
        calls.append(QStringLiteral("reveal:%1/%2").arg(surface, target));
        if (reveal_outcome != RevealOutcome::Revealed)
            *error = QStringLiteral("no such target");
        return reveal_outcome;
    }
    bool ScrollHome(const QString& surface, QString* error) override {
        calls.append(QStringLiteral("scrollHome:%1").arg(surface));
        if (!scroll_lands)
            *error = QStringLiteral("did not reach its start");
        return scroll_lands;
    }
    bool ScrollEnd(const QString& surface, QString* error) override {
        calls.append(QStringLiteral("scrollEnd:%1").arg(surface));
        if (!scroll_lands)
            *error = QStringLiteral("did not reach its end");
        return scroll_lands;
    }
    bool SetSourcePickerOpen(bool open, QString* error) override {
        calls.append(QStringLiteral("sourcePicker:%1").arg(open ? QStringLiteral("open") : QStringLiteral("close")));
        if (!Outcome(error))
            return false;
        state.source_picker_open = open;
        ++revision;
        return true;
    }
    bool SetNotificationHubOpen(bool open, QString* error) override {
        calls.append(QStringLiteral("hub:%1").arg(open ? QStringLiteral("open") : QStringLiteral("close")));
        if (!Outcome(error))
            return false;
        state.notification_hub_open = open;
        ++revision;
        return true;
    }
    bool ClearNotifications(QString* error) override {
        calls.append(QStringLiteral("clearAll"));
        return Outcome(error);
    }

    bool EditOpen(QString* error) override {
        calls.append(QStringLiteral("edit.open"));
        if (!Outcome(error))
            return false;
        // A recording file that exists is probed before it reaches the timeline.
        if (!open_has_effect)
            return true;
        if (open_probes_first)
            state.edit_import_pending = true;
        else
            state.edit_session_open = true;
        return true;
    }
    bool EditPlayPause(QString* error) override {
        calls.append(QStringLiteral("edit.playPause"));
        return Outcome(error);
    }
    bool EditSeek(qint64 position_ms, QString* error) override {
        calls.append(QStringLiteral("edit.seek:%1").arg(position_ms));
        return Outcome(error);
    }
    bool EditSetTrimIn(qint64 position_ms, QString* error) override {
        calls.append(QStringLiteral("edit.setTrimIn:%1").arg(position_ms));
        return Outcome(error);
    }
    bool EditSetTrimOut(qint64 position_ms, QString* error) override {
        calls.append(QStringLiteral("edit.setTrimOut:%1").arg(position_ms));
        return Outcome(error);
    }
    bool EditTimelineHome(QString* error) override {
        calls.append(QStringLiteral("edit.timelineHome"));
        return Outcome(error);
    }
    bool EditTimelineEnd(QString* error) override {
        calls.append(QStringLiteral("edit.timelineEnd"));
        return Outcome(error);
    }
    bool EditClose(QString* error) override {
        calls.append(QStringLiteral("edit.close"));
        if (!Outcome(error))
            return false;
        state.edit_session_open = false;
        return true;
    }

    bool UpdateCheck(QString* error) override {
        calls.append(QStringLiteral("update.check"));
        if (!Outcome(error))
            return false;
        state.update_state = QStringLiteral("checking");
        state.update_checking = true;
        return true;
    }
    bool UpdateApply(QString* error) override {
        calls.append(QStringLiteral("update.apply"));
        if (!Outcome(error))
            return false;
        updater_launched = true;
        return true;
    }
    [[nodiscard]] QJsonObject UpdaterLaunchSnapshot() const override {
        QJsonObject json;
        json.insert(QStringLiteral("pid"), updater_launched ? 4242 : 0);
        json.insert(QStringLiteral("targetVersion"),
                    updater_launched ? QJsonValue(state.update_available_version) : QJsonValue(QJsonValue::Null));
        return json;
    }

    bool updater_launched = false;

  private:
    [[nodiscard]] static QJsonObject Marker(const QString& name) {
        return QJsonObject{{QStringLiteral("marker"), name}};
    }
    [[nodiscard]] bool Outcome(QString* error) const {
        if (allow_intents)
            return true;
        *error = QStringLiteral("refused by the application");
        return false;
    }
    [[nodiscard]] EditIntent Target(QString* error) const {
        if (!allow_intents) {
            *error = QStringLiteral("refused by the application");
            return EditIntent::Failed;
        }
        if (!targets_exist) {
            *error = QStringLiteral("no such clip");
            return EditIntent::InvalidTarget;
        }
        return EditIntent::Delivered;
    }
    EditIntent ModelEdit(QString* error) {
        const EditIntent intent = Target(error);
        if (intent != EditIntent::Delivered)
            return intent;
        ++edit_outcome.sequence;
        edit_outcome.accepted = model_accepts;
        edit_outcome.error = model_accepts ? QString() : QStringLiteral("The edit would overlap another clip");
        if (model_accepts)
            ++state.edit_workspace_revision;
        return intent;
    }
};

constexpr const char* kRunId = "run-0123456789ab";

ParsedRequest Request(const QString& command, QJsonObject params = {}, int protocol = kMinimumProtocolVersion) {
    ParsedRequest request;
    request.protocol = protocol;
    request.id = QStringLiteral("1");
    request.command = command;
    request.params = std::move(params);
    return request;
}

ParsedRequest RequestV2(const QString& command, QJsonObject params = {}) {
    return Request(command, std::move(params), 2);
}

QJsonObject Hello(LiveVerifyDispatcher& dispatcher, const QString& run_id = QString::fromLatin1(kRunId),
                  int protocol = kMinimumProtocolVersion) {
    return dispatcher.Dispatch(
        Request(QStringLiteral("system.hello"), QJsonObject{{QStringLiteral("runId"), run_id}}, protocol));
}

// Parameters good enough to pass validation for whichever command is asked for.
// Built from the descriptor table rather than from a second hand-written list --
// the same reason the validator is generic.
QJsonObject PlausibleParams(const CommandDescriptor& command) {
    QJsonObject params;
    for (const CommandParameter& parameter : command.parameters) {
        if (!parameter.required)
            continue;
        if (parameter.type == QLatin1String("enum"))
            params.insert(parameter.name, parameter.values.value(0));
        else if (parameter.type == QLatin1String("int"))
            params.insert(parameter.name, 1);
        else if (parameter.type == QLatin1String("bool"))
            params.insert(parameter.name, true);
        else
            params.insert(parameter.name, QStringLiteral("x"));
    }
    if (command.name == QLatin1String("system.hello"))
        params.insert(QStringLiteral("runId"), QString::fromLatin1(kRunId));
    // The "any" parameters whose type the command itself checks.
    if (command.name == QLatin1String("edit.setVolume"))
        params.insert(QStringLiteral("volume"), 0.5);
    if (command.name == QLatin1String("edit.confirmMonitoring"))
        params.insert(QStringLiteral("trackIds"), QJsonArray{QStringLiteral("track-1")});
    if (command.name == QLatin1String("edit.setFixedTimelineEnd"))
        params.insert(QStringLiteral("endMs"), 60000);
    return params;
}

} // namespace

// ---------------------------------------------------------------------------
// Option gate
// ---------------------------------------------------------------------------

TEST(LiveVerifyOptions, AbsentOptionMeansNoControlChannel) {
    const ControlOptions options = ParseControlOptions({QStringLiteral("exosnap.exe")});
    EXPECT_FALSE(options.requested);
    EXPECT_TRUE(options.error.isEmpty());
}

TEST(LiveVerifyOptions, DebugLikeArgumentsDoNotArmTheChannel) {
    // The gate must be the option and nothing else -- not a harness flag, not a
    // trace flag, not an environment-shaped argument.
    const ControlOptions options = ParseControlOptions(
        {QStringLiteral("exosnap.exe"), QStringLiteral("--window-trace"), QStringLiteral("--smoke-test")});
    EXPECT_FALSE(options.requested);
}

TEST(LiveVerifyOptions, MissingRunIdIsAnErrorNotASilentNormalLaunch) {
    const ControlOptions options =
        ParseControlOptions({QStringLiteral("exosnap.exe"), QStringLiteral("--live-verify-control")});
    EXPECT_TRUE(options.requested);
    EXPECT_FALSE(options.error.isEmpty());
    EXPECT_TRUE(options.run_id.isEmpty());
}

TEST(LiveVerifyOptions, MalformedRunIdIsRejected) {
    for (const QString& bad : {QStringLiteral("short"), QStringLiteral("has space here"),
                               QStringLiteral("slash/es/are/paths"), QString(65, QLatin1Char('a'))}) {
        const ControlOptions options =
            ParseControlOptions({QStringLiteral("exosnap.exe"), QStringLiteral("--live-verify-control"), bad});
        EXPECT_TRUE(options.requested);
        EXPECT_FALSE(options.error.isEmpty()) << bad.toStdString();
    }
}

TEST(LiveVerifyOptions, ValidRunIdProducesAStableEndpointName) {
    const ControlOptions options = ParseControlOptions(
        {QStringLiteral("exosnap.exe"), QStringLiteral("--live-verify-control"), QString::fromLatin1(kRunId)});
    ASSERT_TRUE(options.requested);
    EXPECT_TRUE(options.error.isEmpty());
    EXPECT_EQ(options.run_id, QString::fromLatin1(kRunId));
    EXPECT_EQ(PipeNameForRunId(options.run_id), QStringLiteral("\\\\.\\pipe\\ExoSnap.LiveVerify.run-0123456789ab"));
}

// ---------------------------------------------------------------------------
// Wire format
// ---------------------------------------------------------------------------

TEST(LiveVerifyProtocol, ParsesAWellFormedRequest) {
    ParsedRequest request;
    ParseFailure failure;
    ASSERT_TRUE(
        ParseRequest(Line(QStringLiteral(R"({"protocol":1,"id":"42","command":"record.pause","params":{"a":1}})")),
                     &request, &failure));
    EXPECT_EQ(request.protocol, 1);
    EXPECT_EQ(request.id, QStringLiteral("42"));
    EXPECT_EQ(request.command, QStringLiteral("record.pause"));
    EXPECT_EQ(request.params.value(QStringLiteral("a")).toInt(), 1);
    EXPECT_FALSE(request.include_state);
}

TEST(LiveVerifyProtocol, BothSupportedVersionsParse) {
    for (int version : {kMinimumProtocolVersion, kLatestProtocolVersion}) {
        ParsedRequest request;
        ParseFailure failure;
        ASSERT_TRUE(
            ParseRequest(Line(QStringLiteral(R"({"protocol":%1,"id":"1","command":"ui.getState"})").arg(version)),
                         &request, &failure))
            << version;
        EXPECT_EQ(request.protocol, version);
    }
}

TEST(LiveVerifyProtocol, IncludeStateIsProtocolTwoOnly) {
    ParsedRequest v2;
    ParseFailure failure;
    ASSERT_TRUE(ParseRequest(
        Line(QStringLiteral(R"({"protocol":2,"id":"1","command":"ui.getState","includeState":true})")), &v2, &failure));
    EXPECT_TRUE(v2.include_state);

    // A v1 client cannot ask for a field its envelope has no room for. Ignored,
    // not rejected: the flag is additive and refusing it would break a client
    // that merely sent one field too many.
    ParsedRequest v1;
    ASSERT_TRUE(
        ParseRequest(Line(QStringLiteral(R"({"protocol":1,"id":"1","command":"record.snapshot","includeState":true})")),
                     &v1, &failure));
    EXPECT_FALSE(v1.include_state);
}

TEST(LiveVerifyProtocol, MalformedJsonIsRejectedWithoutAnId) {
    ParsedRequest request;
    ParseFailure failure;
    EXPECT_FALSE(ParseRequest(Line(QStringLiteral("{not json")), &request, &failure));
    EXPECT_EQ(failure.code, QString::fromLatin1(error_code::kMalformedRequest));
}

TEST(LiveVerifyProtocol, UnsupportedProtocolVersionIsItsOwnErrorAndKeepsTheId) {
    ParsedRequest request;
    ParseFailure failure;
    EXPECT_FALSE(
        ParseRequest(Line(QStringLiteral(R"({"protocol":99,"id":"7","command":"system.hello"})")), &request, &failure));
    EXPECT_EQ(failure.code, QString::fromLatin1(error_code::kProtocolVersionMismatch));
    EXPECT_EQ(failure.id, QStringLiteral("7"));
    // Answered in the newest dialect this build speaks: there is no agreed one.
    EXPECT_EQ(failure.protocol, kLatestProtocolVersion);
}

TEST(LiveVerifyProtocol, MissingCommandIsRejected) {
    ParsedRequest request;
    ParseFailure failure;
    EXPECT_FALSE(ParseRequest(Line(QStringLiteral(R"({"protocol":1,"id":"7"})")), &request, &failure));
    EXPECT_EQ(failure.code, QString::fromLatin1(error_code::kMalformedRequest));
}

TEST(LiveVerifyProtocol, NonObjectParamsIsRejected) {
    ParsedRequest request;
    ParseFailure failure;
    EXPECT_FALSE(
        ParseRequest(Line(QStringLiteral(R"({"protocol":1,"id":"7","command":"x","params":5})")), &request, &failure));
    EXPECT_EQ(failure.code, QString::fromLatin1(error_code::kInvalidParams));
}

TEST(LiveVerifyProtocol, OversizedRequestIsRejectedBeforeParsing) {
    ParsedRequest request;
    ParseFailure failure;
    const QByteArray oversized(kMaxRequestBytes + 1, 'x');
    EXPECT_FALSE(ParseRequest(oversized, &request, &failure));
    EXPECT_EQ(failure.code, QString::fromLatin1(error_code::kRequestTooLarge));
}

TEST(LiveVerifyProtocol, SerializedLinesAreOneCompactObjectEach) {
    const QByteArray line = SerializeLine(MakeEvent(1, QStringLiteral("app.ready"), QJsonObject{}));
    EXPECT_TRUE(line.endsWith('\n'));
    EXPECT_EQ(line.count('\n'), 1);
    EXPECT_TRUE(QJsonDocument::fromJson(line).isObject());
}

TEST(LiveVerifyProtocol, EventsCarryTheRevisionOnlyOnProtocolTwo) {
    const QJsonObject v1 = MakeEvent(1, QStringLiteral("app.ready"), QJsonObject{}, 17);
    EXPECT_FALSE(v1.contains(QStringLiteral("stateRevision")));

    const QJsonObject v2 = MakeEvent(2, QStringLiteral("app.ready"), QJsonObject{}, 17);
    EXPECT_EQ(v2.value(QStringLiteral("stateRevision")).toInt(), 17);
}

TEST(LiveVerifyProtocol, ProtocolTwoErrorCodesFoldOntoCommandFailedForVersionOne) {
    // The refusal is still reported to a v1 client; only its category is
    // coarser, which is exactly what that client has always been told.
    for (const char* code : {error_code::kInvalidState, error_code::kBlocked, error_code::kOperationFailed}) {
        EXPECT_EQ(ErrorCodeForProtocol(QString::fromLatin1(code), 1), QString::fromLatin1(error_code::kCommandFailed));
        EXPECT_EQ(ErrorCodeForProtocol(QString::fromLatin1(code), 2), QString::fromLatin1(code));
    }
    // Everything a v1 client already knows keeps its own name in both versions.
    EXPECT_EQ(ErrorCodeForProtocol(QString::fromLatin1(error_code::kInvalidParams), 1),
              QString::fromLatin1(error_code::kInvalidParams));
}

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------

TEST(LiveVerifyDispatcher, DisabledRecordingClipRequestsNeverReachTheApplicationSource) {
    if constexpr (exosnap::product::kRecordingClipsAvailable) {
        GTEST_SKIP() << "covers the disabled product capability";
    } else {
        FakeSource source;
        source.state.can_request_clip = true;
        LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
        Hello(dispatcher, QString::fromLatin1(kRunId), 2);
        for (const auto& name : {QStringLiteral("clip.request"), QStringLiteral("clip.confirm")}) {
            const QJsonObject params{{QStringLiteral("durationSeconds"), QStringLiteral("30")},
                                     {QStringLiteral("jobId"), QStringLiteral("stale-job")},
                                     {QStringLiteral("generation"), QStringLiteral("1")}};
            const auto response = dispatcher.Dispatch(RequestV2(name, params));
            EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kUnavailable));
            EXPECT_TRUE(source.calls.isEmpty());
        }
    }
}
TEST(LiveVerifyDispatcher, RefusesEveryCommandBeforeTheHandshake) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    for (const QString& command : LiveVerifyDispatcher::CommandNames(kLatestProtocolVersion)) {
        if (command == QStringLiteral("system.hello"))
            continue;
        const QJsonObject response = dispatcher.Dispatch(RequestV2(command));
        EXPECT_FALSE(Ok(response)) << command.toStdString();
        EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kHandshakeRequired)) << command.toStdString();
    }
    EXPECT_TRUE(source.calls.isEmpty());
}

TEST(LiveVerifyDispatcher, HelloWithTheWrongRunIdPoisonsTheConnection) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    const QJsonObject response = Hello(dispatcher, QStringLiteral("some-other-run"));
    EXPECT_FALSE(Ok(response));
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kRunIdMismatch));
    EXPECT_TRUE(dispatcher.connectionPoisoned());
    EXPECT_FALSE(dispatcher.handshakeComplete());
}

TEST(LiveVerifyDispatcher, HelloWithoutARunIdIsInvalidParams) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    const QJsonObject response = dispatcher.Dispatch(Request(QStringLiteral("system.hello")));
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kInvalidParams));
    EXPECT_FALSE(dispatcher.connectionPoisoned());
}

TEST(LiveVerifyDispatcher, HelloAnswersWithIdentityAndTheExactCommandSurface) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    const QJsonObject response = Hello(dispatcher);
    ASSERT_TRUE(Ok(response));
    const QJsonObject result = response.value(QStringLiteral("result")).toObject();
    EXPECT_EQ(result.value(QStringLiteral("protocol")).toInt(), 1);
    EXPECT_EQ(result.value(QStringLiteral("runId")).toString(), QString::fromLatin1(kRunId));
    EXPECT_EQ(result.value(QStringLiteral("productVersion")).toString(), QStringLiteral("0.9.0-test"));
    EXPECT_EQ(result.value(QStringLiteral("commands")).toArray().size(), LiveVerifyDispatcher::CommandNames(1).size());
    EXPECT_EQ(result.value(QStringLiteral("events")).toArray().size(), LiveVerifyDispatcher::EventNames(1).size());
    EXPECT_TRUE(dispatcher.handshakeComplete());
    EXPECT_EQ(dispatcher.negotiatedProtocol(), 1);
}

TEST(LiveVerifyDispatcher, ProtocolOneHelloIsUnchangedByProtocolTwoExisting) {
    // The v1 payload gains nothing: same four keys beside the identity, no
    // stateRevision, no errorCodes, no supportedProtocols.
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    const QJsonObject response = Hello(dispatcher);
    ASSERT_TRUE(Ok(response));
    EXPECT_FALSE(response.contains(QStringLiteral("stateRevision")));
    EXPECT_FALSE(response.contains(QStringLiteral("settled")));

    const QJsonObject result = response.value(QStringLiteral("result")).toObject();
    EXPECT_FALSE(result.contains(QStringLiteral("errorCodes")));
    EXPECT_FALSE(result.contains(QStringLiteral("supportedProtocols")));
}

TEST(LiveVerifyDispatcher, ProtocolTwoHelloCarriesDiscoveryAndTheRevision) {
    FakeSource source;
    source.revision = 42;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    const QJsonObject response = Hello(dispatcher, QString::fromLatin1(kRunId), 2);
    ASSERT_TRUE(Ok(response));
    EXPECT_EQ(response.value(QStringLiteral("stateRevision")).toInt(), 42);
    EXPECT_EQ(dispatcher.negotiatedProtocol(), 2);

    const QJsonObject result = response.value(QStringLiteral("result")).toObject();
    EXPECT_EQ(result.value(QStringLiteral("protocol")).toInt(), 2);
    EXPECT_EQ(result.value(QStringLiteral("errorCodes")).toArray().size(), AllErrorCodes().size());
    EXPECT_EQ(result.value(QStringLiteral("supportedProtocols")).toArray().size(), 2);
}

TEST(LiveVerifyDispatcher, HelloTwiceOnOneConnectionIsRefused) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher)));
    EXPECT_EQ(ErrorCode(Hello(dispatcher)), QString::fromLatin1(error_code::kAlreadyHandshaken));
}

TEST(LiveVerifyDispatcher, SwitchingProtocolMidConnectionIsFatal) {
    // The handshake fixes the dialect. A client that switches is either two
    // clients on one credential or a bug that would otherwise show up as fields
    // quietly missing from half the transcript.
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));
    const QJsonObject response = dispatcher.Dispatch(Request(QStringLiteral("record.snapshot")));
    EXPECT_FALSE(Ok(response));
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kProtocolVersionMismatch));
    EXPECT_TRUE(dispatcher.connectionPoisoned());
}

TEST(LiveVerifyDispatcher, ResetSessionRequiresANewHandshake) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher)));
    dispatcher.ResetSession();
    EXPECT_EQ(ErrorCode(dispatcher.Dispatch(Request(QStringLiteral("record.snapshot")))),
              QString::fromLatin1(error_code::kHandshakeRequired));
    EXPECT_EQ(dispatcher.negotiatedProtocol(), kMinimumProtocolVersion);
}

// ---------------------------------------------------------------------------
// Allowlist and versioning
// ---------------------------------------------------------------------------

TEST(LiveVerifyDispatcher, EveryListedCommandIsActuallyImplemented) {
    // Guards the exact drift the "listed but not implemented" branch exists for.
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    for (const CommandDescriptor& command : AllCommands()) {
        if (command.name == QStringLiteral("system.hello"))
            continue;
        source.state = PermissiveState();
        if (command.name == QStringLiteral("edit.play"))
            source.state.edit_playback = QStringLiteral("paused");
        // Two preconditions genuinely disagree about where the user has to be:
        // the source picker belongs to Record, and the scroll surfaces do not
        // exist there. Put each command in the page its own policy names.
        if (command.name.startsWith(QStringLiteral("ui.reveal")) ||
            command.name.startsWith(QStringLiteral("ui.scroll"))) {
            // PlausibleParams picks the first enum value for `surface`, which is
            // settings -- and a surface is only addressable while it is the
            // current page.
            source.state.page = QString::fromLatin1(page_name::kSettings);
        }
        // Settings, profiles and the in-depth switch are locked while a recording
        // is in flight, the way the Settings controls are.
        if (command.name.startsWith(QStringLiteral("settings.")) ||
            command.name.startsWith(QStringLiteral("audio.routing.")) ||
            command.name.startsWith(QStringLiteral("profiles.")) ||
            command.name == QStringLiteral("diagnostics.setInDepth")) {
            source.state.recording_state = QStringLiteral("Ready");
            source.state.profile_built_in = false;
        }
        // The blocking-surface commands are the inverse of every other one here:
        // they REQUIRE their surface, because they are its own buttons.
        if (command.name.startsWith(QStringLiteral("recovery."))) {
            source.state.blocking_surface = QString::fromLatin1(blocking_surface_name::kRecovery);
            source.state.recovery_candidate_count = 1;
        } else if (command.name.startsWith(QStringLiteral("crashReport."))) {
            source.state.blocking_surface = QString::fromLatin1(blocking_surface_name::kCrashReport);
        } else if (command.name.startsWith(QStringLiteral("recordingError."))) {
            source.state.blocking_surface = QString::fromLatin1(blocking_surface_name::kRecordingError);
            source.state.recording_error_can_send_report = true;
        }
        if (command.name == QStringLiteral("export.cancel"))
            source.state.edit_export_running = true;
        // The review dialog's buttons need the review; reset needs a finished run.
        if (command.name == QStringLiteral("export.confirm") || command.name == QStringLiteral("export.discard"))
            source.state.edit_export_state = QStringLiteral("review");
        if (command.name == QStringLiteral("export.reset"))
            source.state.edit_export_state = QStringLiteral("completed");
        // The side panels exist on the Edit page only.
        if (command.name == QStringLiteral("edit.setPanel") || command.name == QStringLiteral("edit.setSourceTab"))
            source.state.page = QString::fromLatin1(page_name::kEdit);
        const QJsonObject response = dispatcher.Dispatch(RequestV2(command.name, PlausibleParams(command)));
        bool clip_refused = false;
        if constexpr (!exosnap::product::kRecordingClipsAvailable)
            clip_refused =
                command.name == QLatin1String("clip.request") || command.name == QLatin1String("clip.confirm");
        if (clip_refused) {
            EXPECT_FALSE(Ok(response));
            EXPECT_EQ(ErrorCode(response), QLatin1String(error_code::kUnavailable));
        } else {
            EXPECT_TRUE(Ok(response)) << command.name.toStdString() << ": " << ErrorCode(response).toStdString();
        }
    }
}

TEST(LiveVerifyDispatcher, ProtocolTwoCommandsAreUnknownToAProtocolOneClient) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher)));

    for (const CommandDescriptor& command : AllCommands()) {
        if (command.minimum_protocol <= 1)
            continue;
        const QJsonObject response = dispatcher.Dispatch(Request(command.name, PlausibleParams(command)));
        EXPECT_FALSE(Ok(response)) << command.name.toStdString();
        EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kUnknownCommand)) << command.name.toStdString();
    }
    EXPECT_TRUE(source.calls.isEmpty());
}

TEST(LiveVerifyDispatcher, TheProtocolOneCommandSurfaceIsExactlyTheOriginalNineteen) {
    // The v1 contract, written down. A command added to the table with
    // minimum_protocol 1 by accident would widen a surface that is supposed to
    // be frozen, and this is what says so.
    const QStringList expected = {QStringLiteral("app.snapshot"),        QStringLiteral("diagnostics.snapshot"),
                                  QStringLiteral("editor.snapshot"),     QStringLiteral("overlay.snapshot"),
                                  QStringLiteral("preview.snapshot"),    QStringLiteral("record.captureFrame"),
                                  QStringLiteral("record.pause"),        QStringLiteral("record.result"),
                                  QStringLiteral("record.resume"),       QStringLiteral("record.selectTarget"),
                                  QStringLiteral("record.snapshot"),     QStringLiteral("record.split"),
                                  QStringLiteral("record.start"),        QStringLiteral("record.stop"),
                                  QStringLiteral("system.capabilities"), QStringLiteral("system.hello"),
                                  QStringLiteral("system.snapshot"),     QStringLiteral("webcam.overlay.set"),
                                  QStringLiteral("window.moveToScreen"), QStringLiteral("window.snapshot")};
    EXPECT_EQ(expected.size(), 20);
    EXPECT_EQ(LiveVerifyDispatcher::CommandNames(1), expected);
}

TEST(LiveVerifyDispatcher, UnknownCommandsFailClosed) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    // Nothing generic is reachable: no object lookup, no method invocation, no
    // property write, no shell, no file access -- and no prefix match onto a
    // command that does exist.
    for (const QString& command :
         {QStringLiteral("qobject.invoke"), QStringLiteral("qml.setProperty"), QStringLiteral("shell.exec"),
          QStringLiteral("file.read"), QStringLiteral("eval"), QStringLiteral("record"),
          QStringLiteral("record.startNow"), QStringLiteral("RECORD.START"), QStringLiteral("record.start "),
          QStringLiteral("ui.navigateTo"), QStringLiteral("edit."), QStringLiteral("notification.triggerAction")}) {
        const QJsonObject response = dispatcher.Dispatch(RequestV2(command));
        EXPECT_FALSE(Ok(response)) << command.toStdString();
        EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kUnknownCommand)) << command.toStdString();
    }
    EXPECT_TRUE(source.calls.isEmpty());
}

TEST(LiveVerifyDispatcher, SnapshotCommandsRouteToTheMatchingSource) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher)));

    const std::pair<QString, QString> expectations[] = {
        {QStringLiteral("system.snapshot"), QStringLiteral("system")},
        {QStringLiteral("app.snapshot"), QStringLiteral("app")},
        {QStringLiteral("window.snapshot"), QStringLiteral("window")},
        {QStringLiteral("preview.snapshot"), QStringLiteral("preview")},
        {QStringLiteral("record.snapshot"), QStringLiteral("record")},
        {QStringLiteral("record.result"), QStringLiteral("record.result")},
        {QStringLiteral("overlay.snapshot"), QStringLiteral("overlay")},
        {QStringLiteral("editor.snapshot"), QStringLiteral("editor")},
        {QStringLiteral("diagnostics.snapshot"), QStringLiteral("diagnostics")},
    };
    for (const auto& [command, marker] : expectations) {
        const QJsonObject response = dispatcher.Dispatch(Request(command));
        ASSERT_TRUE(Ok(response)) << command.toStdString();
        EXPECT_EQ(response.value(QStringLiteral("result")).toObject().value(QStringLiteral("marker")).toString(),
                  marker);
    }
}

TEST(LiveVerifyDispatcher, ObservabilityQueriesRouteToTheMatchingSurface) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const std::pair<QString, QString> expectations[] = {
        {QStringLiteral("pipeline.snapshot"), QStringLiteral("pipeline")},
        {QStringLiteral("settings.snapshot"), QStringLiteral("settings")},
        {QStringLiteral("diagnostics.results"), QStringLiteral("diagnostics.results")},
        {QStringLiteral("environment.snapshot"), QStringLiteral("environment")},
        {QStringLiteral("windows.snapshot"), QStringLiteral("windows")},
        {QStringLiteral("events.recent"), QStringLiteral("events")},
        {QStringLiteral("session.latest"), QStringLiteral("session")},
    };
    for (const auto& [command, marker] : expectations) {
        const QJsonObject response = dispatcher.Dispatch(RequestV2(command));
        ASSERT_TRUE(Ok(response)) << command.toStdString();
        EXPECT_EQ(response.value(QStringLiteral("result")).toObject().value(QStringLiteral("marker")).toString(),
                  marker);
        // Every one of them is an observation, so none may carry the flag that
        // says an action's postcondition holds.
        EXPECT_FALSE(response.contains(QStringLiteral("settled"))) << command.toStdString();
    }
}

TEST(LiveVerifyDispatcher, AppIdentityAnswersTheSameIdentityTheHandshakeCarries) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    const QJsonObject hello = Hello(dispatcher, QString::fromLatin1(kRunId), 2);
    ASSERT_TRUE(Ok(hello));

    const QJsonObject response = dispatcher.Dispatch(RequestV2(QStringLiteral("app.identity")));
    ASSERT_TRUE(Ok(response));

    // One identity, two ways to read it. The handshake merges it into its result
    // alongside the negotiated protocol and the command surface, so the assertion
    // is field-wise rather than object equality -- but every field has to agree,
    // because a second identity would be a second thing to keep in step with the
    // build.
    const QJsonObject identity = response.value(QStringLiteral("result")).toObject();
    const QJsonObject hello_result = hello.value(QStringLiteral("result")).toObject();
    ASSERT_FALSE(identity.isEmpty());
    for (auto it = identity.begin(); it != identity.end(); ++it)
        EXPECT_EQ(hello_result.value(it.key()), it.value()) << it.key().toStdString();
}

TEST(LiveVerifyDispatcher, SessionGetPassesTheIdThroughAndRefusesAnEmptyOne) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    QJsonObject params;
    params.insert(QStringLiteral("recordingSessionId"), QStringLiteral("rec-42"));
    const QJsonObject response = dispatcher.Dispatch(RequestV2(QStringLiteral("session.get"), params));
    ASSERT_TRUE(Ok(response));
    EXPECT_EQ(response.value(QStringLiteral("result")).toObject().value(QStringLiteral("requestedId")).toString(),
              QStringLiteral("rec-42"));

    // session.latest is the SAME surface with no id, not a second command with
    // its own report.
    const QJsonObject latest = dispatcher.Dispatch(RequestV2(QStringLiteral("session.latest")));
    ASSERT_TRUE(Ok(latest));
    EXPECT_TRUE(
        latest.value(QStringLiteral("result")).toObject().value(QStringLiteral("requestedId")).toString().isEmpty());

    QJsonObject empty;
    empty.insert(QStringLiteral("recordingSessionId"), QString());
    EXPECT_EQ(ErrorCode(dispatcher.Dispatch(RequestV2(QStringLiteral("session.get"), empty))),
              QString::fromLatin1(error_code::kInvalidParams));
}

TEST(LiveVerifyDispatcher, AMalformedEventFilterIsAClientErrorAndNotAnEmptyResult) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    QJsonObject params;
    params.insert(QStringLiteral("severity"), QStringLiteral("nonsense"));
    const QJsonObject response = dispatcher.Dispatch(RequestV2(QStringLiteral("events.recent"), params));
    // Answering `ok` with an empty list would let a check that filters for
    // errors pass because its filter never matched anything.
    EXPECT_FALSE(Ok(response));
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kInvalidParams));
}

TEST(LiveVerifyDispatcher, ObservabilityQueriesAreProtocolTwoOnly) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 1)));

    for (const QString& command :
         {QStringLiteral("app.identity"), QStringLiteral("pipeline.snapshot"), QStringLiteral("settings.snapshot"),
          QStringLiteral("diagnostics.results"), QStringLiteral("environment.snapshot"),
          QStringLiteral("windows.snapshot"), QStringLiteral("events.recent"), QStringLiteral("session.latest"),
          QStringLiteral("session.get")}) {
        EXPECT_EQ(ErrorCode(dispatcher.Dispatch(Request(command))), QString::fromLatin1(error_code::kUnknownCommand))
            << command.toStdString();
    }
}

TEST(LiveVerifyDispatcher, SettingsSetAnswersWithTheReconciledValueAndNotTheRequestedOne) {
    FakeSource source;
    source.state.recording_state = QStringLiteral("Ready");
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    QJsonObject params;
    params.insert(QStringLiteral("key"), QStringLiteral("video.container"));
    params.insert(QStringLiteral("value"), QStringLiteral("MP4"));
    const QJsonObject response = dispatcher.Dispatch(RequestV2(QStringLiteral("settings.set"), params));
    ASSERT_TRUE(Ok(response));

    EXPECT_EQ(source.last_settings_key, QStringLiteral("video.container"));
    EXPECT_EQ(source.last_settings_value.toString(), QStringLiteral("MP4"));
    const QJsonObject result = response.value(QStringLiteral("result")).toObject();
    // The response is a READ BACK, not an echo. MP4 turns an AV1 request into
    // H.264, and a caller has to be able to see that without a second round trip
    // -- so both the read-back and what was asked for are present.
    EXPECT_EQ(result.value(QStringLiteral("marker")).toString(), QStringLiteral("settings.get"));
    EXPECT_EQ(result.value(QStringLiteral("requestedKey")).toString(), QStringLiteral("video.container"));
    EXPECT_EQ(result.value(QStringLiteral("requested")).toString(), QStringLiteral("MP4"));
}

TEST(LiveVerifyDispatcher, WebcamOverlaySetAnswersWithTheAppliedStateAndNotTheRequest) {
    FakeSource source;
    source.state.recording_state = QStringLiteral("Recording");
    // What a session installs after clamping. The request below asks for a
    // rectangle that runs off the frame; a caller that compared a recording
    // against what it asked for would read the clamp as a defect.
    source.applied_overlay.insert(QStringLiteral("x"), 0.5);
    source.applied_overlay.insert(QStringLiteral("width"), 0.5);
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    QJsonObject params;
    params.insert(QStringLiteral("x"), 0.9);
    params.insert(QStringLiteral("width"), 0.5);
    const QJsonObject response = dispatcher.Dispatch(RequestV2(QStringLiteral("webcam.overlay.set"), params));
    ASSERT_TRUE(Ok(response));

    const QJsonObject result = response.value(QStringLiteral("result")).toObject();
    const QJsonObject applied = result.value(QStringLiteral("applied")).toObject();
    EXPECT_DOUBLE_EQ(applied.value(QStringLiteral("x")).toDouble(), 0.5);
    EXPECT_DOUBLE_EQ(result.value(QStringLiteral("requested")).toObject().value(QStringLiteral("x")).toDouble(), 0.9)
        << "the request stays visible beside the applied state, not in place of it";
    EXPECT_EQ(result.value(QStringLiteral("appliedSequence")).toInt(), 1);
    EXPECT_GT(result.value(QStringLiteral("appliedQpc100ns")).toDouble(), 0.0);
}

TEST(LiveVerifyDispatcher, TheAppliedSequenceAdvancesOnlyForAnUpdateThatReachedTheSession) {
    FakeSource source;
    source.state.recording_state = QStringLiteral("Recording");
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    QJsonObject params;
    params.insert(QStringLiteral("x"), 0.1);
    const QJsonObject first = dispatcher.Dispatch(RequestV2(QStringLiteral("webcam.overlay.set"), params));
    ASSERT_TRUE(Ok(first));
    const QJsonObject second = dispatcher.Dispatch(RequestV2(QStringLiteral("webcam.overlay.set"), params));
    ASSERT_TRUE(Ok(second));

    // Two identical requests, two sequences. That is what lets a caller tell a
    // second request from one that was dropped -- the effect repeats, the answer
    // does not.
    EXPECT_EQ(first.value(QStringLiteral("result")).toObject().value(QStringLiteral("appliedSequence")).toInt(), 1);
    EXPECT_EQ(second.value(QStringLiteral("result")).toObject().value(QStringLiteral("appliedSequence")).toInt(), 2);

    source.allow_intents = false;
    const QJsonObject refused = dispatcher.Dispatch(RequestV2(QStringLiteral("webcam.overlay.set"), params));
    EXPECT_FALSE(Ok(refused));
    EXPECT_EQ(source.overlay_sequence, 2u) << "a refused apply must not advance the sequence";
}

TEST(LiveVerifyDispatcher, WebcamOverlaySetIsRefusedWhenThereIsNoRecordingToApplyItTo) {
    // The mirror image of settings.set: that one is refused DURING a recording,
    // this one outside it. Answering from a stored setting would report a state
    // no frame was composited with.
    FakeSource source;
    source.state.recording_state = QStringLiteral("Ready");
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    QJsonObject params;
    params.insert(QStringLiteral("x"), 0.1);
    const QJsonObject response = dispatcher.Dispatch(RequestV2(QStringLiteral("webcam.overlay.set"), params));
    EXPECT_FALSE(Ok(response));
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kInvalidState));
    EXPECT_FALSE(source.calls.contains(QStringLiteral("webcam.overlay.set")))
        << "refused before the intent ran, not accepted and then ignored";
}

TEST(LiveVerifyDispatcher, OnlyTheNamedFieldsReachTheSource) {
    // A caller moving the rectangle must not silently reset an opacity it never
    // mentioned. The dispatcher passes the parameters through untouched; the
    // merge against the current state is the source's job, and this pins that the
    // dispatcher does not invent the absent keys.
    FakeSource source;
    source.state.recording_state = QStringLiteral("Recording");
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    QJsonObject params;
    params.insert(QStringLiteral("x"), 0.25);
    params.insert(QStringLiteral("y"), 0.75);
    ASSERT_TRUE(Ok(dispatcher.Dispatch(RequestV2(QStringLiteral("webcam.overlay.set"), params))));

    EXPECT_TRUE(source.last_overlay_fields.contains(QStringLiteral("x")));
    EXPECT_TRUE(source.last_overlay_fields.contains(QStringLiteral("y")));
    EXPECT_FALSE(source.last_overlay_fields.contains(QStringLiteral("opacity")));
    EXPECT_FALSE(source.last_overlay_fields.contains(QStringLiteral("mirror")));
}

TEST(LiveVerifyDispatcher, AnAcknowledgementIsNotEvidenceThatTheRecorderChanged) {
    // The falsification 091-55 rests on. A control channel that answers
    // "applied B, sequence N" while nothing reached the recorder is exactly the
    // false success the whole acknowledgement design exists to make visible, and
    // the answer alone cannot distinguish the two cases -- which is why the
    // verdict is decided by the encoded frames and the acknowledgement is only
    // the lower time bound they are judged against.
    FakeSource source;
    source.state.recording_state = QStringLiteral("Recording");
    source.applied_overlay.insert(QStringLiteral("x"), 0.5);
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    QJsonObject params;
    params.insert(QStringLiteral("x"), 0.5);
    const QJsonObject response = dispatcher.Dispatch(RequestV2(QStringLiteral("webcam.overlay.set"), params));

    // The fake never touched a recorder, and the response is indistinguishable
    // from one that did.
    ASSERT_TRUE(Ok(response));
    const QJsonObject result = response.value(QStringLiteral("result")).toObject();
    EXPECT_EQ(result.value(QStringLiteral("appliedSequence")).toInt(), 1);
    EXPECT_DOUBLE_EQ(result.value(QStringLiteral("applied")).toObject().value(QStringLiteral("x")).toDouble(), 0.5);
}

TEST(LiveVerifyDispatcher, SettingsSetIsRefusedWhileARecordingIsInFlight) {
    FakeSource source;
    source.state.recording_state = QStringLiteral("Recording");
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    QJsonObject params;
    params.insert(QStringLiteral("key"), QStringLiteral("video.container"));
    params.insert(QStringLiteral("value"), QStringLiteral("MP4"));
    const QJsonObject response = dispatcher.Dispatch(RequestV2(QStringLiteral("settings.set"), params));
    EXPECT_FALSE(Ok(response));
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kBlocked));
    // Refused BEFORE the intent ran: an accepted-then-ignored write is the false
    // success this whole precondition design exists to remove.
    EXPECT_FALSE(source.calls.contains(QStringLiteral("settings.set:video.container")));
}

TEST(LiveVerifyDispatcher, AnUnknownSettingsKeyIsAClientErrorOnBothRead) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    QJsonObject params;
    params.insert(QStringLiteral("key"), QStringLiteral("nope"));
    const QJsonObject response = dispatcher.Dispatch(RequestV2(QStringLiteral("settings.get"), params));
    EXPECT_FALSE(Ok(response));
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kInvalidParams));
}

TEST(LiveVerifyDispatcher, SettingsSetRequiresBothAKeyAndAValue) {
    FakeSource source;
    source.state.recording_state = QStringLiteral("Ready");
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    QJsonObject no_value;
    no_value.insert(QStringLiteral("key"), QStringLiteral("video.container"));
    EXPECT_EQ(ErrorCode(dispatcher.Dispatch(RequestV2(QStringLiteral("settings.set"), no_value))),
              QString::fromLatin1(error_code::kInvalidParams));

    // `value` is declared "any" because its type depends on the key -- but "any"
    // still has to be PRESENT, or a write with no value would read as a write of
    // whatever the key already holds.
    QJsonObject no_key;
    no_key.insert(QStringLiteral("value"), true);
    EXPECT_EQ(ErrorCode(dispatcher.Dispatch(RequestV2(QStringLiteral("settings.set"), no_key))),
              QString::fromLatin1(error_code::kInvalidParams));
}

TEST(LiveVerifyDispatcher, ProfileSelectionAssertsItsOwnPostcondition) {
    FakeSource source;
    source.state.recording_state = QStringLiteral("Ready");
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    QJsonObject params;
    params.insert(QStringLiteral("id"), QStringLiteral("preset.mine"));
    const QJsonObject response = dispatcher.Dispatch(RequestV2(QStringLiteral("profiles.select"), params));
    ASSERT_TRUE(Ok(response));
    EXPECT_TRUE(source.calls.contains(QStringLiteral("profiles.select:preset.mine")));
    EXPECT_EQ(response.value(QStringLiteral("result")).toObject().value(QStringLiteral("marker")).toString(),
              QStringLiteral("profiles"));

    // A source that accepts the selection without making it is an operational
    // failure, not a success -- the command is declared synchronous.
    FakeSource silent;
    silent.state.recording_state = QStringLiteral("Ready");
    silent.allow_intents = true;
    LiveVerifyDispatcher second(&silent, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(second, QString::fromLatin1(kRunId), 2)));
    silent.state.profile_id = QStringLiteral("preset.other");
    // Pin the id so the fake's own assignment cannot satisfy the postcondition.
    silent.allow_intents = false;
    EXPECT_FALSE(Ok(second.Dispatch(RequestV2(QStringLiteral("profiles.select"), params))));
}

TEST(LiveVerifyDispatcher, CountdownCancelIsSynchronousAndProvesTheCountdownEnded) {
    FakeSource source;
    source.state.recording_state = QStringLiteral("Countdown");
    source.state.countdown_active = true;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(RequestV2(QStringLiteral("record.cancelCountdown")));
    ASSERT_TRUE(Ok(response));
    EXPECT_TRUE(source.calls.contains(QStringLiteral("cancelCountdown")));
    // Declared synchronous, so it settles in this very response -- and it only
    // gets to say so because the dispatcher re-read the state and found the
    // countdown gone.
    EXPECT_TRUE(response.value(QStringLiteral("settled")).toBool());

    // With no countdown there is nothing to cancel, and the refusal is about the
    // state rather than about a product rule.
    FakeSource ready;
    ready.state.countdown_active = false;
    LiveVerifyDispatcher second(&ready, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(second, QString::fromLatin1(kRunId), 2)));
    const QJsonObject refused = second.Dispatch(RequestV2(QStringLiteral("record.cancelCountdown")));
    EXPECT_EQ(ErrorCode(refused), QString::fromLatin1(error_code::kInvalidState));
}

TEST(LiveVerifyDispatcher, ABlockingSurfaceActionMustLeaveTheSurfaceClosed) {
    FakeSource source;
    source.state.blocking_surface = QString::fromLatin1(blocking_surface_name::kRecordingError);
    source.state.recording_error_can_send_report = true;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(RequestV2(QStringLiteral("recordingError.dismiss")));
    ASSERT_TRUE(Ok(response));
    EXPECT_TRUE(source.calls.contains(QStringLiteral("recordingError.dismiss")));
    EXPECT_TRUE(response.value(QStringLiteral("result")).toObject().value(QStringLiteral("blockingSurface")).isNull());

    // A surface that stays up after its own button was pressed is an operational
    // failure. Nothing else in the protocol would notice it.
    FakeSource stuck;
    stuck.allow_intents = false;
    stuck.state.blocking_surface = QString::fromLatin1(blocking_surface_name::kRecordingError);
    LiveVerifyDispatcher second(&stuck, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(second, QString::fromLatin1(kRunId), 2)));
    EXPECT_FALSE(Ok(second.Dispatch(RequestV2(QStringLiteral("recordingError.dismiss")))));
}

TEST(LiveVerifyDispatcher, NotificationActionsAddressAnEntryBySequenceAndNotByRow) {
    FakeSource source;
    source.state.notification_count = 3;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    QJsonObject params;
    params.insert(QStringLiteral("sequence"), 42);
    ASSERT_TRUE(Ok(dispatcher.Dispatch(RequestV2(QStringLiteral("notification.dismiss"), params))));
    EXPECT_TRUE(source.calls.contains(QStringLiteral("notification.dismiss:42")));

    params.insert(QStringLiteral("action"), QStringLiteral("secondary"));
    const QJsonObject invoked = dispatcher.Dispatch(RequestV2(QStringLiteral("notification.invokeAction"), params));
    ASSERT_TRUE(Ok(invoked));
    EXPECT_TRUE(source.calls.contains(QStringLiteral("notification.invokeAction:42/secondary")));
    // An action navigates, opens Explorer or relaunches elevated. None of that
    // is observable in the response, so it must not claim to be settled.
    EXPECT_FALSE(invoked.value(QStringLiteral("settled")).toBool());

    // The default slot is the primary button, so a client that only has one
    // action does not have to name it.
    QJsonObject bare;
    bare.insert(QStringLiteral("sequence"), 7);
    ASSERT_TRUE(Ok(dispatcher.Dispatch(RequestV2(QStringLiteral("notification.invokeAction"), bare))));
    EXPECT_TRUE(source.calls.contains(QStringLiteral("notification.invokeAction:7/primary")));
}

TEST(LiveVerifyDispatcher, ExportAndDiagnosticsRunAreAcceptedWithoutClaimingCompletion) {
    FakeSource source;
    source.state.edit_session_open = true;
    source.state.can_export = true;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject started = dispatcher.Dispatch(RequestV2(QStringLiteral("export.start")));
    ASSERT_TRUE(Ok(started));
    // A remux runs on its own thread. `ok` means accepted; the export state is
    // what a client waits on.
    EXPECT_FALSE(started.value(QStringLiteral("settled")).toBool());

    const QJsonObject checked = dispatcher.Dispatch(RequestV2(QStringLiteral("diagnostics.run")));
    ASSERT_TRUE(Ok(checked));
    EXPECT_FALSE(checked.value(QStringLiteral("settled")).toBool());
    EXPECT_TRUE(source.calls.contains(QStringLiteral("diagnostics.run")));

    // A second check while one is running is refused rather than queued.
    source.state.diagnostics_checking = true;
    EXPECT_EQ(ErrorCode(dispatcher.Dispatch(RequestV2(QStringLiteral("diagnostics.run")))),
              QString::fromLatin1(error_code::kInvalidState));
}

// A quit is only asked for here; the answer comes from the close guards and the
// completion is the process ending, so an accepted quit never claims to be settled
// and a guard's refusal reaches the client as a refusal, not as success.
TEST(LiveVerifyDispatcher, AppQuitRoutesThroughTheCloseGuardsWithoutClaimingCompletion) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject accepted = dispatcher.Dispatch(RequestV2(QStringLiteral("app.quit")));
    ASSERT_TRUE(Ok(accepted));
    EXPECT_FALSE(accepted.value(QStringLiteral("settled")).toBool());
    EXPECT_TRUE(source.calls.contains(QStringLiteral("app.quit")));

    source.allow_intents = false;
    const QJsonObject refused = dispatcher.Dispatch(RequestV2(QStringLiteral("app.quit")));
    EXPECT_FALSE(Ok(refused));
}

// The in-depth diagnostics switch has no settings key any more, so this command
// is the whole surface a check has for it. Synchronous: the switch either moved
// or the intent was refused, and there is nothing to wait for either way.
TEST(LiveVerifyDispatcher, SetInDepthCarriesTheFlagAndSettlesImmediately) {
    FakeSource source;
    source.state.recording_state = QStringLiteral("Ready");
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    QJsonObject params;
    params.insert(QStringLiteral("enabled"), true);
    const QJsonObject enabled = dispatcher.Dispatch(RequestV2(QStringLiteral("diagnostics.setInDepth"), params));
    ASSERT_TRUE(Ok(enabled));
    EXPECT_TRUE(enabled.value(QStringLiteral("settled")).toBool());
    EXPECT_TRUE(source.calls.contains(QStringLiteral("diagnostics.setInDepth:1")));

    params.insert(QStringLiteral("enabled"), false);
    ASSERT_TRUE(Ok(dispatcher.Dispatch(RequestV2(QStringLiteral("diagnostics.setInDepth"), params))));
    EXPECT_TRUE(source.calls.contains(QStringLiteral("diagnostics.setInDepth:0")));

    // The switch is disabled while a recording is in flight, and the command is
    // refused there rather than reaching the source at all.
    source.state.recording_state = QStringLiteral("Recording");
    const int calls_before = source.calls.size();
    EXPECT_EQ(ErrorCode(dispatcher.Dispatch(RequestV2(QStringLiteral("diagnostics.setInDepth"), params))),
              QString::fromLatin1(error_code::kBlocked));
    EXPECT_EQ(source.calls.size(), calls_before);
}

TEST(LiveVerifyDispatcher, ReadOnlyCommandsCarryNoSettledFlag) {
    // `settled` says an ACTION's postcondition holds. A query that reported it
    // would let a client read "settled" off a snapshot and conclude something
    // completed.
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));
    const QJsonObject response = dispatcher.Dispatch(RequestV2(QStringLiteral("record.snapshot")));
    ASSERT_TRUE(Ok(response));
    EXPECT_FALSE(response.contains(QStringLiteral("settled")));
    EXPECT_TRUE(response.contains(QStringLiteral("stateRevision")));
}

TEST(LiveVerifyDispatcher, RecordIntentsRouteToTheMatchingApplicationIntent) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher)));

    for (const QString& command :
         {QStringLiteral("record.start"), QStringLiteral("record.pause"), QStringLiteral("record.resume"),
          QStringLiteral("record.stop"), QStringLiteral("record.split"), QStringLiteral("record.captureFrame")}) {
        ASSERT_TRUE(Ok(dispatcher.Dispatch(Request(command)))) << command.toStdString();
    }
    EXPECT_EQ(source.calls,
              QStringList({QStringLiteral("start"), QStringLiteral("pause"), QStringLiteral("resume"),
                           QStringLiteral("stop"), QStringLiteral("split"), QStringLiteral("captureFrame")}));
}

TEST(LiveVerifyDispatcher, TransportIntentsAreNeverReportedAsSettled) {
    // Start and stop are asynchronous by nature; the returned snapshot describes
    // the moment the intent was accepted, not its effect.
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));
    for (const QString& command : {QStringLiteral("record.start"), QStringLiteral("record.stop")}) {
        const QJsonObject response = dispatcher.Dispatch(RequestV2(command));
        ASSERT_TRUE(Ok(response)) << command.toStdString();
        EXPECT_FALSE(response.value(QStringLiteral("settled")).toBool()) << command.toStdString();
    }
}

TEST(LiveVerifyDispatcher, ARefusedIntentIsAReportableFailureNotACrash) {
    FakeSource source;
    source.allow_intents = false;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher)));

    const QJsonObject response = dispatcher.Dispatch(Request(QStringLiteral("record.start")));
    EXPECT_FALSE(Ok(response));
    // Protocol 1 still calls every refusal command_failed.
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kCommandFailed));
    EXPECT_EQ(ErrorOf(response).value(QStringLiteral("message")).toString(),
              QStringLiteral("refused by the application"));
}

TEST(LiveVerifyDispatcher, AnIntentThatFailsWithTheStateStillPermittingItIsOperationFailed) {
    FakeSource source;
    source.allow_intents = false;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(RequestV2(QStringLiteral("record.start")));
    EXPECT_FALSE(Ok(response));
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kOperationFailed));
}

TEST(LiveVerifyDispatcher, ParameterValidationRunsBeforeTheIntent) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    EXPECT_EQ(ErrorCode(dispatcher.Dispatch(RequestV2(QStringLiteral("window.moveToScreen")))),
              QString::fromLatin1(error_code::kInvalidParams));
    EXPECT_EQ(
        ErrorCode(dispatcher.Dispatch(RequestV2(QStringLiteral("record.selectTarget"),
                                                QJsonObject{{QStringLiteral("kind"), QStringLiteral("everything")}}))),
        QString::fromLatin1(error_code::kInvalidParams));
    EXPECT_EQ(
        ErrorCode(dispatcher.Dispatch(RequestV2(
            QStringLiteral("ui.navigate"), QJsonObject{{QStringLiteral("page"), QStringLiteral("unknown-page")}}))),
        QString::fromLatin1(error_code::kInvalidParams));
    EXPECT_EQ(
        ErrorCode(dispatcher.Dispatch(RequestV2(
            QStringLiteral("edit.seek"), QJsonObject{{QStringLiteral("positionMs"), QStringLiteral("halfway")}}))),
        QString::fromLatin1(error_code::kInvalidParams));
    EXPECT_TRUE(source.calls.isEmpty());
}

TEST(LiveVerifyDispatcher, RegionCommandsCarryTheRectAndDisplayToTheSource) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject selected =
        dispatcher.Dispatch(RequestV2(QStringLiteral("record.selectRegion"),
                                      QJsonObject{{QStringLiteral("display"), QStringLiteral("\\\\.\\DISPLAY2")},
                                                  {QStringLiteral("x"), 100},
                                                  {QStringLiteral("y"), 50},
                                                  {QStringLiteral("width"), 640},
                                                  {QStringLiteral("height"), 360}}));
    ASSERT_TRUE(Ok(selected)) << selected.value(QStringLiteral("error")).toString().toStdString();
    const QJsonObject opened = dispatcher.Dispatch(RequestV2(QStringLiteral("record.openRegionSelector")));
    ASSERT_TRUE(Ok(opened)) << opened.value(QStringLiteral("error")).toString().toStdString();

    ASSERT_EQ(source.calls.size(), 2);
    EXPECT_EQ(source.calls[0], QStringLiteral("selectRegion:\\\\.\\DISPLAY2/100,50 640x360"));
    EXPECT_EQ(source.calls[1], QStringLiteral("openRegionSelector:"));
    // The rect parameters are required: a payload missing them is a client
    // error answered before the intent runs.
    EXPECT_EQ(ErrorCode(dispatcher.Dispatch(
                  RequestV2(QStringLiteral("record.selectRegion"), QJsonObject{{QStringLiteral("x"), 0}}))),
              QString::fromLatin1(error_code::kInvalidParams));
    EXPECT_EQ(source.calls.size(), 2);
}

TEST(LiveVerifyDispatcher, CapabilitiesMatchTheCommandsThatWillBeAccepted) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher)));

    const QJsonArray advertised = dispatcher.Dispatch(Request(QStringLiteral("system.capabilities")))
                                      .value(QStringLiteral("result"))
                                      .toObject()
                                      .value(QStringLiteral("commands"))
                                      .toArray();
    QStringList names;
    for (const QJsonValue& value : advertised)
        names.append(value.toString());
    EXPECT_EQ(names, LiveVerifyDispatcher::CommandNames(1));
}

TEST(LiveVerifyDispatcher, WithoutASourceEveryCommandReportsUnavailable) {
    LiveVerifyDispatcher dispatcher(nullptr, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher)));
    EXPECT_EQ(ErrorCode(dispatcher.Dispatch(Request(QStringLiteral("record.snapshot")))),
              QString::fromLatin1(error_code::kUnavailable));
}

// ---------------------------------------------------------------------------
// Protocol 2: state, revision, availability
// ---------------------------------------------------------------------------

TEST(LiveVerifyDispatcher, GetStatePublishesProductVocabularyOnly) {
    FakeSource source;
    source.state.page = QString::fromLatin1(page_name::kSettings);
    source.state.recording_state = QStringLiteral("Ready");
    source.state.edit_session_open = true;
    source.state.edit_visible = false;
    source.revision = 185;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject state =
        dispatcher.Dispatch(RequestV2(QStringLiteral("ui.getState"))).value(QStringLiteral("result")).toObject();
    EXPECT_EQ(state.value(QStringLiteral("page")).toString(), QStringLiteral("settings"));
    EXPECT_EQ(state.value(QStringLiteral("recordingState")).toString(), QStringLiteral("Ready"));
    EXPECT_EQ(state.value(QStringLiteral("editSession")).toString(), QStringLiteral("open"));
    EXPECT_FALSE(state.value(QStringLiteral("editVisible")).toBool());
    EXPECT_TRUE(state.value(QStringLiteral("blockingSurface")).isNull());
    EXPECT_EQ(state.value(QStringLiteral("sourcePicker")).toString(), QStringLiteral("closed"));
    EXPECT_EQ(state.value(QStringLiteral("notificationHub")).toString(), QStringLiteral("closed"));
    EXPECT_EQ(state.value(QStringLiteral("stateRevision")).toInt(), 185);
    EXPECT_TRUE(state.contains(QStringLiteral("availableActions")));
}

TEST(LiveVerifyDispatcher, BlockingSurfacesAreObservable) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    for (const char* surface : {blocking_surface_name::kRecovery, blocking_surface_name::kCrashReport,
                                blocking_surface_name::kRecordingError}) {
        source.state.blocking_surface = QString::fromLatin1(surface);
        const QJsonObject state =
            dispatcher.Dispatch(RequestV2(QStringLiteral("ui.getState"))).value(QStringLiteral("result")).toObject();
        EXPECT_EQ(state.value(QStringLiteral("blockingSurface")).toString(), QString::fromLatin1(surface));
    }
}

TEST(LiveVerifyDispatcher, IncludeStateAttachesTheWholeStateAndIsOffByDefault) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    EXPECT_FALSE(dispatcher.Dispatch(RequestV2(QStringLiteral("record.snapshot"))).contains(QStringLiteral("state")));

    ParsedRequest with_state = RequestV2(QStringLiteral("record.snapshot"));
    with_state.include_state = true;
    const QJsonObject response = dispatcher.Dispatch(with_state);
    ASSERT_TRUE(Ok(response));
    EXPECT_TRUE(response.value(QStringLiteral("state")).toObject().contains(QStringLiteral("availableActions")));
}

TEST(LiveVerifyDispatcher, NavigationAnswersTheResultingPageAndSettlesInTheSameResponse) {
    FakeSource source;
    source.state.page = QString::fromLatin1(page_name::kRecord);
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    for (const char* destination : {page_name::kSettings, page_name::kEdit}) {
        const QString page = QString::fromLatin1(destination);
        SCOPED_TRACE(page.toStdString());
        const QJsonObject response =
            dispatcher.Dispatch(RequestV2(QStringLiteral("ui.navigate"), QJsonObject{{QStringLiteral("page"), page}}));
        ASSERT_TRUE(Ok(response));
        EXPECT_TRUE(response.value(QStringLiteral("settled")).toBool());
        EXPECT_EQ(response.value(QStringLiteral("result")).toObject().value(QStringLiteral("page")).toString(), page);
        EXPECT_EQ(source.calls.last(), QStringLiteral("navigate:%1").arg(page));
    }
}

TEST(LiveVerifyDispatcher, LegacyLogsNavigationNormalizesToDiagnosticsLogs) {
    FakeSource source;
    source.state.page = QString::fromLatin1(page_name::kRecord);
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(
        RequestV2(QStringLiteral("ui.navigate"), QJsonObject{{QStringLiteral("page"), QStringLiteral("logs")}}));
    ASSERT_TRUE(Ok(response));
    EXPECT_TRUE(response.value(QStringLiteral("settled")).toBool());
    // The legacy spelling is an input alias only: the answer names the canonical
    // destination and the state carries the logs subview, never a phantom fifth
    // page.
    EXPECT_EQ(response.value(QStringLiteral("result")).toObject().value(QStringLiteral("page")).toString(),
              QStringLiteral("diagnostics"));
    EXPECT_EQ(source.state.diagnostics_section, QString::fromLatin1(diagnostics_section_name::kLogs));
}

TEST(LiveVerifyDispatcher, NavigatingToTheCurrentPageIsASuccessfulNoOp) {
    FakeSource source;
    source.state.page = QString::fromLatin1(page_name::kSettings);
    const std::uint64_t before = source.revision;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(
        RequestV2(QStringLiteral("ui.navigate"), QJsonObject{{QStringLiteral("page"), QStringLiteral("settings")}}));
    ASSERT_TRUE(Ok(response));
    EXPECT_TRUE(response.value(QStringLiteral("settled")).toBool());
    // The fake advances its revision on every Navigate; the point here is only
    // that the response is a success and the page is the one asked for.
    EXPECT_GE(static_cast<std::uint64_t>(response.value(QStringLiteral("stateRevision")).toDouble()), before);
}

TEST(LiveVerifyDispatcher, NavigationIsBlockedNotInvalidUnderABlockingSurface) {
    FakeSource source;
    source.state.blocking_surface = QString::fromLatin1(blocking_surface_name::kRecovery);
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(
        RequestV2(QStringLiteral("ui.navigate"), QJsonObject{{QStringLiteral("page"), QStringLiteral("settings")}}));
    EXPECT_FALSE(Ok(response));
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kBlocked));
    EXPECT_TRUE(ErrorOf(response)
                    .value(QStringLiteral("requires"))
                    .toObject()
                    .value(QStringLiteral("blockingSurface"))
                    .isNull());
    EXPECT_EQ(ErrorOf(response)
                  .value(QStringLiteral("actual"))
                  .toObject()
                  .value(QStringLiteral("blockingSurface"))
                  .toString(),
              QStringLiteral("recovery"));
    EXPECT_TRUE(source.calls.isEmpty());
}

// ---------------------------------------------------------------------------
// record.start truthfulness (D-1)
// ---------------------------------------------------------------------------

TEST(LiveVerifyRecordStart, ReadyWithNoBlockerIsAccepted) {
    FakeSource source;
    source.state = PermissiveState();
    source.state.recording_state = QStringLiteral("Ready");
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));
    EXPECT_TRUE(Ok(dispatcher.Dispatch(RequestV2(QStringLiteral("record.start")))));
    EXPECT_EQ(source.calls, QStringList({QStringLiteral("start")}));
}

TEST(LiveVerifyRecordStart, ABlockingSurfaceRefusesWithBlockedAndNeverPressesStart) {
    // The false success this cut exists to remove: the old channel checked
    // canStart() alone, answered ok:true, and the product path then dropped the
    // start with nothing but a log line.
    for (const char* surface : {blocking_surface_name::kRecovery, blocking_surface_name::kCrashReport,
                                blocking_surface_name::kRecordingError}) {
        FakeSource source;
        source.state = PermissiveState();
        source.state.recording_state = QStringLiteral("Ready");
        source.state.blocking_surface = QString::fromLatin1(surface);
        LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
        ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

        const QJsonObject response = dispatcher.Dispatch(RequestV2(QStringLiteral("record.start")));
        EXPECT_FALSE(Ok(response)) << surface;
        EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kBlocked)) << surface;
        EXPECT_EQ(ErrorOf(response)
                      .value(QStringLiteral("actual"))
                      .toObject()
                      .value(QStringLiteral("blockingSurface"))
                      .toString(),
                  QString::fromLatin1(surface));
        EXPECT_TRUE(source.calls.isEmpty()) << surface;
    }
}

TEST(LiveVerifyRecordStart, ADiagnosticsBlockerIsBlockedNotInvalidState) {
    FakeSource source;
    source.state = PermissiveState();
    source.state.recording_state = QStringLiteral("Blocked");
    source.state.can_start = false;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(RequestV2(QStringLiteral("record.start")));
    EXPECT_FALSE(Ok(response));
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kBlocked));
}

TEST(LiveVerifyRecordStart, AWrongTransportStateIsInvalidState) {
    FakeSource source;
    source.state = PermissiveState();
    source.state.recording_state = QStringLiteral("Recording");
    source.state.can_start = false;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(RequestV2(QStringLiteral("record.start")));
    EXPECT_FALSE(Ok(response));
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kInvalidState));
    EXPECT_TRUE(
        ErrorOf(response).value(QStringLiteral("requires")).toObject().value(QStringLiteral("canStart")).toBool());
    EXPECT_FALSE(
        ErrorOf(response).value(QStringLiteral("actual")).toObject().value(QStringLiteral("canStart")).toBool());
}

TEST(LiveVerifyRecordStart, ProtocolOneAlsoStopsReportingASuccessItDidNotGet) {
    // The truthfulness fix is not protocol-gated: a v1 client is told
    // command_failed rather than ok:true, because ok:true was objectively wrong.
    FakeSource source;
    source.state = PermissiveState();
    source.state.recording_state = QStringLiteral("Ready");
    source.state.blocking_surface = QString::fromLatin1(blocking_surface_name::kRecovery);
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher)));

    const QJsonObject response = dispatcher.Dispatch(Request(QStringLiteral("record.start")));
    EXPECT_FALSE(Ok(response));
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kCommandFailed));
    // ... and without the structured cause, which is a protocol-2 field.
    EXPECT_FALSE(ErrorOf(response).contains(QStringLiteral("requires")));
    EXPECT_TRUE(source.calls.isEmpty());
}

// ---------------------------------------------------------------------------
// Edit
// ---------------------------------------------------------------------------

TEST(LiveVerifyEdit, EveryEditCommandExceptOpenNeedsASession) {
    FakeSource source;
    source.state = PermissiveState();
    source.state.edit_session_open = false;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    // The commands that fill an empty workspace, step its history (deleting the
    // last clip leaves an undo step and no session), or change something that
    // is not on the timeline. Everything else edits content that has to exist.
    const QStringList without_session = {
        QStringLiteral("edit.open"),        QStringLiteral("edit.importMedia"), QStringLiteral("edit.appendRecording"),
        QStringLiteral("edit.appendMedia"), QStringLiteral("edit.undo"),        QStringLiteral("edit.redo"),
        QStringLiteral("edit.setOption"),   QStringLiteral("edit.setVolume"),   QStringLiteral("edit.setPanel"),
        QStringLiteral("edit.setSourceTab")};
    for (const CommandDescriptor& command : AllCommands()) {
        if (!command.name.startsWith(QStringLiteral("edit.")) || without_session.contains(command.name))
            continue;
        const QJsonObject response = dispatcher.Dispatch(RequestV2(command.name, PlausibleParams(command)));
        EXPECT_FALSE(Ok(response)) << command.name.toStdString();
        EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kInvalidState)) << command.name.toStdString();
        EXPECT_EQ(ErrorOf(response)
                      .value(QStringLiteral("requires"))
                      .toObject()
                      .value(QStringLiteral("editSession"))
                      .toString(),
                  QStringLiteral("open"));
    }
    // No implicit open, and no half-applied mutation on the way to the refusal.
    EXPECT_TRUE(source.calls.isEmpty());
}

TEST(LiveVerifyEdit, SeekAndTrimReachTheAdapterWithTheRequestedPosition) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    ASSERT_TRUE(Ok(dispatcher.Dispatch(
        RequestV2(QStringLiteral("edit.seek"), QJsonObject{{QStringLiteral("positionMs"), 5000}}))));
    ASSERT_TRUE(Ok(dispatcher.Dispatch(
        RequestV2(QStringLiteral("edit.setTrimIn"), QJsonObject{{QStringLiteral("positionMs"), 4000}}))));
    ASSERT_TRUE(Ok(dispatcher.Dispatch(
        RequestV2(QStringLiteral("edit.setTrimOut"), QJsonObject{{QStringLiteral("positionMs"), 9000}}))));
    EXPECT_EQ(source.calls, QStringList({QStringLiteral("edit.seek:5000"), QStringLiteral("edit.setTrimIn:4000"),
                                         QStringLiteral("edit.setTrimOut:9000")}));
}

TEST(LiveVerifyEdit, CloseIsRefusedWhileAnExportIsRunning) {
    FakeSource source;
    source.state.edit_export_running = true;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(RequestV2(QStringLiteral("edit.close")));
    EXPECT_FALSE(Ok(response));
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kInvalidState));
    EXPECT_TRUE(source.calls.isEmpty());
}

TEST(LiveVerifyEdit, CloseSettlesOnTheSessionActuallyBeingClosed) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(RequestV2(QStringLiteral("edit.close")));
    ASSERT_TRUE(Ok(response));
    EXPECT_TRUE(response.value(QStringLiteral("settled")).toBool());
    EXPECT_FALSE(source.State().edit_session_open);
}

// ---------------------------------------------------------------------------
// Edit workspace
// ---------------------------------------------------------------------------

namespace {

QJsonObject ResultOf(const QJsonObject& response) {
    return response.value(QStringLiteral("result")).toObject();
}

QJsonObject EditReport(const QJsonObject& response) {
    return ResultOf(response).value(QStringLiteral("edit")).toObject();
}

} // namespace

TEST(LiveVerifyEditWorkspace, OpeningARecordingThatIsStillBeingProbedIsAcceptedNotSettled) {
    FakeSource source;
    source.state.edit_session_open = false;
    source.open_probes_first = true;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject probing = dispatcher.Dispatch(RequestV2(QStringLiteral("edit.open")));
    ASSERT_TRUE(Ok(probing)) << ErrorCode(probing).toStdString();
    EXPECT_FALSE(probing.value(QStringLiteral("settled")).toBool());

    // A recording that reaches the timeline at once is settled in the answer.
    source.open_probes_first = false;
    source.state.edit_import_pending = false;
    const QJsonObject opened = dispatcher.Dispatch(RequestV2(QStringLiteral("edit.open")));
    ASSERT_TRUE(Ok(opened));
    EXPECT_TRUE(opened.value(QStringLiteral("settled")).toBool());
}

TEST(LiveVerifyEditWorkspace, AnOpenThatNeitherOpensNorProbesIsAFailure) {
    FakeSource source;
    source.state.edit_session_open = false;
    source.open_has_effect = false;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    EXPECT_EQ(ErrorCode(dispatcher.Dispatch(RequestV2(QStringLiteral("edit.open")))),
              QString::fromLatin1(error_code::kOperationFailed));
}

TEST(LiveVerifyEditWorkspace, AnAcceptedEditReportsTheRevisionItMovedAndTheResultingTimeline) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(RequestV2(
        QStringLiteral("edit.moveClip"),
        QJsonObject{{QStringLiteral("clipId"), 3}, {QStringLiteral("atMs"), 2500}, {QStringLiteral("snap"), false}}));
    ASSERT_TRUE(Ok(response)) << ErrorCode(response).toStdString();
    EXPECT_TRUE(response.value(QStringLiteral("settled")).toBool());
    EXPECT_EQ(source.calls, QStringList{QStringLiteral("edit.moveClip:3/2500/0")});
    const QJsonObject edit = EditReport(response);
    EXPECT_TRUE(edit.value(QStringLiteral("attempted")).toBool());
    EXPECT_TRUE(edit.value(QStringLiteral("changed")).toBool());
    EXPECT_EQ(edit.value(QStringLiteral("revisionBefore")).toInt(), 7);
    EXPECT_EQ(edit.value(QStringLiteral("revisionAfter")).toInt(), 8);
    // The editor snapshot rides along: it is the postcondition, not an echo.
    EXPECT_EQ(ResultOf(response).value(QStringLiteral("marker")).toString(), QStringLiteral("editor"));
}

TEST(LiveVerifyEditWorkspace, AModelRefusalIsBlockedInTheWordsTheTimelineShows) {
    FakeSource source;
    source.model_accepts = false;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(RequestV2(QStringLiteral("edit.split")));
    EXPECT_FALSE(Ok(response));
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kBlocked));
    EXPECT_EQ(ErrorOf(response).value(QStringLiteral("message")).toString(),
              QStringLiteral("The edit would overlap another clip"));
    EXPECT_EQ(ErrorOf(response).value(QStringLiteral("requires")).toObject().value(QStringLiteral("edit")).toString(),
              QStringLiteral("accepted"));
    EXPECT_EQ(ErrorOf(response).value(QStringLiteral("actual")).toObject().value(QStringLiteral("edit")).toString(),
              QStringLiteral("refused"));
    EXPECT_EQ(source.state.edit_workspace_revision, 7U);
}

TEST(LiveVerifyEditWorkspace, AStaleExpectedRevisionIsRefusedBeforeTheEditRuns) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(
        RequestV2(QStringLiteral("edit.delete"), QJsonObject{{QStringLiteral("expectedRevision"), 6}}));
    EXPECT_FALSE(Ok(response));
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kInvalidState));
    EXPECT_EQ(ErrorOf(response)
                  .value(QStringLiteral("requires"))
                  .toObject()
                  .value(QStringLiteral("workspaceRevision"))
                  .toInt(),
              6);
    EXPECT_EQ(
        ErrorOf(response).value(QStringLiteral("actual")).toObject().value(QStringLiteral("workspaceRevision")).toInt(),
        7);
    EXPECT_TRUE(source.calls.isEmpty());

    // The revision the caller has seen goes through.
    const QJsonObject current = dispatcher.Dispatch(
        RequestV2(QStringLiteral("edit.delete"), QJsonObject{{QStringLiteral("expectedRevision"), 7}}));
    EXPECT_TRUE(Ok(current)) << ErrorCode(current).toStdString();
}

TEST(LiveVerifyEditWorkspace, AnIdThatNamesNothingIsAClientErrorAndNeverAnEdit) {
    FakeSource source;
    source.targets_exist = false;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(RequestV2(
        QStringLiteral("edit.trimClip"),
        QJsonObject{{QStringLiteral("clipId"), 99}, {QStringLiteral("inMs"), 0}, {QStringLiteral("outMs"), 1000}}));
    EXPECT_FALSE(Ok(response));
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kInvalidParams));
    EXPECT_EQ(source.edit_outcome.sequence, 0U);
}

TEST(LiveVerifyEditWorkspace, NumbersThatAreNotWholeOrInRangeNeverReachTheModel) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QList<QPair<QString, QJsonObject>> bad = {
        {QStringLiteral("edit.moveClip"), {{QStringLiteral("clipId"), 1.5}, {QStringLiteral("atMs"), 0}}},
        {QStringLiteral("edit.moveClip"), {{QStringLiteral("clipId"), 1}, {QStringLiteral("atMs"), -5}}},
        {QStringLiteral("edit.selectClip"), {{QStringLiteral("clipId"), -1}}},
        {QStringLiteral("edit.split"), {{QStringLiteral("expectedRevision"), 7.25}}},
        {QStringLiteral("edit.setVolume"), {{QStringLiteral("volume"), 1.5}}},
        {QStringLiteral("edit.setVolume"), {{QStringLiteral("volume"), QStringLiteral("loud")}}},
        {QStringLiteral("edit.setJunctionCurve"),
         {{QStringLiteral("curve"), QStringLiteral("bias")}, {QStringLiteral("bias"), 2}}},
        {QStringLiteral("edit.confirmMonitoring"), {{QStringLiteral("trackIds"), QStringLiteral("track-1")}}},
        {QStringLiteral("edit.confirmMonitoring"), {{QStringLiteral("trackIds"), QJsonArray{1, 2}}}},
        {QStringLiteral("edit.setFixedTimelineEnd"), {{QStringLiteral("fixed"), true}}},
    };
    for (const auto& [command, params] : bad) {
        const QJsonObject response = dispatcher.Dispatch(RequestV2(command, params));
        EXPECT_FALSE(Ok(response)) << command.toStdString();
        EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kInvalidParams)) << command.toStdString();
    }
    EXPECT_TRUE(source.calls.isEmpty()) << source.calls.join(QLatin1Char(',')).toStdString();
}

TEST(LiveVerifyEditWorkspace, SelectionSettlesOnTheSelectionTheStateReports) {
    FakeSource source;
    source.state.edit_selected_clip = 0;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response =
        dispatcher.Dispatch(RequestV2(QStringLiteral("edit.selectClip"), QJsonObject{{QStringLiteral("clipId"), 4}}));
    ASSERT_TRUE(Ok(response)) << ErrorCode(response).toStdString();
    EXPECT_EQ(source.state.edit_selected_clip, 4U);

    // A transition is named by its outgoing clip.
    ASSERT_TRUE(Ok(dispatcher.Dispatch(
        RequestV2(QStringLiteral("edit.selectTransition"), QJsonObject{{QStringLiteral("clipId"), 4}}))));
    EXPECT_EQ(source.state.edit_selected_transition, 4U);
}

TEST(LiveVerifyEditWorkspace, SelectionCommandsDoNotPretendToBeEdits) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response =
        dispatcher.Dispatch(RequestV2(QStringLiteral("edit.selectClip"), QJsonObject{{QStringLiteral("clipId"), 2}}));
    ASSERT_TRUE(Ok(response));
    EXPECT_FALSE(ResultOf(response).contains(QStringLiteral("edit")));
    EXPECT_EQ(source.state.edit_workspace_revision, 7U);
}

TEST(LiveVerifyEditWorkspace, HistoryStepsFollowTheirOwnAvailability) {
    FakeSource source;
    source.state.edit_can_undo = false;
    source.state.edit_can_redo = true;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject refused = dispatcher.Dispatch(RequestV2(QStringLiteral("edit.undo")));
    EXPECT_EQ(ErrorCode(refused), QString::fromLatin1(error_code::kInvalidState));
    EXPECT_TRUE(source.calls.isEmpty());

    const QJsonObject redone = dispatcher.Dispatch(RequestV2(QStringLiteral("edit.redo")));
    ASSERT_TRUE(Ok(redone));
    EXPECT_TRUE(EditReport(redone).value(QStringLiteral("changed")).toBool());
}

TEST(LiveVerifyEditWorkspace, TransitionEditsNeedASelectedTransition) {
    FakeSource source;
    source.state.edit_selected_transition = 0;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    for (const auto& [command, params] : QList<QPair<QString, QJsonObject>>{
             {QStringLiteral("edit.removeTransition"), {}},
             {QStringLiteral("edit.setJunctionWindow"), {{QStringLiteral("startMs"), 0}, {QStringLiteral("endMs"), 1}}},
             {QStringLiteral("edit.setJunctionCurve"), {{QStringLiteral("curve"), QStringLiteral("smooth")}}},
             {QStringLiteral("edit.setJunctionAudioLaw"), {{QStringLiteral("law"), QStringLiteral("equalPower")}}},
             {QStringLiteral("edit.setJunctionCut"), {{QStringLiteral("atMs"), 10}}}}) {
        const QJsonObject response = dispatcher.Dispatch(RequestV2(command, params));
        EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kInvalidState)) << command.toStdString();
    }
    EXPECT_TRUE(source.calls.isEmpty());
}

TEST(LiveVerifyEditWorkspace, ImportIsAcceptedNotFinished) {
    FakeSource source;
    source.state.edit_session_open = false;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(RequestV2(
        QStringLiteral("edit.importMedia"),
        QJsonObject{{QStringLiteral("path"), QStringLiteral("C:/media/a.mkv")}, {QStringLiteral("append"), true}}));
    ASSERT_TRUE(Ok(response)) << ErrorCode(response).toStdString();
    EXPECT_FALSE(response.value(QStringLiteral("settled")).toBool());
    EXPECT_EQ(source.calls, QStringList{QStringLiteral("edit.importMedia:C:/media/a.mkv/1/-1")});

    source.targets_exist = false;
    const QJsonObject missing =
        dispatcher.Dispatch(RequestV2(QStringLiteral("edit.importMedia"),
                                      QJsonObject{{QStringLiteral("path"), QStringLiteral("C:/media/none.mkv")}}));
    EXPECT_EQ(ErrorCode(missing), QString::fromLatin1(error_code::kInvalidParams));
}

TEST(LiveVerifyEditWorkspace, PlaybackSettlesOnlyWhenThePlayerReportsIt) {
    FakeSource source;
    source.state.edit_playback = QStringLiteral("paused");
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject play = dispatcher.Dispatch(RequestV2(QStringLiteral("edit.play")));
    ASSERT_TRUE(Ok(play));
    EXPECT_FALSE(play.value(QStringLiteral("settled")).toBool());

    // Pausing a paused preview is already where it was asked to be.
    const QJsonObject pause = dispatcher.Dispatch(RequestV2(QStringLiteral("edit.pause")));
    ASSERT_TRUE(Ok(pause));
    EXPECT_TRUE(pause.value(QStringLiteral("settled")).toBool());
    EXPECT_EQ(source.calls, (QStringList{QStringLiteral("edit.setPlaying:1"), QStringLiteral("edit.setPlaying:0")}));
}

TEST(LiveVerifyEditWorkspace, MonitoringConfirmationNeedsTracksAndSettlesOnTheRequirementClearing) {
    FakeSource source;
    source.state.edit_monitoring_confirmation_required = true;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(
        RequestV2(QStringLiteral("edit.confirmMonitoring"),
                  QJsonObject{{QStringLiteral("trackIds"), QJsonArray{QStringLiteral("a"), QStringLiteral("b")}},
                              {QStringLiteral("overlapConfirmed"), true}}));
    ASSERT_TRUE(Ok(response));
    EXPECT_FALSE(response.value(QStringLiteral("settled")).toBool());
    EXPECT_EQ(source.calls, QStringList{QStringLiteral("edit.confirmMonitoring:a,b/1")});

    source.state.edit_monitoring_track_count = 0;
    const QJsonObject none = dispatcher.Dispatch(
        RequestV2(QStringLiteral("edit.confirmMonitoring"), QJsonObject{{QStringLiteral("trackIds"), QJsonArray{}}}));
    EXPECT_EQ(ErrorCode(none), QString::fromLatin1(error_code::kInvalidState));
}

TEST(LiveVerifyEditWorkspace, OptionsSettleOnTheReportedValue) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(
        RequestV2(QStringLiteral("edit.setOption"), QJsonObject{{QStringLiteral("option"), QStringLiteral("ripple")},
                                                                {QStringLiteral("enabled"), true}}));
    ASSERT_TRUE(Ok(response));
    EXPECT_TRUE(source.state.edit_ripple);
}

TEST(LiveVerifyEditPanels, PanelsBelongToTheEditPage) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(
        RequestV2(QStringLiteral("edit.setPanel"),
                  QJsonObject{{QStringLiteral("panel"), QStringLiteral("details")}, {QStringLiteral("open"), true}}));
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kInvalidState));
    EXPECT_EQ(ErrorOf(response).value(QStringLiteral("requires")).toObject().value(QStringLiteral("page")).toString(),
              QStringLiteral("edit"));
    EXPECT_TRUE(source.calls.isEmpty());
}

TEST(LiveVerifyEditPanels, OpeningAndPinningSettleOnTheLayoutThePageReports) {
    FakeSource source;
    source.state.page = QString::fromLatin1(page_name::kEdit);
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    ASSERT_TRUE(Ok(dispatcher.Dispatch(
        RequestV2(QStringLiteral("edit.setPanel"), QJsonObject{{QStringLiteral("panel"), QStringLiteral("media")},
                                                               {QStringLiteral("open"), true},
                                                               {QStringLiteral("pinned"), true}}))));
    EXPECT_EQ(source.state.edit_media_panel, QStringLiteral("pinned"));
    ASSERT_TRUE(Ok(dispatcher.Dispatch(RequestV2(
        QStringLiteral("edit.setSourceTab"), QJsonObject{{QStringLiteral("tab"), QStringLiteral("transitions")}}))));
    EXPECT_EQ(source.state.edit_source_tab, QStringLiteral("transitions"));

    // The page did not do what was asked: a failure, not a quiet success.
    source.panels_follow = false;
    const QJsonObject ignored = dispatcher.Dispatch(
        RequestV2(QStringLiteral("edit.setPanel"),
                  QJsonObject{{QStringLiteral("panel"), QStringLiteral("details")}, {QStringLiteral("open"), true}}));
    EXPECT_EQ(ErrorCode(ignored), QString::fromLatin1(error_code::kOperationFailed));
}

TEST(LiveVerifyEditPanels, PinningIsRefusedWhenTheViewerIsTooNarrow) {
    FakeSource source;
    source.state.page = QString::fromLatin1(page_name::kEdit);
    source.state.edit_details_panel_can_pin = false;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(
        RequestV2(QStringLiteral("edit.setPanel"), QJsonObject{{QStringLiteral("panel"), QStringLiteral("details")},
                                                               {QStringLiteral("open"), true},
                                                               {QStringLiteral("pinned"), true}}));
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kBlocked));
    EXPECT_EQ(ErrorOf(response).value(QStringLiteral("requires")).toObject().value(QStringLiteral("canPin")).toBool(),
              true);
    EXPECT_TRUE(source.calls.isEmpty());
}

TEST(LiveVerifyEditState, TheStateCarriesTheEditorRevisionSelectionAndPanels) {
    FakeSource source;
    source.state.edit_media_panel = QStringLiteral("open");
    source.state.edit_source_tab = QStringLiteral("media");
    source.state.edit_workspace_error = QStringLiteral("The edit would overlap another clip");
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject editor = ResultOf(dispatcher.Dispatch(RequestV2(QStringLiteral("ui.getState"))))
                                   .value(QStringLiteral("editor"))
                                   .toObject();
    EXPECT_EQ(editor.value(QStringLiteral("workspaceRevision")).toInt(), 7);
    EXPECT_EQ(editor.value(QStringLiteral("selectedClip")).toInt(), 1);
    EXPECT_EQ(editor.value(QStringLiteral("workspaceError")).toString(),
              QStringLiteral("The edit would overlap another clip"));
    const QJsonObject panels = editor.value(QStringLiteral("panels")).toObject();
    EXPECT_EQ(panels.value(QStringLiteral("media")).toObject().value(QStringLiteral("state")).toString(),
              QStringLiteral("open"));
    EXPECT_EQ(panels.value(QStringLiteral("sourceTab")).toString(), QStringLiteral("media"));
    EXPECT_TRUE(editor.value(QStringLiteral("options")).toObject().value(QStringLiteral("snapping")).toBool());
}

// ---------------------------------------------------------------------------
// Export review and options
// ---------------------------------------------------------------------------

TEST(LiveVerifyExport, TheReviewButtonsNeedAReview) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    for (const char* command : {"export.confirm", "export.discard"}) {
        const QJsonObject response = dispatcher.Dispatch(RequestV2(QString::fromLatin1(command)));
        EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kInvalidState)) << command;
        EXPECT_EQ(ErrorOf(response)
                      .value(QStringLiteral("requires"))
                      .toObject()
                      .value(QStringLiteral("exportState"))
                      .toString(),
                  QStringLiteral("review"));
    }
    EXPECT_TRUE(source.calls.isEmpty());

    source.state.edit_export_state = QStringLiteral("review");
    const QJsonObject confirmed = dispatcher.Dispatch(RequestV2(QStringLiteral("export.confirm")));
    ASSERT_TRUE(Ok(confirmed));
    EXPECT_FALSE(confirmed.value(QStringLiteral("settled")).toBool());
    ASSERT_TRUE(Ok(dispatcher.Dispatch(RequestV2(QStringLiteral("export.discard")))));
    EXPECT_EQ(source.state.edit_export_state, QStringLiteral("idle"));
}

TEST(LiveVerifyExport, DiscardThatLeavesTheReviewUpIsAFailedPostcondition) {
    FakeSource source;
    source.state.edit_export_state = QStringLiteral("review");
    source.discard_returns_to_idle = false;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(RequestV2(QStringLiteral("export.discard")));
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kOperationFailed));
}

TEST(LiveVerifyExport, OptionsAreReadBackAndARefusedValueIsAFailure) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject accepted = dispatcher.Dispatch(RequestV2(
        QStringLiteral("export.setOptions"), QJsonObject{{QStringLiteral("span"), QStringLiteral("occupied")},
                                                         {QStringLiteral("container"), QStringLiteral("mp4")}}));
    ASSERT_TRUE(Ok(accepted)) << ErrorCode(accepted).toStdString();
    EXPECT_EQ(ResultOf(accepted).value(QStringLiteral("export")).toObject().value(QStringLiteral("span")).toString(),
              QStringLiteral("occupied"));

    source.refused_export_keys = {QStringLiteral("mode")};
    const QJsonObject refused = dispatcher.Dispatch(RequestV2(
        QStringLiteral("export.setOptions"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("originalStreams")}}));
    EXPECT_EQ(ErrorCode(refused), QString::fromLatin1(error_code::kOperationFailed));

    source.state.edit_export_running = true;
    const QJsonObject running = dispatcher.Dispatch(RequestV2(
        QStringLiteral("export.setOptions"), QJsonObject{{QStringLiteral("span"), QStringLiteral("entire")}}));
    EXPECT_EQ(ErrorCode(running), QString::fromLatin1(error_code::kInvalidState));
}

TEST(LiveVerifyExport, ResetNeedsAFinishedRun) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    EXPECT_EQ(ErrorCode(dispatcher.Dispatch(RequestV2(QStringLiteral("export.reset")))),
              QString::fromLatin1(error_code::kInvalidState));
    source.state.edit_export_state = QStringLiteral("failed");
    ASSERT_TRUE(Ok(dispatcher.Dispatch(RequestV2(QStringLiteral("export.reset")))));
    EXPECT_EQ(source.state.edit_export_state, QStringLiteral("idle"));
}

// ---------------------------------------------------------------------------
// Scroll / reveal
// ---------------------------------------------------------------------------

TEST(LiveVerifyReveal, AnUnknownTargetIsInvalidParamsNotASilentNoOp) {
    // The --settings-visual-bottom trap: a reveal that quietly did nothing while
    // every capture taken afterwards claimed to show the end of the page.
    FakeSource source;
    source.state.page = QString::fromLatin1(page_name::kSettings);
    source.reveal_outcome = LiveVerifySource::RevealOutcome::UnknownTarget;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(
        RequestV2(QStringLiteral("ui.reveal"), QJsonObject{{QStringLiteral("surface"), QStringLiteral("settings")},
                                                           {QStringLiteral("target"), QStringLiteral("nowhere")}}));
    EXPECT_FALSE(Ok(response));
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kInvalidParams));
}

TEST(LiveVerifyReveal, ScrollingIsRefusedOnAPageThatDoesNotScroll) {
    FakeSource source;
    source.state.page = QString::fromLatin1(page_name::kRecord);
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(
        RequestV2(QStringLiteral("ui.scrollEnd"), QJsonObject{{QStringLiteral("surface"), QStringLiteral("logs")}}));
    EXPECT_FALSE(Ok(response));
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kInvalidState));
}

TEST(LiveVerifyReveal, ASurfaceIsOnlyAddressableWhileItIsTheCurrentPage) {
    // The four destinations stay resident after their first visit (QCR-602), so
    // the Settings item is still reachable from Logs -- and scrolling a page
    // nobody is looking at, then reporting where it landed, is evidence of
    // nothing. Refused as invalid_state, NOT as an unknown target: the name is
    // correct, the place is not.
    FakeSource source;
    source.state.page = QString::fromLatin1(page_name::kLogs);
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject reveal = dispatcher.Dispatch(
        RequestV2(QStringLiteral("ui.reveal"), QJsonObject{{QStringLiteral("surface"), QStringLiteral("settings")},
                                                           {QStringLiteral("target"), QStringLiteral("appearance")}}));
    EXPECT_FALSE(Ok(reveal));
    EXPECT_EQ(ErrorCode(reveal), QString::fromLatin1(error_code::kInvalidState));
    EXPECT_EQ(ErrorOf(reveal).value(QStringLiteral("requires")).toObject().value(QStringLiteral("page")).toString(),
              QStringLiteral("settings"));
    EXPECT_EQ(ErrorOf(reveal).value(QStringLiteral("actual")).toObject().value(QStringLiteral("page")).toString(),
              QStringLiteral("logs"));

    const QJsonObject scroll = dispatcher.Dispatch(RequestV2(
        QStringLiteral("ui.scrollEnd"), QJsonObject{{QStringLiteral("surface"), QStringLiteral("settings")}}));
    EXPECT_EQ(ErrorCode(scroll), QString::fromLatin1(error_code::kInvalidState));
    EXPECT_TRUE(source.calls.isEmpty());
}

TEST(LiveVerifyReveal, ARealTargetThatDidNotLandIsNotReportedAsAnUnknownOne) {
    FakeSource source;
    source.state.page = QString::fromLatin1(page_name::kSettings);
    source.reveal_outcome = LiveVerifySource::RevealOutcome::Failed;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(
        RequestV2(QStringLiteral("ui.reveal"), QJsonObject{{QStringLiteral("surface"), QStringLiteral("settings")},
                                                           {QStringLiteral("target"), QStringLiteral("appearance")}}));
    EXPECT_FALSE(Ok(response));
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kOperationFailed));
}

TEST(LiveVerifyReveal, AScrollThatDoesNotLandIsAFailureNotASettledSuccess) {
    FakeSource source;
    source.state.page = QString::fromLatin1(page_name::kLogs);
    source.scroll_lands = false;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(
        RequestV2(QStringLiteral("ui.scrollEnd"), QJsonObject{{QStringLiteral("surface"), QStringLiteral("logs")}}));
    EXPECT_FALSE(Ok(response));
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kOperationFailed));
}

// ---------------------------------------------------------------------------
// Popups
// ---------------------------------------------------------------------------

TEST(LiveVerifyPopups, OpeningAndClosingThePickerIsObservableAndIdempotent) {
    FakeSource source;
    source.state.page = QString::fromLatin1(page_name::kRecord);
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    ASSERT_TRUE(Ok(dispatcher.Dispatch(RequestV2(QStringLiteral("sourcePicker.open")))));
    EXPECT_TRUE(source.State().source_picker_open);
    ASSERT_TRUE(Ok(dispatcher.Dispatch(RequestV2(QStringLiteral("sourcePicker.open")))));

    ASSERT_TRUE(Ok(dispatcher.Dispatch(RequestV2(QStringLiteral("sourcePicker.close")))));
    EXPECT_FALSE(source.State().source_picker_open);
    ASSERT_TRUE(Ok(dispatcher.Dispatch(RequestV2(QStringLiteral("sourcePicker.close")))));
}

TEST(LiveVerifyPopups, ThePickerBelongsToRecord) {
    FakeSource source;
    source.state.page = QString::fromLatin1(page_name::kSettings);
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(RequestV2(QStringLiteral("sourcePicker.open")));
    EXPECT_FALSE(Ok(response));
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kInvalidState));
}

TEST(LiveVerifyPopups, NotificationHubOpensClosesAndClearsIdempotently) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    ASSERT_TRUE(Ok(dispatcher.Dispatch(RequestV2(QStringLiteral("notificationHub.open")))));
    EXPECT_TRUE(source.State().notification_hub_open);
    ASSERT_TRUE(Ok(dispatcher.Dispatch(RequestV2(QStringLiteral("notification.clearAll")))));
    ASSERT_TRUE(Ok(dispatcher.Dispatch(RequestV2(QStringLiteral("notification.clearAll")))));
    ASSERT_TRUE(Ok(dispatcher.Dispatch(RequestV2(QStringLiteral("notificationHub.close")))));
    EXPECT_FALSE(source.State().notification_hub_open);
    ASSERT_TRUE(Ok(dispatcher.Dispatch(RequestV2(QStringLiteral("notificationHub.close")))));
}

// ---------------------------------------------------------------------------
// ipc.describe
// ---------------------------------------------------------------------------

TEST(LiveVerifyDescribe, DescribesEveryCommandItWillAccept) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject described =
        dispatcher.Dispatch(RequestV2(QStringLiteral("ipc.describe"))).value(QStringLiteral("result")).toObject();
    EXPECT_EQ(described.value(QStringLiteral("commands")).toArray().size(),
              LiveVerifyDispatcher::CommandNames(2).size());
    EXPECT_EQ(described.value(QStringLiteral("errorCodes")).toArray().size(), AllErrorCodes().size());
    EXPECT_EQ(described.value(QStringLiteral("supportedProtocols")).toArray().size(), 2);
}

TEST(LiveVerifyDescribe, IdempotencyIsDeclaredAndPlayPauseIsTheExceptionThatIsNot) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonArray commands = dispatcher.Dispatch(RequestV2(QStringLiteral("ipc.describe")))
                                    .value(QStringLiteral("result"))
                                    .toObject()
                                    .value(QStringLiteral("commands"))
                                    .toArray();
    int non_idempotent = 0;
    bool saw_play_pause = false;
    for (const QJsonValue& value : commands) {
        const QJsonObject command = value.toObject();
        if (!command.value(QStringLiteral("idempotent")).toBool())
            ++non_idempotent;
        if (command.value(QStringLiteral("name")).toString() == QStringLiteral("edit.playPause")) {
            saw_play_pause = true;
            EXPECT_FALSE(command.value(QStringLiteral("idempotent")).toBool());
            EXPECT_EQ(command.value(QStringLiteral("settle")).toString(), QStringLiteral("synchronous"));
        }
        if (command.value(QStringLiteral("name")).toString() == QStringLiteral("ui.navigate")) {
            EXPECT_TRUE(command.value(QStringLiteral("idempotent")).toBool());
            EXPECT_EQ(command.value(QStringLiteral("settle")).toString(), QStringLiteral("synchronous"));
        }
        if (command.value(QStringLiteral("name")).toString() == QStringLiteral("record.start"))
            EXPECT_EQ(command.value(QStringLiteral("settle")).toString(), QStringLiteral("asynchronous"));
    }
    EXPECT_TRUE(saw_play_pause);
    // The seven transport intents (six plus record.addMarker -- a second marker
    // is a second marker), edit.playPause, profiles.create (two creates with the
    // same name are two profiles), notification.invokeAction (an action
    // navigates, opens a folder or relaunches), notification.raise (two calls are
    // two notifications), and webcam.overlay.set, whose effect repeats but whose
    // answer does not: the applied sequence advances on every accepted call, which
    // is what lets a caller tell a second identical request from a dropped one.
    // Twelve editor edits act on what the timeline holds now, so a repeat is a
    // second edit: importMedia, appendRecording and appendMedia (another copy),
    // split, delete, closeGap and deleteTrack (the next target), undo and redo
    // (the next history step), applyCrossfade and removeTransition (the next
    // cut), and frameStep (the next frame).
    EXPECT_EQ(non_idempotent, 24);
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

TEST(LiveVerifyDispatcher, UpdateCheckIsAcceptedWithoutClaimingAnAnswer) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(RequestV2(QStringLiteral("update.check")));
    ASSERT_TRUE(Ok(response));
    ASSERT_TRUE(response.contains(QStringLiteral("settled")));
    EXPECT_FALSE(response.value(QStringLiteral("settled")).toBool())
        << "the check runs on a pool thread; its answer is the card's next state";
    EXPECT_TRUE(source.calls.contains(QStringLiteral("update.check")));
}

TEST(LiveVerifyDispatcher, UpdateApplyRoutesToTheCardsPrimaryActionAndReportsTheChild) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(RequestV2(QStringLiteral("update.apply")));
    ASSERT_TRUE(Ok(response));
    EXPECT_FALSE(response.value(QStringLiteral("settled")).toBool())
        << "the updater was started; the UPDATE has not happened";
    EXPECT_TRUE(source.calls.contains(QStringLiteral("update.apply")));

    // The response already names the child, so a client never has to discover it.
    const QJsonObject launch =
        response.value(QStringLiteral("result")).toObject().value(QStringLiteral("updaterLaunch")).toObject();
    EXPECT_EQ(launch.value(QStringLiteral("pid")).toInt(), 4242);
    EXPECT_EQ(launch.value(QStringLiteral("targetVersion")).toString(), QStringLiteral("0.9.1"));
}

// The build-tree case, pinned. LaunchUpdater() stages its runtime from paths
// relative to applicationDirPath(), which only an installed tree satisfies, so a
// run from a build tree reaches the card's primary action and the LAUNCH is what
// fails. The card is genuinely offering an update, so no precondition catches
// it: the honest answer is operation_failed, never a settled success and never
// the invalid_state that "nothing to apply" earns.
TEST(LiveVerifyDispatcher, UpdateApplyThatCannotLaunchTheUpdaterIsAFailureNotASettledSuccess) {
    FakeSource source;
    source.allow_intents = false;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(RequestV2(QStringLiteral("update.apply")));
    EXPECT_FALSE(Ok(response));
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kOperationFailed));
    EXPECT_TRUE(source.calls.contains(QStringLiteral("update.apply")))
        << "the offer was real; the action must have been attempted before it failed";
    EXPECT_FALSE(response.contains(QStringLiteral("settled"))) << "a failure must not also claim an outcome";
}

TEST(LiveVerifyDispatcher, UpdateApplyWithNothingOfferedNeverReachesTheSource) {
    FakeSource source;
    source.state.update_state = QStringLiteral("uptodate");
    source.state.update_available = false;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject response = dispatcher.Dispatch(RequestV2(QStringLiteral("update.apply")));
    EXPECT_FALSE(Ok(response));
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kInvalidState));
    EXPECT_FALSE(source.calls.contains(QStringLiteral("update.apply")))
        << "the card's button would have re-checked here; an apply must not silently mean check";
}

TEST(LiveVerifyDispatcher, UpdateGetStateIsReadOnlyAndCarriesTheLaunchSnapshot) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));

    const QJsonObject before = dispatcher.Dispatch(RequestV2(QStringLiteral("update.getState")));
    ASSERT_TRUE(Ok(before));
    EXPECT_FALSE(before.contains(QStringLiteral("settled"))) << "a query must not look like a completed action";
    const QJsonObject result = before.value(QStringLiteral("result")).toObject();
    EXPECT_EQ(result.value(QStringLiteral("state")).toString(), QStringLiteral("available"));
    EXPECT_EQ(result.value(QStringLiteral("availableVersion")).toString(), QStringLiteral("0.9.1"));
    EXPECT_EQ(result.value(QStringLiteral("updaterLaunch")).toObject().value(QStringLiteral("pid")).toInt(), 0);
}

TEST(LiveVerifyDispatcher, UpdateCommandsAreProtocolTwoOnly) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 1)));
    for (const QString& name :
         {QStringLiteral("update.getState"), QStringLiteral("update.check"), QStringLiteral("update.apply")}) {
        const QJsonObject response = dispatcher.Dispatch(Request(name, {}, 1));
        EXPECT_FALSE(Ok(response)) << name.toStdString();
        EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kUnknownCommand)) << name.toStdString();
    }
}

TEST(LiveVerifyDispatcher, AudioRoutingEditsRequireNonnegativeObservedRevision) {
    FakeSource source;
    source.state.recording_state = QStringLiteral("Idle");
    source.state.countdown_active = false;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Ok(Hello(dispatcher, QString::fromLatin1(kRunId), 2)));
    for (const auto& command :
         {QStringLiteral("audio.routing.addTrack"), QStringLiteral("audio.routing.assignSource")}) {
        QJsonObject params{{QStringLiteral("sourceId"), QStringLiteral("source")},
                           {QStringLiteral("trackId"), QStringLiteral("track")},
                           {QStringLiteral("mode"), 1},
                           {QStringLiteral("expectedRevision"), -1}};
        if (command == QStringLiteral("audio.routing.addTrack"))
            params = {{QStringLiteral("expectedRevision"), -1}};
        const auto response = dispatcher.Dispatch(RequestV2(command, params));
        EXPECT_FALSE(Ok(response));
        EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kInvalidParams));
    }
    EXPECT_FALSE(source.calls.contains(QStringLiteral("audio.routing.addTrack:-1")));
    const auto added = dispatcher.Dispatch(
        RequestV2(QStringLiteral("audio.routing.addTrack"), {{QStringLiteral("expectedRevision"), 17}}));
    ASSERT_TRUE(Ok(added));
    EXPECT_TRUE(source.calls.contains(QStringLiteral("audio.routing.addTrack:17")));
    const auto assigned = dispatcher.Dispatch(
        RequestV2(QStringLiteral("audio.routing.assignSource"), {{QStringLiteral("sourceId"), QStringLiteral("source")},
                                                                 {QStringLiteral("trackId"), QStringLiteral("track")},
                                                                 {QStringLiteral("mode"), 1},
                                                                 {QStringLiteral("expectedRevision"), 18}}));
    ASSERT_TRUE(Ok(assigned));
    EXPECT_TRUE(source.calls.contains(QStringLiteral("audio.routing.assignSource:source:track:1:18")));
}
