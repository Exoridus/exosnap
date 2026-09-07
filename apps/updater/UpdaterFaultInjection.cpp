#include "UpdaterFaultInjection.h"

#include <QtGlobal>

namespace exosnap::updater {
namespace {

constexpr const char* kVariable = "EXOSNAP_UPDATER_FAULT";

} // namespace

const char* InjectedFaultVariableName() {
    return kVariable;
}

InjectedFault ParseInjectedFault(const QString& value) {
    if (value.compare(QStringLiteral("uacDeclined"), Qt::CaseInsensitive) == 0)
        return InjectedFault::UacDeclined;
    return InjectedFault::None;
}

InjectedFault CurrentInjectedFault() {
    // Read on every call, never cached: a launcher arms this for one child
    // process, and a cached value would outlive the run it was meant for.
    return ParseInjectedFault(qEnvironmentVariable(kVariable));
}

} // namespace exosnap::updater
