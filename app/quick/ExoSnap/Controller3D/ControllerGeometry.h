#pragma once

#include "ControllerState.h"

#include <QQuick3DGeometry>
#include <QtQmlIntegration/qqmlintegration.h>

namespace exosnap::quick {

/// Uploads one immutable controller primitive into the scene that owns it.
/// Geometry objects belong to a single scene, so every scene creates its own
/// while the vertex bytes stay implicitly shared with the cached model.
class ControllerGeometry : public QQuick3DGeometry {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(ControllerState* controller READ controller WRITE setController NOTIFY sourceChanged FINAL)
    Q_PROPERTY(int mesh READ mesh WRITE setMesh NOTIFY sourceChanged FINAL)

  public:
    explicit ControllerGeometry(QQuick3DObject* parent = nullptr);
    ControllerState* controller() const noexcept {
        return controller_;
    }
    int mesh() const noexcept {
        return mesh_;
    }
    void setController(ControllerState* controller);
    void setMesh(int mesh);

  signals:
    void sourceChanged();

  private:
    void Upload();
    ControllerState* controller_ = nullptr;
    int mesh_ = -1;
};

} // namespace exosnap::quick
