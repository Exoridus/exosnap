#pragma once

#include <QByteArray>
#include <QHash>
#include <QPointF>
#include <QString>
#include <QStringList>

namespace exosnap::quick {

struct MappedGamepadArtwork {
    QByteArray svg;
    QHash<QString, QPointF> stick_travel;
    QStringList interactive_ids;
};

/// Prepares immutable idle/pressed SVG groups in the compositor's logical
/// canvas. Call when building an atlas, never while composing video frames.
[[nodiscard]] MappedGamepadArtwork PrepareMappedGamepadArtwork(const QString& mapping_path, QString& error);

} // namespace exosnap::quick
