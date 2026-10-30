#include "models/EditClipboard.h"
#include "models/EditExportProfile.h"
#include "models/EditSelection.h"
#include "models/EditSourceImport.h"
#include "models/EditTime.h"
#include "models/EditWorkspace.h"
#include <algorithm>
#include <array>
#include <gtest/gtest.h>
#include <limits>

using namespace exosnap::edit;

namespace {
Id source(Workspace& workspace, const wchar_t* path = L"clip.mkv") {
    Asset asset;
    asset.path = path;
    asset.duration = 10000000;
    return workspace.addAsset(asset);
}
} // namespace

TEST(EditWorkspace, DefaultTrimKeepsUnselectedSuffixAtItsCommittedTime) {
    Workspace w;
    const auto asset = source(w);
    ASSERT_TRUE(w.insert(asset));
    const auto first = w.selection();
    ASSERT_TRUE(w.insert(asset));
    const auto suffix = w.selection();
    ASSERT_EQ(w.apply(Trim{first, TrimEdge::Right, -1'000'000}, w.revision()).status, CommandStatus::Applied);
    EXPECT_EQ(w.clip(suffix)->start, 10'000'000);
    EXPECT_EQ(w.clip(first)->end(), 9'000'000);
}

TEST(EditWorkspaceGrid, AudioTrimUsesOutputSamplesAndKeepsTheUneditedLegacyEdge) {
    struct SampleInterval {
        uint32_t rate = 0;
        TrimEdge edge = TrimEdge::Left;
        Time in_us = 0;
        Time out_us = 0;
        Time expected_in = 0;
        Time expected_out = 0;
    };
    for (const auto sample :
         {SampleInterval{48000, TrimEdge::Left, 63, 83, 63, 83}, SampleInterval{48000, TrimEdge::Right, 63, 83, 63, 84},
          SampleInterval{44100, TrimEdge::Left, 23, 45, 21, 45}, SampleInterval{44100, TrimEdge::Right, 23, 45, 23, 44},
          SampleInterval{96000, TrimEdge::Left, 31, 42, 21, 42}, SampleInterval{96000, TrimEdge::Right, 31, 42, 31, 52},
          SampleInterval{48000, TrimEdge::Left, 63, 82, 42, 82}, SampleInterval{48000, TrimEdge::Right, 64, 83, 64, 85},
          SampleInterval{0, TrimEdge::Left, 63, 83, 63, 83}, SampleInterval{0, TrimEdge::Right, 63, 83, 63, 84}}) {
        const auto edge = sample.edge;
        SCOPED_TRACE(::testing::Message() << "rate=" << sample.rate << " edge=" << static_cast<int>(edge)
                                          << " source=" << sample.in_us << ".." << sample.out_us);
        Workspace workspace;
        Asset asset;
        asset.path = L"audio.wav";
        asset.duration = 1000;
        auto descriptor = std::make_shared<ValidatedSourceDescriptor>();
        SourceStreamDescriptor stream;
        stream.ref = {0, exosnap::engine::AudioTrackId{{1}}, 0, exosnap::engine::TimelineStreamKind::Audio};
        stream.source_span = edge == TrimEdge::Left ? Span{0, sample.out_us} : Span{sample.in_us, 1000};
        stream.codec.sample_rate = sample.rate;
        stream.codec.channels = 2;
        descriptor->streams.push_back(stream);
        asset.source_descriptor = std::move(descriptor);
        ASSERT_TRUE(workspace.insert(workspace.addAsset(std::move(asset)), 0));
        const auto id = workspace.selection();
        const auto delta = edge == TrimEdge::Left ? sample.in_us : sample.out_us - 1000;
        ASSERT_EQ(workspace.apply(Trim{id, edge, delta}, workspace.revision()).status, CommandStatus::Applied);
        EXPECT_EQ(workspace.clip(id)->source_in, sample.expected_in);
        EXPECT_EQ(workspace.clip(id)->source_out, sample.expected_out);
        EXPECT_EQ(workspace.apply(Trim{id, edge, edge == TrimEdge::Left ? 1000 : -1000}, workspace.revision()).status,
                  CommandStatus::NoOp);
        EXPECT_EQ(workspace.clip(id)->source_in, sample.expected_in);
        EXPECT_EQ(workspace.clip(id)->source_out, sample.expected_out);
    }
}

TEST(EditWorkspace, FixedEndSurvivesDeletionAndUndoAndExtendsForLaterContent) {
    Workspace w;
    const auto asset = source(w);
    ASSERT_TRUE(w.insert(asset));
    const auto first = w.selection();
    ASSERT_EQ(w.apply(SetTimelineEnd{{TimelineEndMode::FixedEnd, 15'000'000}}, w.revision()).status,
              CommandStatus::Applied);
    ASSERT_TRUE(w.remove(first, false));
    EXPECT_EQ(w.duration(), 15'000'000);
    EXPECT_EQ(w.timeline().result_out_us, 15'000'000);
    ASSERT_TRUE(w.undo());
    ASSERT_TRUE(w.move(first, 12'000'000));
    EXPECT_EQ(w.duration(), 22'000'000);
    ASSERT_TRUE(w.undo());
    EXPECT_EQ(w.snapshot()->timeline_end.end_us, 15'000'000);
    EXPECT_EQ(w.apply(SetTimelineEnd{{TimelineEndMode::FixedEnd, 1}}, w.revision()).status, CommandStatus::Rejected);
}

TEST(EditWorkspace, SpacerIsTransparentUndoableMetadataAndDoesNotCloseTimeOnRemoval) {
    Workspace w;
    const auto track = w.tracks().front().id;
    ASSERT_EQ(w.apply(AddSpacer{track, 0, 2'000'000}, w.revision()).status, CommandStatus::Applied);
    const auto spacer = w.snapshot()->spacers.front().id;
    EXPECT_TRUE(w.assets().empty());
    EXPECT_TRUE(w.timeline().clips.empty());
    EXPECT_EQ(w.duration(), 2'000'000);
    ASSERT_EQ(w.apply(MoveSpacer{spacer, 3'000'000}, w.revision()).status, CommandStatus::Applied);
    const auto before = w.snapshot();
    EXPECT_EQ(w.apply(AddSpacer{track, 4'000'000, 2'000'000}, w.revision()).status, CommandStatus::Rejected);
    EXPECT_EQ(w.revision(), before->revision);
    ASSERT_EQ(w.apply(RemoveSpacer{spacer}, w.revision()).status, CommandStatus::Applied);
    ASSERT_TRUE(w.undo());
    EXPECT_EQ(w.snapshot()->spacers.front().start_us, 3'000'000);
    EXPECT_EQ(w.apply(TrimSpacer{spacer, TrimEdge::Right, -2'000'000}, w.revision()).status, CommandStatus::Rejected);
    ASSERT_TRUE(w.redo());
    EXPECT_TRUE(w.snapshot()->spacers.empty());
}

TEST(EditWorkspace, MovePreviewShowsRippleSuffixAndRejectsAStaleReleaseAtomically) {
    Workspace w;
    const auto asset = source(w);
    ASSERT_TRUE(w.insert(asset));
    const auto first = w.selection();
    ASSERT_TRUE(w.insert(asset));
    const auto suffix = w.selection();
    Move command{{first}, 2'000'000};
    command.ripple = true;
    const auto frozen = w.snapshot();
    const auto preview = PreviewEdit(*frozen, command);
    ASSERT_TRUE(preview.valid);
    const auto projected = std::find_if(preview.clips.begin(), preview.clips.end(),
                                        [suffix](const auto& clip) { return clip.id == suffix; });
    ASSERT_NE(projected, preview.clips.end());
    EXPECT_EQ(projected->start, 12'000'000);
    EXPECT_EQ(w.clip(suffix)->start, 10'000'000);
    ASSERT_EQ(w.apply(command, frozen->revision).status, CommandStatus::Applied);
    const auto revision = w.revision();
    EXPECT_EQ(w.apply(command, frozen->revision).status, CommandStatus::Rejected);
    EXPECT_EQ(w.revision(), revision);
    ASSERT_TRUE(w.undo());
    EXPECT_EQ(w.clip(first)->start, 0);
    EXPECT_EQ(w.clip(suffix)->start, 10'000'000);
}

TEST(EditWorkspace, AutoCrossfadeOnlyControlsNewJunctions) {
    Workspace w;
    const auto asset = source(w);
    ASSERT_TRUE(w.insert(asset));
    ASSERT_TRUE(w.insert(asset));
    const auto incoming = w.selection();
    Move command{{incoming}, -2'000'000};
    command.auto_crossfade = false;
    ASSERT_EQ(w.apply(command, w.revision()).status, CommandStatus::Applied);
    ASSERT_EQ(w.transitions().size(), 2u);
    for (const auto& junction : w.transitions())
        EXPECT_EQ(junction.kind, TransitionKind::HardCut);
    ASSERT_TRUE(w.undo());
    command.auto_crossfade = true;
    ASSERT_EQ(w.apply(command, w.revision()).status, CommandStatus::Applied);
    for (const auto& junction : w.transitions())
        EXPECT_EQ(junction.kind, TransitionKind::Crossfade);
}

TEST(EditWorkspace, CloseGapPreviewsWholeTimelineAndRefusesToCutOtherLayers) {
    Workspace w;
    const auto asset = source(w);
    ASSERT_TRUE(w.insert(asset, 2'000'000));
    const auto lead = w.selection();
    ASSERT_EQ(w.apply(SetTimelineEnd{{TimelineEndMode::FixedEnd, 15'000'000}}, w.revision()).status,
              CommandStatus::Applied);
    const auto frozen = w.snapshot();
    const auto preview = PreviewEdit(*frozen, CloseGap{{0, 2'000'000}});
    ASSERT_TRUE(preview.valid);
    EXPECT_EQ(w.clip(lead)->start, 2'000'000);
    ASSERT_EQ(w.apply(CloseGap{{0, 2'000'000}}, frozen->revision).status, CommandStatus::Applied);
    EXPECT_EQ(w.clip(lead)->start, 0);
    EXPECT_EQ(w.duration(), 15'000'000);
    ASSERT_TRUE(w.undo());
    const auto before = w.snapshot();
    const auto invalid = PreviewEdit(*before, CloseGap{{1'000'000, 3'000'000}});
    EXPECT_FALSE(invalid.valid);
    EXPECT_EQ(w.apply(CloseGap{{1'000'000, 3'000'000}}, before->revision).status, CommandStatus::Rejected);
    EXPECT_EQ(w.revision(), before->revision);
    EXPECT_EQ(w.clips(), before->clips);
}

TEST(EditWorkspace, RemovingEarlierTimePreservesCustomJunctionOffsetsAndHistory) {
    for (int operation = 0; operation < 4; ++operation) {
        for (const bool hard_cut : {false, true}) {
            SCOPED_TRACE(operation);
            SCOPED_TRACE(hard_cut);
            Workspace w;
            const auto asset = source(w);
            Id prefix = 0;
            if (operation == 2) {
                Asset short_asset;
                short_asset.path = L"prefix.mkv";
                short_asset.duration = 2'000'000;
                ASSERT_TRUE(w.insert(w.addAsset(short_asset)));
                prefix = w.selection();
            }
            ASSERT_TRUE(w.insert(asset, 2'000'000));
            const auto outgoing = w.selection();
            ASSERT_TRUE(w.insert(asset, 12'000'000));
            const auto incoming = w.selection();
            ASSERT_TRUE(w.crossfade(outgoing, incoming, 2'000'000));
            ASSERT_FALSE(w.transitions().empty());
            const auto junction_id = w.transitions().front().id;
            if (hard_cut) {
                ASSERT_EQ(w.apply(SetHardCut{junction_id, 11'000'000}, w.revision()).status, CommandStatus::Applied);
            } else {
                ASSERT_EQ(w.apply(SetJunctionWindow{junction_id, {10'500'000, 11'500'000}}, w.revision()).status,
                          CommandStatus::Applied);
            }
            EditCommand command = CloseGap{{0, 2'000'000}};
            if (operation == 1)
                command = RippleRemoveRange{{0, 2'000'000}};
            else if (operation == 2)
                command = RippleRemoveClips{{prefix}};
            else if (operation == 3)
                command = KeepRange{{2'000'000, 20'000'000}};
            const auto frozen = w.snapshot();
            const auto preview = PreviewEdit(*frozen, command);
            ASSERT_TRUE(preview.valid);
            const auto projected = std::find_if(preview.junctions.begin(), preview.junctions.end(),
                                                [junction_id](const auto& item) { return item.id == junction_id; });
            ASSERT_NE(projected, preview.junctions.end());
            EXPECT_EQ(projected->incoming_anchor_us, 8'000'000);
            if (hard_cut) {
                EXPECT_EQ(projected->kind, TransitionKind::HardCut);
                EXPECT_EQ(projected->cut_us, 9'000'000);
            } else {
                EXPECT_EQ(projected->kind, TransitionKind::Crossfade);
                EXPECT_EQ(projected->in_us, 8'500'000);
                EXPECT_EQ(projected->out_us, 9'500'000);
            }
            EXPECT_EQ(w.transitions(), frozen->junctions);
            ASSERT_EQ(w.apply(command, frozen->revision).status, CommandStatus::Applied);
            EXPECT_EQ(w.transitions(), preview.junctions);
            ASSERT_TRUE(w.undo());
            EXPECT_EQ(w.clips(), frozen->clips);
            EXPECT_EQ(w.transitions(), frozen->junctions);
            ASSERT_TRUE(w.redo());
            EXPECT_EQ(w.transitions(), preview.junctions);
        }
    }
}

