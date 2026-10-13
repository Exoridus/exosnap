#include "services/RecordingArtifactRegistry.h"

#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <windows.h>

namespace exosnap {
namespace {

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};

engine::FileIdentity CreateTestArtifact(const std::filesystem::path& path) {
    Handle file{CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr)};
    EXPECT_NE(file.value, INVALID_HANDLE_VALUE);
    engine::FileIdentity result;
    FILE_ID_INFO info{};
    EXPECT_NE(GetFileInformationByHandleEx(file.value, FileIdInfo, &info, sizeof(info)), 0);
    result.volume_serial = info.VolumeSerialNumber;
    std::copy(std::begin(info.FileId.Identifier), std::end(info.FileId.Identifier), result.file_id.begin());
    return result;
}

engine::RecordingMediaDescriptor Descriptor(const std::filesystem::path& path, const engine::FileIdentity& identity) {
    engine::RecordingMediaDescriptor descriptor;
    descriptor.recording_id.bytes[0] = 1;
    descriptor.segment_id.bytes[0] = 2;
    descriptor.writer_generation = 3;
    descriptor.descriptor_revision = 4;
    descriptor.file_identity = identity;
    descriptor.path = path.string();
    return descriptor;
}

RecordingArtifactKey Key() {
    engine::RecordingId recording;
    recording.bytes[0] = 1;
    engine::SegmentId segment;
    segment.bytes[0] = 2;
    return {recording, segment, 3, 4};
}

TEST(RecordingArtifactRegistryTest, DeferredDeletionSurvivesRegistryLifetime) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const std::filesystem::path path = directory.filePath("source.mkv").toStdWString();
    const auto identity = CreateTestArtifact(path);
    std::optional<RecordingArtifactRegistry::SourceLease> lease;
    {
        RecordingArtifactRegistry registry;
        ASSERT_TRUE(registry.RegisterSource(Descriptor(path, identity), true));
        lease = registry.AcquireSourceLease(Key(), identity);
        ASSERT_TRUE(lease.has_value());
        EXPECT_EQ(registry.RequestArtifactDeletion(Key()), ArtifactDisposition::DeferredByLease);
        EXPECT_TRUE(registry.MarkWriterClosed(Key()));
    }
    EXPECT_TRUE(std::filesystem::exists(path));
    lease.reset();
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(RecordingArtifactRegistryTest, RenamePreservesPinnedReaderAndReplacementPath) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const std::filesystem::path source = directory.filePath("source.mkv").toStdWString();
    const std::filesystem::path master = directory.filePath("source.edit.mkv").toStdWString();
    const auto identity = CreateTestArtifact(source);
    RecordingArtifactRegistry registry;
    ASSERT_TRUE(registry.RegisterSource(Descriptor(source, identity), false));
    auto first = registry.AcquireSourceLease(Key(), identity);
    ASSERT_TRUE(first.has_value());
    ASSERT_EQ(registry.RenameClosed(Key(), master), ArtifactDisposition::Completed);
    const auto replacement_identity = CreateTestArtifact(source);
    EXPECT_NE(identity, replacement_identity);
    auto second = registry.AcquireSourceLease(Key(), identity, {{4, 8, {}, {}}}, 8);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->path(), master);
    ASSERT_EQ(second->extents().size(), 1);
    EXPECT_EQ(second->readable_end_offset(), 8);
    EXPECT_FALSE(registry.AcquireSourceLease(Key(), identity, {{4, 9, {}, {}}}, 8).has_value());
    EXPECT_EQ(registry.RequestArtifactDeletion(Key()), ArtifactDisposition::DeferredByLease);
    first.reset();
    EXPECT_TRUE(std::filesystem::exists(master));
    second.reset();
    EXPECT_FALSE(std::filesystem::exists(master));
    EXPECT_TRUE(std::filesystem::exists(source));
}

