#include "LiveVerifyDispatcher.h"

#include "LiveVerifyCommandPolicy.h"
#include "LiveVerifySource.h"

#include <QJsonArray>
#include <QJsonValue>

#include <cmath>
#include <optional>
#include <utility>

namespace exosnap::live_verify {
namespace {

QString Text(const char* literal) {
    return QString::fromLatin1(literal);
}

// Outcome / Succeeded / Failed are the shared execution result
// (control/session.h): what a command produced, before anything decides which
// fields this protocol version is allowed to carry.
using exosnap::control::Failed;
using exosnap::control::Outcome;
using exosnap::control::Succeeded;

// An intent that passed its precondition and still came back false. Rather than
// invent a second explanation, the policy is asked again against the state as it
// is NOW: if the product refused because something changed between the check and
// the call, the client gets that reason in the same structured form it would
// have got a moment earlier. Only when the state still permits the command is
// this an operational failure.
Outcome IntentRefused(const CommandDescriptor& command, const LiveVerifySource& source, const QString& error) {
    const PreconditionVerdict verdict = Evaluate(command, source.State());
    if (!verdict.allowed())
        return Failed(verdict.code, verdict.message, verdict.requirements, verdict.actual);
    return Failed(error_code::kOperationFailed, error);
}

QString ParamString(const QJsonObject& params, const char* name) {
    return params.value(Text(name)).toString();
}

qint64 ParamInt(const QJsonObject& params, const char* name) {
    return static_cast<qint64>(params.value(Text(name)).toDouble());
}

bool ParamBool(const QJsonObject& params, const char* name) {
    return params.value(Text(name)).toBool();
}

// --- Command execution ------------------------------------------------------

// The update area's own snapshot, plus what the last apply actually launched.
// The update fields are lifted out of the SAME StateToJson the shell snapshot
// publishes rather than assembled a second time, so update.getState and
// ui.getState can never describe different updates.
QJsonObject UpdatePayload(const LiveVerifySource& source) {
    QJsonObject result = StateToJson(source.State(), source.StateRevision()).value(QStringLiteral("update")).toObject();
    // Empty before the first apply. Carrying it here is what removes discovery
    // from the client: which child, at which endpoint, pinned to which version
    // is decided by the launch and reported by the launch.
    result.insert(QStringLiteral("updaterLaunch"), source.UpdaterLaunchSnapshot());
    return result;
}

Outcome ExecuteReadOnly(const QString& command, const QJsonObject& params, LiveVerifySource& source,
                        const QJsonObject& capabilities, const QJsonObject& described) {
    // system.capabilities and ipc.describe are assembled by the shared session
    // from this process's own command table and event list -- one payload
    // builder, so the two endpoints cannot describe themselves differently.
    if (command == QLatin1String("system.capabilities"))
        return Succeeded(capabilities);
    if (command == QLatin1String("ipc.describe"))
        return Succeeded(described);
    if (command == QLatin1String("ui.getState"))
        return Succeeded(StateToJson(source.State(), source.StateRevision()));
    if (command == QLatin1String("update.getState"))
        return Succeeded(UpdatePayload(source));
    if (command == QLatin1String("system.snapshot"))
        return Succeeded(source.SystemSnapshot());
    if (command == QLatin1String("app.snapshot"))
        return Succeeded(source.AppSnapshot());
    if (command == QLatin1String("window.snapshot"))
        return Succeeded(source.WindowSnapshot());
    if (command == QLatin1String("preview.snapshot"))
        return Succeeded(source.PreviewSnapshot());
    if (command == QLatin1String("record.snapshot"))
        return Succeeded(source.RecordSnapshot());
    if (command == QLatin1String("record.result"))
        return Succeeded(source.RecordResult());
    if (command == QLatin1String("overlay.snapshot"))
        return Succeeded(source.OverlaySnapshot());
    if (command == QLatin1String("editor.snapshot"))
        return Succeeded(source.EditorSnapshot());
    if (command == QLatin1String("diagnostics.snapshot"))
        return Succeeded(source.DiagnosticsSnapshot());

    // --- Observability ------------------------------------------------------
    // app.identity answers the SAME object system.hello carries. Deliberately
    // not a second identity: a handshake is a one-shot, and a client that wants
    // to re-read the build it is talking to mid-run had no way to.
    if (command == QLatin1String("app.identity"))
        return Succeeded(source.Identity());
    if (command == QLatin1String("pipeline.snapshot"))
        return Succeeded(source.PipelineSnapshot());
    if (command == QLatin1String("settings.snapshot"))
        return Succeeded(source.SettingsSnapshot());
    if (command == QLatin1String("diagnostics.results"))
        return Succeeded(source.DiagnosticsResults());
    if (command == QLatin1String("environment.snapshot"))
        return Succeeded(source.EnvironmentSnapshot());
    if (command == QLatin1String("windows.snapshot"))
        return Succeeded(source.WindowsSnapshot());
    if (command == QLatin1String("events.recent")) {
        QString error;
        const QJsonObject events = source.RecentEvents(params, &error);
        // A malformed filter is a client error, not an empty result: silently
        // widening a mistyped severity to "everything" is how a check that
        // filters for errors passes on a stream of Info records.
        if (!error.isEmpty())
            return Failed(error_code::kInvalidParams, error);
        return Succeeded(events);
    }
    if (command == QLatin1String("settings.describe"))
        return Succeeded(source.SettingsDescribe());
    if (command == QLatin1String("settings.get")) {
        QString error;
        const QJsonObject values = source.SettingsGet(ParamString(params, "key"), &error);
        if (!error.isEmpty())
            return Failed(error_code::kInvalidParams, error);
        return Succeeded(values);
    }
    if (command == QLatin1String("clip.snapshot"))
        return Succeeded(source.ClipSnapshot());
    if (command == QLatin1String("audio.routing.snapshot"))
        return Succeeded(source.AudioRoutingSnapshot());
    if (command == QLatin1String("profiles.list"))
        return Succeeded(source.ProfilesSnapshot());
    if (command == QLatin1String("notifications.snapshot"))
        return Succeeded(source.NotificationsSnapshot());
    if (command == QLatin1String("session.latest"))
        return Succeeded(source.SessionReport(QString()));
    if (command == QLatin1String("session.get")) {
        const QString id = ParamString(params, "recordingSessionId");
        if (id.isEmpty())
            return Failed(error_code::kInvalidParams, QStringLiteral("recordingSessionId must not be empty"));
        return Succeeded(source.SessionReport(id));
    }

    // Reachable only if a read-only command is added to the policy table without
    // a branch here; loud rather than silently answering nothing.
    return Failed(error_code::kUnknownCommand, QStringLiteral("Command %1 is listed but not implemented").arg(command));
}

// The reveal/scroll surface has to be the page the user is on. The pages stay
// resident after their first visit (QCR-602), so a Settings section IS still
// addressable from the Logs page -- and scrolling a page nobody is looking at,
// then reporting where it landed, is evidence of nothing.
//
// Enforced here rather than in the precondition table because it depends on a
// PARAMETER, and the table's predicates read only the state. It is still
// `invalid_state` with the same requires/actual shape a precondition produces,
// so a client sees no seam.
Outcome RefuseUnlessCurrentPage(const QString& surface, const LiveVerifySource& source) {
    const AutomationState state = source.State();
    if (state.page == surface)
        return Succeeded({});
    QJsonObject requirements;
    QJsonObject actual;
    requirements.insert(QStringLiteral("page"), surface);
    actual.insert(QStringLiteral("page"), state.page);
    return Failed(error_code::kInvalidState,
                  QStringLiteral("The %1 surface can only be addressed while it is the current page").arg(surface),
                  requirements, actual);
}

QJsonObject PopupResult(const char* key, bool open) {
    QJsonObject result;
    result.insert(Text(key), open ? QStringLiteral("open") : QStringLiteral("closed"));
    return result;
}

// A synchronous command whose postcondition did not hold when the state was read
// back. Declared synchronous means observable in this very response; if it is
// not, the honest answer is a failure, not `settled:false`.
Outcome PostconditionMissed(const QString& command, const QString& expectation) {
    return Failed(error_code::kOperationFailed,
                  QStringLiteral("%1 was accepted but %2 did not hold").arg(command, expectation));
}

Outcome Mismatch(const char* code, const QString& message, const QString& key, const QJsonValue& required,
                 const QJsonValue& observed) {
    QJsonObject requirements;
    QJsonObject actual;
    requirements.insert(key, required);
    actual.insert(key, observed);
    return Failed(Text(code), message, requirements, actual);
}

// An integral parameter, or `fallback` when it is absent. The shared validator
// only knows "a number"; an id or a millisecond position that is not a whole
// number would otherwise be truncated into a different value without a word.
std::optional<qint64> IntegralParam(const QJsonObject& params, const char* name, qint64 fallback, qint64 minimum,
                                    QString* error) {
    const QJsonValue value = params.value(Text(name));
    if (value.isUndefined() || value.isNull())
        return fallback;
    const double number = value.toDouble();
    if (!value.isDouble() || !std::isfinite(number) || std::trunc(number) != number ||
        std::abs(number) > 9007199254740992.0) {
        *error = QStringLiteral("\"%1\" must be a whole number").arg(Text(name));
        return std::nullopt;
    }
    if (number < static_cast<double>(minimum)) {
        *error = QStringLiteral("\"%1\" must be at least %2").arg(Text(name)).arg(minimum);
        return std::nullopt;
    }
    return static_cast<qint64>(number);
}

// The intent's own answer, before anything about the model's.
std::optional<Outcome> IntentFailure(LiveVerifySource::EditIntent intent, const CommandDescriptor& command,
                                     const LiveVerifySource& source, const QString& error) {
    switch (intent) {
    case LiveVerifySource::EditIntent::Delivered:
        return std::nullopt;
    case LiveVerifySource::EditIntent::InvalidTarget:
        return Failed(error_code::kInvalidParams, error);
    case LiveVerifySource::EditIntent::Failed:
        break;
    }
    return IntentRefused(command, source, error);
}

// The optimistic-concurrency check every timeline mutation offers. Refused
// before the edit runs: applying an edit to a timeline the caller has not seen
// is exactly the race the revision exists to rule out.
std::optional<Outcome> RefuseStaleRevision(const QJsonObject& params, const LiveVerifySource& source) {
    QString error;
    const std::optional<qint64> expected = IntegralParam(params, "expectedRevision", -1, -1, &error);
    if (!expected.has_value())
        return Failed(error_code::kInvalidParams, error);
    if (*expected < 0)
        return std::nullopt;
    const quint64 current = source.State().edit_workspace_revision;
    if (static_cast<quint64>(*expected) == current)
        return std::nullopt;
    return Mismatch(error_code::kInvalidState,
                    QStringLiteral("The timeline is at revision %1, not %2").arg(current).arg(*expected),
                    QStringLiteral("workspaceRevision"), static_cast<double>(*expected), static_cast<double>(current));
}

// Runs one timeline mutation and reports what the model did with it. The
// editor snapshot is the postcondition; `edit` says whether the model accepted
// the attempt and whether its revision moved. A model refusal is `blocked`:
// the workspace is in an ordinary state and its rules said no, in the words the
// timeline header shows.
template <class Intent>
Outcome TimelineEdit(const CommandDescriptor& command, const QJsonObject& params, LiveVerifySource& source,
                     Intent&& intent) {
    if (const std::optional<Outcome> stale = RefuseStaleRevision(params, source))
        return *stale;
    QString error;
    const quint64 revision_before = source.State().edit_workspace_revision;
    const quint64 attempts_before = source.LastEditOutcome().sequence;
    if (const std::optional<Outcome> failed = IntentFailure(intent(&error), command, source, error))
        return *failed;
    const LiveVerifySource::EditOutcome outcome = source.LastEditOutcome();
    const quint64 revision_after = source.State().edit_workspace_revision;
    const bool attempted = outcome.sequence != attempts_before;
    if (attempted && !outcome.accepted) {
        return Mismatch(error_code::kBlocked,
                        outcome.error.isEmpty() ? QStringLiteral("The timeline refused the edit") : outcome.error,
                        QStringLiteral("edit"), QStringLiteral("accepted"), QStringLiteral("refused"));
    }
    QJsonObject result = source.EditorSnapshot();
    QJsonObject edit;
    edit.insert(QStringLiteral("attempted"), attempted);
    edit.insert(QStringLiteral("changed"), revision_after != revision_before);
    edit.insert(QStringLiteral("revisionBefore"), static_cast<double>(revision_before));
    edit.insert(QStringLiteral("revisionAfter"), static_cast<double>(revision_after));
    result.insert(QStringLiteral("edit"), edit);
    return Succeeded(result);
}

QString PanelStateName(bool open, bool pinned) {
    return !open ? QStringLiteral("closed") : pinned ? QStringLiteral("pinned") : QStringLiteral("open");
}

// The editor commands, or nullopt when `command` is not one of them.
std::optional<Outcome> ExecuteEditWorkspace(const CommandDescriptor& command, const QJsonObject& params,
                                            LiveVerifySource& source) {
    const QString& name = command.name;
    QString error;
    const auto integral = [&](const char* key, qint64 fallback, qint64 minimum) {
        return IntegralParam(params, key, fallback, minimum, &error);
    };
    const auto invalid = [&]() { return Failed(error_code::kInvalidParams, error); };
    const auto snapshot = [&]() { return Succeeded(source.EditorSnapshot()); };

    // --- Workspace contents ---------------------------------------------------

    if (name == QLatin1String("edit.importMedia") || name == QLatin1String("edit.appendRecording")) {
        const auto at = integral("atMs", -1, -1);
        if (!at)
            return invalid();
        const QString path = ParamString(params, "path");
        const LiveVerifySource::EditIntent intent =
            name == QLatin1String("edit.importMedia")
                ? source.EditImportMedia(path, ParamBool(params, "append"), *at, &error)
                : source.EditAppendRecording(path, *at, &error);
        if (const std::optional<Outcome> failed = IntentFailure(intent, command, source, error))
            return *failed;
        // The probe runs off this thread; importPending going false again, with
        // the media list or the workspace error changed, is the completion.
        return Succeeded(source.EditorSnapshot(), /*settled=*/false);
    }
    if (name == QLatin1String("edit.appendMedia")) {
        const auto media = integral("mediaId", 0, 1);
        const auto at = integral("atMs", -1, -1);
        if (!media || !at)
            return invalid();
        return TimelineEdit(command, params, source,
                            [&](QString* e) { return source.EditAppendMedia(*media, *at, e); });
    }

    // --- Selection ---------------------------------------------------------------
    // Selection is not an edit, so it has no model outcome; the state that
    // reports it is the postcondition.

    if (name == QLatin1String("edit.selectClip") || name == QLatin1String("edit.selectTransition")) {
        const auto id = integral("clipId", 0, 0);
        if (!id)
            return invalid();
        const bool clip = name == QLatin1String("edit.selectClip");
        const LiveVerifySource::EditIntent intent =
            clip ? source.EditSelectClip(*id, &error) : source.EditSelectTransition(*id, &error);
        if (const std::optional<Outcome> failed = IntentFailure(intent, command, source, error))
            return *failed;
        const AutomationState after = source.State();
        const quint64 selected = clip ? after.edit_selected_clip : after.edit_selected_transition;
        if (selected != static_cast<quint64>(*id))
            return PostconditionMissed(name, QStringLiteral("the selection becoming %1").arg(*id));
        return snapshot();
    }
    if (name == QLatin1String("edit.selectGap")) {
        const auto track = integral("trackId", 0, 1);
        const auto at = integral("atMs", 0, 0);
        if (!track || !at)
            return invalid();
        if (const std::optional<Outcome> failed =
                IntentFailure(source.EditSelectGap(*track, *at, &error), command, source, error))
            return *failed;
        if (!source.State().edit_gap_selected)
            return PostconditionMissed(name, QStringLiteral("a selected gap"));
        return snapshot();
    }

    // --- Timeline edits ----------------------------------------------------------

    if (name == QLatin1String("edit.moveClip")) {
        const auto clip = integral("clipId", 0, 1);
        const auto at = integral("atMs", 0, 0);
        if (!clip || !at)
            return invalid();
        const bool snap = params.contains(QStringLiteral("snap")) ? ParamBool(params, "snap") : true;
        return TimelineEdit(command, params, source,
                            [&](QString* e) { return source.EditMoveClip(*clip, *at, snap, e); });
    }
    if (name == QLatin1String("edit.trimClip")) {
        const auto clip = integral("clipId", 0, 1);
        const auto in = integral("inMs", 0, 0);
        const auto out = integral("outMs", 0, 0);
        if (!clip || !in || !out)
            return invalid();
        return TimelineEdit(command, params, source,
                            [&](QString* e) { return source.EditTrimClip(*clip, *in, *out, e); });
    }
    if (name == QLatin1String("edit.split"))
        return TimelineEdit(command, params, source, [&](QString* e) { return source.EditSplit(e); });
    if (name == QLatin1String("edit.delete")) {
        const bool ripple = ParamBool(params, "ripple");
        return TimelineEdit(command, params, source, [&](QString* e) { return source.EditDelete(ripple, e); });
    }
    if (name == QLatin1String("edit.closeGap"))
        return TimelineEdit(command, params, source, [&](QString* e) { return source.EditCloseGap(e); });
    if (name == QLatin1String("edit.deleteTrack")) {
        const auto track = integral("trackId", 0, 1);
        if (!track)
            return invalid();
        const bool confirmed = ParamBool(params, "confirmed");
        return TimelineEdit(command, params, source,
                            [&](QString* e) { return source.EditDeleteTrack(*track, confirmed, e); });
    }
    if (name == QLatin1String("edit.undo"))
        return TimelineEdit(command, params, source, [&](QString* e) { return source.EditUndo(e); });
    if (name == QLatin1String("edit.redo"))
        return TimelineEdit(command, params, source, [&](QString* e) { return source.EditRedo(e); });
    if (name == QLatin1String("edit.applyCrossfade")) {
        const auto clip = integral("clipId", 0, 0);
        const auto duration = integral("durationMs", 0, 1);
        if (!clip || !duration)
            return invalid();
        return TimelineEdit(command, params, source,
                            [&](QString* e) { return source.EditApplyCrossfade(*clip, *duration, e); });
    }
    if (name == QLatin1String("edit.removeTransition"))
        return TimelineEdit(command, params, source, [&](QString* e) { return source.EditRemoveTransition(e); });
    if (name == QLatin1String("edit.setJunctionWindow")) {
        const auto start = integral("startMs", 0, 0);
        const auto end = integral("endMs", 0, 0);
        if (!start || !end)
            return invalid();
        return TimelineEdit(command, params, source,
                            [&](QString* e) { return source.EditSetJunctionWindow(*start, *end, e); });
    }
    if (name == QLatin1String("edit.setJunctionCurve")) {
        const QJsonValue bias = params.value(QStringLiteral("bias"));
        if (!bias.isUndefined() && !bias.isNull() &&
            (!bias.isDouble() || !std::isfinite(bias.toDouble()) || std::abs(bias.toDouble()) > 1.0))
            return Failed(error_code::kInvalidParams, QStringLiteral("\"bias\" must be a number in [-1, 1]"));
        const QString curve = ParamString(params, "curve");
        return TimelineEdit(command, params, source,
                            [&](QString* e) { return source.EditSetJunctionCurve(curve, bias.toDouble(0.0), e); });
    }
    if (name == QLatin1String("edit.setJunctionAudioLaw")) {
        const QString law = ParamString(params, "law");
        return TimelineEdit(command, params, source,
                            [&](QString* e) { return source.EditSetJunctionAudioLaw(law, e); });
    }
    if (name == QLatin1String("edit.setJunctionCut")) {
        const auto at = integral("atMs", 0, 0);
        if (!at)
            return invalid();
        return TimelineEdit(command, params, source, [&](QString* e) { return source.EditSetJunctionCut(*at, e); });
    }
    if (name == QLatin1String("edit.setClipFade")) {
        const auto clip = integral("clipId", 0, 1);
        const auto duration = integral("durationMs", 0, 0);
        if (!clip || !duration)
            return invalid();
        const bool incoming = ParamString(params, "edge") == QLatin1String("in");
        return TimelineEdit(command, params, source,
                            [&](QString* e) { return source.EditSetClipFade(*clip, incoming, *duration, e); });
    }
    if (name == QLatin1String("edit.setFixedTimelineEnd")) {
        const bool fixed = ParamBool(params, "fixed");
        const auto end = integral("endMs", 0, 0);
        if (!end)
            return invalid();
        if (fixed && !params.contains(QStringLiteral("endMs")))
            return Failed(error_code::kInvalidParams, QStringLiteral("A fixed timeline end needs \"endMs\""));
        return TimelineEdit(command, params, source,
                            [&](QString* e) { return source.EditSetFixedTimelineEnd(fixed, *end, e); });
    }

    // --- Interaction options and the time selection ------------------------------

    if (name == QLatin1String("edit.setOption")) {
        const QString option = ParamString(params, "option");
        const bool enabled = ParamBool(params, "enabled");
        if (const std::optional<Outcome> failed =
                IntentFailure(source.EditSetOption(option, enabled, &error), command, source, error))
            return *failed;
        const AutomationState after = source.State();
        const bool now = option == QLatin1String("snapping")        ? after.edit_snapping
                         : option == QLatin1String("ripple")        ? after.edit_ripple
                         : option == QLatin1String("autoCrossfade") ? after.edit_auto_crossfade
                                                                    : after.edit_linked_selection;
        if (now != enabled)
            return PostconditionMissed(
                name,
                QStringLiteral("%1 becoming %2").arg(option, enabled ? QStringLiteral("on") : QStringLiteral("off")));
        return snapshot();
    }
    if (name == QLatin1String("edit.setSelectionBoundary")) {
        const bool out = ParamString(params, "boundary") == QLatin1String("out");
        if (const std::optional<Outcome> failed =
                IntentFailure(source.EditSetSelectionBoundary(out, &error), command, source, error))
            return *failed;
        return snapshot();
    }

    // --- Playback and monitoring -------------------------------------------------

    if (name == QLatin1String("edit.play") || name == QLatin1String("edit.pause")) {
        const bool play = name == QLatin1String("edit.play");
        if (const std::optional<Outcome> failed =
                IntentFailure(source.EditSetPlaying(play, &error), command, source, error))
            return *failed;
        // The worker confirms the transport; editPlayback is the completion.
        const bool reached =
            source.State().edit_playback == (play ? QLatin1String("playing") : QLatin1String("paused"));
        return Succeeded(source.EditorSnapshot(), reached);
    }
    if (name == QLatin1String("edit.frameStep")) {
        const int direction = ParamString(params, "direction") == QLatin1String("previous") ? -1 : 1;
        if (const std::optional<Outcome> failed =
                IntentFailure(source.EditFrameStep(direction, &error), command, source, error))
            return *failed;
        return snapshot();
    }
    if (name == QLatin1String("edit.setVolume")) {
        const QJsonValue value = params.value(QStringLiteral("volume"));
        if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < 0.0 || value.toDouble() > 1.0)
            return Failed(error_code::kInvalidParams, QStringLiteral("\"volume\" must be a number in [0, 1]"));
        if (const std::optional<Outcome> failed =
                IntentFailure(source.EditSetVolume(value.toDouble(), &error), command, source, error))
            return *failed;
        return snapshot();
    }
    if (name == QLatin1String("edit.confirmMonitoring")) {
        const QJsonValue ids = params.value(QStringLiteral("trackIds"));
        if (!ids.isArray())
            return Failed(error_code::kInvalidParams, QStringLiteral("\"trackIds\" must be an array of strings"));
        QStringList track_ids;
        for (const QJsonValue& id : ids.toArray()) {
            if (!id.isString() || id.toString().isEmpty())
                return Failed(error_code::kInvalidParams, QStringLiteral("\"trackIds\" must be an array of strings"));
            track_ids.append(id.toString());
        }
        if (const std::optional<Outcome> failed =
                IntentFailure(source.EditConfirmMonitoring(track_ids, ParamBool(params, "overlapConfirmed"), &error),
                              command, source, error))
            return *failed;
        // The player rebuilds its audio graph on its worker; the confirmation
        // requirement clearing is the completion.
        return Succeeded(source.EditorSnapshot(), !source.State().edit_monitoring_confirmation_required);
    }

