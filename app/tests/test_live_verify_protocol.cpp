// Pure protocol + allowlist coverage for the Live Verify control channel.
//
// Everything here runs without a pipe, a window or a GPU, which is the point:
// the rejection surface (malformed JSON, wrong protocol version, missing
// handshake, wrong run id, unknown command, bad parameters, a refusing intent)
// is the security boundary, and a boundary that can only be exercised against a
// live application is a boundary nobody exercises.

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

// Records what was asked of it and answers with markers, so a dispatcher that
// routed a command to the wrong member is a failing assertion rather than a
// plausible-looking snapshot.
class FakeSource final : public LiveVerifySource {
  public:
    QStringList calls;
    bool allow_intents = true;

    [[nodiscard]] QJsonObject Identity() const override {
        return QJsonObject{{QStringLiteral("productVersion"), QStringLiteral("0.9.0-test")}};
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

ParsedRequest Request(const QString& command, QJsonObject params = {}) {
    ParsedRequest request;
    request.id = QStringLiteral("1");
    request.command = command;
    request.params = std::move(params);
    return request;
}

QJsonObject Hello(LiveVerifyDispatcher& dispatcher, const QString& run_id = QString::fromLatin1(kRunId)) {
    return dispatcher.Dispatch(Request(QStringLiteral("system.hello"), QJsonObject{{QStringLiteral("runId"), run_id}}));
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
    EXPECT_EQ(request.id, QStringLiteral("42"));
    EXPECT_EQ(request.command, QStringLiteral("record.pause"));
    EXPECT_EQ(request.params.value(QStringLiteral("a")).toInt(), 1);
}

TEST(LiveVerifyProtocol, MalformedJsonIsRejectedWithoutAnId) {
    ParsedRequest request;
    ParseFailure failure;
    EXPECT_FALSE(ParseRequest(Line(QStringLiteral("{not json")), &request, &failure));
    EXPECT_EQ(failure.code, QString::fromLatin1(error_code::kMalformedRequest));
}

TEST(LiveVerifyProtocol, WrongProtocolVersionIsItsOwnErrorAndKeepsTheId) {
    ParsedRequest request;
    ParseFailure failure;
    EXPECT_FALSE(
        ParseRequest(Line(QStringLiteral(R"({"protocol":99,"id":"7","command":"system.hello"})")), &request, &failure));
    EXPECT_EQ(failure.code, QString::fromLatin1(error_code::kProtocolVersionMismatch));
    EXPECT_EQ(failure.id, QStringLiteral("7"));
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
    const QByteArray line = SerializeLine(MakeEvent(QStringLiteral("app.ready"), QJsonObject{}));
    EXPECT_TRUE(line.endsWith('\n'));
    EXPECT_EQ(line.count('\n'), 1);
    EXPECT_TRUE(QJsonDocument::fromJson(line).isObject());
}

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------

TEST(LiveVerifyDispatcher, RefusesEveryCommandBeforeTheHandshake) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    for (const QString& command : LiveVerifyDispatcher::CommandNames()) {
        if (command == QStringLiteral("system.hello"))
            continue;
        const QJsonObject response = dispatcher.Dispatch(Request(command));
        EXPECT_FALSE(response.value(QStringLiteral("ok")).toBool()) << command.toStdString();
        EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kHandshakeRequired)) << command.toStdString();
    }
    EXPECT_TRUE(source.calls.isEmpty());
}

TEST(LiveVerifyDispatcher, HelloWithTheWrongRunIdPoisonsTheConnection) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    const QJsonObject response = Hello(dispatcher, QStringLiteral("some-other-run"));
    EXPECT_FALSE(response.value(QStringLiteral("ok")).toBool());
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
    ASSERT_TRUE(response.value(QStringLiteral("ok")).toBool());
    const QJsonObject result = response.value(QStringLiteral("result")).toObject();
    EXPECT_EQ(result.value(QStringLiteral("protocol")).toInt(), kProtocolVersion);
    EXPECT_EQ(result.value(QStringLiteral("runId")).toString(), QString::fromLatin1(kRunId));
    EXPECT_EQ(result.value(QStringLiteral("productVersion")).toString(), QStringLiteral("0.9.0-test"));
    EXPECT_EQ(result.value(QStringLiteral("commands")).toArray().size(), LiveVerifyDispatcher::CommandNames().size());
    EXPECT_EQ(result.value(QStringLiteral("events")).toArray().size(), LiveVerifyDispatcher::EventNames().size());
    EXPECT_TRUE(dispatcher.handshakeComplete());
}

TEST(LiveVerifyDispatcher, HelloTwiceOnOneConnectionIsRefused) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Hello(dispatcher).value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(ErrorCode(Hello(dispatcher)), QString::fromLatin1(error_code::kAlreadyHandshaken));
}

TEST(LiveVerifyDispatcher, ResetSessionRequiresANewHandshake) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Hello(dispatcher).value(QStringLiteral("ok")).toBool());
    dispatcher.ResetSession();
    EXPECT_EQ(ErrorCode(dispatcher.Dispatch(Request(QStringLiteral("record.snapshot")))),
              QString::fromLatin1(error_code::kHandshakeRequired));
}

// ---------------------------------------------------------------------------
// Allowlist
// ---------------------------------------------------------------------------

