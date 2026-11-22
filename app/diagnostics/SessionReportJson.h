#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace exosnap::diagnostics {

[[nodiscard]] inline QJsonDocument ReadSessionReportJson(const QByteArray& bytes) {
    const auto document = QJsonDocument::fromJson(bytes);
    if (!document.isObject())
        return {};

    auto report = document.object();
    const auto failure = report.value(QStringLiteral("error"));
    if (failure.isUndefined())
        return document;
    if (!failure.isObject())
        return {};

    // Older reports stored unstructured error text that can include absolute paths.
    auto error = failure.toObject();
    error.remove(QStringLiteral("detail"));
    report.insert(QStringLiteral("error"), error);
    return QJsonDocument(report);
}

} // namespace exosnap::diagnostics
