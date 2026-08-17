#include <control/session.h>

namespace exosnap::control {

Outcome Succeeded(QJsonObject result, bool settled) {
    Outcome outcome;
    outcome.result = std::move(result);
    outcome.settled = settled;
    return outcome;
}

Outcome Failed(QString code, QString message, QJsonObject requirements, QJsonObject actual) {
    Outcome outcome;
    outcome.ok = false;
    // A refusal never claims a postcondition. The envelope drops `settled` on
    // the error path anyway; this keeps the value itself from being misread by
    // anything that logs the Outcome.
    outcome.settled = false;
    outcome.code = std::move(code);
    outcome.message = std::move(message);
    outcome.requirements = std::move(requirements);
    outcome.actual = std::move(actual);
    return outcome;
}

Outcome Failed(const char* code, QString message, QJsonObject requirements, QJsonObject actual) {
    return Failed(QString::fromLatin1(code), std::move(message), std::move(requirements), std::move(actual));
}

QJsonArray ToJsonArray(const QStringList& values) {
    QJsonArray array;
    for (const QString& value : values)
        array.append(value);
    return array;
}

} // namespace exosnap::control
