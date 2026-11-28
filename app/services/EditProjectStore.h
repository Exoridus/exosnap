#pragma once

#include "models/EditCommandTypes.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QString>

namespace exosnap::edit {

class EditProjectStore {
    Q_DECLARE_TR_FUNCTIONS(EditProjectStore)
  public:
    // A recovered copy is retired only if the bytes originally recovered are
    // still present. Newer or replaced recovery files are left alone.
    static bool RemoveCheckpoint(const QString& path, const QByteArray& digest);
    // Read publishes validated outputs only on success (empty error text).
    static QString Read(const QString& path, EditSnapshot& snapshot, ProjectSessionState& state, QByteArray& digest);
    // An empty expected digest on Save requires a new destination.
    static QString Write(const QString& path, const QByteArray& expected, EditSnapshot snapshot, QByteArray& digest,
                         bool checkpoint, const ProjectSessionState& state);
};

} // namespace exosnap::edit
