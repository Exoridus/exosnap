#pragma once

#include "OverlayAdapter.h"

#include <QObject>
#include <QtQmlIntegration/qqmlintegration.h>

// The component suite needs the production state enum without constructing
// the application-owned adapter and its recording dependencies.
class OverlayStateTestEnums final : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(OverlayAdapter)
    QML_UNCREATABLE("Only overlay state values are available in component tests")

  public:
    enum State {
        Hidden = exosnap::quick::OverlayAdapter::Hidden,
        Recording = exosnap::quick::OverlayAdapter::Recording,
        Paused = exosnap::quick::OverlayAdapter::Paused,
        Warning = exosnap::quick::OverlayAdapter::Warning
    };
    Q_ENUM(State)
};
