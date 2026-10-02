// UpdateLaunchPlan.cpp -- pure, UI-agnostic helpers behind
// UpdateService::LaunchUpdater(). No Win32: the staging file list, the recursive
// staging copy, the updater argv, and Scoop-path detection so they can be
// unit-tested headless (see app/tests/test_update_launch_plan.cpp).

#include "UpdateService.h"

#include <control/options.h>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

namespace exosnap {

namespace {

// Copies one directory tree, preserving relative layout. Returns false on the
// first entry that cannot be created or copied.
bool CopyRuntimeTree(const QString& source_dir, const QString& destination_dir, const QString& relative_label,
                     QString* error) {
    QDirIterator it(source_dir, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString source = it.next();
        const QString relative = QDir(source_dir).relativeFilePath(source);
        const QString destination = QDir(destination_dir).filePath(relative);
        if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
            *error = QStringLiteral("Failed to create staging directory for: %1/%2").arg(relative_label, relative);
            return false;
        }
        if (!QFile::copy(source, destination)) {
            *error = QStringLiteral("Failed to stage updater file: %1/%2").arg(relative_label, relative);
            return false;
        }
    }
    return true;
}

} // namespace

bool StageUpdaterRuntime(const QString& app_dir, const QString& staging_dir, QString* error) {
    for (const QString& rel : UpdaterStagingFileList()) {
        if (rel.endsWith(QLatin1Char('/'))) {
            const QString trimmed = rel.left(rel.size() - 1);
            const QString source_dir = QDir(app_dir).filePath(trimmed);
            if (!QFileInfo(source_dir).isDir()) {
                *error = QStringLiteral("Updater runtime directory missing: %1").arg(rel);
                return false;
            }
            if (!CopyRuntimeTree(source_dir, QDir(staging_dir).filePath(trimmed), trimmed, error))
                return false;
            continue;
        }
        const QString source = QDir(app_dir).filePath(rel);
        const QString destination = QDir(staging_dir).filePath(rel);
        if (!QFileInfo::exists(source)) {
            *error = QStringLiteral("Updater runtime file missing: %1").arg(rel);
            return false;
        }
        if (!QDir().mkpath(QFileInfo(destination).absolutePath()) || !QFile::copy(source, destination)) {
            *error = QStringLiteral("Failed to stage updater file: %1").arg(rel);
            return false;
        }
    }
    return true;
}

QStringList UpdaterStagingFileList() {
    // Relative to QCoreApplication::applicationDirPath(). Forward slashes; the
    // copy step resolves them against the native app dir. An entry ending in '/'
    // is a directory staged recursively, which keeps the QML import trees whole
    // (qmldir + plugins.qmltypes + plugin DLL + the style's own .qml files)
    // instead of listing each file of a Qt module by hand.
    //
    // The updater is a Qt Quick application: it needs the Quick/Qml runtime,
    // the Basic Controls style it imports, QML's layout and shape modules, and
    // the windows platform plugin. QtTest/QuickTest are deliberately absent.
    return {
        QStringLiteral("exosnap-updater.exe"),
        QStringLiteral("Qt6Core.dll"),
        QStringLiteral("Qt6Gui.dll"),
        QStringLiteral("Qt6Qml.dll"),
        QStringLiteral("Qt6QmlMeta.dll"),
        QStringLiteral("Qt6QmlModels.dll"),
        QStringLiteral("Qt6QmlWorkerScript.dll"),
        // Hard import dependencies of Qt6Qml/Qt6Quick on Windows.
        QStringLiteral("Qt6Network.dll"),
        QStringLiteral("Qt6OpenGL.dll"),
        QStringLiteral("Qt6Quick.dll"),
        QStringLiteral("Qt6QuickControls2.dll"),
        QStringLiteral("Qt6QuickControls2Basic.dll"),
        // The Basic style's implementation library, imported by its QML plugin.
        QStringLiteral("Qt6QuickControls2BasicStyleImpl.dll"),
        QStringLiteral("Qt6QuickControls2Impl.dll"),
        QStringLiteral("Qt6QuickTemplates2.dll"),
        QStringLiteral("Qt6QuickLayouts.dll"),
        QStringLiteral("Qt6QuickShapes.dll"),
        QStringLiteral("Qt6Svg.dll"),
        QStringLiteral("qml/QtQml/"),
        // The Controls module root: qmldir + its plugin. Basic/ and impl/ are
        // the only style submodules staged; the others are optional imports the
        // qmldir tolerates and the updater never uses.
        QStringLiteral("qml/QtQuick/Controls/qmldir"),
        QStringLiteral("qml/QtQuick/Controls/plugins.qmltypes"),
        QStringLiteral("qml/QtQuick/Controls/qtquickcontrols2plugin.dll"),
        QStringLiteral("qml/QtQuick/Controls/Basic/"),
        QStringLiteral("qml/QtQuick/Controls/impl/"),
        QStringLiteral("qml/QtQuick/Layouts/"),
        QStringLiteral("qml/QtQuick/Shapes/"),
        QStringLiteral("qml/QtQuick/Templates/"),
        QStringLiteral("qml/QtQuick/Window/"),
        QStringLiteral("plugins/platforms/qwindows.dll"),
    };
}

