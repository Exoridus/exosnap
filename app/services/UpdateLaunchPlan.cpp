// UpdateLaunchPlan.cpp -- pure, UI-agnostic helpers behind
// UpdateService::LaunchUpdater(). No Win32, no I/O: just the staging file list,
// the updater argv, and Scoop-path detection so they can be unit-tested headless
// (see app/tests/test_update_launch_plan.cpp).

#include "UpdateService.h"

#include <QString>
#include <QStringList>

namespace exosnap {

QStringList UpdaterStagingFileList() {
    // Relative to QCoreApplication::applicationDirPath(). Forward slashes; the
    // copy step resolves them against the native app dir. Keep this minimal: the
    // updater is a small Qt Widgets app that only needs Core/Gui/Widgets plus the
    // windows platform plugin. The styles plugin is optional and copied on top.
    return {
        QStringLiteral("exosnap-updater.exe"),
        QStringLiteral("Qt6Core.dll"),
        QStringLiteral("Qt6Gui.dll"),
        QStringLiteral("Qt6Widgets.dll"),
        QStringLiteral("plugins/platforms/qwindows.dll"),
    };
}

QStringList BuildUpdaterArgs(const exosnap::update::UpdateState& st, const QString& install_dir, quint32 pid,
                             const QString& current_version) {
    using exosnap::update::InstallMode;
    using exosnap::update::UpdateChannel;

    QStringList args;
    args << QStringLiteral("--channel")
         << (st.channel == UpdateChannel::Preview ? QStringLiteral("preview") : QStringLiteral("stable"));
    args << QStringLiteral("--install-mode")
         << (st.install_mode == InstallMode::Installed ? QStringLiteral("installed") : QStringLiteral("portable"));
    args << QStringLiteral("--install-dir") << install_dir;
    args << QStringLiteral("--app-pid") << QString::number(pid);
    args << QStringLiteral("--current-version") << current_version;
    return args;
}

QString ResolveUpdateCardState(bool update_available, bool is_scoop, const QString& applied_version,
                               const QString& available_version) {
    if (!update_available)
        return QStringLiteral("uptodate");
    if (is_scoop)
        return QStringLiteral("scoop");
    // Loop guard: the updater already ran for this version (a stale releases-API
    // cache is re-offering it). A manual check clears applied_version upstream so a
    // still-applicable version re-arms to "available".
    if (!applied_version.isEmpty() && available_version == applied_version)
        return QStringLiteral("pending");
    return QStringLiteral("available");
}

bool UpdateService::IsScoopManagedInstall(const QString& app_dir_path) {
    // Scoop lays apps out under "<scoop root>/apps/<name>/current". Normalise
    // separators and case first.
    QString normalised = app_dir_path;
    normalised.replace(QLatin1Char('\\'), QLatin1Char('/'));

    // Default layout under %USERPROFILE%\scoop (or the shell-global root): the path
    // carries a literal "/scoop/apps/" marker segment.
    if (normalised.contains(QStringLiteral("/scoop/apps/"), Qt::CaseInsensitive))
        return true;

    // Relocated root ($env:SCOOP): "<root>/apps/<name>/current" has no "scoop"
    // segment, but still uses Scoop's "apps" + "current" junction layout. Require
    // both an "/apps/" segment and a "current" path component so plain "/apps/"
    // trees (e.g. "D:/apps/ExoSnap") don't match.
    if (normalised.contains(QStringLiteral("/apps/"), Qt::CaseInsensitive)) {
        const QStringList parts = normalised.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (const QString& part : parts) {
            if (part.compare(QStringLiteral("current"), Qt::CaseInsensitive) == 0)
                return true;
        }
    }
    return false;
}

} // namespace exosnap
