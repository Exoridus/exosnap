#include "models/EditExportProfile.h"
#include "models/EditSourceImport.h"
#include "models/EditWorkspace.h"
#include <algorithm>
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

TEST(EditSourceImport, ResolvingAnAmbiguousMixEnablesOnlyTheChosenStreams) {
    const auto metadata = ProbedSource({2, 1});
    Workspace workspace;
    Asset asset;
    asset.path = L"probed.mkv";
    asset.duration = metadata.duration_us;
    auto imported = DescribeSourceImport(metadata);
    asset.source_descriptor = imported.descriptor;
    asset.input_mix_selection = imported.input_mix;
    asset.mix_decision_required = imported.mix_decision_required;
    const auto id = workspace.addAsset(std::move(asset));
    ASSERT_TRUE(workspace.insert(id, 0));
    ASSERT_TRUE(workspace.resolveInputMix(id, {exosnap::engine::AudioTrackId{{11}}}));
    EXPECT_FALSE(workspace.asset(id)->mix_decision_required);
    std::vector<bool> enabled;
    for (const auto& clip : workspace.clips())
        if (clip.stream && clip.stream->kind == exosnap::engine::TimelineStreamKind::Audio)
            enabled.push_back(clip.enabled);
    EXPECT_EQ(enabled, (std::vector<bool>{false, true}));
    ASSERT_TRUE(workspace.undo());
    for (const auto& clip : workspace.clips())
        if (clip.stream && clip.stream->kind == exosnap::engine::TimelineStreamKind::Audio)
            EXPECT_FALSE(clip.enabled);
}

TEST(EditSourceImport, UnboundBytesOrUnknownLayoutsKeepTheLinkedImport) {
    auto unbound = ProbedSource({2});
    unbound.content_identity_valid = false;
    EXPECT_FALSE(DescribeSourceImport(unbound).descriptor);
    EXPECT_FALSE(DescribeSourceImport(ProbedSource({6})).descriptor);
}