    // --- Panels ------------------------------------------------------------------

    if (name == QLatin1String("edit.setPanel")) {
        const QString panel = ParamString(params, "panel");
        const bool open = ParamBool(params, "open");
        const bool pinned = open && ParamBool(params, "pinned");
        const bool media = panel == QLatin1String("media");
        const AutomationState before = source.State();
        if (pinned && !(media ? before.edit_media_panel_can_pin : before.edit_details_panel_can_pin)) {
            // The page's own rule: a pinned panel has to leave the viewer at
            // least its minimum width.
            return Mismatch(error_code::kBlocked,
                            QStringLiteral("The viewer is too narrow to pin the %1 panel").arg(panel),
                            QStringLiteral("canPin"), true, false);
        }
        if (const std::optional<Outcome> failed =
                IntentFailure(source.EditSetPanel(panel, open, pinned, &error), command, source, error))
            return *failed;
        const QString expected = PanelStateName(open, pinned);
        const QString reached = media ? source.State().edit_media_panel : source.State().edit_details_panel;
        if (reached != expected)
            return PostconditionMissed(name, QStringLiteral("the %1 panel becoming %2").arg(panel, expected));
        return snapshot();
    }
    if (name == QLatin1String("edit.setSourceTab")) {
        const QString tab = ParamString(params, "tab");
        if (const std::optional<Outcome> failed =
                IntentFailure(source.EditSetSourceTab(tab, &error), command, source, error))
            return *failed;
        if (source.State().edit_source_tab != tab)
            return PostconditionMissed(name, QStringLiteral("the %1 tab showing").arg(tab));
        return snapshot();
    }

