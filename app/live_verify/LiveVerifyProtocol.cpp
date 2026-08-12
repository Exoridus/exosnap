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

bool ParseRequest(const QByteArray& line, ParsedRequest* out, ParseFailure* failure) {
    if (line.size() > kMaxRequestBytes) {
        *failure = {{},
                    QString::fromLatin1(error_code::kRequestTooLarge),
                    QStringLiteral("Request exceeds %1 bytes").arg(kMaxRequestBytes)};
        return false;
    }

    QJsonParseError parse_error{};
    const QJsonDocument document = QJsonDocument::fromJson(line, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        *failure = {{},
                    QString::fromLatin1(error_code::kMalformedRequest),
                    parse_error.error != QJsonParseError::NoError ? parse_error.errorString()
                                                                  : QStringLiteral("Request is not a JSON object")};
        return false;
    }

    const QJsonObject object = document.object();
    const QString id = RecoverId(object);

    const QJsonValue protocol = object.value(QStringLiteral("protocol"));
    if (!protocol.isDouble()) {
        *failure = {id, QString::fromLatin1(error_code::kMalformedRequest),
                    QStringLiteral("Request has no numeric \"protocol\" field")};
        return false;
    }
    if (static_cast<int>(protocol.toDouble()) != kProtocolVersion) {
        *failure = {id, QString::fromLatin1(error_code::kProtocolVersionMismatch),
                    QStringLiteral("Server speaks protocol %1, client sent %2")
                        .arg(kProtocolVersion)
                        .arg(static_cast<int>(protocol.toDouble()))};
        return false;
    }

    const QJsonValue command = object.value(QStringLiteral("command"));
    if (!command.isString() || command.toString().isEmpty()) {
        *failure = {id, QString::fromLatin1(error_code::kMalformedRequest),
                    QStringLiteral("Request has no non-empty string \"command\" field")};
        return false;
    }

    const QJsonValue params = object.value(QStringLiteral("params"));
    if (!params.isUndefined() && !params.isNull() && !params.isObject()) {
        *failure = {id, QString::fromLatin1(error_code::kInvalidParams),
                    QStringLiteral("\"params\" must be an object when present")};
        return false;
    }

    out->id = id;
    out->command = command.toString();
    out->params = params.isObject() ? params.toObject() : QJsonObject{};
    return true;
}

QJsonObject MakeSuccessResponse(const QString& id, QJsonObject result) {
    QJsonObject response;
    response.insert(QStringLiteral("protocol"), kProtocolVersion);
    response.insert(QStringLiteral("id"), id);
    response.insert(QStringLiteral("ok"), true);
    response.insert(QStringLiteral("result"), result);
    return response;
}

QJsonObject MakeErrorResponse(const QString& id, const QString& code, const QString& message) {
    QJsonObject error;
    error.insert(QStringLiteral("code"), code);
    error.insert(QStringLiteral("message"), message);

    QJsonObject response;
    response.insert(QStringLiteral("protocol"), kProtocolVersion);
    response.insert(QStringLiteral("id"), id);
    response.insert(QStringLiteral("ok"), false);
    response.insert(QStringLiteral("error"), error);
    return response;
}

QJsonObject MakeEvent(const QString& name, QJsonObject data) {
    QJsonObject event;
    event.insert(QStringLiteral("protocol"), kProtocolVersion);
    event.insert(QStringLiteral("event"), name);
    event.insert(QStringLiteral("data"), data);
    return event;
}

QByteArray SerializeLine(const QJsonObject& object) {
    QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact);
    line.append('\n');
    return line;
}

} // namespace exosnap::live_verify
