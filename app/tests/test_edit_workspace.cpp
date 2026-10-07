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
