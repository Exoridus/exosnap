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

bool UpdateService::IsScoopManagedInstall(const QString& app_dir_path) {
    // Scoop lays apps out under "<scoop root>/apps/<name>/current". Normalise
    // separators and case, then look for the "/scoop/apps/" marker segment.
    QString normalised = app_dir_path;
    normalised.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return normalised.contains(QStringLiteral("/scoop/apps/"), Qt::CaseInsensitive);
}

} // namespace exosnap