TEST(LiveVerifyDispatcher, EveryListedCommandIsActuallyImplemented) {
    // Guards the exact drift the "listed but not implemented" branch exists for.
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Hello(dispatcher).value(QStringLiteral("ok")).toBool());

    for (const QString& command : LiveVerifyDispatcher::CommandNames()) {
        if (command == QStringLiteral("system.hello"))
            continue;
        QJsonObject params;
        if (command == QStringLiteral("window.moveToScreen"))
            params.insert(QStringLiteral("screen"), QStringLiteral("\\\\.\\DISPLAY1"));
        if (command == QStringLiteral("record.selectTarget"))
            params.insert(QStringLiteral("kind"), QStringLiteral("monitor"));
        const QJsonObject response = dispatcher.Dispatch(Request(command, params));
        EXPECT_TRUE(response.value(QStringLiteral("ok")).toBool()) << command.toStdString();
        EXPECT_NE(ErrorCode(response), QString::fromLatin1(error_code::kUnknownCommand)) << command.toStdString();
    }
}

TEST(LiveVerifyDispatcher, UnknownCommandsFailClosed) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Hello(dispatcher).value(QStringLiteral("ok")).toBool());

    // Nothing generic is reachable: no object lookup, no method invocation, no
    // property write, no shell, no file access -- and no prefix match onto a
    // command that does exist.
    for (const QString& command :
         {QStringLiteral("qobject.invoke"), QStringLiteral("qml.setProperty"), QStringLiteral("shell.exec"),
          QStringLiteral("file.read"), QStringLiteral("eval"), QStringLiteral("record"),
          QStringLiteral("record.startNow"), QStringLiteral("RECORD.START"), QStringLiteral("record.start ")}) {
        const QJsonObject response = dispatcher.Dispatch(Request(command));
        EXPECT_FALSE(response.value(QStringLiteral("ok")).toBool()) << command.toStdString();
        EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kUnknownCommand)) << command.toStdString();
    }
    EXPECT_TRUE(source.calls.isEmpty());
}

TEST(LiveVerifyDispatcher, SnapshotCommandsRouteToTheMatchingSource) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Hello(dispatcher).value(QStringLiteral("ok")).toBool());

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
        ASSERT_TRUE(response.value(QStringLiteral("ok")).toBool()) << command.toStdString();
        EXPECT_EQ(response.value(QStringLiteral("result")).toObject().value(QStringLiteral("marker")).toString(),
                  marker);
    }
}

TEST(LiveVerifyDispatcher, RecordIntentsRouteToTheMatchingApplicationIntent) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Hello(dispatcher).value(QStringLiteral("ok")).toBool());

    for (const QString& command :
         {QStringLiteral("record.start"), QStringLiteral("record.pause"), QStringLiteral("record.resume"),
          QStringLiteral("record.stop"), QStringLiteral("record.split"), QStringLiteral("record.captureFrame")}) {
        ASSERT_TRUE(dispatcher.Dispatch(Request(command)).value(QStringLiteral("ok")).toBool());
    }
    EXPECT_EQ(source.calls,
              QStringList({QStringLiteral("start"), QStringLiteral("pause"), QStringLiteral("resume"),
                           QStringLiteral("stop"), QStringLiteral("split"), QStringLiteral("captureFrame")}));
}

TEST(LiveVerifyDispatcher, ARefusedIntentIsAReportableFailureNotACrash) {
    FakeSource source;
    source.allow_intents = false;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Hello(dispatcher).value(QStringLiteral("ok")).toBool());

    const QJsonObject response = dispatcher.Dispatch(Request(QStringLiteral("record.start")));
    EXPECT_FALSE(response.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(ErrorCode(response), QString::fromLatin1(error_code::kCommandFailed));
    EXPECT_EQ(ErrorOf(response).value(QStringLiteral("message")).toString(),
              QStringLiteral("refused by the application"));
}

TEST(LiveVerifyDispatcher, ParameterValidationRunsBeforeTheIntent) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Hello(dispatcher).value(QStringLiteral("ok")).toBool());

    EXPECT_EQ(ErrorCode(dispatcher.Dispatch(Request(QStringLiteral("window.moveToScreen")))),
              QString::fromLatin1(error_code::kInvalidParams));
    EXPECT_EQ(
        ErrorCode(dispatcher.Dispatch(Request(QStringLiteral("record.selectTarget"),
                                              QJsonObject{{QStringLiteral("kind"), QStringLiteral("everything")}}))),
        QString::fromLatin1(error_code::kInvalidParams));
    EXPECT_TRUE(source.calls.isEmpty());
}

TEST(LiveVerifyDispatcher, CapabilitiesMatchTheCommandsThatWillBeAccepted) {
    FakeSource source;
    LiveVerifyDispatcher dispatcher(&source, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Hello(dispatcher).value(QStringLiteral("ok")).toBool());

    const QJsonArray advertised = dispatcher.Dispatch(Request(QStringLiteral("system.capabilities")))
                                      .value(QStringLiteral("result"))
                                      .toObject()
                                      .value(QStringLiteral("commands"))
                                      .toArray();
    QStringList names;
    for (const QJsonValue& value : advertised)
        names.append(value.toString());
    EXPECT_EQ(names, LiveVerifyDispatcher::CommandNames());
}

TEST(LiveVerifyDispatcher, WithoutASourceEveryCommandReportsUnavailable) {
    LiveVerifyDispatcher dispatcher(nullptr, QString::fromLatin1(kRunId));
    ASSERT_TRUE(Hello(dispatcher).value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(ErrorCode(dispatcher.Dispatch(Request(QStringLiteral("record.snapshot")))),
              QString::fromLatin1(error_code::kUnavailable));
}
