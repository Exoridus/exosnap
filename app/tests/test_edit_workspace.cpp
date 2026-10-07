#include "models/EditExportProfile.h"
#include "models/EditWorkspace.h"
#include <gtest/gtest.h>

using namespace exosnap::edit;

namespace {
Id source(Workspace& workspace, const wchar_t* path = L"clip.mkv") {
    Asset asset;
    asset.path = path;
    asset.duration = 10000000;
    return workspace.addAsset(asset);
}
} // namespace

TEST(EditWorkspace, InsertOrdersLinkedTracksAndRejectsOverlap) {
    Workspace w;
    const auto asset = source(w);
    ASSERT_TRUE(w.insert(asset, 10000000));
    ASSERT_TRUE(w.insert(asset, 0));
    EXPECT_EQ(w.clips().size(), 4u);
    EXPECT_EQ(w.clips()[0].start, 0);
    EXPECT_EQ(w.clips()[1].start, 10000000);
    EXPECT_FALSE(w.insert(asset, 5000000));
    EXPECT_EQ(w.duration(), 20000000);
}

TEST(EditWorkspace, TrimSplitAndMovePreserveLinkedAudio) {
    Workspace w;
    ASSERT_TRUE(w.insert(source(w)));
    const auto id = w.selection();
    ASSERT_TRUE(w.trim(id, 1000000, 9000000));
    EXPECT_EQ(w.clips()[0].start, 1000000);
    EXPECT_EQ(w.clips()[1].source_in, 1000000);
    ASSERT_TRUE(w.split(id, 5000000));
    ASSERT_EQ(w.clips().size(), 4u);
    EXPECT_EQ(w.clips()[1].source_in, 5000000);
    EXPECT_EQ(w.clips()[3].group, w.clips()[1].group);
    ASSERT_TRUE(w.move(id, 0));
    EXPECT_EQ(w.clips()[2].start, 0);
    EXPECT_FALSE(w.move(id, 3000000));
}

TEST(EditWorkspace, RippleDeleteUndoRedoRestoreOnlyClipValues) {
    Workspace w;
    const auto asset = source(w);
    ASSERT_TRUE(w.insert(asset));
    const auto first = w.selection();
    ASSERT_TRUE(w.insert(asset));
    const auto before = w.clips();
    ASSERT_TRUE(w.remove(first, true));
    EXPECT_EQ(w.duration(), 10000000);
    EXPECT_EQ(w.clips()[0].start, 0);
    ASSERT_TRUE(w.undo());
    EXPECT_EQ(w.clips(), before);
    ASSERT_TRUE(w.redo());
    EXPECT_EQ(w.clips().size(), 2u);
    EXPECT_EQ(w.assets().size(), 1u);
}

TEST(EditWorkspace, OrdinaryDeleteRetainsGapAndEvaluationReturnsNoSource) {
    Workspace w;
    const auto asset = source(w);
    ASSERT_TRUE(w.insert(asset));
    const auto first = w.selection();
    ASSERT_TRUE(w.insert(asset));
    ASSERT_TRUE(w.remove(first, false));
    EXPECT_EQ(w.active(5000000), nullptr);
    ASSERT_NE(w.active(15000000), nullptr);
    EXPECT_EQ(w.duration(), 20000000);
}

TEST(EditWorkspace, UndoBranchDiscardsRedoAndAssetsStayAvailable) {
    Workspace w;
    const auto asset = source(w);
    ASSERT_TRUE(w.insert(asset));
    ASSERT_TRUE(w.undo());
    EXPECT_TRUE(w.canRedo());
    ASSERT_TRUE(w.insert(asset, 3000000));
    EXPECT_FALSE(w.canRedo());
    EXPECT_EQ(w.assets().size(), 1u);
}

TEST(EditWorkspace, SnapsToBoundariesPlayheadAndMarkersWithoutKeyframeRestriction) {
    Workspace w;
    const auto asset = source(w);
    ASSERT_TRUE(w.insert(asset));
    w.seek(3500000);
    EXPECT_EQ(w.snap(3510000, 0, 50000), 3500000);
    EXPECT_EQ(w.snap(10020000, 0, 50000), 10000000);
    EXPECT_EQ(w.snap(7200000, 0, 50000), 7200000);
}

