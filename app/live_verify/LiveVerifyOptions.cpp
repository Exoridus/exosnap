#include "LiveVerifyOptions.h"

namespace exosnap::live_verify {

ControlOptions ParseControlOptions(const QStringList& arguments) {
    return exosnap::control::ParseControlOptions(arguments, QString::fromLatin1(kControlOption));
}

QString PipeNameForRunId(const QString& run_id) {
    return exosnap::control::PipeName(QString::fromLatin1(kControlRole), run_id);
}

} // namespace exosnap::live_verify
