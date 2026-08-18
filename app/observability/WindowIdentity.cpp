#include "observability/WindowIdentity.h"

#include "observability/ObservabilityJson.h"

#include <QJsonArray>
#include <QSet>

namespace exosnap::observability {

QString WindowRoleForObjectName(const QString& object_name, bool is_root) {
    if (is_root)
        return QString::fromLatin1(window_role::kMain);
    // The overlay objectNames set in Main.qml. Matched exactly, not by prefix:
    // a prefix match would quietly file a NEW overlay under an existing role.
    if (object_name == QLatin1String("quickOverlayRecording"))
        return QString::fromLatin1(window_role::kRecordingOverlay);
    if (object_name == QLatin1String("quickOverlayDiagnostics"))
        return QString::fromLatin1(window_role::kDiagnosticsOverlay);
    if (object_name == QLatin1String("quickOverlayQuickControls"))
        return QString::fromLatin1(window_role::kQuickControls);
    if (object_name == QLatin1String("quickOverlayNotificationToast"))
        return QString::fromLatin1(window_role::kNotificationToast);
    if (object_name == QLatin1String("quickOverlayCountdown"))
        return QString::fromLatin1(window_role::kCountdown);
    return QString::fromLatin1(window_role::kUnknown);
}

QJsonObject WindowSnapshotToJson(const std::vector<WindowFacts>& windows, qint64 process_id) {
    QJsonArray array;
    QSet<QString> seen_roles;
    QSet<QString> titles;
    bool duplicate_role = false;
    bool duplicate_title = false;

    for (const WindowFacts& window : windows) {
        QJsonObject json;
        json.insert(QStringLiteral("role"), window.role);
        json.insert(QStringLiteral("objectName"), TextOrNull(window.object_name));
        json.insert(QStringLiteral("title"), TextOrNull(window.title));
        json.insert(QStringLiteral("visible"), window.visible);
        json.insert(QStringLiteral("exposed"), window.exposed);
        json.insert(QStringLiteral("nativeWindowCreated"), window.native_window_created);
        json.insert(QStringLiteral("screen"), TextOrNull(window.screen));
        json.insert(QStringLiteral("processId"), static_cast<double>(process_id));
        if (window.native_window_created)
            json.insert(QStringLiteral("native"), window.native);
        array.append(json);

        if (window.role != QLatin1String(window_role::kUnknown) && seen_roles.contains(window.role))
            duplicate_role = true;
        seen_roles.insert(window.role);

        if (!window.title.isEmpty()) {
            if (titles.contains(window.title))
                duplicate_title = true;
            titles.insert(window.title);
        }
    }

    QJsonObject json;
    json.insert(QStringLiteral("windows"), array);
    json.insert(QStringLiteral("count"), array.size());
    json.insert(QStringLiteral("processId"), static_cast<double>(process_id));
    // Stated as data, not left to a test to re-derive: roles must be unique
    // (they are the automation identity), and two top-level windows sharing an
    // OS title is the Wave B defect this snapshot exists to make visible.
    json.insert(QStringLiteral("rolesUnique"), !duplicate_role);
    json.insert(QStringLiteral("titlesUnique"), !duplicate_title);
    // The contract, published so no consumer has to be told it out of band.
    json.insert(QStringLiteral("identity"), QStringLiteral("role+processId"));
    return json;
}

} // namespace exosnap::observability
