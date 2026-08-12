#include "LiveVerifyDispatcher.h"

#include "LiveVerifySource.h"

#include <QJsonArray>
#include <QJsonValue>

namespace exosnap::live_verify {
namespace {

const QString kHello = QStringLiteral("system.hello");

// The single authority for what exists. CommandNames(), system.capabilities and
// Dispatch() all read it, so the three can never disagree.
const QStringList& AllCommands() {
    static const QStringList commands = {
        QStringLiteral("system.hello"),         QStringLiteral("system.capabilities"),
        QStringLiteral("system.snapshot"),      QStringLiteral("app.snapshot"),
        QStringLiteral("window.snapshot"),      QStringLiteral("window.moveToScreen"),
        QStringLiteral("preview.snapshot"),     QStringLiteral("record.snapshot"),
        QStringLiteral("record.selectTarget"),  QStringLiteral("record.start"),
        QStringLiteral("record.pause"),         QStringLiteral("record.resume"),
        QStringLiteral("record.stop"),          QStringLiteral("record.split"),
        QStringLiteral("record.captureFrame"),  QStringLiteral("record.result"),
        QStringLiteral("overlay.snapshot"),     QStringLiteral("editor.snapshot"),
        QStringLiteral("diagnostics.snapshot"),
    };
    return commands;
}

QJsonArray ToArray(const QStringList& values) {
    QJsonArray array;
    for (const QString& value : values)
        array.append(value);
    return array;
}

} // namespace

LiveVerifyDispatcher::LiveVerifyDispatcher(LiveVerifySource* source, QString run_id)
    : source_(source), run_id_(std::move(run_id)) {
}

QStringList LiveVerifyDispatcher::CommandNames() {
    QStringList sorted = AllCommands();
    sorted.sort();
    return sorted;
}

QStringList LiveVerifyDispatcher::EventNames() {
    return {QStringLiteral("app.ready"), QStringLiteral("record.resultReady"), QStringLiteral("record.stateChanged"),
            QStringLiteral("window.screenChanged")};
}

void LiveVerifyDispatcher::ResetSession() {
    handshake_complete_ = false;
    poisoned_ = false;
}

QJsonObject LiveVerifyDispatcher::HandleHello(const ParsedRequest& request) {
    if (handshake_complete_) {
        return MakeErrorResponse(request.id, QString::fromLatin1(error_code::kAlreadyHandshaken),
                                 QStringLiteral("This connection has already completed its handshake"));
    }

    const QJsonValue run_id = request.params.value(QStringLiteral("runId"));
    if (!run_id.isString()) {
        return MakeErrorResponse(request.id, QString::fromLatin1(error_code::kInvalidParams),
                                 QStringLiteral("system.hello requires a string \"runId\" parameter"));
    }
    if (run_id.toString() != run_id_) {
        // Fatal on purpose. The run id is the connection credential; a client
        // that guessed wrong is not given a second guess on a live application.
        poisoned_ = true;
        return MakeErrorResponse(request.id, QString::fromLatin1(error_code::kRunIdMismatch),
                                 QStringLiteral("Run id does not match this process"));
    }

    handshake_complete_ = true;

    QJsonObject result = source_ != nullptr ? source_->Identity() : QJsonObject{};
    result.insert(QStringLiteral("protocol"), kProtocolVersion);
    result.insert(QStringLiteral("runId"), run_id_);
    result.insert(QStringLiteral("commands"), ToArray(CommandNames()));
    result.insert(QStringLiteral("events"), ToArray(EventNames()));
    return MakeSuccessResponse(request.id, result);
}

QJsonObject LiveVerifyDispatcher::Dispatch(const ParsedRequest& request) {
    if (request.command == kHello)
        return HandleHello(request);

    if (!handshake_complete_) {
        return MakeErrorResponse(request.id, QString::fromLatin1(error_code::kHandshakeRequired),
                                 QStringLiteral("system.hello must be the first command on a connection"));
    }

    if (!AllCommands().contains(request.command)) {
        // Fail closed. No prefix matching, no "did you mean", no reflection.
        return MakeErrorResponse(request.id, QString::fromLatin1(error_code::kUnknownCommand),
                                 QStringLiteral("Unknown command: %1").arg(request.command));
    }

    if (source_ == nullptr) {
        return MakeErrorResponse(request.id, QString::fromLatin1(error_code::kUnavailable),
                                 QStringLiteral("No application state is bound to this server"));
    }

    if (request.command == QStringLiteral("system.capabilities")) {
        QJsonObject result;
        result.insert(QStringLiteral("protocol"), kProtocolVersion);
        result.insert(QStringLiteral("commands"), ToArray(CommandNames()));
        result.insert(QStringLiteral("events"), ToArray(EventNames()));
        return MakeSuccessResponse(request.id, result);
    }

    if (request.command == QStringLiteral("system.snapshot"))
        return MakeSuccessResponse(request.id, source_->SystemSnapshot());
    if (request.command == QStringLiteral("app.snapshot"))
        return MakeSuccessResponse(request.id, source_->AppSnapshot());
    if (request.command == QStringLiteral("window.snapshot"))
        return MakeSuccessResponse(request.id, source_->WindowSnapshot());
    if (request.command == QStringLiteral("preview.snapshot"))
        return MakeSuccessResponse(request.id, source_->PreviewSnapshot());
    if (request.command == QStringLiteral("record.snapshot"))
        return MakeSuccessResponse(request.id, source_->RecordSnapshot());
    if (request.command == QStringLiteral("record.result"))
        return MakeSuccessResponse(request.id, source_->RecordResult());
    if (request.command == QStringLiteral("overlay.snapshot"))
        return MakeSuccessResponse(request.id, source_->OverlaySnapshot());
    if (request.command == QStringLiteral("editor.snapshot"))
        return MakeSuccessResponse(request.id, source_->EditorSnapshot());
    if (request.command == QStringLiteral("diagnostics.snapshot"))
        return MakeSuccessResponse(request.id, source_->DiagnosticsSnapshot());

    // The intents. Each answers with the snapshot its own domain owns, so a
    // client never has to guess whether a command "took" -- though the
    // authoritative confirmation is still the event or a later snapshot, because
    // start/stop are asynchronous by nature.
    QString error;

    if (request.command == QStringLiteral("window.moveToScreen")) {
        const QJsonValue screen = request.params.value(QStringLiteral("screen"));
        if (!screen.isString() || screen.toString().isEmpty()) {
            return MakeErrorResponse(request.id, QString::fromLatin1(error_code::kInvalidParams),
                                     QStringLiteral("window.moveToScreen requires a non-empty string \"screen\""));
        }
        if (!source_->MoveWindowToScreen(screen.toString(), &error))
            return MakeErrorResponse(request.id, QString::fromLatin1(error_code::kCommandFailed), error);
        return MakeSuccessResponse(request.id, source_->WindowSnapshot());
    }

    if (request.command == QStringLiteral("record.selectTarget")) {
        const QJsonValue kind = request.params.value(QStringLiteral("kind"));
        if (!kind.isString() ||
            (kind.toString() != QStringLiteral("monitor") && kind.toString() != QStringLiteral("window"))) {
            return MakeErrorResponse(request.id, QString::fromLatin1(error_code::kInvalidParams),
                                     QStringLiteral("record.selectTarget requires \"kind\" of \"monitor\" or "
                                                    "\"window\""));
        }
        const QJsonValue filter = request.params.value(QStringLiteral("titleFilter"));
        if (!filter.isUndefined() && !filter.isNull() && !filter.isString()) {
            return MakeErrorResponse(request.id, QString::fromLatin1(error_code::kInvalidParams),
                                     QStringLiteral("\"titleFilter\" must be a string when present"));
        }
        if (!source_->SelectRecordTarget(kind.toString(), filter.toString(), &error))
            return MakeErrorResponse(request.id, QString::fromLatin1(error_code::kCommandFailed), error);
        return MakeSuccessResponse(request.id, source_->RecordSnapshot());
    }

    const auto record_intent = [&](bool (LiveVerifySource::*intent)(QString*)) {
        if (!(source_->*intent)(&error))
            return MakeErrorResponse(request.id, QString::fromLatin1(error_code::kCommandFailed), error);
        return MakeSuccessResponse(request.id, source_->RecordSnapshot());
    };

    if (request.command == QStringLiteral("record.start"))
        return record_intent(&LiveVerifySource::RecordStart);
    if (request.command == QStringLiteral("record.pause"))
        return record_intent(&LiveVerifySource::RecordPause);
    if (request.command == QStringLiteral("record.resume"))
        return record_intent(&LiveVerifySource::RecordResume);
    if (request.command == QStringLiteral("record.stop"))
        return record_intent(&LiveVerifySource::RecordStop);
    if (request.command == QStringLiteral("record.split"))
        return record_intent(&LiveVerifySource::RecordSplit);
    if (request.command == QStringLiteral("record.captureFrame"))
        return record_intent(&LiveVerifySource::RecordCaptureFrame);

    // Unreachable while AllCommands() and the branches above agree; kept so a
    // name added to the list without a handler fails loudly instead of silently
    // answering nothing.
    return MakeErrorResponse(request.id, QString::fromLatin1(error_code::kUnknownCommand),
                             QStringLiteral("Command %1 is listed but not implemented").arg(request.command));
}

} // namespace exosnap::live_verify