TEST(RecordingArtifactRegistryTest, ChangedIdentityAndOccupiedRenameTargetAreRejected) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const std::filesystem::path source = directory.filePath("source.mkv").toStdWString();
    const std::filesystem::path target = directory.filePath("occupied.edit.mkv").toStdWString();
    const auto identity = CreateTestArtifact(source);
    (void)CreateTestArtifact(target);
    RecordingArtifactRegistry registry;
    ASSERT_TRUE(registry.RegisterSource(Descriptor(source, identity), false));
    EXPECT_EQ(registry.RenameClosed(Key(), target), ArtifactDisposition::Failure);
    EXPECT_TRUE(std::filesystem::exists(source));
    EXPECT_TRUE(std::filesystem::exists(target));
    ASSERT_TRUE(DeleteFileW(source.c_str()));
    const auto replacement_identity = CreateTestArtifact(source);
    EXPECT_NE(identity, replacement_identity);
    EXPECT_FALSE(registry.AcquireSourceLease(Key(), identity).has_value());
    EXPECT_EQ(registry.RequestArtifactDeletion(Key()), ArtifactDisposition::Failure);
    EXPECT_TRUE(std::filesystem::exists(source));
}

TEST(RecordingArtifactRegistryTest, ExternalPublicationRebindsPinnedIdentityWithoutMovingBytes) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const std::filesystem::path partial = directory.filePath("source.partial.mkv").toStdWString();
    const std::filesystem::path published = directory.filePath("source.mkv").toStdWString();
    const std::filesystem::path other = directory.filePath("other.mkv").toStdWString();
    const auto identity = CreateTestArtifact(partial);
    const auto other_identity = CreateTestArtifact(other);
    RecordingArtifactRegistry registry;
    ASSERT_TRUE(registry.RegisterSource(Descriptor(partial, identity), true));
    EXPECT_EQ(registry.RebindClosedSource(Key(), published), ArtifactDisposition::Failure);
    ASSERT_TRUE(registry.MarkWriterClosed(Key()));
    auto first = registry.AcquireSourceLease(Key(), identity);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(registry.RebindClosedSource(Key(), other), ArtifactDisposition::Failure);
    ASSERT_TRUE(MoveFileExW(partial.c_str(), published.c_str(), MOVEFILE_WRITE_THROUGH));
    EXPECT_EQ(registry.RebindClosedSource(Key(), published), ArtifactDisposition::Completed);
    auto second = registry.AcquireSourceLease(Key(), identity);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->path(), published);
    EXPECT_EQ(second->identity(), identity);
    EXPECT_EQ(first->identity(), identity);
    EXPECT_FALSE(registry.AcquireSourceLease(Key(), other_identity).has_value());
    first.reset();
    second.reset();
    EXPECT_TRUE(std::filesystem::exists(published));
    EXPECT_TRUE(std::filesystem::exists(other));
}

TEST(RecordingArtifactRegistryTest, DeferredRenameKeepsReaderPathUntilLeaseRelease) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const std::filesystem::path source = directory.filePath("source.mkv").toStdWString();
    const std::filesystem::path master = directory.filePath("source.edit.mkv").toStdWString();
    const auto identity = CreateTestArtifact(source);
    RecordingArtifactRegistry registry;
    ASSERT_TRUE(registry.RegisterSource(Descriptor(source, identity), false));
    auto lease = registry.AcquireSourceLease(Key(), identity);
    ASSERT_TRUE(lease.has_value());
    EXPECT_EQ(registry.RenameClosed(Key(), master, true), ArtifactDisposition::DeferredByLease);
    EXPECT_TRUE(std::filesystem::exists(source));
    EXPECT_FALSE(std::filesystem::exists(master));
    EXPECT_EQ(lease->path(), source);
    lease.reset();
    EXPECT_EQ(registry.RenameClosed(Key(), master, true), ArtifactDisposition::Completed);
    EXPECT_FALSE(std::filesystem::exists(source));
    EXPECT_TRUE(std::filesystem::exists(master));
}

} // namespace
} // namespace exosnap