TEST(EditWorkspace, MissingSegmentKeepsItsDurationAndMultipleTracksAreSupported) {
    Workspace w;
    const auto first = source(w);
    Asset missing;
    missing.path = L"missing.mkv";
    missing.duration = 3000000;
    missing.state = AssetState::Missing;
    const auto second = w.addAsset(missing);
    ASSERT_TRUE(w.insert(first));
    ASSERT_TRUE(w.insert(second));
    EXPECT_EQ(w.duration(), 13000000);
    EXPECT_EQ(w.asset(w.active(11000000)->asset)->state, AssetState::Missing);
    w.addTrack(TrackType::Video);
    w.addTrack(TrackType::Audio);
    EXPECT_EQ(w.tracks().size(), 4u);
}

TEST(EditWorkspace, BatchInsertionKeepsSourceOrderAndLinksBothTracks) {
    Workspace workspace;
    const auto first = source(workspace, L"b.mkv");
    const auto second = source(workspace, L"a.mkv");
    ASSERT_TRUE(workspace.insertAssets({first, second}, 3000000));
    ASSERT_EQ(workspace.clips().size(), 4U);
    for (const auto type : {TrackType::Video, TrackType::Audio}) {
        const auto* left = workspace.active(3000000, type);
        const auto* right = workspace.active(13000000, type);
        ASSERT_NE(left, nullptr);
        ASSERT_NE(right, nullptr);
        EXPECT_EQ(left->asset, first);
        EXPECT_EQ(right->asset, second);
        EXPECT_EQ(left->start, 3000000);
        EXPECT_EQ(right->start, 13000000);
        EXPECT_NE(left->group, right->group);
    }
    EXPECT_EQ(workspace.active(3000000)->group, workspace.active(3000000, TrackType::Audio)->group);
    EXPECT_EQ(workspace.active(13000000)->group, workspace.active(13000000, TrackType::Audio)->group);
    const auto inserted = workspace.clips();
    ASSERT_TRUE(workspace.undo());
    EXPECT_TRUE(workspace.clips().empty());
    EXPECT_FALSE(workspace.canUndo());
    EXPECT_EQ(workspace.assets().size(), 2U);
    ASSERT_TRUE(workspace.redo());
    EXPECT_EQ(workspace.clips(), inserted);
}

TEST(EditWorkspace, BatchOverlapRejectsTheEntireCommandIncludingItsNonOverlappingPrefix) {
    Workspace workspace;
    const auto first = source(workspace, L"existing.mkv");
    const auto second = source(workspace, L"new.mkv");
    ASSERT_TRUE(workspace.insert(first, 15000000));
    const auto original = workspace.clips();
    EXPECT_FALSE(workspace.insertAssets({second, second}, 0));
    EXPECT_EQ(workspace.clips(), original);
    ASSERT_TRUE(workspace.undo());
    EXPECT_TRUE(workspace.clips().empty());
    EXPECT_FALSE(workspace.canUndo());
}

TEST(EditExportProfile, MatchSourcePreservesResolutionWithoutRendering) {
    const auto profile = ResolveExportProfile(ExportProfile::MatchSource, 3440, 1440);
    EXPECT_EQ(profile.width, 3440);
    EXPECT_EQ(profile.height, 1440);
    EXPECT_FALSE(profile.requires_render);
}

TEST(EditExportProfile, YoutubeProfilesResolveTheirAdvertisedRasterAndRequireRendering) {
    struct Expected {
        ExportProfile profile = ExportProfile::MatchSource;
        int width = 0;
        int height = 0;
    };
    for (const auto expected :
         {Expected{ExportProfile::Youtube1080, 1920, 1080}, Expected{ExportProfile::Youtube1440, 2560, 1440},
          Expected{ExportProfile::Youtube4K, 3840, 2160}}) {
        const auto profile = ResolveExportProfile(expected.profile, 1280, 720);
        EXPECT_EQ(profile.width, expected.width);
        EXPECT_EQ(profile.height, expected.height);
        EXPECT_TRUE(profile.requires_render);
    }
}

