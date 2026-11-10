#pragma once

#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <gamepad/gamepad.h>

namespace exosnap::quick {

struct MappedGamepadControl {
    gamepad::Control control = gamepad::Control::Count;
    gamepad::Control click_control = gamepad::Control::Count;
    bool live = false;
    /// Highlight of the active control, shared by the 2D and 3D views.
    QColor press_color;
};

struct MappedGamepadArtwork {
    QByteArray svg;
    QHash<QString, QPointF> stick_travel;
    QStringList interactive_ids;
    QHash<QString, MappedGamepadControl> controls;
};

/// Prepares immutable idle/pressed SVG groups in the compositor's logical
/// canvas. Call when building an atlas, never while composing video frames.
[[nodiscard]] MappedGamepadArtwork PrepareMappedGamepadArtwork(const QString& mapping_path, QString& error);

} // namespace exosnap::quick
