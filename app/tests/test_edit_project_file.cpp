#include <gtest/gtest.h>

#include "models/EditProjectFile.h"
#include "models/EditWorkspace.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QTemporaryDir>

#include <limits>

namespace exosnap::edit {
namespace {

TEST(EditWorkspace, ProjectRestoreValidatesBeforeReplacingAndStartsWithEmptyHistory) {
    Workspace original;
    Asset source;
    source.path = L"clip.mkv";
    source.duration = 10'000'000;
    ASSERT_TRUE(original.insert(original.addAsset(source)));
    const auto clip = original.selection();
    ASSERT_EQ(original.apply(Trim{clip, TrimEdge::Right, -1'000'000}, original.revision()).status,
              CommandStatus::Applied);
    const auto saved = original.snapshot();

    Workspace reopened;
    ASSERT_TRUE(reopened.restoreProject(*saved));
    EXPECT_EQ(reopened.clips(), saved->clips);
    EXPECT_FALSE(reopened.undo());
    EXPECT_FALSE(reopened.redo());

    auto corrupt = *saved;
    corrupt.clips.front().id = corrupt.tracks.front().id;
    EXPECT_FALSE(reopened.restoreProject(corrupt));
    EXPECT_EQ(reopened.clips(), saved->clips);

    corrupt = *saved;
    corrupt.clips.front().source_in = (std::numeric_limits<Time>::min)();
    EXPECT_FALSE(reopened.restoreProject(corrupt));
    EXPECT_EQ(reopened.clips(), saved->clips);
}

QByteArray ReadBytes(const QString& path) {
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly))
        return {};
    return input.readAll();
}

bool PutBytes(const QString& path, const QByteArray& bytes) {
    QFile output(path);
    return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.flush();
}

