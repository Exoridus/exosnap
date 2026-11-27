#include "models/EditProjectCodec.h"
#include "models/EditWorkspace.h"
#include "services/EditProjectStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QTemporaryDir>
#include <filesystem>
#include <gtest/gtest.h>

namespace exosnap::edit {
namespace {

QByteArray Contents(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

TEST(EditProjectStore, MovingProjectRebasesChildrenAndPreservesExternalReferences) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const auto old_dir = temp.filePath(QStringLiteral("old"));
    const auto new_dir = temp.filePath(QStringLiteral("new"));
    ASSERT_TRUE(QDir().mkpath(old_dir));
    ASSERT_TRUE(QDir().mkpath(new_dir));
    const auto original = QDir(old_dir).filePath(QStringLiteral("Schnitt.exoproj"));
    const auto moved = QDir(new_dir).filePath(QStringLiteral("Schnitt.exoproj"));
    Workspace workspace;
    Asset source;
    source.path = QDir(old_dir).filePath(QStringLiteral("media/Grüße.mkv")).toStdWString();
    source.duration = 3'000'000;
    ASSERT_TRUE(workspace.insert(workspace.addAsset(source)));
    source.path = temp.filePath(QStringLiteral("external.mkv")).toStdWString();
    ASSERT_NE(workspace.addAsset(source), 0U);
    ProjectSessionState settings{{{420, RecordingMarkerType::Cut, "Cut"}}, false};
    QByteArray digest;
    ASSERT_TRUE(EditProjectStore::Write(original, {}, *workspace.snapshot(), digest, false, settings).isEmpty());
    ASSERT_TRUE(QFile::rename(original, moved));
    EditSnapshot reopened;
    ProjectSessionState restored;
    QByteArray observed;
    ASSERT_TRUE(EditProjectStore::Read(moved, reopened, restored, observed).isEmpty());
    EXPECT_EQ(observed, digest);
    EXPECT_EQ(restored, settings);
    ASSERT_EQ(reopened.assets.size(), 2U);
    EXPECT_EQ(reopened.assets[0]->path, QDir(new_dir).filePath(QStringLiteral("media/Grüße.mkv")).toStdWString());
    EXPECT_EQ(reopened.assets[1]->path, source.path);
    EXPECT_EQ(reopened.clips, workspace.clips());
}

TEST(EditProjectStore, AnExternalDirectoryLinkRoundTripsAsAnAbsoluteReference) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const auto external = temp.filePath(QStringLiteral("external"));
    const auto folder = temp.filePath(QStringLiteral("project"));
    ASSERT_TRUE(QDir().mkpath(external));
    ASSERT_TRUE(QDir().mkpath(folder));
    QFile media(QDir(external).filePath(QStringLiteral("source.mkv")));
    ASSERT_TRUE(media.open(QIODevice::WriteOnly));
    media.close();
    const auto link = QDir(folder).filePath(QStringLiteral("media"));
    std::error_code error;
    std::filesystem::create_directory_symlink(QDir(external).filesystemAbsolutePath(),
                                              QFileInfo(link).filesystemAbsoluteFilePath(), error);
    if (error)
        GTEST_SKIP() << "Directory symlink unavailable: " << error.message();
    Asset asset;
    asset.path = QDir(link).filePath(QStringLiteral("source.mkv")).toStdWString();
    asset.duration = 1000000;
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(workspace.addAsset(asset)));
    const auto path = QDir(folder).filePath(QStringLiteral("saved.exoproj"));
    QByteArray digest;
    ASSERT_TRUE(EditProjectStore::Write(path, {}, *workspace.snapshot(), digest, false, {}).isEmpty());
    EditSnapshot restored;
    ProjectSessionState state;
    ASSERT_TRUE(EditProjectStore::Read(path, restored, state, digest).isEmpty());
    ASSERT_EQ(restored.assets.size(), 1U);
    EXPECT_EQ(restored.assets.front()->path, asset.path);
}

TEST(EditProjectStore, ConflictsAndWriteLocksPreservePublishedFile) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const auto path = temp.filePath(QStringLiteral("project.exoproj"));
    Workspace workspace;
    QByteArray digest;
    ASSERT_TRUE(EditProjectStore::Write(path, {}, *workspace.snapshot(), digest, false, {}).isEmpty());
    const auto original = Contents(path);
    ASSERT_FALSE(original.isEmpty());
    QByteArray next;
    EXPECT_FALSE(EditProjectStore::Write(path, {}, *workspace.snapshot(), next, false, {}).isEmpty());
    EXPECT_EQ(Contents(path), original);
    QLockFile lock(path + QStringLiteral(".write-lock"));
    ASSERT_TRUE(lock.tryLock(0));
    EXPECT_FALSE(EditProjectStore::Write(path, digest, *workspace.snapshot(), next, false, {}).isEmpty());
    lock.unlock();
    QFile modified(path);
    ASSERT_TRUE(modified.open(QIODevice::WriteOnly | QIODevice::Append));
    ASSERT_EQ(modified.write("changed"), 7);
    modified.close();
    EXPECT_FALSE(EditProjectStore::Write(path, digest, *workspace.snapshot(), next, false, {}).isEmpty());
    EXPECT_EQ(Contents(path), original + "changed");
}

TEST(EditProjectStore, TraversalAndFutureSchemaLeaveOutputsUntouched) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const auto path = temp.filePath(QStringLiteral("invalid.exoproj"));
    Workspace workspace;
    Asset source;
    source.path = L"../unrelated.mkv";
    source.duration = 1'000'000;
    ASSERT_NE(workspace.addAsset(source), 0U);
    auto encoded = project::Encode(*workspace.snapshot());
    ASSERT_TRUE(encoded);
    EditSnapshot result;
    result.id_high_watermark = 42;
    ProjectSessionState settings;
    QByteArray digest("original");
    for (const bool future : {false, true}) {
        if (future)
            (*encoded)[8] = 2;
        const QByteArray payload(reinterpret_cast<const char*>(encoded->data()),
                                 static_cast<qsizetype>(encoded->size()));
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(payload), payload.size());
        ASSERT_EQ(file.write(QCryptographicHash::hash(payload, QCryptographicHash::Sha256)), 32);
        file.close();
        EXPECT_FALSE(EditProjectStore::Read(path, result, settings, digest).isEmpty());
        EXPECT_EQ(result.id_high_watermark, 42U);
        EXPECT_EQ(digest, QByteArray("original"));
    }
}

} // namespace
} // namespace exosnap::edit
