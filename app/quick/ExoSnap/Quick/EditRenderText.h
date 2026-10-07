#pragma once

#include <QByteArray>
#include <QCoreApplication>
#include <QString>

namespace exosnap::quick {

inline QString TranslateEditRenderReason(const QString& reason) {
    const auto detail = reason.indexOf(QStringLiteral(" ("));
    const auto policy = detail >= 0 ? reason.first(detail) : reason;
    return QCoreApplication::translate("EditRender", policy.toUtf8().constData()) +
           (detail >= 0 ? reason.sliced(detail) : QString());
}

} // namespace exosnap::quick
