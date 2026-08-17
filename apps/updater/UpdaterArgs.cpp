#include "UpdaterArgs.h"

#include <QDir>
#include <QFileInfo>

#include <cstdio>

#include <update/swap_engine.h>
#include <update/update_checker.h>

namespace {

void ArgError(const QString& message) {
    std::fprintf(stderr, "exosnap-updater: %s\n", qPrintable(message));
}

} // namespace

const QStringList& PreviewStateNames() {
    static const QStringList names = {
        QStringLiteral("download"), QStringLiteral("progress"), QStringLiteral("amber"),
        QStringLiteral("red"),      QStringLiteral("green"),    QStringLiteral("reboot"),
    };
    return names;
}

bool IsKnownPreviewState(const QString& value) {
    return PreviewStateNames().contains(value);
}

ManualContext ResolveManualContext(exosnap::update::InstallMode detected, const QString& registry_install_path,
                                   const QString& exe_dir) {
    ManualContext context;
    context.install_mode = detected;
    if (detected == exosnap::update::InstallMode::Installed && !registry_install_path.isEmpty()) {
        context.install_dir = QDir::cleanPath(registry_install_path);
        return context;
    }
    // Portable, or an installed copy whose registry stamp is missing: this
    // process was started by hand, so it is running FROM the installation --
    // unlike the staged handoff copy, which deliberately lives elsewhere. That
    // difference is exactly why `mode` is a field and not a derivation.
    context.install_dir = QDir::cleanPath(exe_dir);
    return context;
}

bool UpdateChecksEnabled(const UpdaterArgs& args) {
    return exosnap::update::IsUpdateCheckEnabled() || !args.base_url.isEmpty();
}

QString ReadInstalledVersion(const QString& install_dir) {
    if (install_dir.isEmpty())
        return {};
    const QString exe = QDir(install_dir).filePath(QStringLiteral("exosnap.exe"));
    if (!QFileInfo::exists(exe))
        return {};
    const std::optional<std::string> product =
        exosnap::update::ReadProductVersionString(QDir::toNativeSeparators(exe).toStdWString());
    return product.has_value() ? QString::fromStdString(*product) : QString();
}

std::optional<UpdaterArgs> ParseUpdaterArgs(const QStringList& argv) {
    using exosnap::update::InstallMode;
    using exosnap::update::UpdateChannel;

    UpdaterArgs args;
    // Handoff mode is not a flag: it is the presence of context only a launcher
    // can know. Channel, base URL and preview state are configuration a person
    // may reasonably pass by hand, so they deliberately do NOT arm the pipeline.
    bool handoff_context = false;

    // Simple index scan; argv[0] is the executable path.
    for (qsizetype i = 1; i < argv.size(); ++i) {
        const QString& flag = argv[i];

        // Every recognised flag except --verify-reinstall takes exactly one value.
        auto take_value = [&](QString& out) -> bool {
            if (i + 1 >= argv.size()) {
                ArgError(QStringLiteral("missing value for %1").arg(flag));
                return false;
            }
            out = argv[++i];
            return true;
        };

        QString value;
        if (flag == QStringLiteral("--verify-reinstall")) {
            // The only boolean flag: it takes no value.
            args.verify_reinstall = true;
            handoff_context = true;
        } else if (flag == QStringLiteral("--channel")) {
            if (!take_value(value)) {
                return std::nullopt;
            }
            if (value == QStringLiteral("stable")) {
                args.channel = UpdateChannel::Stable;
            } else if (value == QStringLiteral("preview")) {
                args.channel = UpdateChannel::Preview;
            } else {
                ArgError(QStringLiteral("invalid --channel '%1' (expected stable|preview)").arg(value));
                return std::nullopt;
            }
        } else if (flag == QStringLiteral("--install-mode")) {
            if (!take_value(value)) {
                return std::nullopt;
            }
            if (value == QStringLiteral("installed")) {
                args.install_mode = InstallMode::Installed;
            } else if (value == QStringLiteral("portable")) {
                args.install_mode = InstallMode::Portable;
            } else {
                ArgError(QStringLiteral("invalid --install-mode '%1' (expected installed|portable)").arg(value));
                return std::nullopt;
            }
        } else if (flag == QStringLiteral("--install-dir")) {
            if (!take_value(args.install_dir)) {
                return std::nullopt;
            }
            handoff_context = true;
        } else if (flag == QStringLiteral("--app-pid")) {
            if (!take_value(value)) {
                return std::nullopt;
            }
            bool ok = false;
            args.app_pid = value.toUInt(&ok);
            if (!ok) {
                ArgError(QStringLiteral("invalid --app-pid '%1' (expected an unsigned integer)").arg(value));
                return std::nullopt;
            }
            handoff_context = true;
        } else if (flag == QStringLiteral("--current-version")) {
            if (!take_value(args.current_version)) {
                return std::nullopt;
            }
            handoff_context = true;
        } else if (flag == QStringLiteral("--target-version")) {
            if (!take_value(args.target_version)) {
                return std::nullopt;
            }
            handoff_context = true;
        } else if (flag == QStringLiteral("--base-url")) {
            if (!take_value(args.base_url)) {
                return std::nullopt;
            }
        } else if (flag == QStringLiteral("--preview-state")) {
            if (!take_value(value)) {
                return std::nullopt;
            }
            if (!IsKnownPreviewState(value)) {
                ArgError(QStringLiteral("invalid --preview-state '%1' (expected %2)")
                             .arg(value, PreviewStateNames().join(QLatin1Char('|'))));
                return std::nullopt;
            }
            args.preview_state = value;
        } else {
            ArgError(QStringLiteral("unknown argument '%1'").arg(flag));
            return std::nullopt;
        }
    }

    args.mode = handoff_context ? exosnap::update::UpdaterMode::LegacyHandoff : exosnap::update::UpdaterMode::Manual;

    // Only a handoff has to name the install directory: it launched from a
    // staged copy that is deliberately NOT the installation. A manual start
    // derives its own context (ResolveManualContext), which is the whole point
    // of a double-clickable updater -- refusing there produced a process that
    // exited 2 with no console and no window, i.e. visibly nothing.
    if (args.mode == exosnap::update::UpdaterMode::LegacyHandoff &&
        args.install_mode == exosnap::update::InstallMode::Portable && args.install_dir.isEmpty()) {
        ArgError(QStringLiteral("--install-dir is required in portable install mode"));
        return std::nullopt;
    }

    // The verification reinstall gate compares the manifest version against
    // --current-version. Without one there is nothing to compare, and silently
    // degrading to "install whatever the channel offers" would defeat the gate.
    if (args.verify_reinstall && args.current_version.isEmpty()) {
        ArgError(QStringLiteral("--verify-reinstall requires --current-version"));
        return std::nullopt;
    }

    return args;
}
