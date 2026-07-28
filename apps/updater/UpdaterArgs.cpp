#include "UpdaterArgs.h"

#include <cstdio>

namespace {

void ArgError(const QString& message) {
    std::fprintf(stderr, "exosnap-updater: %s\n", qPrintable(message));
}

} // namespace

std::optional<UpdaterArgs> ParseUpdaterArgs(const QStringList& argv) {
    using exosnap::update::InstallMode;
    using exosnap::update::UpdateChannel;

    UpdaterArgs args;

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
        } else if (flag == QStringLiteral("--channel")) {
            if (!take_value(value)) {
                return std::nullopt;
            }
            if (value == QStringLiteral("stable")) {
                args.channel = UpdateChannel::Stable;
            } else if (value == QStringLiteral("preview")) {
                args.channel = UpdateChannel::Preview;
            } else {
                ArgError(QStringLiteral("invalid --channel '%1' (expected stable|preview)")
                             .arg(value));
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
                ArgError(QStringLiteral("invalid --install-mode '%1' (expected installed|portable)")
                             .arg(value));
                return std::nullopt;
            }
        } else if (flag == QStringLiteral("--install-dir")) {
            if (!take_value(args.install_dir)) {
                return std::nullopt;
            }
        } else if (flag == QStringLiteral("--app-pid")) {
            if (!take_value(value)) {
                return std::nullopt;
            }
            bool ok = false;
            args.app_pid = value.toUInt(&ok);
            if (!ok) {
                ArgError(QStringLiteral("invalid --app-pid '%1' (expected an unsigned integer)")
                             .arg(value));
                return std::nullopt;
            }
        } else if (flag == QStringLiteral("--current-version")) {
            if (!take_value(args.current_version)) {
                return std::nullopt;
            }
        } else if (flag == QStringLiteral("--base-url")) {
            if (!take_value(args.base_url)) {
                return std::nullopt;
            }
        } else if (flag == QStringLiteral("--preview-state")) {
            if (!take_value(value)) {
                return std::nullopt;
            }
            const bool known = value == QStringLiteral("progress") ||
                               value == QStringLiteral("amber") ||
                               value == QStringLiteral("red") ||
                               value == QStringLiteral("green");
            if (!known) {
                ArgError(
                    QStringLiteral("invalid --preview-state '%1' (expected progress|amber|red|green)")
                        .arg(value));
                return std::nullopt;
            }
            args.preview_state = value;
        } else {
            ArgError(QStringLiteral("unknown argument '%1'").arg(flag));
            return std::nullopt;
        }
    }

    if (args.install_mode == exosnap::update::InstallMode::Portable && args.install_dir.isEmpty()) {
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
