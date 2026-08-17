#include <control/command_policy.h>

namespace exosnap::control {

PreconditionVerdict Allowed() {
    return {};
}

PreconditionVerdict Refuse(const char* code, QString message, const QString& key, QJsonValue required,
                           QJsonValue observed) {
    PreconditionVerdict verdict;
    verdict.code = QString::fromLatin1(code);
    verdict.message = std::move(message);
    verdict.requirements.insert(key, std::move(required));
    verdict.actual.insert(key, std::move(observed));
    return verdict;
}

std::optional<QString> ValidateParams(const QString& command_name, const QVector<CommandParameter>& parameters,
                                      const QJsonObject& params) {
    for (const CommandParameter& parameter : parameters) {
        const QJsonValue value = params.value(parameter.name);
        const bool absent = value.isUndefined() || value.isNull();
        if (absent) {
            if (!parameter.required)
                continue;
            return QStringLiteral("%1 requires a \"%2\" parameter").arg(command_name, parameter.name);
        }
        if (parameter.type == QLatin1String("string")) {
            if (!value.isString() || value.toString().isEmpty())
                return QStringLiteral("\"%1\" must be a non-empty string").arg(parameter.name);
        } else if (parameter.type == QLatin1String("int")) {
            if (!value.isDouble())
                return QStringLiteral("\"%1\" must be a number").arg(parameter.name);
        } else if (parameter.type == QLatin1String("bool")) {
            if (!value.isBool())
                return QStringLiteral("\"%1\" must be a boolean").arg(parameter.name);
        } else if (parameter.type == QLatin1String("enum")) {
            if (!value.isString() || !parameter.values.contains(value.toString())) {
                return QStringLiteral("\"%1\" must be one of: %2")
                    .arg(parameter.name, parameter.values.join(QStringLiteral(", ")));
            }
        }
    }
    return std::nullopt;
}

QString SettleName(Settle settle) {
    switch (settle) {
    case Settle::Synchronous:
        return QStringLiteral("synchronous");
    case Settle::Asynchronous:
        return QStringLiteral("asynchronous");
    case Settle::NotApplicable:
        break;
    }
    return QStringLiteral("none");
}

QJsonArray DescribeParameters(const QVector<CommandParameter>& parameters) {
    QJsonArray described;
    for (const CommandParameter& parameter : parameters) {
        QJsonObject json;
        json.insert(QStringLiteral("name"), parameter.name);
        json.insert(QStringLiteral("type"), parameter.type);
        json.insert(QStringLiteral("required"), parameter.required);
        if (!parameter.values.isEmpty()) {
            QJsonArray values;
            for (const QString& value : parameter.values)
                values.append(value);
            json.insert(QStringLiteral("values"), values);
        }
        described.append(json);
    }
    return described;
}

} // namespace exosnap::control
