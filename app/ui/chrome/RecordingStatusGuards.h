#pragma once

#include <QString>
#include <QStringView>

namespace exosnap::ui::chrome {

inline bool ShouldShowRecordingRuntimeForStatus(QStringView status_label) {
    const QString normalized = status_label.trimmed().toString().toUpper();
    return normalized == QStringLiteral("REC") || normalized == QStringLiteral("PAUSED") ||
           normalized == QStringLiteral("STOPPING");
}

inline bool ShouldOpenRecordingDiagnosticsForStatus(QStringView status_label) {
    const QString normalized = status_label.trimmed().toString().toUpper();
    return normalized == QStringLiteral("BLOCKED") || normalized == QStringLiteral("ERROR");
}

// Scopes the transient post-recording "Saved" pill to the Record page. The
// title-bar pill should only read "Saved" while the Record result/completed
// dock is visible; on every other page (Settings/Logs/Diagnostics/…) a SAVED
// label is normalized to the steady READY status so it does not linger as stale
// global state. Every other status label is global and returned unchanged.
inline QString ScopeStatusLabelForActivePage(QStringView status_label, bool on_record_page) {
    const QString normalized = status_label.trimmed().toString().toUpper();
    if (!on_record_page && normalized == QStringLiteral("SAVED"))
        return QStringLiteral("READY");
    return status_label.toString();
}

} // namespace exosnap::ui::chrome
