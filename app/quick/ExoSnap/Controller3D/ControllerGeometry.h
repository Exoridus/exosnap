#pragma once

#include "ControllerMechanics.h"
#include <QQuick3DGeometry>
#include <QtQmlIntegration/qqmlintegration.h>

namespace exosnap::quick {

class ControllerGeometry : public QQuick3DGeometry {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(Part part READ part WRITE setPart NOTIFY geometryChanged FINAL)
    Q_PROPERTY(int generation READ generation NOTIFY geometryChanged FINAL)

  public:
    enum Part { Stem, Rim, Cap, Well, Shell };
    Q_ENUM(Part)
    explicit ControllerGeometry(QQuick3DObject* parent = nullptr);
    Part part() const {
        return part_;
    }
    int generation() const {
        return generation_;
    }
    void setPart(Part part);
    bool SetDimensions(const StickDimensions& dimensions);

  signals:
    void geometryChanged();

  private:
    void Rebuild();
    Part part_ = Stem;
    int generation_ = 0;
    StickDimensions dimensions_;
};

} // namespace exosnap::quick