QStringList BuildUpdaterArgs(const QString& handoff_path, const QString& automation_run_id) {
    QStringList args;
    // The whole operation, by reference. One option, one document, one version
    // gate -- instead of seven search arguments the child had to re-resolve a
    // release from.
    args << QString::fromLatin1(exosnap::update_handoff::kApplyHandoffOption) << handoff_path;
    // Only when this process is itself being driven. A normal launch passes
    // nothing, and the updater then creates no endpoint at all. Deliberately not
    // a handoff field: this names a control session, not a product operation.
    if (!automation_run_id.isEmpty())
        args << QString::fromLatin1(exosnap::control::option::kUpdaterControl) << automation_run_id;
    return args;
}

QProcessEnvironment UpdaterChildEnvironment(const QProcessEnvironment& parent) {
    QProcessEnvironment child = parent;
    // Removing an absent key is a no-op, so this is safe on an environment that
    // never had it. Spelled out rather than taken from a Qt header: Qt exposes no
    // constant for it, and a typo here would be invisible -- the child would just
    // keep inheriting the variable.
    child.remove(QStringLiteral("QT_QPA_DISABLE_REDIRECTION_SURFACE"));
    return child;
}

exosnap::update_handoff::UpdateHandoff BuildUpdateHandoff(const exosnap::update::UpdateState& st,
                                                          const UpdateService::PreparedUpdate& prepared,
                                                          const QString& install_dir, quint32 pid,
                                                          const QString& current_version, bool verify_reinstall) {
    exosnap::update_handoff::UpdateHandoff handoff;
    handoff.update_transaction_id = prepared.update_transaction_id;
    // The exact release tag the check offered, verbatim. The updater compares
    // the signed manifest against THIS string, which is what keeps the version
    // the user was offered, the version the What's-new payload describes and the
    // version actually installed from ever being three different answers.
    handoff.target_version = QString::fromStdString(st.available_version_raw);
    handoff.current_version = current_version;
    handoff.manifest_path = prepared.manifest_path;
    handoff.manifest_signature_path = prepared.manifest_signature_path;
    handoff.install_mode = st.install_mode;
    handoff.install_dir = install_dir;
    handoff.app_pid = pid;
    // the updater's own same-version gate. Only ever set for a run the
    // user explicitly started with --verify-update-reinstall.
    handoff.verify_reinstall = verify_reinstall;
    return handoff;
}

QString HandoffRefusalReason(const exosnap::update::UpdateState& st, const UpdateService::PreparedUpdate& prepared) {
    if (st.available_version_raw.empty())
        return QStringLiteral("There is no offered version to hand over.");
    if (!prepared.error.isEmpty())
        return prepared.error;
    if (prepared.update_transaction_id.isEmpty() || prepared.manifest_path.isEmpty() ||
        prepared.manifest_signature_path.isEmpty())
        return QStringLiteral("This update has not been prepared yet. Check for updates again.");
    // Exact string equality, the same rule the updater's target gate uses. A
    // preparation left over from a previous offer would hand the updater a
    // transaction for a release the user did not accept.
    if (prepared.target_version != QString::fromStdString(st.available_version_raw))
        return QStringLiteral("The prepared update is for version %1, but %2 is on offer. Check for updates again.")
            .arg(prepared.target_version, QString::fromStdString(st.available_version_raw));
    return {};
}

QString ResolveUpdateCardState(bool update_available, bool is_scoop, const QString& applied_version,
                               const QString& available_version, bool verify_reinstall_mode,
                               const QString& current_version, UpdateHandoffPhase handoff_phase) {
    if (handoff_phase == UpdateHandoffPhase::UpdaterRunning)
        return QStringLiteral("updater-running");
    if (handoff_phase == UpdateHandoffPhase::ClosingForHandoff)
        return QStringLiteral("pending");
    if (!update_available)
        return QStringLiteral("uptodate");
    if (is_scoop)
        return QStringLiteral("scoop");
    // Verification reinstall: the offered version IS the running one.
    // Exact string equality — the engine granted the offer on the same basis.
    if (verify_reinstall_mode && !available_version.isEmpty() && available_version == current_version)
        return QStringLiteral("verify-reinstall");
    // Loop guard: the updater already ran for this version (a stale releases-API
    // cache is re-offering it). A manual check clears applied_version upstream so a
    // still-applicable version re-arms to "available".
    if (!applied_version.isEmpty() && available_version == applied_version)
        return QStringLiteral("pending");
    return QStringLiteral("available");
}

QString AppliedVersionForCommittedHandoff(const QString& target_version, bool verification_reinstall) {
    return verification_reinstall ? QString() : target_version;
}

QString ReconcileAppliedVersionOnStartup(const QString& /*persisted_applied_version*/) {
    return {};
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
    // a "current" component sitting exactly two components after an "apps"
    // component (i.e. "apps/<name>/current"), not just anywhere in the path, so
    // trees like "C:/apps/current/ExoSnap" or "D:/Media/current/apps/" don't match.
    if (normalised.contains(QStringLiteral("/apps/"), Qt::CaseInsensitive)) {
        const QStringList parts = normalised.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (int i = 0; i < parts.size(); ++i) {
            if (parts[i].compare(QStringLiteral("apps"), Qt::CaseInsensitive) == 0 && i + 2 < parts.size() &&
                parts[i + 2].compare(QStringLiteral("current"), Qt::CaseInsensitive) == 0)
                return true;
        }
    }
    return false;
}

} // namespace exosnap
