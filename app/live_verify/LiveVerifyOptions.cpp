#include "LiveVerifyOptions.h"

namespace exosnap::live_verify {

bool IsValidRunId(const QString& run_id) {
    if (run_id.size() < 8 || run_id.size() > 64)
        return false;
    for (const QChar character : run_id) {
        const bool allowed = character.isLetterOrNumber() && character.unicode() < 128;
        if (allowed || character == QLatin1Char('.') || character == QLatin1Char('_') ||
            character == QLatin1Char('-')) {
            continue;
        }
        return false;
    }
    return true;
}

ControlOptions ParseControlOptions(const QStringList& arguments) {
    ControlOptions options;
    const int index = arguments.indexOf(QString::fromLatin1(kControlOption));
    if (index < 0)
        return options;

    options.requested = true;
    if (index + 1 >= arguments.size()) {
        options.error = QStringLiteral("%1 requires a run id argument").arg(QString::fromLatin1(kControlOption));
        return options;
    }

    const QString run_id = arguments.at(index + 1);
    if (!IsValidRunId(run_id)) {
        options.error = QStringLiteral("%1 run id must be 8-64 characters of [A-Za-z0-9._-]")
                            .arg(QString::fromLatin1(kControlOption));
        return options;
    }

    options.run_id = run_id;
    return options;
}

QString PipeNameForRunId(const QString& run_id) {
    return QStringLiteral("\\\\.\\pipe\\ExoSnap.LiveVerify.%1").arg(run_id);
}

} // namespace exosnap::live_verify
