#include <control/options.h>

namespace exosnap::control {

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

ControlOptions ParseControlOptions(const QStringList& arguments, const QString& option) {
    ControlOptions options;
    const int index = arguments.indexOf(option);
    if (index < 0)
        return options;

    options.requested = true;
    if (index + 1 >= arguments.size()) {
        options.error = QStringLiteral("%1 requires a run id argument").arg(option);
        return options;
    }

    const QString run_id = arguments.at(index + 1);
    if (!IsValidRunId(run_id)) {
        options.error = QStringLiteral("%1 run id must be 8-64 characters of [A-Za-z0-9._-]").arg(option);
        return options;
    }

    options.run_id = run_id;
    return options;
}

QString PipeName(const QString& role, const QString& run_id) {
    return QStringLiteral("\\\\.\\pipe\\ExoSnap.%1.%2").arg(role, run_id);
}

} // namespace exosnap::control