TEST(EditExportProfile, ArchiveKeepsSourceResolutionButStillRequiresRendering) {
    const auto profile = ResolveExportProfile(ExportProfile::Archive, 2560, 1600);
    EXPECT_EQ(profile.width, 2560);
    EXPECT_EQ(profile.height, 1600);
    EXPECT_TRUE(profile.requires_render);
}

TEST(EditWorkspace, CrossfadeMovesLinkedSuffixAndRemovalRestoresHardCuts) {
    Workspace w;
    ASSERT_TRUE(w.insert(source(w, L"a.mkv")));
    const auto a = w.selection();
    ASSERT_TRUE(w.insert(source(w, L"b.mkv")));
    const auto b = w.selection();
    ASSERT_TRUE(w.insert(source(w, L"c.mkv")));
    const auto original = w.clips();
    ASSERT_TRUE(w.crossfade(a, b));
    EXPECT_EQ(w.clip(a)->start, 0);
    EXPECT_EQ(w.clip(b)->start, 9'500'000);
    EXPECT_EQ(w.duration(), 29'500'000);
    for (const auto& c : w.clips())
        if (c.group == w.clip(b)->group)
            EXPECT_EQ(c.start, 9'500'000);
    EXPECT_FALSE(w.insert(source(w, L"unrelated.mkv"), 9'600'000));
    EXPECT_FALSE(w.move(b, 9'000'000));
    ASSERT_TRUE(w.removeCrossfade(a));
    EXPECT_EQ(w.clips(), original);
    ASSERT_TRUE(w.undo());
    EXPECT_EQ(w.transitions().size(), 1u);
    ASSERT_TRUE(w.redo());
    EXPECT_TRUE(w.transitions().empty());
}

TEST(EditWorkspace, CrossfadeClampAndDurationEditAreUndoableTransactions) {
    Workspace w;
    ASSERT_TRUE(w.insert(source(w)));
    const auto a = w.selection();
    ASSERT_TRUE(w.insert(source(w)));
    const auto b = w.selection();
    ASSERT_TRUE(w.crossfade(a, b, 50'000'000));
    EXPECT_EQ(w.transitions()[0].duration, 9'999'999);
    EXPECT_EQ(w.clip(a)->source_out, 10'000'000);
    EXPECT_EQ(w.clip(b)->source_in, 0);
    ASSERT_TRUE(w.crossfade(a, b, 1'000'000));
    EXPECT_EQ(w.clip(b)->start, 9'000'000);
    ASSERT_TRUE(w.undo());
    EXPECT_EQ(w.clip(b)->start, 1);
    ASSERT_TRUE(w.undo());
    EXPECT_TRUE(w.transitions().empty());
    EXPECT_EQ(w.clip(b)->start, 10'000'000);
    ASSERT_TRUE(w.redo());
    ASSERT_TRUE(w.redo());
    EXPECT_EQ(w.clip(b)->start, 9'000'000);
}

TEST(EditTimelineEvaluation, CrossfadeUsesContinuousSourcesAndComplementaryLinkedWeights) {
    Workspace w;
    ASSERT_TRUE(w.insert(source(w, L"a.mkv")));
    const auto a = w.selection();
    ASSERT_TRUE(w.insert(source(w, L"b.mkv")));
    const auto b = w.selection();
    ASSERT_TRUE(w.crossfade(a, b, 1'000'000));
    const auto timeline = w.timeline();
    for (const Time at : {8'999'999LL, 9'000'000LL, 9'250'000LL, 9'500'000LL, 9'750'000LL, 9'999'999LL, 10'000'000LL}) {
        const auto value = exosnap::engine::EvaluateTimeline(timeline, at);
        EXPECT_TRUE(value.error.empty());
        EXPECT_FALSE(value.gap);
        if (at < 9'000'000 || at >= 10'000'000) {
            ASSERT_EQ(value.video.size(), 1u);
            EXPECT_EQ(value.video[0].clip, at < 9'000'000 ? a : b);
            EXPECT_EQ(value.video[0].source_us, at < 9'000'000 ? at : at - 9'000'000);
            EXPECT_EQ(value.video[0].weight, 1);
        } else {
            ASSERT_EQ(value.video.size(), 2u);
            ASSERT_EQ(value.audio.size(), 2u);
            EXPECT_EQ(value.video[0].clip, a);
            EXPECT_EQ(value.video[1].clip, b);
            EXPECT_EQ(value.video[0].source_us, at);
            EXPECT_EQ(value.video[1].source_us, at - 9'000'000);
            EXPECT_DOUBLE_EQ(value.video[1].weight, static_cast<double>(at - 9'000'000) / 1'000'000);
            EXPECT_DOUBLE_EQ(value.video[0].weight + value.video[1].weight, 1);
            EXPECT_EQ(value.audio[0].weight, value.video[0].weight);
            EXPECT_EQ(value.audio[1].weight, value.video[1].weight);
        }
    }
}

