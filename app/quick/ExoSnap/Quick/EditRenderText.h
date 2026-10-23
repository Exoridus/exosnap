#pragma once

#include "models/EditCommandTypes.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QString>

namespace exosnap::quick {

inline QString TranslateEditError(const edit::EditError& error) {
    using Code = edit::EditErrorCode;
    switch (error.code) {
    case Code::TrackLocked:
        return QCoreApplication::translate("Edit", "A track affected by this edit is locked.");
    case Code::LinkedScopeRequired:
        return QCoreApplication::translate("Edit", "Include the linked tracks before editing this range.");
    case Code::StaleRevision:
        return QCoreApplication::translate("Edit", "The timeline changed. Start the edit again.");
    case Code::SourceBounds:
        return QCoreApplication::translate("Edit", "The source does not contain enough media for this edit.");
    case Code::TransitionBoundaryConflict:
        return QCoreApplication::translate("Edit", "The edit would cross a fade or transition boundary.");
    case Code::SourceChanged:
        return QCoreApplication::translate("Edit", "The source changed. Relink it before editing.");
    case Code::MissingSource:
        return QCoreApplication::translate("Edit", "The source is unavailable.");
    case Code::InvalidTime:
        return QCoreApplication::translate("Edit", "The time is outside the valid editing range.");
    default:
        return QCoreApplication::translate("Edit", "This edit is not valid at the selected position.");
    }
}

inline QString TranslateEditRenderReason(const QString& reason) {
    const auto detail = reason.indexOf(QStringLiteral(" ("));
    const auto policy = detail >= 0 ? reason.first(detail) : reason;
    // Literal keys make non-Qt engine diagnostics discoverable by the translation extractor.
    constexpr const char* engine_reasons[] = {
        QT_TRANSLATE_NOOP("EditRender", "Audio preparation failed."),
        QT_TRANSLATE_NOOP("EditRender", "Invalid audio interval boundary."),
        QT_TRANSLATE_NOOP("EditRender", "Invalid audio monitor gain."),
        QT_TRANSLATE_NOOP("EditRender", "Invalid audio playback region."),
        QT_TRANSLATE_NOOP("EditRender", "Invalid audio preparation request."),
        QT_TRANSLATE_NOOP("EditRender", "The selected container stream is not video."),
    };
    const auto utf8 = policy.toUtf8();
    const char* key = utf8.constData();
    for (const auto* candidate : engine_reasons)
        if (policy == QLatin1String(candidate)) {
            key = candidate;
            break;
        }
    return QCoreApplication::translate("EditRender", key) + (detail >= 0 ? reason.sliced(detail) : QString());
}

} // namespace exosnap::quick