    return std::nullopt;
}

Outcome ExecuteMutating(const CommandDescriptor& command, const ParsedRequest& request, LiveVerifySource& source) {
    QString error;
    const QJsonObject& params = request.params;

    if (command.name == QLatin1String("app.quit")) {
        if (!source.AppQuit(&error))
            return IntentRefused(command, source, error);
        // Accepted, not finished: the exit happens after this answer is sent, and
        // its completion is the process ending.
        QJsonObject result;
        result.insert(QStringLiteral("quitting"), true);
        return Succeeded(result, /*settled=*/false);
    }

    if (command.name == QLatin1String("window.moveToScreen")) {
        if (!source.MoveWindowToScreen(ParamString(params, "screen"), &error))
            return IntentRefused(command, source, error);
        return Succeeded(source.WindowSnapshot(), /*settled=*/false);
    }

    if (command.name == QLatin1String("record.selectTarget")) {
        if (!source.SelectRecordTarget(ParamString(params, "kind"), ParamString(params, "titleFilter"), &error))
            return IntentRefused(command, source, error);
        return Succeeded(source.RecordSnapshot());
    }

    if (command.name == QLatin1String("record.selectRegion")) {
        if (!source.SelectRecordRegion(ParamString(params, "display"), static_cast<int>(ParamInt(params, "x")),
                                       static_cast<int>(ParamInt(params, "y")),
                                       static_cast<int>(ParamInt(params, "width")),
                                       static_cast<int>(ParamInt(params, "height")), &error))
            return IntentRefused(command, source, error);
        return Succeeded(source.RecordSnapshot());
    }

    if (command.name == QLatin1String("record.openRegionSelector")) {
        if (!source.OpenRegionSelector(ParamString(params, "display"), &error))
            return IntentRefused(command, source, error);
        return Succeeded(source.RecordSnapshot());
    }

    // The six transport intents. Asynchronous by nature: the returned snapshot
    // describes the state at the moment the intent was accepted, and the
    // authoritative confirmation is the record.stateChanged event or the next
    // stateRevision.
    const auto transport = [&](bool (LiveVerifySource::*intent)(QString*)) {
        if (!(source.*intent)(&error))
            return IntentRefused(command, source, error);
        return Succeeded(source.RecordSnapshot(), /*settled=*/false);
    };
    if (command.name == QLatin1String("record.start"))
        return transport(&LiveVerifySource::RecordStart);
    if (command.name == QLatin1String("record.pause"))
        return transport(&LiveVerifySource::RecordPause);
    if (command.name == QLatin1String("record.resume"))
        return transport(&LiveVerifySource::RecordResume);
    if (command.name == QLatin1String("record.stop"))
        return transport(&LiveVerifySource::RecordStop);
    if (command.name == QLatin1String("record.split"))
        return transport(&LiveVerifySource::RecordSplit);
    if (command.name == QLatin1String("record.captureFrame"))
        return transport(&LiveVerifySource::RecordCaptureFrame);
    if (command.name == QLatin1String("record.addMarker"))
        return transport(&LiveVerifySource::RecordAddMarker);

    if (command.name == QLatin1String("record.cancelCountdown")) {
        if (!source.RecordCancelCountdown(&error))
            return IntentRefused(command, source, error);
        // Declared synchronous, so the countdown really has to be gone by the
        // time this answers -- the transport press that cancels it is a direct
        // connection, and anything else would be `settled:false`.
        if (source.State().countdown_active)
            return PostconditionMissed(command.name, QStringLiteral("the countdown ending"));
        return Succeeded(source.RecordSnapshot());
    }

    if (command.name == QLatin1String("ui.navigate")) {
        const QString page = ParamString(params, "page");
        if (!source.Navigate(page, &error))
            return IntentRefused(command, source, error);
        // The RESULTING page, not the requested one. Navigating to the page you
        // are already on is a successful no-op, which is the same answer. The
        // legacy "logs" request is the one alias: the shell normalizes it to
        // Diagnostics with the logs subview, so the canonical page is what the
        // state reports and what satisfies the postcondition.
        const AutomationState after = source.State();
        const bool normalized_logs = page == QLatin1String(page_name::kLogs) &&
                                     after.page == QLatin1String(page_name::kDiagnostics) &&
                                     after.diagnostics_section == QLatin1String(diagnostics_section_name::kLogs);
        if (after.page != page && !normalized_logs)
            return PostconditionMissed(command.name, QStringLiteral("the shell reaching \"%1\"").arg(page));
        QJsonObject result;
        result.insert(QStringLiteral("page"), after.page);
        return Succeeded(result);
    }

    if (command.name == QLatin1String("ui.reveal")) {
        const QString surface = ParamString(params, "surface");
        const QString target = ParamString(params, "target");
        if (const Outcome wrong = RefuseUnlessCurrentPage(surface, source); !wrong.ok)
            return wrong;
        switch (source.Reveal(surface, target, &error)) {
        case LiveVerifySource::RevealOutcome::UnknownTarget:
            return Failed(error_code::kInvalidParams, error);
        case LiveVerifySource::RevealOutcome::Failed:
            return IntentRefused(command, source, error);
        case LiveVerifySource::RevealOutcome::Revealed:
            break;
        }
        QJsonObject result;
        result.insert(QStringLiteral("surface"), surface);
        result.insert(QStringLiteral("target"), target);
        result.insert(QStringLiteral("revealed"), true);
        return Succeeded(result);
    }

    if (command.name == QLatin1String("ui.scrollHome") || command.name == QLatin1String("ui.scrollEnd")) {
        const bool to_end = command.name.endsWith(QLatin1String("End"));
        const QString surface = ParamString(params, "surface");
        if (const Outcome wrong = RefuseUnlessCurrentPage(surface, source); !wrong.ok)
            return wrong;
        const bool moved = to_end ? source.ScrollEnd(surface, &error) : source.ScrollHome(surface, &error);
        if (!moved)
            return IntentRefused(command, source, error);
        QJsonObject result;
        result.insert(QStringLiteral("surface"), surface);
        result.insert(to_end ? QStringLiteral("atEnd") : QStringLiteral("atHome"), true);
        return Succeeded(result);
    }

    if (command.name == QLatin1String("edit.open")) {
        if (!source.EditOpen(&error))
            return IntentRefused(command, source, error);
        // Settled once the recording is on the timeline. A recording whose file
        // is present is probed first, and the session opens when that lands.
        const AutomationState after = source.State();
        if (!after.edit_session_open && !after.edit_import_pending)
            return PostconditionMissed(command.name, QStringLiteral("an open edit session"));
        return Succeeded(source.EditorSnapshot(), after.edit_session_open && !after.edit_import_pending);
    }

    if (command.name == QLatin1String("edit.close")) {
        if (!source.EditClose(&error))
            return IntentRefused(command, source, error);
        if (source.State().edit_session_open)
            return PostconditionMissed(command.name, QStringLiteral("a closed edit session"));
        return Succeeded(source.EditorSnapshot());
    }

    const auto edit_intent = [&](bool (LiveVerifySource::*intent)(QString*)) {
        if (!(source.*intent)(&error))
            return IntentRefused(command, source, error);
        // The editor snapshot IS the postcondition: it carries the clamped
        // position, the ordered trim range and the playback flag the adapter
        // published while answering.
        return Succeeded(source.EditorSnapshot());
    };
    if (command.name == QLatin1String("edit.playPause"))
        return edit_intent(&LiveVerifySource::EditPlayPause);
    if (command.name == QLatin1String("edit.timelineHome"))
        return edit_intent(&LiveVerifySource::EditTimelineHome);
    if (command.name == QLatin1String("edit.timelineEnd"))
        return edit_intent(&LiveVerifySource::EditTimelineEnd);

    const auto edit_position = [&](bool (LiveVerifySource::*intent)(qint64, QString*)) {
        if (!(source.*intent)(ParamInt(params, "positionMs"), &error))
            return IntentRefused(command, source, error);
        return Succeeded(source.EditorSnapshot());
    };
    if (command.name == QLatin1String("edit.seek"))
        return edit_position(&LiveVerifySource::EditSeek);
    if (command.name == QLatin1String("edit.setTrimIn"))
        return edit_position(&LiveVerifySource::EditSetTrimIn);
    if (command.name == QLatin1String("edit.setTrimOut"))
        return edit_position(&LiveVerifySource::EditSetTrimOut);

    if (const std::optional<Outcome> edit = ExecuteEditWorkspace(command, params, source))
        return *edit;

    if (command.name == QLatin1String("sourcePicker.open") || command.name == QLatin1String("sourcePicker.close")) {
        const bool open = command.name.endsWith(QLatin1String("open"));
        if (!source.SetSourcePickerOpen(open, &error))
            return IntentRefused(command, source, error);
        if (source.State().source_picker_open != open)
            return PostconditionMissed(command.name, QStringLiteral("the picker changing state"));
        return Succeeded(PopupResult("sourcePicker", open));
    }

    if (command.name == QLatin1String("notificationHub.open") ||
        command.name == QLatin1String("notificationHub.close")) {
        const bool open = command.name.endsWith(QLatin1String("open"));
        if (!source.SetNotificationHubOpen(open, &error))
            return IntentRefused(command, source, error);
        if (source.State().notification_hub_open != open)
            return PostconditionMissed(command.name, QStringLiteral("the hub changing state"));
        return Succeeded(PopupResult("notificationHub", open));
    }

    if (command.name == QLatin1String("update.check")) {
        if (!source.UpdateCheck(&error))
            return IntentRefused(command, source, error);
        // Accepted, not answered. The check runs on a pool thread and reports
        // through the card's next state, which is a stateRevision advance.
        return Succeeded(UpdatePayload(source), /*settled=*/false);
    }

    if (command.name == QLatin1String("update.apply")) {
        if (!source.UpdateApply(&error))
            return IntentRefused(command, source, error);
        // The updater process has been started by the time this returns
        // (LaunchUpdater stages and spawns synchronously), so the response
        // already carries the child's pid, its staged binary and the endpoint it
        // was given. The UPDATE, of course, has not happened -- hence
        // settled:false and a completion that lives in another process.
        return Succeeded(UpdatePayload(source), /*settled=*/false);
    }

    // --- Settings and profiles ----------------------------------------------

    if (command.name == QLatin1String("webcam.overlay.set")) {
        QJsonObject applied;
        if (!source.WebcamOverlaySet(params, &applied, &error))
            return IntentRefused(command, source, error);
        // The applied state, the sequence it was given and the counter reading
        // after the store. The request is echoed beside it rather than in place
        // of it, so a clamp is visible without a second round trip.
        applied.insert(QStringLiteral("requested"), params);
        return Succeeded(applied);
    }

    if (command.name == QLatin1String("settings.set")) {
        const QString key = ParamString(params, "key");
        if (!source.SettingsSet(key, params.value(QStringLiteral("value")), &error))
            return IntentRefused(command, source, error);
        // The RECONCILED value, read back through the same key. A response that
        // echoed the request would hide the whole point of writing through the
        // product edge: MP4 turns an AV1 request into H.264, and the caller has
        // to be able to see that without a second round trip.
        QString read_error;
        QJsonObject result = source.SettingsGet(key, &read_error);
        result.insert(QStringLiteral("requested"), params.value(QStringLiteral("value")));
        return Succeeded(result);
    }

    if (command.name == QLatin1String("clip.request")) {
        if (!source.ClipRequest(ParamString(params, "durationSeconds"), &error))
            return IntentRefused(command, source, error);
        return Succeeded(source.ClipSnapshot());
    }
    if (command.name == QLatin1String("clip.confirm")) {
        if (!source.ClipConfirm(ParamString(params, "jobId"), ParamString(params, "generation"), &error))
            return IntentRefused(command, source, error);
        return Succeeded(source.ClipSnapshot());
    }
    if (command.name == QLatin1String("clip.open")) {
        if (!source.ClipOpen(ParamString(params, "assetId"), &error))
            return IntentRefused(command, source, error);
        return Succeeded(source.EditorSnapshot());
    }
    if (command.name == QLatin1String("audio.routing.setTrackRecordEnabled")) {
        const qint64 revision = params.value(QStringLiteral("expectedRevision")).toInteger(-1);
        if (revision < 0)
            return Failed(error_code::kInvalidParams, QStringLiteral("expectedRevision must be nonnegative"));
        if (!source.SetAudioTrackRecordEnabled(ParamString(params, "trackId"),
                                               params.value(QStringLiteral("enabled")).toBool(),
                                               static_cast<quint64>(revision), &error))
            return IntentRefused(command, source, error);
        return Succeeded(source.AudioRoutingSnapshot());
    }
    if (command.name == QLatin1String("audio.routing.addTrack") ||
        command.name == QLatin1String("audio.routing.assignSource")) {
        const qint64 revision = params.value(QStringLiteral("expectedRevision")).toInteger(-1);
        if (revision < 0)
            return Failed(error_code::kInvalidParams, QStringLiteral("expectedRevision must be nonnegative"));
        const bool accepted =
            command.name == QLatin1String("audio.routing.addTrack")
                ? source.AddAudioTrack(static_cast<quint64>(revision), &error)
                : source.AssignAudioSource(ParamString(params, "sourceId"), ParamString(params, "trackId"),
                                           params.value(QStringLiteral("mode")).toInt(), static_cast<quint64>(revision),
                                           &error);
        if (!accepted)
            return IntentRefused(command, source, error);
        return Succeeded(source.AudioRoutingSnapshot());
    }
    if (command.name == QLatin1String("settings.reset")) {
        if (!source.SettingsReset(&error))
            return IntentRefused(command, source, error);
        QString read_error;
        return Succeeded(source.SettingsGet(QString(), &read_error));
    }

    if (command.name == QLatin1String("profiles.select")) {
        const QString id = ParamString(params, "id");
        if (!source.ProfileSelect(id, &error))
            return IntentRefused(command, source, error);
        if (source.State().profile_id != id)
            return PostconditionMissed(command.name, QStringLiteral("the profile becoming \"%1\"").arg(id));
        return Succeeded(source.ProfilesSnapshot());
    }

    if (command.name == QLatin1String("profiles.create")) {
        if (!source.ProfileCreate(ParamString(params, "name"), &error))
            return IntentRefused(command, source, error);
        return Succeeded(source.ProfilesSnapshot());
    }

    if (command.name == QLatin1String("profiles.rename")) {
        if (!source.ProfileRename(ParamString(params, "name"), &error))
            return IntentRefused(command, source, error);
        return Succeeded(source.ProfilesSnapshot());
    }

    if (command.name == QLatin1String("profiles.delete")) {
        if (!source.ProfileDelete(&error))
            return IntentRefused(command, source, error);
        return Succeeded(source.ProfilesSnapshot());
    }

    // --- Export ---------------------------------------------------------------
    // Both asynchronous: a remux runs on its own thread and finishes into the
    // export state, which is what a client waits on.

    if (command.name == QLatin1String("export.start")) {
        if (!source.ExportStart(&error))
            return IntentRefused(command, source, error);
        return Succeeded(source.EditorSnapshot(), /*settled=*/false);
    }
    if (command.name == QLatin1String("export.cancel")) {
        if (!source.ExportCancel(&error))
            return IntentRefused(command, source, error);
        return Succeeded(source.EditorSnapshot(), /*settled=*/false);
    }
    if (command.name == QLatin1String("export.setOptions")) {
        if (!source.ExportSetOptions(params, &error))
            return IntentRefused(command, source, error);
        // Read back rather than echoed: the panel is the authority on what it
        // accepted, and a refused value has to show as one.
        const QJsonObject result = source.EditorSnapshot();
        const QJsonObject exported = result.value(QStringLiteral("export")).toObject();
        for (const char* key : {"profile", "mode", "span", "container"}) {
            const QJsonValue requested = params.value(Text(key));
            if (requested.isString() && exported.value(Text(key)) != requested)
                return PostconditionMissed(
                    command.name, QStringLiteral("the export %1 becoming %2").arg(Text(key), requested.toString()));
        }
        return Succeeded(result);
    }
    if (command.name == QLatin1String("export.confirm")) {
        if (!source.ExportConfirm(&error))
            return IntentRefused(command, source, error);
        // The run starts on its own thread, or the plan is redone when the
        // workspace changed under the review; exportState is the completion.
        return Succeeded(source.EditorSnapshot(), /*settled=*/false);
    }
    if (command.name == QLatin1String("export.discard") || command.name == QLatin1String("export.reset")) {
        const bool discard = command.name.endsWith(QLatin1String("discard"));
        if (!(discard ? source.ExportDiscard(&error) : source.ExportReset(&error)))
            return IntentRefused(command, source, error);
        if (source.State().edit_export_state != QLatin1String("idle"))
            return PostconditionMissed(command.name, QStringLiteral("the export options returning"));
        return Succeeded(source.EditorSnapshot());
    }

    // --- Diagnostics and logs --------------------------------------------------

    if (command.name == QLatin1String("diagnostics.run")) {
        if (!source.DiagnosticsRun(&error))
            return IntentRefused(command, source, error);
        // The probe runs on a worker thread; `checking` going false again is the
        // completion, and that is a stateRevision advance.
        return Succeeded(source.DiagnosticsSnapshot(), /*settled=*/false);
    }
    if (command.name == QLatin1String("diagnostics.setInDepth")) {
        const bool enabled = ParamBool(params, "enabled");
        if (!source.DiagnosticsSetInDepth(enabled, &error))
            return IntentRefused(command, source, error);
        return Succeeded(source.DiagnosticsSnapshot());
    }
    if (command.name == QLatin1String("logs.open")) {
        if (!source.LogsOpen(&error))
            return IntentRefused(command, source, error);
        QJsonObject result;
        result.insert(QStringLiteral("page"), source.State().page);
        return Succeeded(result);
    }

    // --- Blocking surfaces ------------------------------------------------------

    if (command.name == QLatin1String("recovery.continue") || command.name == QLatin1String("recovery.discard")) {
        const int index = static_cast<int>(ParamInt(params, "index"));
        const bool discard = command.name.endsWith(QLatin1String("discard"));
        if (!(discard ? source.RecoveryDiscard(index, &error) : source.RecoveryContinue(index, &error)))
            return IntentRefused(command, source, error);
        // Both do real work on real files, off this thread. What is settled is
        // that the request was accepted.
        return Succeeded(StateToJson(source.State(), source.StateRevision()), /*settled=*/false);
    }

    const auto surface_action = [&](bool (LiveVerifySource::*intent)(QString*), const char* expectation) {
        if (!(source.*intent)(&error))
            return IntentRefused(command, source, error);
        if (!source.State().blocking_surface.isEmpty())
            return PostconditionMissed(command.name, Text(expectation));
        return Succeeded(StateToJson(source.State(), source.StateRevision()));
    };
    if (command.name == QLatin1String("recovery.dismiss"))
        return surface_action(&LiveVerifySource::RecoveryDismiss, "the recovery surface closing");
    if (command.name == QLatin1String("crashReport.send"))
        return surface_action(&LiveVerifySource::CrashReportSend, "the crash surface closing");
    if (command.name == QLatin1String("crashReport.decline"))
        return surface_action(&LiveVerifySource::CrashReportDecline, "the crash surface closing");
    if (command.name == QLatin1String("recordingError.dismiss"))
        return surface_action(&LiveVerifySource::RecordingErrorDismiss, "the failure surface closing");
    if (command.name == QLatin1String("recordingError.sendReport"))
        return surface_action(&LiveVerifySource::RecordingErrorSendReport, "the failure surface closing");

    // --- Notifications ----------------------------------------------------------

    if (command.name == QLatin1String("notification.dismiss")) {
        const qint64 sequence = ParamInt(params, "sequence");
        if (!source.NotificationDismiss(sequence, &error))
            return IntentRefused(command, source, error);
        return Succeeded(source.NotificationsSnapshot());
    }

    if (command.name == QLatin1String("notification.invokeAction")) {
        const QString which = ParamString(params, "action");
        if (!source.NotificationInvokeAction(ParamInt(params, "sequence"),
                                             which.isEmpty() ? QStringLiteral("primary") : which, &error)) {
            return IntentRefused(command, source, error);
        }
        // An action navigates, opens Explorer or relaunches elevated. None of
        // those is observable in this response.
        return Succeeded(source.NotificationsSnapshot(), /*settled=*/false);
    }

    if (command.name == QLatin1String("notification.raise")) {
        qint64 sequence = 0;
        if (!source.NotificationRaise(ParamString(params, "type"), ParamString(params, "title"),
                                      ParamString(params, "body"), ParamString(params, "action"),
                                      ParamString(params, "actionPayload"), &sequence, &error)) {
            return IntentRefused(command, source, error);
        }
        QJsonObject result = source.NotificationsSnapshot();
        // The identity the caller needs to dismiss it again, and the reminder that
        // what it just created is not product behaviour.
        result.insert(QStringLiteral("sequence"), static_cast<double>(sequence));
        result.insert(QStringLiteral("synthetic"), true);
        return Succeeded(result);
    }

    if (command.name == QLatin1String("notification.clearAll")) {
        if (!source.ClearNotifications(&error))
            return IntentRefused(command, source, error);
        QJsonObject result;
        result.insert(QStringLiteral("cleared"), true);
        return Succeeded(result);
    }

    return Failed(error_code::kUnknownCommand,
                  QStringLiteral("Command %1 is listed but not implemented").arg(command.name));
}

} // namespace