TEST(EditWorkspace, SplitRetainsOnlyOuterFadesAndPreservesEvaluatedPictureAndAudio) {
    Workspace w;
    ASSERT_TRUE(w.insert(source(w)));
    const auto lead = w.selection();
    const auto original_clips = w.clips();
    for (const auto& clip : original_clips) {
        ASSERT_EQ(w.apply(SetFade{clip.id, FadeEdge::In, {1'000'000, {}}}, w.revision()).status,
                  CommandStatus::Applied);
        ASSERT_EQ(w.apply(SetFade{clip.id, FadeEdge::Out, {1'000'000, {}}}, w.revision()).status,
                  CommandStatus::Applied);
    }
    const auto before = w.snapshot();
    const auto original = w.timeline();
    const auto preview = PreviewEdit(*before, Split{{lead}, 5'000'000});
    ASSERT_TRUE(preview.valid);
    ASSERT_EQ(w.apply(Split{{lead}, 5'000'000}, before->revision).status, CommandStatus::Applied);
    EXPECT_EQ(w.clips(), preview.clips);
    ASSERT_EQ(w.clips().size(), 4u);
    for (const auto& clip : w.clips()) {
        EXPECT_EQ(clip.fade_in.duration_us, clip.start == 0 ? 1'000'000 : 0);
        EXPECT_EQ(clip.fade_out.duration_us, clip.start == 0 ? 0 : 1'000'000);
    }
    const auto gains = [](const exosnap::engine::TimelineSnapshot& timeline, Time at) {
        const auto evaluation = exosnap::engine::EvaluateTimeline(timeline, at);
        EXPECT_TRUE(evaluation.error.empty()) << evaluation.error;
        std::array<double, 2> result{};
        for (const auto& contribution : evaluation.video)
            result[0] += contribution.weight * contribution.opacity;
        for (const auto& contribution : evaluation.audio)
            result[1] += contribution.weight;
        return result;
    };
    for (const Time at : {0, 500'000, 1'000'000, 4'500'000, 4'999'999, 5'000'000, 5'500'000, 9'000'000, 9'500'000})
        EXPECT_EQ(gains(original, at), gains(w.timeline(), at));
    ASSERT_TRUE(w.undo());
    EXPECT_EQ(w.clips(), before->clips);
    ASSERT_TRUE(w.redo());
    EXPECT_EQ(w.clips(), preview.clips);
}

TEST(EditWorkspace, SplitInsideAnyLinkedFadeRejectsAtomicallyAndAllowsItsBoundary) {
    for (const bool fade_in : {false, true}) {
        Workspace w;
        ASSERT_TRUE(w.insert(source(w)));
        const auto lead = w.selection();
        const auto faded = w.clips().front().id == lead ? w.clips().back().id : w.clips().front().id;
        ASSERT_EQ(w.apply(SetFade{faded, fade_in ? FadeEdge::In : FadeEdge::Out, {1'000'000, {}}}, w.revision()).status,
                  CommandStatus::Applied);
        const auto before = w.snapshot();
        const Split invalid{{lead}, fade_in ? 500'000 : 9'500'000};
        const auto preview = PreviewEdit(*before, invalid);
        EXPECT_FALSE(preview.valid);
        const auto rejected = w.apply(invalid, before->revision);
        ASSERT_EQ(rejected.status, CommandStatus::Rejected);
        ASSERT_TRUE(rejected.error);
        EXPECT_EQ(rejected.error->code, EditErrorCode::TransitionBoundaryConflict);
        EXPECT_EQ(w.revision(), before->revision);
        EXPECT_EQ(w.clips(), before->clips);
        ASSERT_TRUE(w.undo());
        EXPECT_EQ(w.clip(faded)->fade_in.duration_us, 0);
        EXPECT_EQ(w.clip(faded)->fade_out.duration_us, 0);
        ASSERT_TRUE(w.redo());
        const Split boundary{{lead}, fade_in ? 1'000'000 : 9'000'000};
        ASSERT_TRUE(PreviewEdit(*w.snapshot(), boundary).valid);
        EXPECT_EQ(w.apply(boundary, w.revision()).status, CommandStatus::Applied);
    }
}

TEST(EditWorkspace, MovingAnOverlapEndpointToANewLayerDropsOnlyItsOldJunctions) {
    Workspace w;
    const auto asset = source(w);
    ASSERT_TRUE(w.insert(asset));
    ASSERT_TRUE(w.insert(asset, 8'000'000));
    const auto incoming = w.selection();
    ASSERT_EQ(w.transitions().size(), 2u);
    Move move{{incoming}, 0};
    move.expand_linked = false;
    move.new_tracks.push_back({999, TrackType::Video});
    move.target_tracks.push_back({incoming, 999});
    move.new_video_front = true;
    const auto before = w.snapshot();
    const auto preview = PreviewEdit(*before, move);
    ASSERT_TRUE(preview.valid);
    EXPECT_EQ(preview.junctions.size(), 1u);
    ASSERT_EQ(w.apply(move, before->revision).status, CommandStatus::Applied);
    EXPECT_EQ(w.clip(incoming)->track, w.tracks().front().id);
    ASSERT_TRUE(w.undo());
    EXPECT_EQ(w.clips(), before->clips);
    EXPECT_EQ(w.transitions(), before->junctions);
}

TEST(EditWorkspace, InsertOrdersLinkedTracksAndRejectsEqualStartOverlap) {
    Workspace w;
    const auto asset = source(w);
    ASSERT_TRUE(w.insert(asset, 10000000));
    ASSERT_TRUE(w.insert(asset, 0));
    EXPECT_EQ(w.clips().size(), 4u);
    EXPECT_EQ(w.clips()[0].start, 0);
    EXPECT_EQ(w.clips()[1].start, 10000000);
    EXPECT_FALSE(w.insert(asset, 0));
    EXPECT_EQ(w.clips().size(), 4u);
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
    EXPECT_FALSE(w.move(id, 5000000));
    ASSERT_TRUE(w.move(id, 3000000));
    EXPECT_EQ(w.transitions().size(), 2u);
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
    ASSERT_TRUE(workspace.insert(first, 10000000));
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

TEST(EditWorkspace, CrossfadeMovesLinkedSuffixAndRemovalBecomesHardCutInPlace) {
    Workspace w;
    ASSERT_TRUE(w.insert(source(w, L"a.mkv")));
    const auto a = w.selection();
    ASSERT_TRUE(w.insert(source(w, L"b.mkv")));
    const auto b = w.selection();
    ASSERT_TRUE(w.insert(source(w, L"c.mkv")));
    ASSERT_TRUE(w.crossfade(a, b));
    EXPECT_EQ(w.clip(a)->start, 0);
    EXPECT_EQ(w.clip(b)->start, 9'500'000);
    EXPECT_EQ(w.duration(), 29'500'000);
    for (const auto& c : w.clips())
        if (c.group == w.clip(b)->group)
            EXPECT_EQ(c.start, 9'500'000);
    EXPECT_FALSE(w.insert(source(w, L"unrelated.mkv"), 9'600'000));
    const auto faded = w.clips();
    const auto videoJunction = [&] {
        return *std::find_if(w.transitions().begin(), w.transitions().end(),
                             [&](const auto& junction) { return junction.outgoing == a; });
    };
    ASSERT_TRUE(w.removeCrossfade(a));
    EXPECT_EQ(w.clips(), faded);
    EXPECT_EQ(w.transitions().size(), 2u);
    EXPECT_EQ(videoJunction().kind, TransitionKind::HardCut);
    EXPECT_GE(videoJunction().cut_us, 9'500'000);
    EXPECT_LE(videoJunction().cut_us, 10'000'000);
    ASSERT_TRUE(w.undo());
    EXPECT_EQ(videoJunction().kind, TransitionKind::Crossfade);
    ASSERT_TRUE(w.redo());
    EXPECT_EQ(videoJunction().kind, TransitionKind::HardCut);
    ASSERT_TRUE(w.crossfade(a, b));
    EXPECT_EQ(videoJunction().kind, TransitionKind::Crossfade);
    EXPECT_EQ(videoJunction().anchor, JunctionAnchor::FullOverlap);
    EXPECT_EQ(w.clips(), faded);
    ASSERT_TRUE(w.undo());
    EXPECT_EQ(videoJunction().kind, TransitionKind::HardCut);
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
        if (at <= 9'000'000 || at >= 10'000'000) {
            ASSERT_EQ(value.video.size(), 1u);
            EXPECT_EQ(value.video[0].clip, at <= 9'000'000 ? a : b);
            EXPECT_EQ(value.video[0].source_us, at <= 9'000'000 ? at : at - 9'000'000);
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

TEST(EditWorkspace, DeleteKeepsSurvivingGeometryAndRippleRejectsCrossingMedia) {
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
        if (ripple) {
            EXPECT_FALSE(w.remove(b, true));
            EXPECT_EQ(w.clips(), before);
            EXPECT_EQ(w.transitions().size(), 4u);
        } else {
            ASSERT_TRUE(w.remove(b, false));
            EXPECT_TRUE(w.transitions().empty());
            EXPECT_EQ(w.clip(c)->start, 19'000'000);
            ASSERT_TRUE(w.undo());
            EXPECT_EQ(w.clips(), before);
            EXPECT_EQ(w.transitions().size(), 4u);
        }
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

TEST(EditTimelineEvaluation, TrimmedIncomingSourceAndMissingAudioKeepOutgoingAudioUnfaded) {
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
    EXPECT_EQ(evaluation.audio[0].weight, 1);
    EXPECT_TRUE(exosnap::engine::EvaluateTimeline(w.timeline(), w.duration()).gap);
}

namespace {
constexpr Time Second = 1'000'000;

Id ContractSource(Workspace& workspace, const wchar_t* path = L"source.mkv", Time duration = 10 * Second) {
    Asset asset;
    asset.path = path;
    asset.duration = duration;
    asset.fps_num = 30;
    asset.fps_den = 1;
    asset.width = 1920;
    asset.height = 1080;
    return workspace.addAsset(std::move(asset));
}

void ExpectSameEdit(const EditSnapshot& actual, const EditSnapshot& expected) {
    EXPECT_EQ(actual.revision, expected.revision);
    EXPECT_EQ(actual.tracks, expected.tracks);
    EXPECT_EQ(actual.clips, expected.clips);
    EXPECT_EQ(actual.junctions, expected.junctions);
    EXPECT_EQ(actual.result_range, expected.result_range);
    EXPECT_EQ(actual.output_fps, expected.output_fps);
}
} // namespace

TEST(EditWorkspaceContracts, RippleConflictIsAtomic) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace), 10 * Second));
    const auto selected = workspace.selection();
    const auto background = ContractSource(workspace, L"background.mkv", 30 * Second);
    const auto audio_track = workspace.addTrack(TrackType::Audio);
    ASSERT_EQ(workspace.apply(AddAssetGroup{background, 0, 0, {audio_track}, {}, false}, workspace.revision()).status,
              CommandStatus::Applied);
    const auto before = workspace.snapshot();
    const auto history = workspace.historyBytes();
    const auto result = workspace.apply(RippleRemoveClips{{selected}, true}, workspace.revision());
    ASSERT_EQ(result.status, CommandStatus::Rejected);
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->code, EditErrorCode::RippleConflict);
    ExpectSameEdit(*workspace.snapshot(), *before);
    EXPECT_EQ(workspace.historyBytes(), history);
    ASSERT_TRUE(workspace.undo());
    EXPECT_EQ(workspace.clips().size(), 2u);
}

TEST(EditWorkspaceContracts, DisjointRippleUsesUnion) {
    Workspace workspace;
    const auto source = ContractSource(workspace);
    ASSERT_TRUE(workspace.insert(source, 10 * Second));
    const auto first = workspace.selection();
    ASSERT_TRUE(workspace.insert(source, 40 * Second));
    const auto second = workspace.selection();
    ASSERT_TRUE(workspace.insert(source, 60 * Second));
    const auto retained = workspace.selection();
    const auto before = workspace.clips();
    const auto result = workspace.apply(RippleRemoveClips{{first, second}, true}, workspace.revision());
    ASSERT_EQ(result.status, CommandStatus::Applied);
    ASSERT_NE(workspace.clip(retained), nullptr);
    EXPECT_EQ(workspace.clip(retained)->start, 40 * Second);
    EXPECT_EQ(workspace.clip(retained)->source_in, 0);
    ASSERT_TRUE(workspace.undo());
    EXPECT_EQ(workspace.clips(), before);
    ASSERT_TRUE(workspace.redo());
    EXPECT_EQ(workspace.clip(retained)->start, 40 * Second);
}

TEST(EditWorkspaceContracts, ResultRangeMapsThroughRipple) {
    Workspace workspace;
    const auto source = ContractSource(workspace);
    ASSERT_TRUE(workspace.insert(source, 10 * Second));
    const auto first = workspace.selection();
    ASSERT_TRUE(workspace.insert(source, 40 * Second));
    const auto second = workspace.selection();
    ASSERT_TRUE(workspace.insert(source, 90 * Second));
    const ResultRange original{5 * Second, ResultOutMode::Fixed, 90 * Second};
    ASSERT_EQ(workspace.apply(SetResultRange{original}, workspace.revision()).status, CommandStatus::Applied);
    ASSERT_EQ(workspace.apply(RippleRemoveClips{{first, second}, true}, workspace.revision()).status,
              CommandStatus::Applied);
    EXPECT_EQ(workspace.snapshot()->result_range, (ResultRange{5 * Second, ResultOutMode::Fixed, 70 * Second}));
    ASSERT_TRUE(workspace.undo());
    EXPECT_EQ(workspace.snapshot()->result_range, original);
}

TEST(EditWorkspaceContracts, GroupDropUndo) {
    Workspace workspace;
    Asset asset;
    asset.path = L"four-streams.mkv";
    asset.duration = 10 * Second;
    asset.width = 1920;
    asset.height = 1080;
    asset.fps_num = 30;
    auto descriptor = std::make_shared<ValidatedSourceDescriptor>();
    descriptor->streams.push_back(
        {{0, exosnap::engine::AudioTrackId{{1}}, 4, exosnap::engine::TimelineStreamKind::Video},
         {0, asset.duration},
         0,
         {},
         {30, 1}});
    InputMixSelection mix;
    mix.authority = InputMixAuthority::VerifiedRecordedDefault;
    for (uint32_t index = 0; index < 4; ++index) {
        const exosnap::engine::AudioTrackId identity{{static_cast<uint8_t>(index + 2)}};
        descriptor->streams.push_back({{0, identity, index, exosnap::engine::TimelineStreamKind::Audio},
                                       {0, asset.duration},
                                       0,
                                       {"A_PCM/INT/LIT", {}, 48000, 2, 16, 0, "stereo"},
                                       {}});
        if (index == 0)
            mix.selected_stream_identities.push_back(identity);
    }
    asset.source_descriptor = std::move(descriptor);
    const auto source = workspace.addAsset(std::move(asset));
    const auto before = workspace.snapshot();
    const EditCommand command = AddAssetGroup{source, 0, 0, {}, mix, true};
    const auto preview = PreviewEdit(*before, command);
    ASSERT_TRUE(preview.valid);
    ExpectSameEdit(*workspace.snapshot(), *before);
    ASSERT_EQ(workspace.apply(command, before->revision).status, CommandStatus::Applied);
    ASSERT_EQ(workspace.clips().size(), 5u);
    ASSERT_EQ(workspace.tracks().size(), 5u);
    const auto after = workspace.snapshot();
    const auto group = after->clips.front().group;
    size_t disabled_audio = 0;
    for (const auto& clip : after->clips) {
        ASSERT_TRUE(clip.stream);
        EXPECT_EQ(clip.stream->asset_id, source);
        EXPECT_EQ(clip.group, group);
        EXPECT_EQ(clip.start, 0);
        if (clip.stream->kind == exosnap::engine::TimelineStreamKind::Audio && !clip.enabled)
            ++disabled_audio;
    }
    EXPECT_EQ(disabled_audio, 3u);
    ASSERT_TRUE(workspace.undo());
    EXPECT_EQ(workspace.tracks(), before->tracks);
    EXPECT_EQ(workspace.clips(), before->clips);
    ASSERT_TRUE(workspace.redo());
    EXPECT_EQ(workspace.tracks(), after->tracks);
    EXPECT_EQ(workspace.clips(), after->clips);
}

TEST(EditWorkspaceContracts, RationalStep) {
    const RationalFps fps{60000, 1001};
    ASSERT_TRUE(fps.frameTime(60000));
    EXPECT_EQ(*fps.frameTime(60000), 1'001'000'000);
    for (int64_t frame = 0; frame < 10000; ++frame) {
        const auto at = fps.frameTime(frame);
        const auto after = fps.frameTime(frame + 1);
        ASSERT_TRUE(at);
        ASSERT_TRUE(after);
        EXPECT_EQ(fps.nextFrame(*at), after);
        EXPECT_EQ(fps.previousFrame(*after), at);
    }
    EXPECT_FALSE((RationalFps{0, 1}).frameTime(0));
    EXPECT_FALSE(fps.frameTime((std::numeric_limits<int64_t>::max)()));
}

TEST(EditRenderSnapshot, ProjectionPreservesAuthoredStateAndRetainsItsRevision) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    const auto first = workspace.selection();
    ASSERT_TRUE(workspace.insert(ContractSource(workspace, L"second.mkv")));
    const auto second = workspace.selection();
    ASSERT_TRUE(workspace.crossfade(first, second, Second));
    ASSERT_EQ(workspace.apply(SetTimelineEnd{{TimelineEndMode::FixedEnd, 25 * Second}}, workspace.revision()).status,
              CommandStatus::Applied);
    const auto snapshot = workspace.snapshot();
    const auto expected = workspace.timeline();
    const auto rendered = RenderTimeline(*snapshot);
    EXPECT_EQ(rendered.clips, expected.clips);
    EXPECT_EQ(rendered.tracks, expected.tracks);
    EXPECT_EQ(rendered.junctions, expected.junctions);
    EXPECT_EQ(rendered.crossfades.size(), expected.crossfades.size());
    ASSERT_FALSE(rendered.crossfades.empty());
    EXPECT_EQ(rendered.crossfades.front().outgoing, first);
    EXPECT_EQ(rendered.crossfades.front().incoming, second);
    EXPECT_EQ(rendered.duration_us, 25 * Second);
    EXPECT_EQ(rendered.result_out_us, 25 * Second);
    EXPECT_EQ(rendered.output_fps_num, 30u);
    ASSERT_TRUE(workspace.remove(first, false));
    EXPECT_NE(workspace.revision(), rendered.revision);
    EXPECT_EQ(RenderTimeline(*snapshot).clips, expected.clips);
    EXPECT_EQ(SnapshotDuration(*snapshot), 25 * Second);
}

TEST(EditWorkspaceGrid, MoveQuantizesTheAnchorAndPreservesALegacySourceWindow) {
    Workspace workspace;
    Asset asset;
    asset.path = L"legacy-ntsc.mkv";
    asset.duration = 100'001;
    asset.fps_num = 30000;
    asset.fps_den = 1001;
    ASSERT_TRUE(workspace.insert(workspace.addAsset(asset), 123));
    const auto id = workspace.selection();
    const auto before = workspace.snapshot();
    const Move command{{id}, 40'000};
    const auto preview = PreviewEdit(*before, command);
    ASSERT_TRUE(preview.valid);
    ASSERT_EQ(workspace.apply(command, before->revision).status, CommandStatus::Applied);
    EXPECT_EQ(workspace.clip(id)->start, 33'366);
    EXPECT_EQ(workspace.clip(id)->source_in, 0);
    EXPECT_EQ(workspace.clip(id)->source_out, 100'001);
    EXPECT_EQ(preview.clips, workspace.clips());
    ASSERT_TRUE(workspace.undo());
    EXPECT_EQ(workspace.clips(), before->clips);
}

TEST(EditWorkspaceGrid, TrimSplitAndFadeUseCanonicalOutputFrames) {
    Workspace workspace;
    Asset asset;
    asset.path = L"ntsc.mkv";
    asset.duration = 1'001'000;
    asset.fps_num = 30000;
    asset.fps_den = 1001;
    ASSERT_TRUE(workspace.insert(workspace.addAsset(asset)));
    const auto id = workspace.selection();
    ASSERT_EQ(workspace.apply(Trim{id, TrimEdge::Right, -960'000}, workspace.revision()).status,
              CommandStatus::Applied);
    EXPECT_EQ(workspace.clip(id)->end(), 33'366);
    ASSERT_TRUE(workspace.undo());
    ASSERT_EQ(workspace.apply(SetFade{id, FadeEdge::In, {40'000}}, workspace.revision()).status,
              CommandStatus::Applied);
    EXPECT_EQ(workspace.clip(id)->fade_in.duration_us, 33'366);
    ASSERT_TRUE(workspace.undo());
    ASSERT_EQ(workspace.apply(Split{{id}, 40'000, true}, workspace.revision()).status, CommandStatus::Applied);
    EXPECT_EQ(workspace.clip(id)->end(), 33'366);
    EXPECT_TRUE(std::any_of(workspace.clips().begin(), workspace.clips().end(),
                            [](const auto& clip) { return clip.start == 33'366; }));
}

TEST(EditWorkspaceGrid, AlignedMovePreservesFrameSpanAcrossUnequalNtscIntervals) {
    Workspace workspace;
    Asset asset;
    asset.path = L"ntsc-span.mkv";
    asset.duration = 1'001'000;
    asset.fps_num = 30000;
    asset.fps_den = 1001;
    ASSERT_TRUE(workspace.insert(workspace.addAsset(asset)));
    const auto id = workspace.selection();
    ASSERT_TRUE(workspace.trim(id, 0, 33'366));
    ASSERT_EQ(workspace.apply(Move{{id}, 33'366}, workspace.revision()).status, CommandStatus::Applied);
    EXPECT_EQ(workspace.clip(id)->start, 33'366);
    EXPECT_EQ(workspace.clip(id)->end(), 66'733);
    ASSERT_TRUE(workspace.undo());
    EXPECT_EQ(workspace.clip(id)->start, 0);
    EXPECT_EQ(workspace.clip(id)->source_out, 33'366);
}

TEST(EditWorkspaceGrid, FadeClampKeepsTheEditedEndpointOnGridAtALegacyEdge) {
    Workspace workspace;
    Asset asset;
    asset.path = L"legacy-fade.mkv";
    asset.duration = 100'001;
    asset.fps_num = 30000;
    asset.fps_den = 1001;
    ASSERT_TRUE(workspace.insert(workspace.addAsset(asset)));
    const auto id = workspace.selection();
    const auto before = workspace.snapshot();
    const SetFade command{id, FadeEdge::In, {99'999}};
    const auto candidate = PreviewEdit(*before, command);
    ASSERT_TRUE(candidate.valid);
    ASSERT_EQ(workspace.apply(command, workspace.revision()).status, CommandStatus::Applied);
    EXPECT_EQ(workspace.clip(id)->fade_in.duration_us, 66'733);
    EXPECT_EQ(workspace.clip(id)->source_out, 100'001);
    EXPECT_EQ(candidate.clips, workspace.clips());
    ASSERT_TRUE(workspace.undo());
    EXPECT_EQ(workspace.clips(), before->clips);
}

TEST(EditWorkspaceGrid, AudioOnlyEditsUseOutputSamplesForEverySourceRate) {
    for (const uint32_t source_rate : {44100u, 48000u, 96000u}) {
        SCOPED_TRACE(source_rate);
        Workspace workspace;
        Asset asset;
        asset.path = L"source-rate.wav";
        asset.duration = 1'000'000;
        auto descriptor = std::make_shared<ValidatedSourceDescriptor>();
        SourceStreamDescriptor stream;
        stream.ref = {0, exosnap::engine::AudioTrackId{{1}}, 0, exosnap::engine::TimelineStreamKind::Audio};
        stream.source_span = {0, asset.duration};
        stream.codec.sample_rate = source_rate;
        stream.codec.channels = 2;
        descriptor->streams.push_back(stream);
        asset.source_descriptor = descriptor;
        ASSERT_TRUE(workspace.insert(workspace.addAsset(asset)));
        const auto id = workspace.selection();
        ASSERT_EQ(workspace.apply(Move{{id}, 41}, workspace.revision()).status, CommandStatus::Applied);
        EXPECT_EQ(workspace.clip(id)->start, 42);
        ASSERT_TRUE(workspace.undo());
        ASSERT_EQ(workspace.apply(Trim{id, TrimEdge::Right, -999'959}, workspace.revision()).status,
                  CommandStatus::Applied);
        EXPECT_EQ(workspace.clip(id)->end(), 42);
        ASSERT_TRUE(workspace.undo());
        ASSERT_EQ(workspace.apply(SetFade{id, FadeEdge::In, {41}}, workspace.revision()).status,
                  CommandStatus::Applied);
        EXPECT_EQ(workspace.clip(id)->fade_in.duration_us, 42);
        ASSERT_TRUE(workspace.undo());
        ASSERT_EQ(workspace.apply(Split{{id}, 41, false}, workspace.revision()).status, CommandStatus::Applied);
        EXPECT_EQ(workspace.clip(id)->end(), 42);
    }
}

TEST(EditTimeGrid, CanonicalNtscInverseNearestAndOverflow) {
    const RationalFps ntsc{30000, 1001};
    EXPECT_EQ(ntsc.frameTime(1), 33'366);
    EXPECT_EQ(ntsc.frameTime(2), 66'733);
    EXPECT_EQ(ntsc.frameIndex(33'366), 1);
    EXPECT_EQ(ntsc.frameIndex(66'732), 1);
    EXPECT_EQ(ntsc.nearestFrame(16'683), 0);
    EXPECT_EQ(ntsc.nearestFrame(16'684), 33'366);
    EXPECT_EQ(ntsc.step(33'366, 1), 66'733);
    EXPECT_EQ(ntsc.step(66'733, -1), 33'366);
    EXPECT_FALSE(ntsc.step(0, -1));
    EXPECT_FALSE(ntsc.step(0, (std::numeric_limits<int64_t>::min)()));
    EXPECT_FALSE(ntsc.step(33'366, (std::numeric_limits<int64_t>::max)()));
    EXPECT_FALSE(ntsc.frameIndex(-1));
    EXPECT_FALSE((RationalFps{0, 1}).nearestFrame(0));
    EXPECT_FALSE((RationalFps{30, 0}).frameIndex(0));
    for (const auto fps : {RationalFps{24, 1}, RationalFps{60, 1}, RationalFps{24000, 1001}, RationalFps{30000, 1001},
                           RationalFps{60000, 1001}})
        for (const int64_t index : {0LL, 1LL, 2LL, 100LL, 1'000'000LL, 864'000'001LL}) {
            const auto boundary = fps.frameTime(index);
            ASSERT_TRUE(boundary);
            EXPECT_EQ(fps.frameIndex(*boundary), index);
            EXPECT_EQ(fps.nearestFrame(*boundary), boundary);
        }
}

TEST(EditTimeGrid, CanonicalOutputSampleBoundariesRoundTripAndTieEarlier) {
    EXPECT_EQ(AudioSampleGrid::sampleTime(1), 21);
    EXPECT_EQ(AudioSampleGrid::sampleTime(2), 42);
    EXPECT_EQ(AudioSampleGrid::sampleTime(3), 63);
    EXPECT_EQ(AudioSampleGrid::sampleTime(4), 83);
    EXPECT_EQ(AudioSampleGrid::sampleIndex(62), 2);
    EXPECT_EQ(AudioSampleGrid::sampleIndex(63), 3);
    EXPECT_EQ(AudioSampleGrid::nearestSample(73), 63);
    EXPECT_EQ(AudioSampleGrid::nearestSample(74), 83);
    EXPECT_EQ(AudioSampleGrid::step(63, 1), 83);
    EXPECT_EQ(AudioSampleGrid::step(83, -1), 63);
    EXPECT_FALSE(AudioSampleGrid::sampleTime(-1));
    EXPECT_FALSE(AudioSampleGrid::sampleTime((std::numeric_limits<int64_t>::max)()));
    EXPECT_FALSE(AudioSampleGrid::step(0, -1));
    EXPECT_FALSE(AudioSampleGrid::step(21, (std::numeric_limits<int64_t>::max)()));
    for (const int64_t index : {0LL, 1LL, 2LL, 3LL, 4LL, 47'999LL, 48'000LL, 691'200'001LL}) {
        const auto boundary = AudioSampleGrid::sampleTime(index);
        ASSERT_TRUE(boundary);
        EXPECT_EQ(AudioSampleGrid::sampleIndex(*boundary), index);
        EXPECT_EQ(AudioSampleGrid::nearestSample(*boundary), boundary);
    }
}

TEST(EditTimeParser, ExactUnitsLocaleRelativeAndNondropTimecode) {
    const TimeFieldContext context{{30000, 1001}, 33'366, TimeFieldFormat::Seconds};
    for (const auto& [text, expected] : std::vector<std::pair<std::string, Time>>{{"0.000001s", 1},
                                                                                  {"1,234567s", 1'234'567},
                                                                                  {"1.234567", 1'234'567},
                                                                                  {"120ms", 120'000},
                                                                                  {"1.125ms", 1'125},
                                                                                  {"2f", 66'733},
                                                                                  {"3smp", 63},
                                                                                  {"+1f", 66'733},
                                                                                  {"-1f", 0},
                                                                                  {"+480smp", 43'375},
                                                                                  {"-20ms", 13'366},
                                                                                  {"00:00:01:00", 1'001'000},
                                                                                  {"00:00:00:01", 33'366}}) {
        SCOPED_TRACE(text);
        const auto parsed = ParseTime(text, context);
        ASSERT_TRUE(parsed);
        EXPECT_EQ(parsed.value_us, expected);
    }
    EXPECT_EQ(FormatTime(1'234'567, context.fps, TimeFieldFormat::Seconds), "1.234567");
    EXPECT_EQ(FormatTime(1'000'000, context.fps, TimeFieldFormat::Seconds), "1");
    EXPECT_EQ(FormatTime(1'001'000, context.fps, TimeFieldFormat::Timecode), "00:00:01:00");
    EXPECT_EQ(FormatTime(63, context.fps, TimeFieldFormat::Samples), "3");
    EXPECT_EQ(ParseTime("3", {context.fps, 0, TimeFieldFormat::Samples}).value_us, 63);
    const auto last = FormatTime((std::numeric_limits<Time>::max)(), context.fps, TimeFieldFormat::Seconds);
    ASSERT_TRUE(last);
    const auto parsed_last = ParseTime(*last, context);
    ASSERT_TRUE(parsed_last);
    EXPECT_EQ(parsed_last.value_us, (std::numeric_limits<Time>::max)());
}

TEST(EditTimeParser, ClockRoundsOnlyPresentationAndParsesWithoutVideoRate) {
    const TimeFieldContext context{{0, 1}, 1'234'567, TimeFieldFormat::Clock};
    EXPECT_EQ(FormatTime(0, context.fps, context.format), "00:00.000");
    EXPECT_EQ(FormatTime(1'234'567, context.fps, context.format), "00:01.235");
    EXPECT_EQ(FormatTime(59'999'499, context.fps, context.format), "00:59.999");
    EXPECT_EQ(FormatTime(59'999'500, context.fps, context.format), "01:00.000");
    EXPECT_EQ(FormatTime(3'599'999'500, context.fps, context.format), "1:00:00.000");
    EXPECT_EQ(FormatTime(3'661'234'567, context.fps, context.format), "1:01:01.235");
    for (const auto& [text, expected] : std::vector<std::pair<std::string, Time>>{
             {"01:02.345", 62'345'000}, {"1:02:03.456", 3'723'456'000}, {"00:00.000001", 1}, {"+1s", 2'234'567}}) {
        SCOPED_TRACE(text);
        const auto parsed = ParseTime(text, context);
        ASSERT_TRUE(parsed);
        EXPECT_EQ(parsed.value_us, expected);
    }
    for (const auto text : {"00:60.000", "1:60:00.000", ":01.000", "00:01f", "00:01s", "00:+1", "00:00.0000001",
                            "9223372036854775807:00:00.000", "+00:01.000"}) {
        SCOPED_TRACE(text);
        EXPECT_FALSE(ParseTime(text, context));
    }
    EXPECT_TRUE(FormatTime((std::numeric_limits<Time>::max)(), context.fps, context.format));
}

TEST(EditTimeParser, RejectsInvalidFramesSyntaxPrecisionAndOverflow) {
    const TimeFieldContext context{{30000, 1001}, 0, TimeFieldFormat::Seconds};
    for (const auto text : {"", "00:00:00:30", "00:00:60:00", "00:00:00;01", "1,2.3s", "1e3s", "1.5f", "0.0000001s",
                            "-1s", "9223372036854775808s", "1 000s"}) {
        SCOPED_TRACE(text);
        EXPECT_FALSE(ParseTime(text, context));
    }
    EXPECT_FALSE(ParseTime("1f", {{0, 1}, 0, TimeFieldFormat::Seconds}));
    EXPECT_FALSE(ParseTime("+1s", {context.fps, (std::numeric_limits<Time>::max)(), TimeFieldFormat::Seconds}));
    EXPECT_FALSE(FormatTime(-1, context.fps, TimeFieldFormat::Seconds));
    EXPECT_FALSE(FormatTime(0, {0, 1}, TimeFieldFormat::Timecode));
}

TEST(EditSelectionContracts, StableShiftSelectionIncludesOffscreenClipsAndLinkedGroups) {
    Workspace workspace;
    const auto source = ContractSource(workspace);
    ASSERT_TRUE(workspace.insert(source));
    const auto first = workspace.selection();
    ASSERT_TRUE(workspace.insert(source));
    const auto second = workspace.selection();
    auto snapshot = *workspace.snapshot();
    std::reverse(snapshot.clips.begin(), snapshot.clips.end());
    EditSelection selection;
    ASSERT_TRUE(selection.selectClip(snapshot, first, false, false, false));
    ASSERT_TRUE(selection.selectClip(snapshot, second, false, true, false));
    EXPECT_EQ(selection.clip_ids, (std::vector<Id>{first, second}));
    ASSERT_TRUE(selection.selectClip(snapshot, first, true, false, true));
    EXPECT_EQ(selection.clip_ids.size(), 3u);
    ASSERT_TRUE(selection.selectClip(snapshot, first, true, false, true));
    EXPECT_EQ(selection.clip_ids, (std::vector<Id>{second}));
}

TEST(EditSelectionContracts, MarqueeSelectsTouchedClipsAddsToBaseAndWidensLinksOnlyWhenAsked) {
    Workspace workspace;
    const auto source = ContractSource(workspace);
    ASSERT_TRUE(workspace.insert(source));
    const auto first = workspace.selection();
    ASSERT_TRUE(workspace.insert(source));
    const auto second = workspace.selection();
    const auto snapshot = *workspace.snapshot();
    const auto* first_clip = workspace.clip(first);
    const auto* second_clip = workspace.clip(second);
    ASSERT_TRUE(first_clip && second_clip);
    EditSelection selection;
    selection.selectArea(snapshot, first_clip->start, first_clip->end(), {first_clip->track}, {}, false);
    EXPECT_EQ(selection.clip_ids, (std::vector<Id>{first}));
    EXPECT_EQ(selection.primary_clip, first);
    selection.selectArea(snapshot, first_clip->start, first_clip->end(), {first_clip->track}, {}, true);
    EXPECT_EQ(selection.clip_ids.size(), 2u);
    selection.selectArea(snapshot, second_clip->start, second_clip->end(), {second_clip->track}, {first}, false);
    EXPECT_EQ(selection.clip_ids, (std::vector<Id>{first, second}));
    EXPECT_EQ(selection.primary_clip, second);
    selection.selectArea(snapshot, 0, 0, {first_clip->track}, {}, false);
    EXPECT_EQ(selection.mode, SelectionMode::None);
    EXPECT_TRUE(selection.clip_ids.empty());
}

TEST(EditSelectionContracts, HeaderSelectionKeepsAMultiSelectionAndClearsOnlyTheEditRange) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    const auto first = workspace.selection();
    ASSERT_TRUE(workspace.insert(ContractSource(workspace, L"other.mkv")));
    const auto second = workspace.selection();
    EditSelection selection;
    selection.active_track = workspace.tracks().front().id;
    ASSERT_TRUE(selection.selectClip(*workspace.snapshot(), first, false, false, false));
    ASSERT_TRUE(selection.selectClip(*workspace.snapshot(), second, true, false, false));
    ASSERT_TRUE(selection.selectClip(*workspace.snapshot(), first, false, false, false));
    EXPECT_EQ(selection.clip_ids, (std::vector<Id>{first, second}));
    selection.setRange({Second, 2 * Second, {workspace.tracks().front().id}, Second});
    ASSERT_TRUE(selection.range);
    ASSERT_TRUE(selection.selectClip(*workspace.snapshot(), first, false, false, false));
    EXPECT_EQ(selection.mode, SelectionMode::Clips);
    EXPECT_FALSE(selection.range);
    EXPECT_EQ(selection.active_track, workspace.tracks().front().id);
}

TEST(EditWorkspaceLocks, LinkedAndAuthoredTrackEditsRefuseTheWholeTransaction) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    const auto selected = workspace.selection();
    const auto audio_track = workspace.tracks().back().id;
    const auto revision = workspace.revision();
    const auto history = workspace.historyBytes();
    ASSERT_TRUE(workspace.setTrackLocked(audio_track, true));
    EXPECT_EQ(workspace.revision(), revision);
    EXPECT_EQ(workspace.historyBytes(), history);
    const auto before = workspace.snapshot();
    const Move command{{selected}, Second};
    EXPECT_FALSE(PreviewEdit(*before, command).valid);
    const auto result = workspace.apply(command, revision);
    ASSERT_EQ(result.status, CommandStatus::Rejected);
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->code, EditErrorCode::TrackLocked);
    EXPECT_EQ(workspace.clips(), before->clips);
    EXPECT_EQ(workspace.apply(SetTrackEnabled{audio_track, false}, revision).status, CommandStatus::Rejected);
    EXPECT_EQ(workspace.historyBytes(), history);
    ASSERT_TRUE(workspace.setTrackLocked(audio_track, false));
    ASSERT_EQ(workspace.apply(command, revision).status, CommandStatus::Applied);
    ASSERT_TRUE(workspace.setTrackLocked(audio_track, true));
    ASSERT_TRUE(workspace.undo());
    EXPECT_EQ(workspace.clips(), before->clips);
}

TEST(EditWorkspaceLinks, IndependentStructuralEditDetachesOnlyItsAffectedMembershipAndUndoRestoresIt) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    const auto selected = workspace.selection();
    const auto before = workspace.snapshot();
    Move command{{selected}, Second};
    command.expand_linked = false;
    ASSERT_EQ(workspace.apply(command, workspace.revision()).status, CommandStatus::Applied);
    EXPECT_EQ(workspace.clip(selected)->group, 0);
    const auto other = std::find_if(workspace.clips().begin(), workspace.clips().end(),
                                    [&](const auto& clip) { return clip.id != selected; });
    ASSERT_NE(other, workspace.clips().end());
    EXPECT_EQ(other->start, 0);
    ASSERT_TRUE(workspace.undo());
    EXPECT_EQ(workspace.clips(), before->clips);
}

TEST(EditWorkspaceRange, ScopeRefusesImplicitLinkExpansion) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    const auto before = workspace.snapshot();
    const auto video = workspace.tracks().front().id;
    const auto refused = workspace.apply(LiftRange{{Second, 2 * Second}, {video}}, workspace.revision());
    ASSERT_EQ(refused.status, CommandStatus::Rejected);
    ASSERT_TRUE(refused.error);
    EXPECT_EQ(refused.error->code, EditErrorCode::LinkedScopeRequired);
    EXPECT_EQ(workspace.clips(), before->clips);
}

TEST(EditWorkspaceRange, SplitUsesBothBoundsAndIsOneUndoTransaction) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    const auto before = workspace.snapshot();
    const auto video = workspace.tracks().front().id;
    const auto audio = workspace.tracks().back().id;
    ASSERT_EQ(workspace.apply(SplitRange{{2 * Second, 4 * Second}, {video, audio}}, workspace.revision()).status,
              CommandStatus::Applied);
    EXPECT_EQ(workspace.clips().size(), 6u);
    for (const auto& clip : workspace.clips())
        if (clip.start == 2 * Second)
            EXPECT_EQ(clip.end(), 4 * Second);
    ASSERT_TRUE(workspace.undo());
    EXPECT_EQ(workspace.clips(), before->clips);
}

TEST(EditWorkspaceGrid, JunctionWindowsUseTheirTrackGridAndCutsStayOnGridAtLegacyEdges) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    const auto first = workspace.selection();
    ASSERT_TRUE(workspace.insert(ContractSource(workspace, L"second.mkv")));
    const auto second = workspace.selection();
    ASSERT_TRUE(workspace.crossfade(first, second, Second));
    const auto junctions = workspace.transitions();
    ASSERT_EQ(junctions.size(), 2u);
    for (const auto& junction : junctions) {
        const bool audio = std::find_if(workspace.tracks().begin(), workspace.tracks().end(), [&](const auto& track) {
                               return track.id == junction.track;
                           })->type == TrackType::Audio;
        ASSERT_EQ(workspace.apply(SetJunctionWindow{junction.id, {9'234'567, 9'765'432}}, workspace.revision()).status,
                  CommandStatus::Applied);
        const auto found = std::find_if(workspace.transitions().begin(), workspace.transitions().end(),
                                        [&](const auto& value) { return value.id == junction.id; });
        ASSERT_NE(found, workspace.transitions().end());
        EXPECT_EQ(found->in_us, audio ? 9'234'563 : 9'233'333);
        EXPECT_EQ(found->out_us, audio ? 9'765'438 : 9'766'666);
    }
    auto legacy = *workspace.snapshot();
    const auto video = std::find_if(legacy.junctions.begin(), legacy.junctions.end(),
                                    [&](const auto& value) { return value.track == legacy.tracks.front().id; });
    ASSERT_NE(video, legacy.junctions.end());
    for (auto& clip : legacy.clips)
        if (clip.id == video->incoming)
            clip.start = 9'000'001;
    const auto preview = PreviewEdit(legacy, SetHardCut{video->id, 9'000'001});
    ASSERT_TRUE(preview.valid);
    const auto cut = std::find_if(preview.junctions.begin(), preview.junctions.end(),
                                  [&](const auto& value) { return value.id == video->id; });
    ASSERT_NE(cut, preview.junctions.end());
    EXPECT_EQ(cut->cut_us, 9'033'333);
}

TEST(EditTrackPolicy, ReorderCannotCrossMediaKindsAndNewVideoRowsStayInTheirGroup) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    const auto video = workspace.tracks().front().id;
    const auto audio = workspace.tracks().back().id;
    const auto revision = workspace.revision();
    EXPECT_EQ(workspace.apply(ReorderTrack{video, 1}, revision).status, CommandStatus::Rejected);
    EXPECT_EQ(workspace.revision(), revision);
    Move move{{workspace.clips().front().id}, 0};
    const auto temporary = workspace.snapshot()->id_high_watermark + 1;
    move.new_tracks = {{temporary, TrackType::Video}};
    move.target_tracks = {{move.targets.front(), temporary}};
    move.expand_linked = false;
    ASSERT_EQ(workspace.apply(move, workspace.revision()).status, CommandStatus::Applied);
    ASSERT_EQ(workspace.tracks().size(), 3u);
    EXPECT_EQ(workspace.tracks()[0].type, TrackType::Video);
    EXPECT_EQ(workspace.tracks()[1].type, TrackType::Video);
    EXPECT_EQ(workspace.tracks()[2].id, audio);
    EXPECT_EQ(workspace.apply(ReorderTrack{video, 1}, workspace.revision()).status, CommandStatus::Applied);
}

TEST(EditWorkspaceGrid, StrictNumericTrimRejectsARequestOutsideTheRepresentableMinimum) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    const auto selected = workspace.selection();
    const auto original = workspace.snapshot();
    Trim trim{selected, TrimEdge::Right, -10 * Second + 1};
    trim.clamp_to_bounds = false;
    EXPECT_EQ(workspace.apply(trim, workspace.revision()).status, CommandStatus::Rejected);
    EXPECT_EQ(workspace.clips(), original->clips);
    EXPECT_EQ(workspace.revision(), original->revision);
    trim = Trim{selected, TrimEdge::Left, -Second};
    trim.clamp_to_bounds = false;
    EXPECT_EQ(workspace.apply(trim, workspace.revision()).status, CommandStatus::Rejected);
    EXPECT_EQ(workspace.clips(), original->clips);
}

TEST(EditClipboard, CopyRetainsInternalJunctionsAndOnlyCopiedLinkMembership) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    const auto first = workspace.selection();
    ASSERT_TRUE(workspace.insert(ContractSource(workspace, L"second.mkv")));
    const auto second = workspace.selection();
    ASSERT_TRUE(workspace.crossfade(first, second, Second));
    std::vector<Id> all;
    for (const auto& clip : workspace.clips())
        all.push_back(clip.id);
    const auto copied = CopyClips(*workspace.snapshot(), all);
    ASSERT_TRUE(copied);
    EXPECT_EQ(copied->clips.size(), 4u);
    EXPECT_EQ(copied->junctions.size(), workspace.transitions().size());
    EXPECT_EQ(copied->assets.size(), 2u);
    EXPECT_EQ(copied->clips.front().start, 0);
    EXPECT_EQ(copied->duration_us, workspace.duration());
    const auto partial = CopyClips(*workspace.snapshot(), {first});
    ASSERT_TRUE(partial);
    ASSERT_EQ(partial->clips.size(), 1u);
    EXPECT_EQ(partial->clips.front().group, 0);
    EXPECT_TRUE(partial->junctions.empty());
    EXPECT_FALSE(CopyClips(*workspace.snapshot(), {}));
    EXPECT_FALSE(CopyClips(*workspace.snapshot(), {999'999}));
}

TEST(EditClipboard, PasteAllocatesFreshInternalIdsAndRestoresExactlyInOneUndo) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    const auto first = workspace.selection();
    ASSERT_TRUE(workspace.insert(ContractSource(workspace, L"second.mkv")));
    const auto second = workspace.selection();
    ASSERT_TRUE(workspace.crossfade(first, second, Second));
    const auto original = workspace.snapshot();
    std::vector<Id> ids;
    for (const auto& clip : original->clips)
        ids.push_back(clip.id);
    const auto copied = CopyClips(*original, ids);
    ASSERT_TRUE(copied);
    const auto at = 30 * Second;
    const PasteClips command{copied, at, workspace.tracks().front().id};
    const auto preview = PreviewEdit(*original, command);
    ASSERT_TRUE(preview.valid);
    ASSERT_EQ(workspace.apply(command, workspace.revision()).status, CommandStatus::Applied);
    EXPECT_EQ(workspace.revision(), original->revision + 1);
    EXPECT_EQ(workspace.clips().size(), original->clips.size() * 2);
    EXPECT_EQ(workspace.transitions().size(), original->junctions.size() * 2);
    for (const auto& clip : workspace.clips())
        if (clip.start >= at) {
            EXPECT_GT(clip.id, original->id_high_watermark);
            EXPECT_GT(clip.group, original->id_high_watermark);
            EXPECT_EQ(clip.source_in, 0);
            EXPECT_EQ(clip.source_out, 10 * Second);
        }
    EXPECT_EQ(preview.clips, workspace.clips());
    EXPECT_EQ(preview.junctions, workspace.transitions());
    ASSERT_TRUE(workspace.undo());
    EXPECT_EQ(workspace.clips(), original->clips);
    EXPECT_EQ(workspace.transitions(), original->junctions);
    ASSERT_TRUE(workspace.redo());
    EXPECT_EQ(workspace.clips(), preview.clips);
}

TEST(EditClipboard, PasteRefusesLockedDestinationsAndChangedSourcesAtomically) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    std::vector<Id> ids;
    for (const auto& clip : workspace.clips())
        ids.push_back(clip.id);
    const auto copied = CopyClips(*workspace.snapshot(), ids);
    ASSERT_TRUE(copied);
    const auto before = workspace.snapshot();
    ASSERT_TRUE(workspace.setTrackLocked(workspace.tracks().back().id, true));
    const auto refused =
        workspace.apply(PasteClips{copied, 20 * Second, workspace.tracks().front().id}, workspace.revision());
    ASSERT_EQ(refused.status, CommandStatus::Rejected);
    ASSERT_TRUE(refused.error);
    EXPECT_EQ(refused.error->code, EditErrorCode::TrackLocked);
    EXPECT_EQ(workspace.clips(), before->clips);
    ASSERT_TRUE(workspace.setTrackLocked(workspace.tracks().back().id, false));
    auto changed = std::make_shared<ClipClipboard>(*copied);
    auto asset = std::make_shared<Asset>(*changed->assets.front());
    asset->path = L"different-source.mkv";
    changed->assets.front() = asset;
    EXPECT_EQ(
        workspace.apply(PasteClips{changed, 20 * Second, workspace.tracks().front().id}, workspace.revision()).status,
        CommandStatus::Rejected);
    EXPECT_EQ(workspace.clips(), before->clips);
    EXPECT_EQ(workspace.revision(), before->revision);
}

TEST(EditWorkspaceLocks, IndependentTrackInsertionDoesNotReorderALockedExistingTrack) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    const auto locked = workspace.tracks().back().id;
    ASSERT_TRUE(workspace.setTrackLocked(locked, true));
    ASSERT_EQ(workspace.apply(CreateTrack{TrackType::Video, 0}, workspace.revision()).status, CommandStatus::Applied);
    const auto revision = workspace.revision();
    EXPECT_EQ(workspace.apply(ReorderTrack{locked, 0}, revision).status, CommandStatus::Rejected);
    EXPECT_EQ(workspace.revision(), revision);
}

TEST(EditClipboardInsert, SampleLengthInsertionRefusesOffFrameVideoSuffixAtomically) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    const auto audio = workspace.clips().back().id;
    Trim short_audio{audio, TrimEdge::Right, 21 - workspace.clip(audio)->duration()};
    short_audio.expand_linked = false;
    ASSERT_EQ(workspace.apply(short_audio, workspace.revision()).status, CommandStatus::Applied);
    ASSERT_EQ(workspace.clip(audio)->duration(), 21);
    const auto copied = CopyClips(*workspace.snapshot(), {audio});
    ASSERT_TRUE(copied);
    const auto before = workspace.snapshot();
    const PasteClips command{copied, 0, workspace.clips().back().track, true};
    EXPECT_FALSE(PreviewEdit(*before, command).valid);
    EXPECT_EQ(workspace.apply(command, before->revision).status, CommandStatus::Rejected);
    EXPECT_EQ(workspace.revision(), before->revision);
    EXPECT_EQ(workspace.clips(), before->clips);
    EXPECT_EQ(workspace.snapshot()->id_high_watermark, before->id_high_watermark);
}

TEST(EditClipboardInsert, InsertionCannotShiftOnlyOneMemberOfAnOffsetLinkedGroup) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    const auto video = workspace.clips().front().id;
    const auto audio = workspace.clips().back().id;
    ASSERT_EQ(workspace.apply(Trim{video, TrimEdge::Right, -9 * Second}, workspace.revision()).status,
              CommandStatus::Applied);
    Move separate{{audio}, 2 * Second};
    separate.expand_linked = false;
    ASSERT_EQ(workspace.apply(separate, workspace.revision()).status, CommandStatus::Applied);
    ASSERT_EQ(workspace.apply(Relink{{video, audio}}, workspace.revision()).status, CommandStatus::Applied);
    ASSERT_NE(workspace.clip(video)->group, 0);
    ASSERT_EQ(workspace.clip(video)->group, workspace.clip(audio)->group);
    const auto copied = CopyClips(*workspace.snapshot(), {video});
    ASSERT_TRUE(copied);
    const auto before = workspace.snapshot();
    const PasteClips command{copied, 1'500'000, workspace.clips().front().track, true};
    EXPECT_FALSE(PreviewEdit(*before, command).valid);
    EXPECT_EQ(workspace.apply(command, before->revision).status, CommandStatus::Rejected);
    EXPECT_EQ(workspace.revision(), before->revision);
    EXPECT_EQ(workspace.clips(), before->clips);
}

TEST(EditClipboardInsert, InsertingAtResultStartIncludesNewClipsAndRestoresInOneUndo) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    std::vector<Id> ids;
    for (const auto& clip : workspace.clips())
        ids.push_back(clip.id);
    const auto copied = CopyClips(*workspace.snapshot(), ids);
    ASSERT_TRUE(copied);
    const auto before = workspace.snapshot();
    ASSERT_EQ(workspace.apply(PasteClips{copied, 0, workspace.tracks().front().id, true}, before->revision).status,
              CommandStatus::Applied);
    EXPECT_EQ(workspace.snapshot()->result_range.in_us, 0);
    EXPECT_EQ(workspace.duration(), 20 * Second);
    EXPECT_EQ(workspace.clips().size(), 4u);
    ASSERT_TRUE(workspace.undo());
    EXPECT_EQ(workspace.clips(), before->clips);
    EXPECT_EQ(workspace.snapshot()->result_range, before->result_range);
}

TEST(EditWorkspaceGain, RejectsValuesOutsideTheAuthoredTargetBoundsAtomically) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    const auto audio_track = workspace.tracks().back().id;
    const auto audio = std::find_if(workspace.clips().begin(), workspace.clips().end(),
                                    [audio_track](const auto& clip) { return clip.track == audio_track; });
    ASSERT_NE(audio, workspace.clips().end());
    const auto audio_id = audio->id;
    for (const auto& command :
         {SetGain{GainTarget::Clip, audio_id, 24.1}, SetGain{GainTarget::Clip, audio_id, -60.1},
          SetGain{GainTarget::Track, audio_track, 12.1}, SetGain{GainTarget::Track, audio_track, -60.1}}) {
        const auto before = workspace.snapshot();
        const auto history = workspace.historyBytes();
        EXPECT_EQ(workspace.apply(command, workspace.revision()).status, CommandStatus::Rejected);
        EXPECT_EQ(workspace.revision(), before->revision);
        EXPECT_EQ(workspace.clips(), before->clips);
        EXPECT_EQ(workspace.tracks(), before->tracks);
        EXPECT_EQ(workspace.historyBytes(), history);
    }
    ASSERT_EQ(workspace.apply(SetGain{GainTarget::Clip, audio_id, 24}, workspace.revision()).status,
              CommandStatus::Applied);
    ASSERT_EQ(workspace.apply(SetGain{GainTarget::Track, audio_track, -60}, workspace.revision()).status,
              CommandStatus::Applied);
}

TEST(EditWorkspaceGain, ExplicitMultiGainChangesOnlyNamedAudioAndUndoesOnce) {
    Workspace workspace;
    const auto asset = ContractSource(workspace);
    ASSERT_TRUE(workspace.insert(asset));
    ASSERT_TRUE(workspace.insert(asset));
    std::vector<Id> audio;
    for (const auto& clip : workspace.clips())
        if (clip.track == workspace.tracks().back().id)
            audio.push_back(clip.id);
    ASSERT_EQ(audio.size(), 2u);
    const auto before = workspace.snapshot();
    ASSERT_EQ(workspace.apply(SetGain{GainTarget::Clip, 0, -12.3, audio}, workspace.revision()).status,
              CommandStatus::Applied);
    for (const auto& clip : workspace.clips())
        EXPECT_DOUBLE_EQ(clip.gain_db, std::find(audio.begin(), audio.end(), clip.id) != audio.end() ? -12.3 : 0);
    ASSERT_TRUE(workspace.undo());
    EXPECT_EQ(workspace.clips(), before->clips);
    ASSERT_TRUE(workspace.setTrackLocked(workspace.tracks().back().id, true));
    const auto revision = workspace.revision();
    EXPECT_EQ(workspace.apply(SetGain{GainTarget::Clip, 0, -6, audio}, revision).status, CommandStatus::Rejected);
    EXPECT_EQ(workspace.clips(), before->clips);
    EXPECT_EQ(workspace.revision(), revision);
    ASSERT_TRUE(workspace.setTrackLocked(workspace.tracks().back().id, false));
    audio.push_back(workspace.clips().front().id);
    EXPECT_EQ(workspace.apply(SetGain{GainTarget::Clip, 0, -6, audio}, revision).status, CommandStatus::Rejected);
    EXPECT_EQ(workspace.clips(), before->clips);
}

TEST(EditWorkspaceGain, AuthoredGainAndRangeDeltaRejectFinerThanTenthsWithoutMutation) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    const auto audio = workspace.clips().back().id;
    const auto track = workspace.clips().back().track;
    ASSERT_EQ(workspace.apply(Unlink{{audio}}, workspace.revision()).status, CommandStatus::Applied);
    const auto before = workspace.snapshot();
    for (const EditCommand& command :
         {EditCommand{SetGain{GainTarget::Clip, audio, 1.23}}, EditCommand{SetGain{GainTarget::Track, track, -2.01}},
          EditCommand{AdjustRangeGain{{2 * Second, 4 * Second}, {track}, 0.05}}}) {
        EXPECT_EQ(workspace.apply(command, workspace.revision()).status, CommandStatus::Rejected);
        EXPECT_EQ(workspace.revision(), before->revision);
        EXPECT_EQ(workspace.clips(), before->clips);
        EXPECT_EQ(workspace.tracks(), before->tracks);
    }
}

TEST(EditWorkspaceRange, ScopedAudioGainSplitsAndCommitsAsOneAtomicCommand) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    const auto audio_track = workspace.tracks().back().id;
    const auto audio = std::find_if(workspace.clips().begin(), workspace.clips().end(),
                                    [&](const auto& clip) { return clip.track == audio_track; });
    ASSERT_NE(audio, workspace.clips().end());
    ASSERT_EQ(workspace.apply(Unlink{{audio->id}}, workspace.revision()).status, CommandStatus::Applied);
    const auto before = workspace.snapshot();
    ASSERT_EQ(workspace.apply(AdjustRangeGain{{2 * Second, 4 * Second}, {audio_track}, 6}, workspace.revision()).status,
              CommandStatus::Applied);
    EXPECT_EQ(workspace.clips().size(), 4u);
    for (const auto& clip : workspace.clips()) {
        if (clip.track == audio_track && clip.start == 2 * Second) {
            EXPECT_EQ(clip.end(), 4 * Second);
            EXPECT_DOUBLE_EQ(clip.gain_db, 6);
        } else {
            EXPECT_DOUBLE_EQ(clip.gain_db, 0);
        }
    }
    ASSERT_TRUE(workspace.undo());
    EXPECT_EQ(workspace.clips(), before->clips);
}

TEST(EditWorkspaceRange, TailLiftRetainsDurationAndUndoRestoresTheFollowingEndMode) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    const auto before = workspace.snapshot();
    const auto video = workspace.tracks().front().id;
    const auto audio = workspace.tracks().back().id;
    ASSERT_EQ(workspace.apply(LiftRange{{8 * Second, 10 * Second}, {video, audio}}, workspace.revision()).status,
              CommandStatus::Applied);
    EXPECT_EQ(workspace.duration(), 10 * Second);
    EXPECT_EQ(workspace.snapshot()->timeline_end.mode, TimelineEndMode::FixedEnd);
    for (const auto& clip : workspace.clips())
        EXPECT_EQ(clip.end(), 8 * Second);
    ASSERT_TRUE(workspace.undo());
    EXPECT_EQ(workspace.clips(), before->clips);
    EXPECT_EQ(workspace.snapshot()->timeline_end, before->timeline_end);
    ASSERT_TRUE(workspace.redo());
    EXPECT_EQ(workspace.duration(), 10 * Second);
}

TEST(EditWorkspaceRange, RippleRefusesToShiftUnscopedIndependentContent) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    const auto audio = workspace.clips().back().id;
    ASSERT_EQ(workspace.apply(Unlink{{audio}}, workspace.revision()).status, CommandStatus::Applied);
    const auto before = workspace.snapshot();
    const auto result =
        workspace.apply(RippleRemoveRange{{Second, 2 * Second}, {workspace.tracks().front().id}}, workspace.revision());
    ASSERT_EQ(result.status, CommandStatus::Rejected);
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->code, EditErrorCode::RippleConflict);
    EXPECT_EQ(workspace.clips(), before->clips);
}

TEST(EditSelectionContracts, AddingVideoRefusesACollapsedSampleRangeWithoutChangingTheOriginal) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    const auto snapshot = workspace.snapshot();
    const auto audio = workspace.tracks().back().id;
    const auto video = workspace.tracks().front().id;
    const auto range = MakeEditRange(*snapshot, 21, 42, {audio}, 21);
    ASSERT_TRUE(range);
    EXPECT_EQ(range->start_us, 21);
    EXPECT_EQ(range->end_us, 42);
    EXPECT_FALSE(MakeEditRange(*snapshot, range->start_us, range->end_us, {video, audio}, range->anchor_us));
    EXPECT_EQ(range->start_us, 21);
    EXPECT_EQ(range->end_us, 42);
}

TEST(EditWorkspaceContracts, InvalidSplitInJunction) {
    Workspace workspace;
    const auto source = ContractSource(workspace);
    ASSERT_TRUE(workspace.insert(source));
    const auto first = workspace.selection();
    ASSERT_TRUE(workspace.insert(source));
    const auto second = workspace.selection();
    ASSERT_TRUE(workspace.crossfade(first, second, Second));
    const auto before = workspace.snapshot();
    const auto result = workspace.apply(Split{{first, second}, 9 * Second + Second / 2, true}, before->revision);
    ASSERT_EQ(result.status, CommandStatus::Rejected);
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->code, EditErrorCode::TransitionBoundaryConflict);
    ExpectSameEdit(*workspace.snapshot(), *before);
    ASSERT_TRUE(workspace.undo());
    EXPECT_TRUE(workspace.transitions().empty());
}

TEST(EditWorkspaceContracts, UndoRevisionNeverGoesBack) {
    Workspace workspace;
    const auto source = ContractSource(workspace);
    const auto initial = workspace.revision();
    ASSERT_TRUE(workspace.insert(source));
    const auto inserted = workspace.revision();
    EXPECT_GT(inserted, initial);
    ASSERT_EQ(workspace.undoCommand().status, CommandStatus::Applied);
    const auto undone = workspace.revision();
    EXPECT_GT(undone, inserted);
    ASSERT_EQ(workspace.redoCommand().status, CommandStatus::Applied);
    EXPECT_GT(workspace.revision(), undone);
}

TEST(EditWorkspaceContracts, StaleAndNoOpCommandsPreserveRevisionHistoryAndIds) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    const auto target = workspace.selection();
    const auto before = workspace.snapshot();
    const auto history = workspace.historyBytes();
    const auto stale = workspace.apply(Move{{target}, Second, {}, {}, true}, before->revision - 1);
    ASSERT_EQ(stale.status, CommandStatus::Rejected);
    ASSERT_TRUE(stale.error);
    EXPECT_EQ(stale.error->code, EditErrorCode::StaleRevision);
    EXPECT_EQ(workspace.apply(Move{{target}, 0, {}, {}, true}, before->revision).status, CommandStatus::NoOp);
    ExpectSameEdit(*workspace.snapshot(), *before);
    EXPECT_EQ(workspace.historyBytes(), history);
    EXPECT_EQ(workspace.undoCommand().status, CommandStatus::Applied);
    EXPECT_TRUE(workspace.clips().empty());
}

namespace {
exosnap::engine::TimelineSnapshot PairSnapshot(bool audio = false) {
    using namespace exosnap::engine;
    TimelineSnapshot snapshot;
    snapshot.duration_us = 18 * Second;
    snapshot.tracks = {{1, audio}};
    snapshot.clips = {{1, 1, 1, audio, 0, 0, 10 * Second}, {2, 2, 2, audio, 8 * Second, 0, 10 * Second}};
    for (auto& clip : snapshot.clips)
        clip.track = 1;
    snapshot.junctions = {{3, 1, 1, 2, JunctionKind::Crossfade, 8 * Second, 10 * Second}};
    return snapshot;
}

Id VideoJunction(const Workspace& workspace, Id outgoing) {
    const auto snapshot = workspace.snapshot();
    for (const auto& junction : snapshot->junctions)
        if (junction.outgoing == outgoing)
            return junction.id;
    return 0;
}
} // namespace

TEST(EditEvaluatorContracts, CurvesAreFiniteMonotonicAndEndpointPreserving) {
    using namespace exosnap::engine;
    for (const auto kind : {CurveKind::Linear, CurveKind::Smooth, CurveKind::Bias}) {
        for (const double bias : {-1.0, -0.6, 0.0, 0.6, 1.0}) {
            const CurveSpec curve{kind, bias};
            ASSERT_TRUE(EvaluateCurve(curve, 0));
            EXPECT_EQ(*EvaluateCurve(curve, 0), 0);
            EXPECT_EQ(*EvaluateCurve(curve, 1), 1);
            double previous = 0;
            for (int step = 0; step <= 100; ++step) {
                const auto value = EvaluateCurve(curve, static_cast<double>(step) / 100);
                ASSERT_TRUE(value);
                EXPECT_GE(*value, previous);
                EXPECT_LE(*value, 1);
                previous = *value;
            }
        }
    }
    EXPECT_DOUBLE_EQ(*EvaluateCurve({CurveKind::Smooth, 0}, 0.25), 0.15625);
    EXPECT_DOUBLE_EQ(*EvaluateCurve({CurveKind::Bias, 1}, 0.5), 0.0625);
    EXPECT_DOUBLE_EQ(*EvaluateCurve({CurveKind::Bias, -1}, 0.5), 0.9375);
    EXPECT_FALSE(EvaluateCurve({CurveKind::Bias, 1.01}, 0.5));
    EXPECT_FALSE(EvaluateCurve({}, (std::numeric_limits<double>::infinity)()));
}

TEST(EditEvaluatorContracts, WhiteCrossfadeStaysWhite) {
    using namespace exosnap::engine;
    auto snapshot = PairSnapshot();
    snapshot.junctions[0].curve = {CurveKind::Bias, 0.6};
    for (const Time at : {8'500'000, 9'000'000, 9'500'000}) {
        const auto evaluation = EvaluateTimeline(snapshot, at);
        ASSERT_TRUE(evaluation.error.empty()) << evaluation.error;
        ASSERT_EQ(evaluation.video_layers.size(), 1u);
        const auto& pair = evaluation.video_layers[0].contributions;
        ASSERT_EQ(pair.size(), 2u);
        EXPECT_DOUBLE_EQ(pair[0].weight + pair[1].weight, 1);
        EXPECT_DOUBLE_EQ(pair[0].weight * pair[0].opacity + pair[1].weight * pair[1].opacity, 1);
    }
}

TEST(EditEvaluatorContracts, TransparentPairOverBackground) {
    using namespace exosnap::engine;
    auto snapshot = PairSnapshot();
    snapshot.tracks.push_back({2, false});
    snapshot.clips[0].opacity = snapshot.clips[1].opacity = 0.4;
    snapshot.clips.push_back({4, 3, 0, false, 0, 0, 18 * Second});
    snapshot.clips.back().track = 2;
    for (const Time at : {8'500'000, 9'000'000, 9'500'000}) {
        const auto evaluation = EvaluateTimeline(snapshot, at);
        ASSERT_TRUE(evaluation.error.empty()) << evaluation.error;
        ASSERT_EQ(evaluation.video_layers.size(), 2u);
        EXPECT_EQ(evaluation.video_layers[0].track, 2u);
        EXPECT_EQ(evaluation.video_layers[1].track, 1u);
        double red = 0;
        double alpha = 0;
        for (const auto& contribution : evaluation.video_layers[1].contributions) {
            red += contribution.weight * contribution.opacity;
            alpha += contribution.weight * contribution.opacity;
        }
        EXPECT_NEAR(red, 0.4, 1e-12);
        EXPECT_NEAR(1 - alpha, 0.6, 1e-12);
    }
}

TEST(EditWorkspaceJunctions, WindowEditKeepsGeometry) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    const auto a = workspace.selection();
    ASSERT_TRUE(workspace.insert(ContractSource(workspace, L"second.mkv")));
    const auto b = workspace.selection();
    ASSERT_TRUE(workspace.crossfade(a, b, 2 * Second));
    const auto geometry = workspace.clips();
    const auto video = VideoJunction(workspace, a);
    ASSERT_NE(video, 0u);
    ASSERT_EQ(workspace.apply(SetJunctionWindow{video, {8'500'000, 9'500'000}}, workspace.revision()).status,
              CommandStatus::Applied);
    EXPECT_EQ(workspace.clips(), geometry);
    const auto snapshot = workspace.snapshot();
    const auto audio = std::find_if(snapshot->junctions.begin(), snapshot->junctions.end(), [&](const auto& junction) {
        const auto* clip = workspace.clip(junction.outgoing);
        return clip && clip->group == workspace.clip(a)->group && junction.id != video;
    });
    ASSERT_NE(audio, snapshot->junctions.end());
    EXPECT_EQ(audio->in_us, 8 * Second);
    EXPECT_EQ(audio->out_us, 10 * Second);
}

TEST(EditWorkspaceJunctions, RemoveTransitionBecomesCut) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    const auto a = workspace.selection();
    ASSERT_TRUE(workspace.insert(ContractSource(workspace, L"second.mkv")));
    const auto b = workspace.selection();
    ASSERT_EQ(workspace.apply(SetFade{a, FadeEdge::Out, {Second, {}}}, workspace.revision()).status,
              CommandStatus::Applied);
    ASSERT_EQ(workspace.apply(SetFade{b, FadeEdge::In, {Second, {}}}, workspace.revision()).status,
              CommandStatus::Applied);
    ASSERT_TRUE(workspace.crossfade(a, b, 2 * Second));
    EXPECT_EQ(workspace.clip(a)->fade_out.duration_us, 0);
    EXPECT_EQ(workspace.clip(b)->fade_in.duration_us, 0);
    const auto geometry = workspace.clips();
    const auto id = VideoJunction(workspace, a);
    ASSERT_TRUE(workspace.removeCrossfade(a));
    EXPECT_EQ(workspace.clips(), geometry);
    const auto snapshot = workspace.snapshot();
    const auto found = std::find_if(snapshot->junctions.begin(), snapshot->junctions.end(),
                                    [id](const auto& junction) { return junction.id == id; });
    ASSERT_NE(found, snapshot->junctions.end());
    EXPECT_EQ(found->kind, TransitionKind::HardCut);
    EXPECT_EQ(found->cut_us, 9 * Second);
    EXPECT_EQ(workspace.clip(a)->fade_out.duration_us, 0);
    EXPECT_EQ(workspace.clip(b)->fade_in.duration_us, 0);
}

TEST(EditWorkspaceJunctions, PartialWindowClampsWithoutReset) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    const auto a = workspace.selection();
    ASSERT_TRUE(workspace.insert(ContractSource(workspace, L"second.mkv")));
    const auto b = workspace.selection();
    ASSERT_TRUE(workspace.crossfade(a, b, 2 * Second));
    const auto id = VideoJunction(workspace, a);
    ASSERT_EQ(workspace.apply(SetJunctionWindow{id, {8'500'000, 9'500'000}}, workspace.revision()).status,
              CommandStatus::Applied);
    ASSERT_EQ(
        workspace.apply(SetJunctionCurve{id, {exosnap::engine::CurveKind::Bias, 0.6}}, workspace.revision()).status,
        CommandStatus::Applied);
    ASSERT_EQ(workspace.apply(Move{{b}, 1'800'000, {}, {}, true}, workspace.revision()).status, CommandStatus::Applied);
    const auto snapshot = workspace.snapshot();
    const auto found = std::find_if(snapshot->junctions.begin(), snapshot->junctions.end(),
                                    [id](const auto& junction) { return junction.id == id; });
    ASSERT_NE(found, snapshot->junctions.end());
    EXPECT_EQ(found->kind, TransitionKind::HardCut);
    EXPECT_EQ(found->cut_us, 10 * Second);
    EXPECT_EQ(found->curve.bias, 0.6);
    EXPECT_EQ(workspace.clip(a)->start, 0);
    EXPECT_EQ(workspace.clip(b)->start, 9'800'000);
}

TEST(EditEvaluatorContracts, HardCutUsesOneContributionAtBothOverlapEndpoints) {
    using namespace exosnap::engine;
    for (const Time cut : {8 * Second, 10 * Second}) {
        auto snapshot = PairSnapshot();
        snapshot.junctions[0].kind = JunctionKind::HardCut;
        snapshot.junctions[0].cut_us = cut;
        for (const Time at : {8 * Second, 9 * Second, 10 * Second - 1, 10 * Second}) {
            const auto evaluation = EvaluateTimeline(snapshot, at);
            ASSERT_TRUE(evaluation.error.empty()) << evaluation.error;
            ASSERT_EQ(evaluation.video.size(), 1u);
            EXPECT_EQ(evaluation.video[0].clip, at < cut ? 1u : 2u);
            EXPECT_EQ(evaluation.video[0].weight, 1);
        }
    }
}

TEST(EditAudioContracts, EqualGainIdenticalAndEqualPowerInvariant) {
    using namespace exosnap::engine;
    for (const auto law : {AudioGainLaw::EqualGain, AudioGainLaw::EqualPower}) {
        auto snapshot = PairSnapshot(true);
        snapshot.junctions[0].audio_law = law;
        const auto intervals = EvaluateAudioIntervals(snapshot, 8 * Second, 10 * Second);
        ASSERT_TRUE(intervals.error.empty()) << intervals.error;
        ASSERT_EQ(intervals.intervals.size(), 1u);
        ASSERT_EQ(intervals.intervals[0].contributions.size(), 2u);
        for (const int64_t sample : {408000, 432000, 456000}) {
            const auto a = EvaluateAudioGain(intervals.intervals[0].contributions[0], sample);
            const auto b = EvaluateAudioGain(intervals.intervals[0].contributions[1], sample);
            ASSERT_TRUE(a);
            ASSERT_TRUE(b);
            if (law == AudioGainLaw::EqualGain)
                EXPECT_NEAR(0.25 * *a + 0.25 * *b, 0.25, 1e-12);
            else
                EXPECT_NEAR(*a * *a + *b * *b, 1, 1e-12);
        }
    }
}

TEST(EditAudioContracts, AbsoluteSourceMappingDoesNotDriftAcrossSplits) {
    using namespace exosnap::engine;
    AudioIntervalContribution left;
    left.clip_start_us = 0;
    left.source_in_us = 8;
    left.source_out_us = 2 * Second;
    auto right = left;
    right.clip_start_us = 1'000'003;
    right.source_in_us = 1'000'011;
    EXPECT_EQ(AudioSourceSample(left, 48000), AudioSourceSample(right, 48000));
    EXPECT_EQ(AudioSourceSample(right, 48000), std::optional<int64_t>{48000});
    EXPECT_EQ(AudioSampleIndex(86'400'000'000), 4'147'200'000);
    EXPECT_EQ(AudioSampleIndex(10), 0);
    EXPECT_EQ(AudioSampleIndex(11), 1);
    EXPECT_EQ(AudioSampleIndex(-11), -1);
}

TEST(EditAudioContracts, MonitorStateExcluded) {
    Workspace workspace;
    const auto asset = ContractSource(workspace);
    ASSERT_TRUE(workspace.insert(asset));
    const auto before = workspace.snapshot();
    exosnap::engine::MonitorSelectionSnapshot selection;
    selection.overlap_confirmed = true;
    ASSERT_TRUE(workspace.setMonitorSelection(asset, selection));
    EXPECT_EQ(workspace.snapshot(), before);
    EXPECT_EQ(workspace.revision(), before->revision);
}

TEST(EditAudioContracts, EditMuteIncluded) {
    using namespace exosnap::engine;
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(ContractSource(workspace)));
    const auto audio = std::find_if(workspace.clips().begin(), workspace.clips().end(),
                                    [&](const auto& clip) { return clip.id != workspace.selection(); });
    ASSERT_NE(audio, workspace.clips().end());
    const auto id = audio->id;
    ASSERT_EQ(EvaluateTimeline(workspace.timeline(), Second).audio.size(), 1u);
    ASSERT_EQ(workspace.apply(SetClipEnabled{id, false}, workspace.revision()).status, CommandStatus::Applied);
    EXPECT_TRUE(EvaluateTimeline(workspace.timeline(), Second).audio.empty());
    EXPECT_TRUE(EvaluateTimeline(workspace.timeline(), Second).audio_gap);
    ASSERT_TRUE(workspace.undo());
    EXPECT_EQ(EvaluateTimeline(workspace.timeline(), Second).audio.size(), 1u);
}

namespace {
exosnap::engine::EditMediaMetadata ProbedSource(const std::vector<uint32_t>& audio_channels) {
    using namespace exosnap::engine;
    EditMediaMetadata metadata;
    metadata.duration_us = 10 * Second;
    metadata.content_identity_valid = true;
    metadata.content_sha256[0] = 7;
    EditSourceStream video;
    video.container_index = 0;
    video.kind = TimelineStreamKind::Video;
    video.fps_num = 60;
    video.fps_den = 1;
    metadata.streams.push_back(video);
    for (uint32_t index = 0; index < audio_channels.size(); ++index) {
        EditSourceStream audio;
        audio.container_index = index + 1;
        audio.kind = TimelineStreamKind::Audio;
        audio.codec.channels = audio_channels[index];
        audio.stream_identity = AudioTrackId{{static_cast<uint8_t>(index + 10)}};
        metadata.streams.push_back(audio);
    }
    return metadata;
}

std::vector<bool> ImportedAudioEnables(const exosnap::engine::EditMediaMetadata& metadata) {
    Workspace workspace;
    Asset asset;
    asset.path = L"probed.mkv";
    asset.duration = metadata.duration_us;
    auto imported = DescribeSourceImport(metadata);
    asset.source_descriptor = imported.descriptor;
    asset.input_mix_selection = imported.input_mix;
    asset.mix_decision_required = imported.mix_decision_required;
    EXPECT_TRUE(workspace.insert(workspace.addAsset(std::move(asset)), 0));
    std::vector<bool> enabled;
    for (const auto& clip : workspace.clips()) {
        EXPECT_TRUE(clip.stream);
        if (clip.stream && clip.stream->kind == exosnap::engine::TimelineStreamKind::Audio)
            enabled.push_back(clip.enabled);
    }
    return enabled;
}
} // namespace

TEST(EditSourceImport, SingleAudioStreamIsSelectedWithoutAQuestion) {
    const auto metadata = ProbedSource({2});
    const auto imported = DescribeSourceImport(metadata);
    ASSERT_TRUE(imported.descriptor);
    EXPECT_EQ(imported.descriptor->streams.size(), 2u);
    EXPECT_FALSE(imported.mix_decision_required);
    EXPECT_EQ(ImportedAudioEnables(metadata), std::vector<bool>{true});
}

TEST(EditSourceImport, AmbiguousStreamsImportDisabledAndRequireADecision) {
    const auto metadata = ProbedSource({2, 1});
    const auto imported = DescribeSourceImport(metadata);
    ASSERT_TRUE(imported.descriptor);
    EXPECT_TRUE(imported.mix_decision_required);
    EXPECT_EQ(ImportedAudioEnables(metadata), (std::vector<bool>{false, false}));
}

TEST(EditSourceImport, VerifiedRecordedDefaultSelectsOnlyItsStreams) {
    auto metadata = ProbedSource({2, 2});
    metadata.audio_metadata.status = exosnap::engine::AudioMetadataStatus::Verified;
    metadata.audio_metadata.monitoring_confirmation_required = false;
    metadata.audio_metadata.manifest.emplace();
    metadata.audio_metadata.manifest->default_monitor_track_ids = {exosnap::engine::AudioTrackId{{11}}};
    EXPECT_FALSE(DescribeSourceImport(metadata).mix_decision_required);
    EXPECT_EQ(ImportedAudioEnables(metadata), (std::vector<bool>{false, true}));
}

namespace {
// A recorded master (stream 10) beside two stems (11, 12), imported with an
// undecided starting mix.
struct UndecidedMix {
    Workspace workspace;
    Id asset = 0;
    UndecidedMix() {
        const auto metadata = ProbedSource({2, 1, 1});
        Asset value;
        value.path = L"probed.mkv";
        value.duration = metadata.duration_us;
        auto imported = DescribeSourceImport(metadata);
        value.source_descriptor = imported.descriptor;
        value.input_mix_selection = imported.input_mix;
        value.mix_decision_required = imported.mix_decision_required;
        asset = workspace.addAsset(std::move(value));
        EXPECT_TRUE(workspace.insert(asset, 0));
    }
    [[nodiscard]] std::vector<bool> enabled() const {
        std::vector<bool> result;
        for (const auto& clip : workspace.clips())
            if (clip.stream && clip.stream->kind == exosnap::engine::TimelineStreamKind::Audio)
                result.push_back(clip.enabled);
        return result;
    }
    [[nodiscard]] Id audioClip(uint8_t identity) const {
        for (const auto& clip : workspace.clips())
            if (clip.stream && clip.stream->stream_identity == exosnap::engine::AudioTrackId{{identity}})
                return clip.id;
        return 0;
    }
    [[nodiscard]] size_t audible() const {
        return EvaluateTimeline(workspace.timeline(), Second).audio.size();
    }
};
const std::vector<exosnap::engine::AudioTrackId> kStems{exosnap::engine::AudioTrackId{{11}},
                                                        exosnap::engine::AudioTrackId{{12}}};
} // namespace

TEST(EditSourceImport, ResolvingAnAmbiguousMixEnablesOnlyTheChosenStreams) {
    UndecidedMix mix;
    EXPECT_EQ(mix.enabled(), (std::vector<bool>{false, false, false}));
    EXPECT_EQ(mix.audible(), 0u);
    ASSERT_TRUE(mix.workspace.resolveInputMix(mix.asset, kStems));
    EXPECT_FALSE(mix.workspace.asset(mix.asset)->mix_decision_required);
    EXPECT_EQ(mix.workspace.asset(mix.asset)->input_mix_selection->selected_stream_identities, kStems);
    EXPECT_EQ(mix.enabled(), (std::vector<bool>{false, true, true}));
    EXPECT_EQ(mix.audible(), 2u);
}

TEST(EditSourceImport, TransientAuditionCanReplaceTheResolvedInputMix) {
    UndecidedMix mix;
    const std::vector<exosnap::engine::AudioTrackId> first{kStems.front()};
    const std::vector<exosnap::engine::AudioTrackId> second{kStems.back()};
    ASSERT_TRUE(mix.workspace.resolveInputMix(mix.asset, first));
    EXPECT_EQ(mix.enabled(), (std::vector<bool>{false, true, false}));
    EXPECT_FALSE(mix.workspace.resolveInputMix(mix.asset, second));
    ASSERT_TRUE(mix.workspace.resolveInputMix(mix.asset, second, true));
    EXPECT_EQ(mix.enabled(), (std::vector<bool>{false, false, true}));
    EXPECT_EQ(mix.audible(), 1u);
    ASSERT_TRUE(mix.workspace.resolveInputMix(mix.asset, first, true));
    EXPECT_EQ(mix.enabled(), (std::vector<bool>{false, true, false}));
    EXPECT_EQ(mix.audible(), 1u);
}

TEST(EditSourceImport, FirstAudioChoiceIsOneUndoableAuthoredChange) {
    UndecidedMix mix;
    const auto before = mix.workspace.revision();
    ASSERT_TRUE(mix.workspace.resolveInputMix(mix.asset, kStems));
    EXPECT_EQ(mix.workspace.revision(), before + 1);
    ASSERT_TRUE(mix.workspace.undo());
    EXPECT_TRUE(mix.workspace.asset(mix.asset)->mix_decision_required);
    EXPECT_EQ(mix.enabled(), (std::vector<bool>{false, false, false}));
    ASSERT_TRUE(mix.workspace.redo());
    EXPECT_FALSE(mix.workspace.asset(mix.asset)->mix_decision_required);
    EXPECT_EQ(mix.enabled(), (std::vector<bool>{false, true, true}));
}

TEST(EditSourceImport, AudioChoiceUndoPreservesEarlierEditsAndTheirHistory) {
    UndecidedMix mix;
    const auto stem = mix.audioClip(11);
    ASSERT_EQ(mix.workspace.apply(Move{{stem}, Second, {}, {}, false}, mix.workspace.revision()).status,
              CommandStatus::Applied);
    ASSERT_TRUE(mix.workspace.resolveInputMix(mix.asset, kStems));
    const auto resolved = mix.enabled();
    ASSERT_TRUE(mix.workspace.undo());
    EXPECT_EQ(mix.enabled(), (std::vector<bool>{false, false, false}));
    EXPECT_EQ(mix.workspace.clip(stem)->start, Second);
    EXPECT_TRUE(mix.workspace.asset(mix.asset)->mix_decision_required);
    ASSERT_TRUE(mix.workspace.undo());
    EXPECT_EQ(mix.workspace.clip(stem)->start, 0);
    ASSERT_TRUE(mix.workspace.undo());
    EXPECT_TRUE(mix.workspace.clips().empty());
    ASSERT_TRUE(mix.workspace.redo());
    EXPECT_EQ(mix.enabled(), (std::vector<bool>{false, false, false}));
    ASSERT_TRUE(mix.workspace.redo());
    EXPECT_EQ(mix.workspace.clip(stem)->start, Second);
    EXPECT_TRUE(mix.workspace.asset(mix.asset)->mix_decision_required);
    ASSERT_TRUE(mix.workspace.redo());
    EXPECT_EQ(mix.enabled(), resolved);
    EXPECT_EQ(mix.workspace.clip(stem)->start, Second);
    EXPECT_FALSE(mix.workspace.asset(mix.asset)->mix_decision_required);
    // The decision is made once; a second confirmation cannot rewrite it.
    EXPECT_FALSE(mix.workspace.resolveInputMix(mix.asset, {exosnap::engine::AudioTrackId{{10}}}));
    EXPECT_EQ(mix.workspace.asset(mix.asset)->input_mix_selection->selected_stream_identities, kStems);
}

TEST(EditSourceImport, ReenablingAStemNeverReactivatesTheRedundantMaster) {
    UndecidedMix mix;
    ASSERT_TRUE(mix.workspace.resolveInputMix(mix.asset, kStems));
    const auto stem = mix.audioClip(11);
    const auto master = mix.audioClip(10);
    ASSERT_EQ(mix.workspace.apply(SetClipEnabled{stem, false}, mix.workspace.revision()).status,
              CommandStatus::Applied);
    ASSERT_EQ(mix.workspace.apply(SetClipEnabled{stem, true}, mix.workspace.revision()).status, CommandStatus::Applied);
    EXPECT_FALSE(mix.workspace.clip(master)->enabled);
    // Placing the source again follows the confirmed provenance, not every stream.
    const auto before = mix.workspace.clips().size();
    ASSERT_TRUE(mix.workspace.insert(mix.asset, 20 * Second));
    for (const auto& clip : mix.workspace.clips())
        if (clip.start == 20 * Second && clip.stream && clip.stream->kind == exosnap::engine::TimelineStreamKind::Audio)
            EXPECT_EQ(clip.enabled, clip.stream->stream_identity != exosnap::engine::AudioTrackId{{10}});
    EXPECT_EQ(mix.workspace.clips().size(), before * 2);
}

TEST(EditSourceImport, ConfirmedSilenceDiffersFromAnUnresolvedMix) {
    UndecidedMix undecided;
    UndecidedMix silent;
    ASSERT_TRUE(silent.workspace.resolveInputMix(silent.asset, {}));
    EXPECT_EQ(undecided.enabled(), silent.enabled());
    EXPECT_TRUE(undecided.workspace.asset(undecided.asset)->mix_decision_required);
    EXPECT_FALSE(silent.workspace.asset(silent.asset)->mix_decision_required);
    ASSERT_TRUE(silent.workspace.asset(silent.asset)->input_mix_selection);
    EXPECT_TRUE(silent.workspace.asset(silent.asset)->input_mix_selection->selected_stream_identities.empty());
    EXPECT_EQ(silent.audible(), 0u);
    ASSERT_TRUE(silent.workspace.undo());
    EXPECT_TRUE(silent.workspace.asset(silent.asset)->mix_decision_required);
    EXPECT_EQ(silent.enabled(), (std::vector<bool>{false, false, false}));
    ASSERT_TRUE(silent.workspace.redo());
    EXPECT_FALSE(silent.workspace.asset(silent.asset)->mix_decision_required);
}

TEST(EditSourceImport, ExplicitEnableEditsSurviveTheDecisionAndMonitoringNeverRewritesIt) {
    UndecidedMix mix;
    const auto master = mix.audioClip(10);
    ASSERT_EQ(mix.workspace.apply(SetClipEnabled{master, true}, mix.workspace.revision()).status,
              CommandStatus::Applied);
    ASSERT_TRUE(mix.workspace.resolveInputMix(mix.asset, kStems));
    EXPECT_EQ(mix.enabled(), (std::vector<bool>{true, true, true}));
    ASSERT_TRUE(mix.workspace.undo());
    EXPECT_EQ(mix.enabled(), (std::vector<bool>{true, false, false}));
    ASSERT_TRUE(mix.workspace.redo());
    ASSERT_EQ(mix.workspace.apply(SetClipEnabled{master, false}, mix.workspace.revision()).status,
              CommandStatus::Applied);
    EXPECT_EQ(mix.enabled(), (std::vector<bool>{false, true, true}));
    // A later monitoring choice is transient preview state: it neither changes
    // the provenance nor reaches the export snapshot.
    exosnap::engine::MonitorSelectionSnapshot monitoring;
    monitoring.tracks.push_back({exosnap::engine::AudioTrackId{{10}}, 1.0f, false});
    ASSERT_TRUE(mix.workspace.setMonitorSelection(mix.asset, monitoring));
    EXPECT_EQ(mix.workspace.asset(mix.asset)->input_mix_selection->selected_stream_identities, kStems);
    for (const auto& asset : mix.workspace.snapshot()->assets)
        EXPECT_FALSE(asset->monitor_selection);
    EXPECT_EQ(mix.audible(), 2u);
}

TEST(EditSourceImport, UnboundBytesOrUnknownLayoutsKeepTheLinkedImport) {
    auto unbound = ProbedSource({2});
    unbound.content_identity_valid = false;
    EXPECT_FALSE(DescribeSourceImport(unbound).descriptor);
    EXPECT_FALSE(DescribeSourceImport(ProbedSource({6})).descriptor);
}

namespace {
std::pair<Id, Id> AdjacentCutWithHandles(Workspace& workspace, RationalFps fps = {60, 1}) {
    Asset asset;
    asset.path = L"handles.mkv";
    asset.duration = *fps.frameTime(600);
    asset.fps_num = fps.num;
    asset.fps_den = fps.den;
    const auto id = workspace.addAsset(asset);
    if (!workspace.insert(id))
        return {};
    const auto outgoing = workspace.selection();
    const auto track = workspace.clip(outgoing)->track;
    const auto cut = *fps.frameTime(300);
    if (!workspace.split(outgoing, cut))
        return {};
    for (const auto& clip : workspace.clips())
        if (clip.track == track && clip.start == cut)
            return {outgoing, clip.id};
    return {};
}
} // namespace

TEST(EditCrossfadeAtCut, ExtendsLinkedSourceHandlesAroundCutWithoutMovingOuterContentAndUndoesOnce) {
    Workspace workspace;
    const auto [outgoing, incoming] = AdjacentCutWithHandles(workspace);
    ASSERT_NE(outgoing, 0);
    ASSERT_NE(incoming, 0);
    ASSERT_TRUE(workspace.insert(workspace.clip(outgoing)->asset, 10'000'000));
    const auto before = workspace.snapshot();
    const auto duration = workspace.duration();
    const CrossfadeAtCut command{outgoing, incoming};
    const auto preview = PreviewEdit(*before, command);
    ASSERT_TRUE(preview.valid);
    EXPECT_EQ(workspace.clips(), before->clips);
    ASSERT_EQ(workspace.apply(command, before->revision).status, CommandStatus::Applied);
    EXPECT_EQ(workspace.clips(), preview.clips);
    EXPECT_EQ(workspace.transitions(), preview.junctions);
    EXPECT_EQ(workspace.duration(), duration);
    ASSERT_EQ(workspace.transitions().size(), 2U);
    for (const auto& old_clip : before->clips) {
        const auto* actual = workspace.clip(old_clip.id);
        ASSERT_NE(actual, nullptr);
        if (old_clip.end() == 5'000'000) {
            EXPECT_EQ(actual->start, old_clip.start);
            EXPECT_EQ(actual->source_in, old_clip.source_in);
            EXPECT_EQ(actual->source_out, 5'250'000);
            EXPECT_EQ(actual->end(), 5'250'000);
        } else if (old_clip.start == 5'000'000) {
            EXPECT_EQ(actual->start, 4'750'000);
            EXPECT_EQ(actual->source_in, 4'750'000);
            EXPECT_EQ(actual->source_out, old_clip.source_out);
            EXPECT_EQ(actual->end(), old_clip.end());
        } else {
            EXPECT_EQ(*actual, old_clip);
        }
    }
    for (const auto& junction : workspace.transitions()) {
        EXPECT_EQ(junction.in_us, 4'750'000);
        EXPECT_EQ(junction.out_us, 5'250'000);
        EXPECT_EQ(junction.cut_us, 5'000'000);
        EXPECT_EQ(junction.kind, TransitionKind::Crossfade);
    }
    ASSERT_TRUE(workspace.undo());
    EXPECT_EQ(workspace.clips(), before->clips);
    EXPECT_EQ(workspace.transitions(), before->junctions);
    EXPECT_EQ(workspace.snapshot()->result_range, before->result_range);
    EXPECT_GE(workspace.snapshot()->id_high_watermark, before->id_high_watermark);
}

TEST(EditCrossfadeAtCut, OddNtscDurationUsesEarlierFloorAndLaterRemainderOnCommonFrameGrid) {
    Workspace workspace;
    const RationalFps fps{30000, 1001};
    const auto [outgoing, incoming] = AdjacentCutWithHandles(workspace, fps);
    ASSERT_NE(incoming, 0);
    const auto before = workspace.snapshot();
    const auto incoming_end = workspace.clip(incoming)->end();
    const auto cut = *fps.frameTime(300);
    const auto start = *fps.frameTime(293);
    const auto end = *fps.frameTime(308);
    const CrossfadeAtCut command{outgoing, incoming, *fps.frameTime(15)};
    ASSERT_EQ(workspace.apply(command, before->revision).status, CommandStatus::Applied);
    for (const auto& junction : workspace.transitions()) {
        EXPECT_EQ(junction.in_us, start);
        EXPECT_EQ(junction.out_us, end);
        EXPECT_EQ(junction.cut_us, cut);
    }
    EXPECT_EQ(workspace.duration(), *fps.frameTime(600));
    EXPECT_EQ(workspace.clip(incoming)->end(), incoming_end);
}

TEST(EditCrossfadeAtCut, MissingHandlesRefuseInsteadOfClampingOrMovingTheSuffix) {
    Workspace workspace;
    Asset asset;
    asset.path = L"full-source.mkv";
    asset.duration = 10'000'000;
    asset.fps_num = 60;
    const auto source_id = workspace.addAsset(asset);
    ASSERT_TRUE(workspace.insert(source_id));
    const auto outgoing = workspace.selection();
    ASSERT_TRUE(workspace.insert(source_id));
    const auto incoming = workspace.selection();
    const auto before = workspace.snapshot();
    const CrossfadeAtCut command{outgoing, incoming};
    const auto preview = PreviewEdit(*before, command);
    EXPECT_FALSE(preview.valid);
    ASSERT_TRUE(preview.error);
    EXPECT_EQ(preview.error->code, EditErrorCode::SourceBounds);
    const auto result = workspace.apply(command, before->revision);
    EXPECT_EQ(result.status, CommandStatus::Rejected);
    EXPECT_EQ(workspace.clips(), before->clips);
    EXPECT_EQ(workspace.revision(), before->revision);
    EXPECT_EQ(workspace.snapshot()->id_high_watermark, before->id_high_watermark);
}

TEST(EditCrossfadeAtCut, LinkedLocksAndFacingFadesRefuseTheWholeCandidate) {
    Workspace workspace;
    const auto [outgoing, incoming] = AdjacentCutWithHandles(workspace);
    ASSERT_NE(incoming, 0);
    const auto audio_track = workspace.tracks().back().id;
    ASSERT_TRUE(workspace.setTrackLocked(audio_track, true));
    auto before = workspace.snapshot();
    auto preview = PreviewEdit(*before, CrossfadeAtCut{outgoing, incoming});
    ASSERT_TRUE(preview.error);
    EXPECT_EQ(preview.error->code, EditErrorCode::TrackLocked);
    EXPECT_EQ(workspace.apply(CrossfadeAtCut{outgoing, incoming}, before->revision).status, CommandStatus::Rejected);
    EXPECT_EQ(workspace.clips(), before->clips);
    ASSERT_TRUE(workspace.setTrackLocked(audio_track, false));
    ASSERT_EQ(workspace.apply(SetFade{outgoing, FadeEdge::Out, {500'000}}, workspace.revision()).status,
              CommandStatus::Applied);
    before = workspace.snapshot();
    preview = PreviewEdit(*before, CrossfadeAtCut{outgoing, incoming});
    ASSERT_TRUE(preview.error);
    EXPECT_EQ(preview.error->code, EditErrorCode::TransitionBoundaryConflict);
    EXPECT_EQ(workspace.apply(CrossfadeAtCut{outgoing, incoming}, before->revision).status, CommandStatus::Rejected);
    EXPECT_EQ(workspace.clips(), before->clips);
}

TEST(EditCrossfadeAtCut, ImmutableStreamBoundsAreStricterThanContainerDuration) {
    Workspace workspace;
    Asset asset;
    asset.path = L"bounded-source.mkv";
    asset.duration = 10'000'000;
    asset.fps_num = 60;
    asset.audio = false;
    auto descriptor = std::make_shared<ValidatedSourceDescriptor>();
    SourceStreamDescriptor stream;
    stream.ref = {0, exosnap::engine::AudioTrackId{{1}}, 0, exosnap::engine::TimelineStreamKind::Video};
    stream.source_span = {3'000'000, 7'000'000};
    stream.fps = {60, 1};
    descriptor->streams.push_back(stream);
    asset.source_descriptor = descriptor;
    const auto source_id = workspace.addAsset(asset);
    ASSERT_TRUE(workspace.insert(source_id));
    const auto outgoing = workspace.selection();
    ASSERT_TRUE(workspace.insert(source_id));
    const auto incoming = workspace.selection();
    ASSERT_EQ(workspace.clip(outgoing)->source_out, 7'000'000);
    ASSERT_EQ(workspace.clip(incoming)->source_in, 3'000'000);
    const auto before = workspace.snapshot();
    const auto preview = PreviewEdit(*before, CrossfadeAtCut{outgoing, incoming});
    ASSERT_TRUE(preview.error);
    EXPECT_EQ(preview.error->code, EditErrorCode::SourceBounds);
    EXPECT_EQ(workspace.apply(CrossfadeAtCut{outgoing, incoming}, before->revision).status, CommandStatus::Rejected);
    EXPECT_EQ(workspace.clips(), before->clips);
}

TEST(EditClipboard, CapturedSourceVersionCannotPasteAfterAdoptingAnotherVersionWithIdenticalMetadata) {
    Workspace workspace;
    Asset asset;
    asset.path = L"versioned-source.mkv";
    asset.duration = 10'000'000;
    asset.fps_num = 60;
    asset.state = AssetState::Available;
    ASSERT_TRUE(workspace.insert(workspace.addAsset(asset)));
    const auto before = workspace.snapshot();
    const auto copied = CopyClips(*before, {workspace.clips().front().id});
    ASSERT_NE(copied, nullptr);
    auto adopted = *before;
    auto source = std::make_shared<Asset>(*adopted.assets.front());
    ++source->source_version;
    adopted.assets.front() = std::move(source);
    const auto preview = PreviewEdit(adopted, PasteClips{copied, 10'000'000});
    EXPECT_FALSE(preview.valid);
    ASSERT_TRUE(preview.error);
    EXPECT_EQ(preview.error->code, EditErrorCode::SourceChanged);
    EXPECT_EQ(workspace.clips(), before->clips);
}

TEST(EditRemoveOverlap, CompleteLinkedCandidateRestoresTheCutWithoutMovingOuterContentAndUndoesOnce) {
    Workspace workspace;
    const auto [outgoing, incoming] = AdjacentCutWithHandles(workspace);
    ASSERT_NE(incoming, 0);
    ASSERT_TRUE(workspace.insert(workspace.clip(outgoing)->asset, 10'000'000));
    const auto adjacent = workspace.snapshot();
    ASSERT_EQ(workspace.apply(CrossfadeAtCut{outgoing, incoming}, workspace.revision()).status, CommandStatus::Applied);
    const auto before = workspace.snapshot();
    ASSERT_EQ(before->junctions.size(), 2U);
    const RemoveOverlap command{before->junctions.front().id};
    const auto preview = PreviewEdit(*before, command);
    ASSERT_TRUE(preview.valid);
    EXPECT_EQ(workspace.clips(), before->clips);
    EXPECT_EQ(workspace.transitions(), before->junctions);
    ASSERT_EQ(workspace.apply(command, before->revision).status, CommandStatus::Applied);
    EXPECT_EQ(workspace.clips(), preview.clips);
    EXPECT_EQ(workspace.clips(), adjacent->clips);
    EXPECT_TRUE(workspace.transitions().empty());
    EXPECT_EQ(workspace.duration(), 20'000'000);
    EXPECT_EQ(workspace.snapshot()->timeline_end, before->timeline_end);
    ASSERT_TRUE(workspace.undo());
    EXPECT_EQ(workspace.clips(), before->clips);
    EXPECT_EQ(workspace.transitions(), before->junctions);
    ASSERT_TRUE(workspace.redo());
    EXPECT_EQ(workspace.clips(), adjacent->clips);
}

TEST(EditRemoveOverlap, ExplicitHardCutTrimsBothSourceMappingsAndKeepsOuterEndpoints) {
    Workspace workspace;
    const auto [outgoing, incoming] = AdjacentCutWithHandles(workspace);
    ASSERT_NE(incoming, 0);
    ASSERT_EQ(workspace.apply(CrossfadeAtCut{outgoing, incoming}, workspace.revision()).status, CommandStatus::Applied);
    const auto junction_id = workspace.transitions().front().id;
    constexpr Time cut = 4'900'000;
    ASSERT_EQ(workspace.apply(SetHardCut{junction_id, cut}, workspace.revision()).status, CommandStatus::Applied);
    const auto before = workspace.snapshot();
    ASSERT_EQ(workspace.apply(RemoveOverlap{junction_id}, before->revision).status, CommandStatus::Applied);
    ASSERT_TRUE(workspace.transitions().empty());
    for (const auto& old : before->clips) {
        const auto* actual = workspace.clip(old.id);
        ASSERT_NE(actual, nullptr);
        if (old.start == 0) {
            EXPECT_EQ(actual->start, old.start);
            EXPECT_EQ(actual->source_in, old.source_in);
            EXPECT_EQ(actual->end(), cut);
            EXPECT_EQ(actual->source_out, old.source_in + cut - old.start);
        } else {
            EXPECT_EQ(actual->start, cut);
            EXPECT_EQ(actual->end(), old.end());
            EXPECT_EQ(actual->source_in, old.source_in + cut - old.start);
            EXPECT_EQ(actual->source_out, old.source_out);
        }
    }
    EXPECT_EQ(workspace.duration(), 10'000'000);
}

TEST(EditRemoveOverlap, LockedLinkedCounterpartAndInvalidRemainingFadeRefuseTheWholeCandidate) {
    Workspace workspace;
    const auto [outgoing, incoming] = AdjacentCutWithHandles(workspace);
    ASSERT_NE(incoming, 0);
    ASSERT_EQ(workspace.apply(CrossfadeAtCut{outgoing, incoming}, workspace.revision()).status, CommandStatus::Applied);
    const auto junction_id = workspace.transitions().front().id;
    ASSERT_TRUE(workspace.setTrackLocked(workspace.tracks().back().id, true));
    auto before = workspace.snapshot();
    auto preview = PreviewEdit(*before, RemoveOverlap{junction_id});
    ASSERT_TRUE(preview.error);
    EXPECT_EQ(preview.error->code, EditErrorCode::TrackLocked);
    EXPECT_EQ(workspace.apply(RemoveOverlap{junction_id}, before->revision).status, CommandStatus::Rejected);
    EXPECT_EQ(workspace.clips(), before->clips);
    ASSERT_TRUE(workspace.setTrackLocked(workspace.tracks().back().id, false));
    ASSERT_EQ(workspace.apply(SetFade{outgoing, FadeEdge::In, {5'100'000}}, workspace.revision()).status,
              CommandStatus::Applied);
    before = workspace.snapshot();
    preview = PreviewEdit(*before, RemoveOverlap{junction_id});
    EXPECT_FALSE(preview.valid);
    ASSERT_TRUE(preview.error);
    EXPECT_EQ(preview.error->code, EditErrorCode::TransitionBoundaryConflict);
    EXPECT_EQ(workspace.apply(RemoveOverlap{junction_id}, before->revision).status, CommandStatus::Rejected);
    EXPECT_EQ(workspace.clips(), before->clips);
    EXPECT_EQ(workspace.transitions(), before->junctions);
    EXPECT_EQ(workspace.revision(), before->revision);
}

TEST(EditRemoveOverlap, AStoredCrossfadeWithoutAnExplicitCutUsesItsWindowMidpoint) {
    Workspace workspace;
    const auto [outgoing, incoming] = AdjacentCutWithHandles(workspace);
    ASSERT_NE(incoming, 0);
    ASSERT_TRUE(workspace.crossfade(outgoing, incoming, 500'000));
    const auto junction = std::find_if(workspace.transitions().begin(), workspace.transitions().end(),
                                       [outgoing](const auto& value) { return value.outgoing == outgoing; });
    ASSERT_NE(junction, workspace.transitions().end());
    ASSERT_LT(junction->cut_us, junction->in_us);
    const auto midpoint = junction->in_us + (junction->out_us - junction->in_us) / 2;
    const auto preview = PreviewEdit(*workspace.snapshot(), RemoveOverlap{junction->id});
    ASSERT_TRUE(preview.valid);
    for (const auto& clip : preview.clips) {
        if (clip.start == 0)
            EXPECT_EQ(clip.end(), midpoint);
        else
            EXPECT_EQ(clip.start, midpoint);
    }
}

TEST(EditWorkspaceGrid, ChangingOnlyFadeCurvePreservesAnUnchangedLegacyEndpoint) {
    Workspace workspace;
    ASSERT_TRUE(workspace.insert(source(workspace)));
    auto snapshot = *workspace.snapshot();
    snapshot.output_fps = {60, 1};
    ASSERT_TRUE(snapshot.output_fps.valid());
    const auto id = snapshot.clips.front().id;
    snapshot.clips.front().fade_in.duration_us = 10'001;
    const auto original = snapshot.clips;
    const auto preview =
        PreviewEdit(snapshot, SetFade{id, FadeEdge::In, {10'001, {exosnap::engine::CurveKind::Bias, .5}}});
    ASSERT_TRUE(preview.valid);
    ASSERT_EQ(preview.clips.size(), original.size());
    EXPECT_EQ(preview.clips.front().fade_in.duration_us, 10'001);
    EXPECT_EQ(preview.clips.front().fade_in.curve, (CurveSpec{exosnap::engine::CurveKind::Bias, .5}));
    auto expected = original;
    expected.front().fade_in.curve = {exosnap::engine::CurveKind::Bias, .5};
    EXPECT_EQ(preview.clips, expected);
}
namespace {
void ExpectAuthoredGridState(const EditSnapshot& actual, const EditSnapshot& expected) {
    // Publication revisions and allocated-ID high watermarks remain monotone across history.
    EXPECT_EQ(actual.tracks, expected.tracks);
    EXPECT_EQ(actual.clips, expected.clips);
    EXPECT_EQ(actual.junctions, expected.junctions);
    EXPECT_EQ(actual.spacers, expected.spacers);
    EXPECT_EQ(actual.result_range, expected.result_range);
    EXPECT_EQ(actual.timeline_end, expected.timeline_end);
    EXPECT_EQ(actual.output_fps, expected.output_fps);
    EXPECT_EQ(actual.output_width, expected.output_width);
    EXPECT_EQ(actual.output_height, expected.output_height);
    EXPECT_EQ(actual.locked_track_ids, expected.locked_track_ids);
    ASSERT_EQ(actual.assets.size(), expected.assets.size());
    for (size_t index = 0; index < actual.assets.size(); ++index) {
        ASSERT_TRUE(actual.assets[index]);
        ASSERT_TRUE(expected.assets[index]);
        EXPECT_EQ(actual.assets[index]->id, expected.assets[index]->id);
        EXPECT_EQ(actual.assets[index]->path, expected.assets[index]->path);
        EXPECT_EQ(actual.assets[index]->duration, expected.assets[index]->duration);
        EXPECT_EQ(actual.assets[index]->source_version, expected.assets[index]->source_version);
        EXPECT_EQ(actual.assets[index]->source_descriptor, expected.assets[index]->source_descriptor);
    }
}

void PopulateLegacyGridHistory(Workspace& workspace, Id& target, Id& neighbor) {
    Asset asset;
    asset.path = L"legacy-grid-history.mkv";
    asset.duration = 2'002'003;
    asset.width = 1920;
    asset.height = 1080;
    asset.fps_num = 30000;
    asset.fps_den = 1001;
    asset.audio = false;
    const auto asset_id = workspace.addAsset(asset);
    ASSERT_TRUE(workspace.insert(asset_id, 0));
    const auto first = workspace.selection();
    ASSERT_TRUE(workspace.insert(asset_id, 3'003'000));
    const auto second = workspace.selection();
    const auto copied = CopyClips(*workspace.snapshot(), {first, second});
    ASSERT_TRUE(copied);
    auto legacy = std::make_shared<ClipClipboard>(*copied);
    ASSERT_EQ(legacy->clips.size(), 2u);
    auto& edited = legacy->clips[0];
    edited.start = 137;
    edited.source_in = 123;
    edited.source_out = 1'001'001;
    edited.fade_in = {10'001, {exosnap::engine::CurveKind::Bias, .2}};
    edited.fade_out = {20'003, {exosnap::engine::CurveKind::Bias, -.3}};
    auto& other = legacy->clips[1];
    other.start = 3'003'017;
    other.source_in = 321;
    other.source_out = 900'013;
    other.fade_in = {7'001, {exosnap::engine::CurveKind::Bias, -.4}};
    other.fade_out = {9'001, {exosnap::engine::CurveKind::Bias, .6}};
    legacy->duration_us = other.end();
    constexpr Time at = 6'006'000;
    ASSERT_EQ(workspace.apply(PasteClips{legacy, at, 0, false, false}, workspace.revision()).status,
              CommandStatus::Applied);
    for (const auto& clip : workspace.clips()) {
        if (clip.start == at + 137)
            target = clip.id;
        if (clip.start == at + 3'003'017)
            neighbor = clip.id;
    }
    ASSERT_NE(target, 0u);
    ASSERT_NE(neighbor, 0u);
}
} // namespace

TEST(EditTimeAcceptance, ChainedAuthoredNtscMovesPreserveIndexSpansAtLongStartsAndReturnExactly) {
    for (const int numerator : {30000, 60000})
        for (const int64_t initial_index : {0LL, 1LL, 864'000'001LL}) {
            SCOPED_TRACE(::testing::Message() << numerator << "/1001 at " << initial_index);
            const auto golden = [numerator](int64_t index) -> Time { return index * 1'000'000LL * 1001 / numerator; };
            Workspace workspace;
            Asset asset;
            asset.path = L"chained-grid.mkv";
            asset.duration = 5'005'000;
            asset.fps_num = numerator;
            asset.fps_den = 1001;
            asset.audio = false;
            asset.width = 1920;
            asset.height = 1080;
            ASSERT_TRUE(workspace.insert(workspace.addAsset(asset), golden(initial_index)));
            const auto id = workspace.selection();
            constexpr int64_t span = 11;
            const auto duration = golden(initial_index + span) - golden(initial_index);
            ASSERT_EQ(
                workspace.apply(Trim{id, TrimEdge::Right, duration - asset.duration}, workspace.revision()).status,
                CommandStatus::Applied);
            const auto original = workspace.snapshot();
            int64_t index = initial_index;
            for (const int direction : {1, -1})
                for (int step = 0; step < 128; ++step) {
                    const auto next = index + direction;
                    const Move command{{id}, golden(next) - golden(index), {}, {}, false, false};
                    const auto before = workspace.snapshot();
                    const auto preview = PreviewEdit(*before, command);
                    ASSERT_TRUE(preview.valid);
                    ASSERT_EQ(workspace.apply(command, before->revision).status, CommandStatus::Applied);
                    ASSERT_TRUE(workspace.clip(id));
                    EXPECT_EQ(workspace.clip(id)->start, golden(next));
                    EXPECT_EQ(workspace.clip(id)->end(), golden(next + span));
                    EXPECT_EQ(workspace.clip(id)->source_in, 0);
                    EXPECT_EQ(workspace.clip(id)->source_out, golden(next + span) - golden(next));
                    EXPECT_EQ(workspace.clips(), preview.clips);
                    index = next;
                }
            EXPECT_EQ(index, initial_index);
            ExpectAuthoredGridState(*workspace.snapshot(), *original);
        }
}

TEST(EditTimeAcceptance, NtscProjectEndAndOverflowPreserveExactAuthoredStateAndHistory) {
    Workspace workspace;
    Asset asset;
    asset.path = L"project-end-grid.mkv";
    asset.duration = 1'001'000;
    asset.fps_num = 30000;
    asset.fps_den = 1001;
    asset.audio = false;
    ASSERT_TRUE(workspace.insert(workspace.addAsset(asset)));
    const auto id = workspace.selection();
    const RationalFps fps{30000, 1001};
    const auto end = fps.frameTime(40).value();
    ASSERT_EQ(workspace.apply(SetTimelineEnd{{TimelineEndMode::FixedEnd, end}}, workspace.revision()).status,
              CommandStatus::Applied);
    ASSERT_EQ(workspace.apply(Move{{id}, fps.frameTime(10).value()}, workspace.revision()).status,
              CommandStatus::Applied);
    EXPECT_EQ(workspace.clip(id)->end(), end);
    EXPECT_EQ(workspace.duration(), end);
    workspace.seek(end + 1);
    EXPECT_EQ(workspace.playhead(), end);
    EXPECT_EQ(fps.previousFrame(workspace.playhead()), fps.frameTime(39));
    EXPECT_EQ(fps.nearestFrame(workspace.playhead()), end);
    const auto before = workspace.snapshot();
    const auto bytes = workspace.historyBytes();
    const auto undo = workspace.canUndo();
    const auto redo = workspace.canRedo();
    for (const auto delta : {(std::numeric_limits<Time>::min)(), (std::numeric_limits<Time>::max)()}) {
        const Move command{{id}, delta, {}, {}, false, false};
        EXPECT_FALSE(PreviewEdit(*before, command).valid);
        EXPECT_EQ(workspace.apply(command, before->revision).status, CommandStatus::Rejected);
        ExpectAuthoredGridState(*workspace.snapshot(), *before);
        EXPECT_EQ(workspace.revision(), before->revision);
        EXPECT_EQ(workspace.historyBytes(), bytes);
        EXPECT_EQ(workspace.canUndo(), undo);
        EXPECT_EQ(workspace.canRedo(), redo);
    }
}

TEST(EditLegacyAcceptance, MoveTrimSplitAndFadeKeepUnrelatedLegacyValuesThroughPreviewUndoAndRedo) {
    for (int operation = 0; operation < 4; ++operation) {
        SCOPED_TRACE(operation);
        Workspace workspace;
        Id target = 0;
        Id neighbor = 0;
        PopulateLegacyGridHistory(workspace, target, neighbor);
        ASSERT_NE(target, 0u);
        ASSERT_NE(neighbor, 0u);
        const auto before = workspace.snapshot();
        const auto old = *workspace.clip(target);
        const auto unrelated = *workspace.clip(neighbor);
        EXPECT_EQ(old.fade_in.duration_us, 10'001);
        EXPECT_EQ(unrelated.fade_out.duration_us, 9'001);
        EXPECT_NE(before->output_fps.nearestFrame(old.start), old.start);
        const EditCommand command = operation == 0   ? EditCommand{Move{{target}, 40'000, {}, {}, false, false}}
                                    : operation == 1 ? EditCommand{Trim{target, TrimEdge::Right, -100'000}}
                                    : operation == 2
                                        ? EditCommand{Split{{target}, old.start + 400'000, false}}
                                        : EditCommand{SetFade{target, FadeEdge::In, {40'000, old.fade_in.curve}}};
        const auto candidate = PreviewEdit(*before, command);
        ASSERT_TRUE(candidate.valid);
        ExpectAuthoredGridState(*workspace.snapshot(), *before);
        ASSERT_EQ(workspace.apply(command, before->revision).status, CommandStatus::Applied);
        const auto committed = workspace.snapshot();
        EXPECT_EQ(committed->clips, candidate.clips);
        EXPECT_EQ(committed->tracks, candidate.tracks);
        EXPECT_EQ(committed->junctions, candidate.junctions);
        EXPECT_EQ(committed->spacers, candidate.spacers);
        EXPECT_EQ(*workspace.clip(neighbor), unrelated);
        const auto edited = *workspace.clip(target);
        const auto fps = before->output_fps;
        if (operation == 0) {
            EXPECT_EQ(edited.start, fps.nearestFrame(old.start + 40'000));
            EXPECT_EQ(edited.duration(), old.duration());
            EXPECT_EQ(edited.source_in, old.source_in);
            EXPECT_EQ(edited.source_out, old.source_out);
            EXPECT_EQ(edited.fade_in, old.fade_in);
            EXPECT_EQ(edited.fade_out, old.fade_out);
        } else if (operation == 1) {
            EXPECT_EQ(edited.start, old.start);
            EXPECT_EQ(edited.end(), fps.nearestFrame(old.end() - 100'000));
            EXPECT_EQ(edited.source_in, old.source_in);
            EXPECT_EQ(edited.fade_in, old.fade_in);
            EXPECT_EQ(edited.fade_out, old.fade_out);
        } else if (operation == 2) {
            const auto cut = fps.nearestFrame(old.start + 400'000).value();
            EXPECT_EQ(edited.start, old.start);
            EXPECT_EQ(edited.end(), cut);
            EXPECT_EQ(edited.source_in, old.source_in);
            EXPECT_EQ(edited.fade_in, old.fade_in);
            EXPECT_EQ(edited.fade_out.duration_us, 0);
            const auto right =
                std::find_if(committed->clips.begin(), committed->clips.end(),
                             [cut, &old](const auto& clip) { return clip.start == cut && clip.asset == old.asset; });
            ASSERT_NE(right, committed->clips.end());
            EXPECT_EQ(right->end(), old.end());
            EXPECT_EQ(right->source_in, edited.source_out);
            EXPECT_EQ(right->source_out, old.source_out);
            EXPECT_EQ(right->fade_in.duration_us, 0);
            EXPECT_EQ(right->fade_out, old.fade_out);
        } else {
            EXPECT_EQ(edited.start, old.start);
            EXPECT_EQ(edited.source_in, old.source_in);
            EXPECT_EQ(edited.source_out, old.source_out);
            EXPECT_EQ(edited.start + edited.fade_in.duration_us, fps.nearestFrame(old.start + 40'000));
            EXPECT_EQ(edited.fade_out, old.fade_out);
        }
        ASSERT_TRUE(workspace.undo());
        ExpectAuthoredGridState(*workspace.snapshot(), *before);
        ASSERT_TRUE(workspace.redo());
        ExpectAuthoredGridState(*workspace.snapshot(), *committed);
    }
}
