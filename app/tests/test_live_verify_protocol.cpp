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
    state.can_capture_frame = true;
    state.can_select_source = true;
    state.can_open_edit = true;
    state.can_add_marker = true;
    state.countdown_active = true;
    state.edit_session_open = true;
    state.edit_visible = true;
    state.can_export = true;
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
        return Marker(QStringLiteral("editor"));
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
    bool LogsOpen(QString* error) override {
        calls.append(QStringLiteral("logs.open"));
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

    bool MoveWindowToScreen(const QString& screen_name, QString* error) override {
        calls.append(QStringLiteral("moveToScreen:%1").arg(screen_name));
        return Outcome(error);
    }
    bool SelectRecordTarget(const QString& kind, const QString& title_filter, QString* error) override {
        calls.append(QStringLiteral("selectTarget:%1/%2").arg(kind, title_filter));
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
        state.page = page;
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
            params.insert(parameter.name, 0);
        else if (parameter.type == QLatin1String("bool"))
            params.insert(parameter.name, true);
        else
            params.insert(parameter.name, QStringLiteral("x"));
    }
    if (command.name == QLatin1String("system.hello"))
        params.insert(QStringLiteral("runId"), QString::fromLatin1(kRunId));
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
        // Settings and profiles are locked while a recording is in flight, the
        // way the Settings controls are.
        if (command.name.startsWith(QStringLiteral("settings.")) ||
            command.name.startsWith(QStringLiteral("profiles."))) {
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
        const QJsonObject response = dispatcher.Dispatch(RequestV2(command.name, PlausibleParams(command)));
        EXPECT_TRUE(Ok(response)) << command.name.toStdString() << ": " << ErrorCode(response).toStdString();
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
                                  QStringLiteral("system.snapshot"),     QStringLiteral("window.moveToScreen"),
                                  QStringLiteral("window.snapshot")};
    EXPECT_EQ(expected.size(), 19);
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
    EXPECT_EQ(ErrorCode(dispatcher.Dispatch(RequestV2(QStringLiteral("ui.navigate"),
                                                      QJsonObject{{QStringLiteral("page"), QStringLiteral("edit")}}))),
              QString::fromLatin1(error_code::kInvalidParams));
    EXPECT_EQ(
        ErrorCode(dispatcher.Dispatch(RequestV2(
            QStringLiteral("edit.seek"), QJsonObject{{QStringLiteral("positionMs"), QStringLiteral("halfway")}}))),
        QString::fromLatin1(error_code::kInvalidParams));
    EXPECT_TRUE(source.calls.isEmpty());
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

    const QJsonObject response = dispatcher.Dispatch(
        RequestV2(QStringLiteral("ui.navigate"), QJsonObject{{QStringLiteral("page"), QStringLiteral("settings")}}));
    ASSERT_TRUE(Ok(response));
    EXPECT_TRUE(response.value(QStringLiteral("settled")).toBool());
    EXPECT_EQ(response.value(QStringLiteral("result")).toObject().value(QStringLiteral("page")).toString(),
              QStringLiteral("settings"));
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

    for (const CommandDescriptor& command : AllCommands()) {
        if (!command.name.startsWith(QStringLiteral("edit.")) || command.name == QStringLiteral("edit.open"))
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
    // same name are two profiles), and notification.invokeAction (an action
    // navigates, opens a folder or relaunches).
    EXPECT_EQ(non_idempotent, 10);
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
