#pragma once

#include <QString>

#include <update/update_types.h>

namespace exosnap::models {

struct AboutInfo {
    QString version;
    QString commit_short;
    QString commit_full;
    QString build_timestamp_utc;
    QString built_display;
    QString build_id;
    QString configuration;
    QString install_mode_label;
    QString channel;
    QString author;
    QString description;
    QString github_url;
    QString author_url;
    QString release_notes_url;
    QString commit_url;
    bool official_build = false;
    bool dirty_source_tree = false;
    bool debug_build = false;
};

struct AboutCopyFields {
    QString version;
    bool official_build = false;
    QString git_commit_full;
    QString build_timestamp_utc;
    QString build_id;
    QString configuration;
    bool dirty_source_tree = false;
    QString install_mode_label;
    QString channel;
    QString executable_path;
    QString executable_sha256;
};

[[nodiscard]] QString FormatBuildTimestampForDisplay(const QString& iso8601_utc);
[[nodiscard]] QString ResolveInstallModeLabel(exosnap::update::InstallMode install_mode, bool is_scoop);
[[nodiscard]] AboutInfo BuildAboutInfo(const QString& channel, exosnap::update::InstallMode install_mode,
                                       bool is_scoop);
[[nodiscard]] AboutCopyFields MakeAboutCopyFields(const AboutInfo& info, const QString& executable_path,
                                                  const QString& executable_sha256);
[[nodiscard]] QString BuildAboutCopyText(const AboutCopyFields& fields);
[[nodiscard]] QString ComputeFileSha256(const QString& path);

} // namespace exosnap::models
