#include "LiveVerifyProtocol.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>

namespace exosnap::live_verify {
namespace {

QString RecoverId(const QJsonObject& object) {
    const QJsonValue id = object.value(QStringLiteral("id"));
    if (id.isString())
        return id.toString();
    // A numeric id is accepted on the way in and echoed as its string form. The
    // alternative -- rejecting it -- turns a trivial client mistake into an
    // unanswerable request.
    if (id.isDouble())
        return QString::number(static_cast<qint64>(id.toDouble()));
    return {};
}

} // namespace

QStringList AllErrorCodes() {
    QStringList codes = {
        QString::fromLatin1(error_code::kMalformedRequest),
        QString::fromLatin1(error_code::kRequestTooLarge),
        QString::fromLatin1(error_code::kProtocolVersionMismatch),
        QString::fromLatin1(error_code::kRunIdMismatch),
        QString::fromLatin1(error_code::kHandshakeRequired),
        QString::fromLatin1(error_code::kAlreadyHandshaken),
        QString::fromLatin1(error_code::kUnknownCommand),
        QString::fromLatin1(error_code::kInvalidParams),
        QString::fromLatin1(error_code::kCommandFailed),
        QString::fromLatin1(error_code::kInvalidState),
        QString::fromLatin1(error_code::kBlocked),
        QString::fromLatin1(error_code::kOperationFailed),
        QString::fromLatin1(error_code::kUnavailable),
        QString::fromLatin1(error_code::kDispatchTimeout),
    };
    codes.sort();
    return codes;
}

QString ErrorCodeForProtocol(const QString& code, int protocol) {
    if (protocol >= 2)
        return code;
    if (code == QLatin1String(error_code::kInvalidState) || code == QLatin1String(error_code::kBlocked) ||
        code == QLatin1String(error_code::kOperationFailed)) {
        return QString::fromLatin1(error_code::kCommandFailed);
    }
    return code;
}

bool ParseRequest(const QByteArray& line, ParsedRequest* out, ParseFailure* failure) {
    if (line.size() > kMaxRequestBytes) {
        *failure = {{},
                    kLatestProtocolVersion,
                    QString::fromLatin1(error_code::kRequestTooLarge),
                    QStringLiteral("Request exceeds %1 bytes").arg(kMaxRequestBytes)};
        return false;
    }

    QJsonParseError parse_error{};
    const QJsonDocument document = QJsonDocument::fromJson(line, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        *failure = {{},
                    kLatestProtocolVersion,
                    QString::fromLatin1(error_code::kMalformedRequest),
                    parse_error.error != QJsonParseError::NoError ? parse_error.errorString()
                                                                  : QStringLiteral("Request is not a JSON object")};
        return false;
    }

    const QJsonObject object = document.object();
    const QString id = RecoverId(object);

    const QJsonValue protocol = object.value(QStringLiteral("protocol"));
    if (!protocol.isDouble()) {
        *failure = {id, kLatestProtocolVersion, QString::fromLatin1(error_code::kMalformedRequest),
                    QStringLiteral("Request has no numeric \"protocol\" field")};
        return false;
    }
    const int requested = static_cast<int>(protocol.toDouble());
    if (!IsSupportedProtocol(requested)) {
        // Answered in the newest version this build speaks. There is no agreed
        // version by definition, and a client that asked for something older
        // than the floor cannot be told about the floor in its own dialect.
        *failure = {id, kLatestProtocolVersion, QString::fromLatin1(error_code::kProtocolVersionMismatch),
                    QStringLiteral("Server speaks protocol %1..%2, client sent %3")
                        .arg(kMinimumProtocolVersion)
                        .arg(kLatestProtocolVersion)
                        .arg(requested)};
        return false;
    }

    const QJsonValue command = object.value(QStringLiteral("command"));
    if (!command.isString() || command.toString().isEmpty()) {
        *failure = {id, requested, QString::fromLatin1(error_code::kMalformedRequest),
                    QStringLiteral("Request has no non-empty string \"command\" field")};
        return false;
    }

    const QJsonValue params = object.value(QStringLiteral("params"));
    if (!params.isUndefined() && !params.isNull() && !params.isObject()) {
        *failure = {id, requested, QString::fromLatin1(error_code::kInvalidParams),
                    QStringLiteral("\"params\" must be an object when present")};
        return false;
    }

    const QJsonValue include_state = object.value(QStringLiteral("includeState"));
    if (!include_state.isUndefined() && !include_state.isNull() && !include_state.isBool()) {
        *failure = {id, requested, QString::fromLatin1(error_code::kInvalidParams),
                    QStringLiteral("\"includeState\" must be a boolean when present")};
        return false;
    }

    out->protocol = requested;
    out->id = id;
    out->command = command.toString();
    out->params = params.isObject() ? params.toObject() : QJsonObject{};
    // Deliberately ignored under protocol 1 rather than rejected: the field is
    // additive, and a v1 response has nowhere to put the state it would ask for.
    out->include_state = requested >= 2 && include_state.toBool(false);
    return true;
}

QJsonObject MakeSuccessResponse(const SuccessEnvelope& envelope) {
    QJsonObject response;
    response.insert(QStringLiteral("protocol"), envelope.protocol);
    response.insert(QStringLiteral("id"), envelope.id);
    response.insert(QStringLiteral("ok"), true);
    response.insert(QStringLiteral("result"), envelope.result);
    if (envelope.state_revision.has_value())
        response.insert(QStringLiteral("stateRevision"), static_cast<double>(*envelope.state_revision));
    if (envelope.settled.has_value())
        response.insert(QStringLiteral("settled"), *envelope.settled);
    if (envelope.state.has_value())
        response.insert(QStringLiteral("state"), *envelope.state);
    return response;
}

QJsonObject MakeErrorResponse(const ErrorEnvelope& envelope) {
    QJsonObject error;
    error.insert(QStringLiteral("code"), ErrorCodeForProtocol(envelope.code, envelope.protocol));
    error.insert(QStringLiteral("message"), envelope.message);
    // Structured cause is protocol 2 only. A v1 client is not made to skip
    // fields it has never been told about.
    if (envelope.protocol >= 2) {
        if (!envelope.requirements.isEmpty())
            error.insert(QStringLiteral("requires"), envelope.requirements);
        if (!envelope.actual.isEmpty())
            error.insert(QStringLiteral("actual"), envelope.actual);
    }

    QJsonObject response;
    response.insert(QStringLiteral("protocol"), envelope.protocol);
    response.insert(QStringLiteral("id"), envelope.id);
    response.insert(QStringLiteral("ok"), false);
    response.insert(QStringLiteral("error"), error);
    if (envelope.state_revision.has_value())
        response.insert(QStringLiteral("stateRevision"), static_cast<double>(*envelope.state_revision));
    return response;
}

QJsonObject MakeEvent(int protocol, const QString& name, QJsonObject data,
                      std::optional<std::uint64_t> state_revision) {
    QJsonObject event;
    event.insert(QStringLiteral("protocol"), protocol);
    event.insert(QStringLiteral("event"), name);
    event.insert(QStringLiteral("data"), data);
    if (protocol >= 2 && state_revision.has_value())
        event.insert(QStringLiteral("stateRevision"), static_cast<double>(*state_revision));
    return event;
}

QByteArray SerializeLine(const QJsonObject& object) {
    QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact);
    line.append('\n');
    return line;
}

} // namespace exosnap::live_verify
