#pragma once

#include <QStringList>

namespace exosnap::diagnostics {

inline bool IsExportableLogComponent(const QString& component) {
    static const QStringList components{QStringLiteral("app"),
                                        QStringLiteral("audio"),
                                        QStringLiteral("capture"),
                                        QStringLiteral("diagnostics"),
                                        QStringLiteral("encoder"),
                                        QStringLiteral("engine"),
                                        QStringLiteral("mux"),
                                        QStringLiteral("record"),
                                        QStringLiteral("record.failure"),
                                        QStringLiteral("recording"),
                                        QStringLiteral("remux"),
                                        QStringLiteral("split"),
                                        QStringLiteral("startup"),
                                        QStringLiteral("storage"),
                                        QStringLiteral("video"),
                                        QStringLiteral("recording.artifacts")};
    return components.contains(component);
}

} // namespace exosnap::diagnostics
