#include "models/EditProjectCodec.h"
#include "models/EditProjectMedia.h"
#include "models/EditWorkspace.h"

#include <gtest/gtest.h>

namespace exosnap::edit {
namespace {

TEST(EditProject, RebindsOnlyForeignAudioIdentitiesAfterMatchingStreamLayout) {
    engine::EditMediaMetadata metadata;
    metadata.content_identity_valid = true;
    metadata.content_sha256[0] = 8;
    metadata.duration_us = 1000000;
    engine::EditSourceStream audio;
    audio.kind = engine::TimelineStreamKind::Audio;
    audio.codec.channels = 2;
    audio.codec.sample_rate = 48000;
    audio.stream_identity.emplace().bytes[0] = 17;
    metadata.streams.push_back(audio);
    const auto saved = DescribeSourceImport(metadata);
    ASSERT_TRUE(saved.descriptor);
    metadata.streams.front().stream_identity->bytes[0] = 24;
    engine::ContainerAudioTrack track;
    track.track_id = *metadata.streams.front().stream_identity;
    metadata.audio_metadata.actual_tracks.push_back(track);
    auto binding = std::make_shared<engine::UnverifiedAudioAssetBinding>();
    binding->actual_tracks.push_back(track);
    metadata.audio_metadata.unverified_asset_binding = binding;
    auto observed = *DescribeSourceImport(metadata).descriptor;
    observed.audio_identities_verified = true;
    EXPECT_FALSE(BindProjectStreamLayout(*saved.descriptor, observed, metadata));
    observed.audio_identities_verified = false;
    observed.streams.front().codec.sample_rate = 44100;
    EXPECT_FALSE(BindProjectStreamLayout(*saved.descriptor, observed, metadata));
    observed.streams.front().codec.sample_rate = 48000;
    ASSERT_TRUE(BindProjectStreamLayout(*saved.descriptor, observed, metadata));
    EXPECT_EQ(metadata.streams.front().stream_identity, audio.stream_identity);
    EXPECT_EQ(metadata.audio_metadata.actual_tracks.front().track_id, audio.stream_identity);
    EXPECT_EQ(metadata.audio_metadata.unverified_asset_binding->actual_tracks.front().track_id, audio.stream_identity);
    EXPECT_EQ(binding->actual_tracks.front().track_id, track.track_id);
}

TEST(EditProject, RoundTripsTheAuthoredTimelineWithFreshHistory) {
    Workspace workspace;
    Asset source;
    source.path = L"media/Gr\u00fc\u00dfe \U0001f3a5.mkv";
    source.name = "Recording";
    source.duration = 6'000'000;
    source.markers.push_back({2000, RecordingMarkerType::Highlight, "Take two"});
    const auto id = workspace.addAsset(source);
    ASSERT_TRUE(workspace.insert(id));
    ASSERT_TRUE(workspace.insert(id, 5'000'000));
    ASSERT_TRUE(workspace.setTrackLocked(workspace.tracks().front().id, true));
    const auto before = workspace.snapshot();
    ASSERT_FALSE(before->junctions.empty());
    ASSERT_TRUE(workspace.canUndo());
    const auto bytes = project::Encode(*before);
    ASSERT_TRUE(bytes);
    EditSnapshot decoded;
    ASSERT_EQ(project::Decode(*bytes, decoded), project::Status::Ok);
    Workspace restored;
    ASSERT_TRUE(restored.restoreProject(decoded));
    const auto after = restored.snapshot();
    EXPECT_EQ(before->tracks, after->tracks);
    EXPECT_EQ(before->clips, after->clips);
    EXPECT_EQ(before->junctions, after->junctions);
    EXPECT_EQ(before->locked_track_ids, after->locked_track_ids);
    EXPECT_EQ(after->assets.front()->path, source.path);
    EXPECT_EQ(after->assets.front()->markers, source.markers);
    EXPECT_EQ(project::Encode(*after), bytes);
    EXPECT_FALSE(restored.canUndo());
    EXPECT_FALSE(restored.canRedo());
}

TEST(EditProject, RejectsInvalidReferencesAndTimesWithoutChangingLiveState) {
    Workspace workspace;
    Asset asset;
    asset.path = L"source.mkv";
    asset.duration = 1'000'000;
    ASSERT_TRUE(workspace.insert(workspace.addAsset(asset)));
    const auto initial = workspace.snapshot();
    auto invalid = *initial;
    invalid.clips.front().start = (std::numeric_limits<Time>::max)();
    EXPECT_FALSE(workspace.restoreProject(invalid));
    invalid = *initial;
    invalid.clips.front().asset = 0;
    EXPECT_FALSE(workspace.restoreProject(invalid));
    invalid = *initial;
    invalid.assets.push_back(invalid.assets.front());
    EXPECT_FALSE(workspace.restoreProject(invalid));
    EXPECT_EQ(initial, workspace.snapshot());
    EXPECT_TRUE(workspace.canUndo());
}

TEST(EditProject, RejectsTruncationTrailingBytesNewerVersionsAndUnboundedCounts) {
    Workspace workspace;
    const auto bytes = project::Encode(*workspace.snapshot());
    ASSERT_TRUE(bytes);
    EditSnapshot untouched;
    untouched.id_high_watermark = 987;
    for (size_t n = 0; n < bytes->size(); ++n)
        ASSERT_EQ(project::Decode(std::span(*bytes).first(n), untouched), project::Status::Invalid);
    auto invalid = *bytes;
    invalid.push_back(0);
    EXPECT_EQ(project::Decode(invalid, untouched), project::Status::Invalid);
    invalid = *bytes;
    invalid[8] = 2;
    EXPECT_EQ(project::Decode(invalid, untouched), project::Status::Unsupported);
    invalid = *bytes;
    std::fill_n(invalid.begin() + 12, 4, 0xff);
    EXPECT_EQ(project::Decode(invalid, untouched), project::Status::Invalid);
    EXPECT_EQ(untouched.id_high_watermark, 987U);
}

} // namespace
} // namespace exosnap::edit
