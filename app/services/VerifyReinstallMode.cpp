// VerifyReinstallMode.cpp -- see header. Pure argv inspection, nothing else.

#include "VerifyReinstallMode.h"

#include <QString>

namespace exosnap::services {

bool HasVerifyUpdateReinstallRequest(const QStringList& args) {
    return args.contains(QString::fromLatin1(kVerifyUpdateReinstallFlag));
}

} // namespace exosnap::services
