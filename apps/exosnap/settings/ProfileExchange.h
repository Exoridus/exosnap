#pragma once

#include "../models/RecordingProfile.h"

#include <QString>

#include <vector>

namespace exosnap {

struct ProfileImportResult {
    bool ok = false;
    QString error_message;
    std::vector<RecordingProfile> profiles;
};

[[nodiscard]] bool ExportProfilesToJsonFile(const QString& file_path, const std::vector<RecordingProfile>& profiles,
                                            QString* out_error_message = nullptr);
[[nodiscard]] ProfileImportResult ImportProfilesFromJsonFile(const QString& file_path);

} // namespace exosnap
