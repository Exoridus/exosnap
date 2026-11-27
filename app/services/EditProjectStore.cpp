#include "EditProjectStore.h"
#include "models/EditProjectCodec.h"
#include "models/EditWorkspace.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSaveFile>

namespace exosnap::edit {
namespace {
using edit::project::MaxBytes;
constexpr qsizetype DigestBytes = 32;

QByteArray ReadDocument(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < DigestBytes ||
        file.size() > static_cast<qint64>(MaxBytes) + DigestBytes)
        return {};
    const auto bytes = file.read(MaxBytes + DigestBytes + 1);
    return file.error() == QFile::NoError && bytes.size() == file.size() ? bytes : QByteArray{};
}

QByteArray Digest(QByteArrayView bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

bool IsDescendant(const QString& relative) {
    return !QDir::isAbsolutePath(relative) && relative != QStringLiteral("..") &&
           !relative.startsWith(QStringLiteral("../"));
}

bool StaysInProject(const QDir& directory, const QString& media) {
    const auto project = directory.canonicalPath();
    if (project.isEmpty())
        return false;
    QFileInfo ancestor(media);
    // Offline media may have no canonical leaf. Resolve the nearest existing
    // ancestor so a containing junction or symlink still classifies the path.
    while (!ancestor.exists()) {
        if (ancestor.isSymLink() || ancestor.isJunction())
            return false;
        const auto parent = ancestor.absolutePath();
        if (parent == ancestor.absoluteFilePath())
            return false;
        ancestor.setFile(parent);
    }
    const auto actual = ancestor.canonicalFilePath();
    return !actual.isEmpty() && IsDescendant(QDir(project).relativeFilePath(actual));
}

bool ResolveReferences(edit::EditSnapshot& snapshot, const QString& path, bool writing) {
    const QDir dir = QFileInfo(path).absoluteDir();
    for (auto& source : snapshot.assets) {
        if (!source)
            return false;
        auto asset = std::make_shared<edit::Asset>(*source);
        auto media = QString::fromStdWString(asset->path);
        if (media.isEmpty() || media.contains(QChar::Null))
            return false;
        media = QDir::fromNativeSeparators(media);
        if (writing) {
            if (!QDir::isAbsolutePath(media))
                return false;
            const auto relative = dir.relativeFilePath(media);
            if (IsDescendant(relative) && StaysInProject(dir, media))
                media = relative;
        } else if (!QDir::isAbsolutePath(media)) {
            if (media.contains(QLatin1Char(':')) || media.split(QLatin1Char('/')).contains(QStringLiteral("..")))
                return false;
            media = dir.absoluteFilePath(media);
            if (!StaysInProject(dir, media))
                return false;
        }
        asset->path = media.toStdWString();
        source = std::move(asset);
    }
    return true;
}

} // namespace

bool EditProjectStore::RemoveCheckpoint(const QString& path, const QByteArray& digest) {
    if (!path.endsWith(QStringLiteral(".exorecovery")) || digest.size() != DigestBytes)
        return false;
    QLockFile lock(path + QStringLiteral(".lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0))
        return false;
    const auto bytes = ReadDocument(path);
    return bytes.size() >= DigestBytes && bytes.last(DigestBytes) == digest &&
           Digest(QByteArrayView(bytes).chopped(DigestBytes)) == digest && QFile::remove(path);
}

QString EditProjectStore::Read(const QString& path, EditSnapshot& snapshot, ProjectSessionState& state,
                               QByteArray& digest) {
    const auto bytes = ReadDocument(path);
    if (bytes.size() < DigestBytes)
        return tr("The project is unreadable or damaged.");
    const auto checksum = bytes.last(DigestBytes);
    const auto payload = QByteArrayView(bytes).chopped(DigestBytes);
    if (checksum != Digest(payload))
        return tr("The project is unreadable or damaged.");
    EditSnapshot candidate;
    ProjectSessionState settings;
    const auto status = project::Decode(
        {reinterpret_cast<const uint8_t*>(payload.data()), static_cast<size_t>(payload.size())}, candidate, &settings);
    if (status == project::Status::Unsupported)
        return tr("This project uses an unsupported file version.");
    if (status != project::Status::Ok || !ResolveReferences(candidate, path, false) ||
        !Workspace{}.restoreProject(candidate))
        return tr("The project contains invalid editor data.");
    snapshot = std::move(candidate);
    state = std::move(settings);
    digest = checksum;
    return {};
}

QString EditProjectStore::Write(const QString& path, const QByteArray& expected, edit::EditSnapshot snapshot,
                                QByteArray& digest, bool checkpoint, const edit::ProjectSessionState& state) {
    if (!edit::Workspace{}.restoreProject(snapshot))
        return tr("The project exceeds the supported file limits.");
    if (!ResolveReferences(snapshot, path, true))
        return tr("A project media path is invalid.");
    const auto encoded = edit::project::Encode(std::move(snapshot), state);
    if (!encoded)
        return tr("The project exceeds the supported file limits.");
    const QByteArrayView payload(reinterpret_cast<const char*>(encoded->data()),
                                 static_cast<qsizetype>(encoded->size()));
    const auto checksum = Digest(payload);
    QLockFile lock(path + QStringLiteral(".write-lock"));
    if (!lock.tryLock(0))
        return tr("Another editor is writing this project.");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(payload.data(), payload.size()) != payload.size() ||
        file.write(checksum) != checksum.size())
        return file.errorString();
    if (!checkpoint) {
        if (expected.isEmpty() && QFileInfo::exists(path))
            return tr("Choose a new project filename to preserve the existing file.");
        if (!expected.isEmpty()) {
            const auto prior = ReadDocument(path);
            if (prior.size() < DigestBytes || prior.last(DigestBytes) != expected ||
                Digest(QByteArrayView(prior).chopped(DigestBytes)) != expected)
                return tr("This project changed outside the editor. Use Save As to keep your edits.");
        }
    }
    if (!file.commit())
        return file.errorString();
    digest = checksum;
    return {};
}

} // namespace exosnap::edit
