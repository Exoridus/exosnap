#include "AboutInfo.h"

#include "ExoSnapBuildInfo.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStringList>

#ifndef EXOSNAP_BUILD_CONFIG
#define EXOSNAP_BUILD_CONFIG "Unknown"
#endif

namespace exosnap::models {
namespace {

constexpr const char* kAppAuthor = "Exoridus";
constexpr const char* kGitHubUrl = "https://github.com/Exoridus/exosnap";
constexpr const char* kAuthorProfileUrl = "https://github.com/Exoridus";
constexpr const char* kReleasesUrl = "https://github.com/Exoridus/exosnap/releases";
constexpr const char* kAppDescription =
    "A calm, preview-first screen recorder with a high-performance GPU pipeline, multi-track audio "
    "routing, and diagnostics when you need them.";
constexpr const char* kDefaultChannel = "Stable";
constexpr const char* kUnavailableCommit = "Unavailable";

} // namespace

QString FormatBuildTimestampForDisplay(const QString& iso8601_utc) {
    const QDateTime parsed = QDateTime::fromString(iso8601_utc, Qt::ISODate);
    if (!parsed.isValid())
        return iso8601_utc;
    return parsed.toUTC().toString(QStringLiteral("yyyy-MM-dd HH:mm")) + QStringLiteral(" UTC");
}

QString ResolveInstallModeLabel(exosnap::update::InstallMode install_mode, bool is_scoop) {
    if (install_mode == exosnap::update::InstallMode::Installed)
        return QStringLiteral("MSI");
    if (is_scoop)
        return QStringLiteral("Scoop");
    return QStringLiteral("Portable");
}

AboutInfo BuildAboutInfo(const QString& channel, exosnap::update::InstallMode install_mode, bool is_scoop) {
    AboutInfo info;
    info.version = QString::fromLatin1(build::kVersion);
    info.commit_short = QString::fromLatin1(build::kGitCommit);
    info.commit_full = QString::fromLatin1(build::kGitCommitFull);
    info.build_timestamp_utc = QString::fromLatin1(build::kBuildTimestampUtc);
    info.built_display = FormatBuildTimestampForDisplay(info.build_timestamp_utc);
    info.build_id = QString::fromLatin1(build::kBuildId);
    info.configuration = QString::fromLatin1(EXOSNAP_BUILD_CONFIG);
    info.install_mode_label = ResolveInstallModeLabel(install_mode, is_scoop);
    info.channel = channel.trimmed().isEmpty() ? QString::fromLatin1(kDefaultChannel) : channel.trimmed();
    info.author = QString::fromLatin1(kAppAuthor);
    info.description = QString::fromLatin1(kAppDescription);
    info.github_url = QString::fromLatin1(kGitHubUrl);
    info.author_url = QString::fromLatin1(kAuthorProfileUrl);
    info.release_notes_url = QString::fromLatin1(kReleasesUrl);
    info.official_build = build::kOfficialBuild;
    info.dirty_source_tree = build::kDirtySourceTree;
    info.debug_build = info.configuration.compare(QStringLiteral("Debug"), Qt::CaseInsensitive) == 0;
    if (info.commit_full != QString::fromLatin1(kUnavailableCommit))
        info.commit_url = QStringLiteral("%1/commit/%2").arg(info.github_url, info.commit_full);
    return info;
}

AboutCopyFields MakeAboutCopyFields(const AboutInfo& info, const QString& executable_path,
                                    const QString& executable_sha256) {
    AboutCopyFields fields;
    fields.version = info.version;
    fields.official_build = info.official_build;
    fields.git_commit_full = info.commit_full;
    fields.build_timestamp_utc = info.build_timestamp_utc;
    fields.build_id = info.build_id;
    fields.configuration = info.configuration;
    fields.dirty_source_tree = info.dirty_source_tree;
    fields.install_mode_label = info.install_mode_label;
    fields.channel = info.channel;
    fields.executable_path = QDir::toNativeSeparators(executable_path);
    fields.executable_sha256 = executable_sha256;
    return fields;
}

QString BuildAboutCopyText(const AboutCopyFields& fields) {
    QStringList lines;
    lines << QStringLiteral("ExoSnap");
    lines << QStringLiteral("Version: %1").arg(fields.version);
    lines << (fields.official_build ? QStringLiteral("Tag: v%1").arg(fields.version)
                                    : QStringLiteral("Tag: (unofficial build)"));
    lines << QStringLiteral("Commit: %1").arg(fields.git_commit_full);
    lines << QStringLiteral("Build time: %1").arg(fields.build_timestamp_utc);
    lines << QStringLiteral("Build ID: %1").arg(fields.build_id.isEmpty() ? QStringLiteral("(none)") : fields.build_id);
    lines << QStringLiteral("Architecture: x64");
    lines << QStringLiteral("Configuration: %1").arg(fields.configuration);
    lines << QStringLiteral("Official build: %1")
                 .arg(fields.official_build ? QStringLiteral("yes") : QStringLiteral("no"));
    if (fields.dirty_source_tree)
        lines << QStringLiteral("Dirty source tree: yes");
    lines << QStringLiteral("Install mode: %1").arg(fields.install_mode_label);
    lines << QStringLiteral("Update channel: %1").arg(fields.channel);
    lines << QStringLiteral("Executable: %1").arg(fields.executable_path);
    lines << QStringLiteral("Executable SHA-256: %1").arg(fields.executable_sha256);
    return lines.join(QLatin1Char('\n'));
}

QString ComputeFileSha256(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return {};
    return QString::fromLatin1(hash.result().toHex());
}

} // namespace exosnap::models
