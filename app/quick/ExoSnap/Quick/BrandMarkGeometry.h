#pragma once

// The canonical mark geometry, as a QML singleton.
//
// ExoBrandMark.qml draws the mark rather than loading the SVG, because the asset
// carries one fixed colour and the in-application mark has to take the theme's
// accent. Drawing it meant the coordinates existed twice; this is what stops
// that. The numbers come from ui/brand/BrandMark.h, which the runtime shell
// renderer and the build-time icon generator also read.
//
// Everything here is CONSTANT: the geometry is a compile-time property of the
// brand, and a mark whose radii could change at runtime would be a different
// mark.

#include <QObject>
#include <QtQmlIntegration/qqmlintegration.h>

#include "ui/brand/BrandMark.h"

namespace exosnap::quick {

class BrandMarkGeometry : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // The design grid every other value below is expressed in.
    Q_PROPERTY(qreal grid READ grid CONSTANT FINAL)
    Q_PROPERTY(qreal center READ center CONSTANT FINAL)
    Q_PROPERTY(qreal outerRadius READ outerRadius CONSTANT FINAL)
    Q_PROPERTY(qreal outerStroke READ outerStroke CONSTANT FINAL)
    Q_PROPERTY(qreal outerOpacity READ outerOpacity CONSTANT FINAL)
    Q_PROPERTY(qreal innerRadius READ innerRadius CONSTANT FINAL)
    Q_PROPERTY(qreal innerStroke READ innerStroke CONSTANT FINAL)
    Q_PROPERTY(qreal dotRadius READ dotRadius CONSTANT FINAL)

  public:
    explicit BrandMarkGeometry(QObject* parent = nullptr) : QObject(parent) {
    }

    [[nodiscard]] static qreal grid() noexcept {
        return ui::brand::kGrid;
    }
    [[nodiscard]] static qreal center() noexcept {
        return ui::brand::kCenter;
    }
    [[nodiscard]] static qreal outerRadius() noexcept {
        return ui::brand::kOuterRadius;
    }
    [[nodiscard]] static qreal outerStroke() noexcept {
        return ui::brand::kOuterStroke;
    }
    [[nodiscard]] static qreal outerOpacity() noexcept {
        return ui::brand::kOuterOpacity;
    }
    [[nodiscard]] static qreal innerRadius() noexcept {
        return ui::brand::kInnerRadius;
    }
    [[nodiscard]] static qreal innerStroke() noexcept {
        return ui::brand::kInnerStroke;
    }
    [[nodiscard]] static qreal dotRadius() noexcept {
        return ui::brand::kDotRadius;
    }
};

} // namespace exosnap::quick