TEST(EditProjectFile, RoundTripsAuthoredStateAndMovesRelativeMediaWithProject) {
    QTemporaryDir source;
    QTemporaryDir moved;
    const auto path = source.filePath(QStringLiteral("project.exosnap-project"));
    const auto moved_path = moved.filePath(QStringLiteral("project.exosnap-project"));
    const auto media = QStringLiteral("media/\u00dcberblick.mkv");
    ASSERT_TRUE(source.isValid());
    ASSERT_TRUE(moved.isValid());

    auto descriptor = std::make_shared<ValidatedSourceDescriptor>();
    const StreamRef video{3, {{3}}, 0, engine::TimelineStreamKind::Video};
    const StreamRef audio{3, {{4}}, 1, engine::TimelineStreamKind::Audio};
    descriptor->file_identity.volume_serial = 123;
    descriptor->asset_identity_fingerprint[31] = 5;
    descriptor->audio_identities_verified = true;
    descriptor->streams = {{video, {0, 1'000'000}}, {audio, {0, 1'000'000}, 0, {"pcm", {1, 2}, 48000, 2}}};
    auto asset = std::make_shared<Asset>();
    asset->id = 3;
    asset->path = source.filePath(media).toStdWString();
    asset->name = QStringLiteral("\u00dcberblick.mkv").toStdString();
    asset->duration = 1'000'000;
    asset->markers = {{12, RecordingMarkerType::Cut, "Cut"}};
    asset->source_descriptor = descriptor;
    asset->input_mix_selection = InputMixSelection{descriptor->asset_identity_fingerprint, {audio.stream_identity}};
    asset->original_recipe = OriginalRecipeAnchor{5, {video, audio}, {0, 1'000'000}};
    asset->source_version = 8;

    ProjectDocument document;
    document.snapping = false;
    document.ripple = true;
    document.auto_crossfade = false;
    document.linked_selection = false;
    auto& snapshot = document.timeline;
    snapshot.assets = {asset};
    snapshot.tracks = {{1, TrackType::Video, "Video"}, {2, TrackType::Audio, "Audio", false, -3}};
    snapshot.clips = {{4, 3, 1, 5, 10'000, 500, 410'000, video},
                      {6, 3, 2, 5, 10'000, 500, 410'000, audio},
                      {7, 3, 1, 0, 400'000, 500'000, 750'000, video}};
    snapshot.clips[0].opacity_channel.keys = {{500, .5, engine::KeyInterpolation::Smooth}};
    snapshot.clips[0].transform_channels[3].keys = {{500, 10, engine::KeyInterpolation::Hold}};
    snapshot.clips[0].fade_in = {2'000, {engine::CurveKind::Bias, -.25}};
    snapshot.clips[1].gain_envelope.ranges = {{100, 400, 50, -2}};
    snapshot.junctions = {{4, 7, TransitionKind::Crossfade, 19'500, 8, 1, 400'000, 419'500}};
    snapshot.spacers = {{9, 1, 700'000, 50'000}};
    snapshot.locked_track_ids = {2};
    snapshot.result_range = {10'000, ResultOutMode::Fixed, 700'000};
    snapshot.timeline_end = {TimelineEndMode::FixedEnd, 750'000};
    snapshot.output_fps = {60'000, 1001};
    snapshot.output_width = 1920;
    snapshot.output_height = 1080;
    snapshot.id_high_watermark = 9'007'199'254'740'995ULL;
    snapshot.source_version_high_watermark = 8;

    QByteArray digest;
    ASSERT_EQ(WriteProjectFile(path, document, QByteArray{}, digest), ProjectFileError::None);
    ASSERT_TRUE(QFile::copy(path, moved_path));
    const auto loaded = ReadProjectFile(moved_path);
    ASSERT_EQ(loaded.error, ProjectFileError::None);
    EXPECT_EQ(loaded.digest, digest);
    EXPECT_FALSE(loaded.document.snapping);
    EXPECT_TRUE(loaded.document.ripple);
    EXPECT_FALSE(loaded.document.auto_crossfade);
    EXPECT_FALSE(loaded.document.linked_selection);
    const auto& reopened = loaded.document.timeline;
    EXPECT_EQ(reopened.tracks, snapshot.tracks);
    EXPECT_EQ(reopened.clips, snapshot.clips);
    EXPECT_EQ(reopened.junctions, snapshot.junctions);
    EXPECT_EQ(reopened.spacers, snapshot.spacers);
    EXPECT_EQ(reopened.locked_track_ids, snapshot.locked_track_ids);
    EXPECT_EQ(reopened.result_range, snapshot.result_range);
    EXPECT_EQ(reopened.timeline_end, snapshot.timeline_end);
    EXPECT_EQ(reopened.output_fps.num, 60'000);
    EXPECT_EQ(reopened.output_fps.den, 1001);
    EXPECT_EQ(reopened.output_width, 1920);
    EXPECT_EQ(reopened.output_height, 1080);
    EXPECT_EQ(reopened.id_high_watermark, snapshot.id_high_watermark);
    EXPECT_EQ(reopened.source_version_high_watermark, 8u);
    ASSERT_EQ(reopened.assets.size(), 1u);
    const auto& binding = *reopened.assets.front();
    EXPECT_EQ(binding.state, AssetState::Missing);
    EXPECT_EQ(binding.path, moved.filePath(media).toStdWString());
    EXPECT_EQ(binding.markers, asset->markers);
    EXPECT_EQ(binding.input_mix_selection, asset->input_mix_selection);
    ASSERT_TRUE(binding.original_recipe);
    EXPECT_EQ(binding.original_recipe->streams, asset->original_recipe->streams);
    ASSERT_TRUE(binding.source_descriptor);
    EXPECT_EQ(binding.source_descriptor->asset_identity_fingerprint, descriptor->asset_identity_fingerprint);
    EXPECT_EQ(binding.source_descriptor->file_identity, engine::FileIdentity{});
    ASSERT_EQ(binding.source_descriptor->streams.size(), 2u);
    EXPECT_EQ(binding.source_descriptor->streams[1].codec.codec_private, (std::vector<uint8_t>{1, 2}));
}

TEST(EditProjectFile, FailedWritePreservesPreviousFileAndDetectsExternalChanges) {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("project.exosnap-project"));
    const ProjectDocument project;
    QByteArray initial;
    ASSERT_EQ(WriteProjectFile(path, project, QByteArray{}, initial), ProjectFileError::None);
    const auto original = ReadBytes(path);
    QByteArray next;
    EXPECT_EQ(WriteProjectFile(path, project, QByteArray{}, next), ProjectFileError::Changed);
    EXPECT_EQ(ReadBytes(path), original);

    auto replacement = project;
    replacement.ripple = true;
    EXPECT_EQ(WriteProjectFile(path, replacement, initial, next), ProjectFileError::None);
    EXPECT_NE(initial, next);
    const auto updated = ReadBytes(path);
    EXPECT_EQ(WriteProjectFile(path, project, initial, next), ProjectFileError::Changed);
    EXPECT_EQ(ReadBytes(path), updated);

    replacement.timeline.assets.push_back(nullptr);
    EXPECT_EQ(WriteProjectFile(path, replacement, std::nullopt, next), ProjectFileError::Invalid);
    EXPECT_EQ(ReadBytes(path), updated);

    QLockFile other_writer(path + QStringLiteral(".exosnap-write.lock"));
    ASSERT_TRUE(other_writer.tryLock());
    EXPECT_EQ(WriteProjectFile(path, project, std::nullopt, next), ProjectFileError::Changed);
    EXPECT_EQ(ReadBytes(path), updated);
    other_writer.unlock();

    ASSERT_TRUE(PutBytes(path, {}));
    EXPECT_EQ(WriteProjectFile(path, project, QByteArray{}, next), ProjectFileError::Changed);
    EXPECT_TRUE(ReadBytes(path).isEmpty());
}

TEST(EditProjectFile, RejectsTruncatedCorruptFutureAndParentRelativeFiles) {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("project.exosnap-project"));
    auto asset = std::make_shared<Asset>();
    asset->id = 3;
    asset->path = directory.filePath(QStringLiteral("aaa/recording.mkv")).toStdWString();
    ProjectDocument project;
    project.timeline.assets = {asset};
    QByteArray digest;
    ASSERT_EQ(WriteProjectFile(path, project, QByteArray{}, digest), ProjectFileError::None);
    const auto original = ReadBytes(path);
    ASSERT_GT(original.size(), 32);
    EXPECT_TRUE(PutBytes(path, original.chopped(1)));
    EXPECT_EQ(ReadProjectFile(path).error, ProjectFileError::Invalid);

    auto corrupt = original;
    corrupt[corrupt.size() - 33] = 127;
    ASSERT_TRUE(PutBytes(path, corrupt));
    EXPECT_EQ(ReadProjectFile(path).error, ProjectFileError::Invalid);

    auto future = original.chopped(32);
    future[QByteArray("EXOSNAP PROJECT\r\n").size() + 3] = 2;
    future += QCryptographicHash::hash(future, QCryptographicHash::Sha256);
    ASSERT_TRUE(PutBytes(path, future));
    EXPECT_EQ(ReadProjectFile(path).error, ProjectFileError::UnsupportedVersion);

    for (const auto* unsafe : {".. /recording.mkv", "..\\xrecording.mkv"}) {
        auto traversal = original.chopped(32);
        ASSERT_TRUE(traversal.contains("aaa/recording.mkv"));
        traversal.replace("aaa/recording.mkv", unsafe);
        traversal += QCryptographicHash::hash(traversal, QCryptographicHash::Sha256);
        ASSERT_TRUE(PutBytes(path, traversal));
        EXPECT_EQ(ReadProjectFile(path).error, ProjectFileError::Invalid);
    }
}

} // namespace
} // namespace exosnap::edit