TEST(EditWorkspace, DeleteRestoresIncidentCrossfadesAndUndoRestoresEntireRelationship) {
    for (bool ripple : {false, true}) {
        Workspace w;
        ASSERT_TRUE(w.insert(source(w)));
        const auto a = w.selection();
        ASSERT_TRUE(w.insert(source(w)));
        const auto b = w.selection();
        ASSERT_TRUE(w.insert(source(w)));
        const auto c = w.selection();
        ASSERT_TRUE(w.crossfade(a, b));
        ASSERT_TRUE(w.crossfade(b, c));
        const auto before = w.clips();
        ASSERT_TRUE(w.remove(b, ripple));
        EXPECT_TRUE(w.transitions().empty());
        EXPECT_EQ(w.clip(c)->start, ripple ? 10'000'000 : 20'000'000);
        ASSERT_TRUE(w.undo());
        EXPECT_EQ(w.clips(), before);
        EXPECT_EQ(w.transitions().size(), 2u);
    }
}

TEST(EditTimelineEvaluation, RejectsInvalidRelationshipsEvenOutsideActiveClipsAndIgnoresStorageOrder) {
    using namespace exosnap::engine;
    TimelineSnapshot snapshot{{{1, 1, 1, false, 0, 0, 10}, {2, 2, 2, false, 5, 0, 10}}, {{1, 2, 5}}, 15};
    EXPECT_TRUE(EvaluateTimeline(snapshot, 7).error.empty());
    std::reverse(snapshot.clips.begin(), snapshot.clips.end());
    EXPECT_TRUE(EvaluateTimeline(snapshot, 7).error.empty());
    snapshot.crossfades = {{1, 1, 10}};
    EXPECT_FALSE(EvaluateTimeline(snapshot, 7).error.empty());
    snapshot.crossfades = {{1, 99, 5}};
    EXPECT_FALSE(EvaluateTimeline(snapshot, 99).error.empty());
}

TEST(EditTimelineEvaluation, TrimmedIncomingSourceAndMissingAudioUseSameOverlap) {
    Workspace w;
    ASSERT_TRUE(w.insert(source(w)));
    const auto a = w.selection();
    Asset silent;
    silent.path = L"silent.mkv";
    silent.duration = 10'000'000;
    silent.audio = false;
    ASSERT_TRUE(w.insert(w.addAsset(silent)));
    const auto b = w.selection();
    ASSERT_TRUE(w.trim(b, 1'000'000, 10'000'000));
    ASSERT_TRUE(w.move(b, 10'000'000));
    ASSERT_TRUE(w.crossfade(a, b, 1'000'000));
    const auto evaluation = exosnap::engine::EvaluateTimeline(w.timeline(), 9'500'000);
    ASSERT_EQ(evaluation.video.size(), 2u);
    EXPECT_EQ(evaluation.video[1].source_us, 1'500'000);
    ASSERT_EQ(evaluation.audio.size(), 1u);
    EXPECT_EQ(evaluation.audio[0].weight, 0.5);
    EXPECT_TRUE(exosnap::engine::EvaluateTimeline(w.timeline(), w.duration()).gap);
}
