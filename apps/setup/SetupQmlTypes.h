#pragma once

#include "SetupController.h"
#include <QtQml/qqmlregistration.h>

struct SetupControllerQml {
    Q_GADGET
    QML_FOREIGN(exosnap::setup::SetupController)
    QML_NAMED_ELEMENT(SetupController)
    QML_UNCREATABLE("SetupController is provided by the setup process")
};