LiveVerifyDispatcher::LiveVerifyDispatcher(LiveVerifySource* source, QString run_id)
    : exosnap::control::ControlSession<AutomationState>(std::move(run_id)), source_(source) {
}

QStringList LiveVerifyDispatcher::CommandNames(int protocol) {
    return CommandNamesForProtocol(protocol);
}

QStringList LiveVerifyDispatcher::EventNames(int protocol) {
    QStringList names = {QStringLiteral("app.ready"), QStringLiteral("record.resultReady"),
                         QStringLiteral("record.stateChanged"), QStringLiteral("window.screenChanged")};
    // Protocol 2's general-purpose settle signal: it fires whenever the
    // observable product state changes, which is what lets a client wait on an
    // asynchronous command without knowing which domain-specific event covers
    // it -- or poll for one that does not exist, as the blocking surfaces had no
    // event at all.
    if (protocol >= 2) {
        names.append(QStringLiteral("ui.stateChanged"));
        // The one update event that is not a state change: it carries the CHILD
        // process's identity and endpoint, which no snapshot of this process's
        // state could report. A runner waits on it instead of watching for a
        // pipe to appear.
        names.append(QStringLiteral("update.updaterLaunched"));
    }
    names.sort();
    return names;
}

QJsonObject LiveVerifyDispatcher::Identity() const {
    return source_ != nullptr ? source_->Identity() : QJsonObject{};
}

const exosnap::control::CommandTable<AutomationState>& LiveVerifyDispatcher::Commands() const {
    return AllCommands();
}

QStringList LiveVerifyDispatcher::EventNamesFor(int protocol) const {
    return EventNames(protocol);
}

bool LiveVerifyDispatcher::HasState() const {
    return source_ != nullptr;
}

AutomationState LiveVerifyDispatcher::StateValue() const {
    return source_ != nullptr ? source_->State() : AutomationState{};
}

std::uint64_t LiveVerifyDispatcher::Revision() const {
    return source_ != nullptr ? source_->StateRevision() : 0;
}

QJsonObject LiveVerifyDispatcher::StateJson(const AutomationState& state, std::uint64_t revision) const {
    return StateToJson(state, revision);
}

exosnap::control::Outcome LiveVerifyDispatcher::Execute(const CommandDescriptor& command,
                                                        const ParsedRequest& request) {
    if (command.mutating)
        return ExecuteMutating(command, request, *source_);
    return ExecuteReadOnly(command.name, request.params, *source_, CapabilitiesPayload(request.protocol),
                           DescribePayload(request.protocol));
}

} // namespace exosnap::live_verify
