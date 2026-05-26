#pragma once

#include <QString>
#include <QStringView>

namespace exosnap::ui::chrome {

inline bool ShouldShowRecordingRuntimeForStatus(QStringView status_label) {
    const QString normalized = status_label.trimmed().toString().toUpper();
    return normalized == QStringLiteral("REC") || normalized == QStringLiteral("PAUSED") ||
           normalized == QStringLiteral("STOPPING");
}

} // namespace exosnap::ui::chrome
