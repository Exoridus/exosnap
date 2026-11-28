#pragma once

#include "EditCommandTypes.h"

#include <QByteArray>
#include <QString>

namespace exosnap::edit {

struct ProjectDocument {
    EditSnapshot timeline;
    bool snapping = true;
    bool ripple = false;
    bool auto_crossfade = true;
    bool linked_selection = true;
};

enum class ProjectFileError { None, Io, Invalid, UnsupportedVersion, Changed };

struct ProjectReadResult {
    ProjectFileError error = ProjectFileError::None;
    ProjectDocument document;
    QByteArray digest;
};

// Returned sources have no live binding. Expected content identities remain available for verification and relinking.
[[nodiscard]] ProjectReadResult ReadProjectFile(const QString& path);

// A present digest guards the current destination. An empty digest requires a new file, and nullopt allows Save As.
[[nodiscard]] ProjectFileError WriteProjectFile(const QString& path, const ProjectDocument& document,
                                                const std::optional<QByteArray>& expected_digest,
                                                QByteArray& written_digest);

} // namespace exosnap::edit
