#include "UpdaterArgs.h"

#include <QDir>
#include <QFileInfo>

#include <cstdio>

#include <update/swap_engine.h>
#include <update/update_checker.h>

namespace exosnap::updater {

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

std::optional<UpdaterCommandLine> ParseUpdaterCommandLine(const QStringList& argv) {
    using exosnap::update::UpdateChannel;

    UpdaterCommandLine parsed;

    // Simple index scan; argv[0] is the executable path.
    for (qsizetype i = 1; i < argv.size(); ++i) {
        const QString& flag = argv[i];

        // Every recognised flag takes exactly one value.
        auto take_value = [&](QString& out) -> bool {
            if (i + 1 >= argv.size()) {
                ArgError(QStringLiteral("missing value for %1").arg(flag));
                return false;
            }
            out = argv[++i];
            return true;
        };

        QString value;
        if (flag == QLatin1String(exosnap::update_handoff::kApplyHandoffOption)) {
            if (!take_value(parsed.handoff_path)) {
                return std::nullopt;
            }
        } else if (flag == QStringLiteral("--channel")) {
            if (!take_value(value)) {
                return std::nullopt;
            }
            if (value == QStringLiteral("stable")) {
                parsed.channel = UpdateChannel::Stable;
            } else if (value == QStringLiteral("preview")) {
                parsed.channel = UpdateChannel::Preview;
            } else {
                ArgError(QStringLiteral("invalid --channel '%1' (expected stable|preview)").arg(value));
                return std::nullopt;
            }
        } else if (flag == QStringLiteral("--base-url")) {
            if (!take_value(parsed.base_url)) {
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
            parsed.preview_state = value;
        } else {
            ArgError(QStringLiteral("unknown argument '%1'").arg(flag));
            return std::nullopt;
        }
    }

    return parsed;
}

UpdaterArgs ArgsFromHandoff(const exosnap::update_handoff::UpdateHandoff& handoff,
                            const UpdaterCommandLine& command_line) {
    UpdaterArgs args;
    args.mode = exosnap::update::UpdaterMode::AppHandoff;
    args.install_mode = handoff.install_mode;
    args.install_dir = handoff.install_dir;
    args.app_pid = handoff.app_pid;
    args.current_version = handoff.current_version;
    args.target_version = handoff.target_version;
    args.update_transaction_id = handoff.update_transaction_id;
    args.manifest_path = handoff.manifest_path;
    args.manifest_signature_path = handoff.manifest_signature_path;
    args.handoff_path = command_line.handoff_path;
    args.verify_reinstall = handoff.verify_reinstall;
    // Deliberately NOT taken from the handoff. A handoff run resolves no feed:
    // the release it installs is already pinned by targetVersion and proven by
    // the handed-over manifest, so a channel and a base URL would be inputs to a
    // search this mode does not perform. Only the dev preview short-circuit
    // stays reachable, because it does no engine work at all.
    args.preview_state = command_line.preview_state;
    return args;
}

UpdaterArgs ArgsForManualStart(const UpdaterCommandLine& command_line) {
    UpdaterArgs args;
    args.mode = exosnap::update::UpdaterMode::Manual;
    args.channel = command_line.channel;
    args.base_url = command_line.base_url;
    args.preview_state = command_line.preview_state;
    // No target, no transaction, no handed-over manifest: this run is not part
    // of anyone's operation and resolves everything itself.
    return args;
}

} // namespace exosnap::updater