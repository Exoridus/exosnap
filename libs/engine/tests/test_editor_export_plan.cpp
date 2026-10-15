#include <exosnap/engine/edit_export_plan.h>
#include <exosnap/engine/edit_timeline.h>
#include <exosnap/engine/edit_timeline_render.h>
#include <exosnap/engine/edit_types.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <utility>

namespace exosnap::engine {
namespace {
struct Fixture {
    TimelineRenderSnapshot snapshot;
    EditExportIntent intent;
    EditExportFacts facts;
    Fixture() {
        auto& t = snapshot.timeline;
        t.duration_us = 4'000'000;
        t.revision = 7;
        t.tracks = {{1, false}, {2, true}, {3, true}, {4, true}, {5, true}};
        TimelineMediaSource source;
        source.id = 1;
        source.path = L"fixture.mkv";
        source.metadata.duration_us = t.duration_us;
        source.metadata.width = 64;
        source.metadata.height = 64;
        source.metadata.fps_num = 30;
        source.metadata.fps_den = 1;
        source.metadata.render_color_supported = true;
        source.metadata.content_identity_valid = true;
        source.metadata.content_sha256[0] = 1;
        source.metadata.audio_metadata.actual_tracks.resize(4);
        snapshot.sources.push_back(std::move(source));
        EditOriginalGroup group;
        group.group = 1;
        for (uint32_t index = 0; index < 5; ++index) {
            TimelineStreamRef stream;
            stream.asset_id = 1;
            stream.container_index = index;
            stream.stream_identity.bytes[0] = static_cast<uint8_t>(index + 1);
            stream.kind = index ? TimelineStreamKind::Audio : TimelineStreamKind::Video;
            group.streams.push_back(stream);
            TimelineClip clip{index + 1, 1, 1, index != 0, 0, 0, t.duration_us};
            clip.track = index + 1;
            clip.stream = stream;
            clip.enabled = clip.original_enabled = index < 3;
            t.clips.push_back(clip);
        }
        facts.original_groups.push_back(std::move(group));
        facts.video_copy_qualified = true;
        facts.original_copy_qualified = true;
        facts.stereo_render_available = true;
        facts.input_mix_validated = true;
        facts.destination_admitted = true;
        intent.destination = L"result.mkv";
    }
    std::shared_ptr<const EditExportPlan> prepare() const {
        return PrepareEditExport(snapshot, intent, facts);
    }
};

TEST(EditorExportPlan, FinishedVideoCopyAudioMix) {
    Fixture f;
    auto plan = f.prepare();
    ASSERT_TRUE(plan->ready());
    EXPECT_EQ(plan->intent.mode, EditExportMode::FinishedStereo);
    EXPECT_EQ(plan->video_path, EditVideoPath::Copy);
    EXPECT_EQ(plan->audio_path, EditAudioPath::RenderStereo);
    EXPECT_EQ(plan->output_audio_streams, 1u);
}

TEST(EditorExportPlan, OriginalFourTracks) {
    Fixture f;
    f.intent.mode = EditExportMode::OriginalStreams;
    auto plan = f.prepare();
    ASSERT_TRUE(plan->ready());
    EXPECT_EQ(plan->video_path, EditVideoPath::Copy);
    EXPECT_EQ(plan->audio_path, EditAudioPath::CopyOriginalStreams);
    EXPECT_EQ(plan->output_audio_streams, 4u);
}

TEST(EditorExportPlan, GainEditBlocksOriginalRecipe) {
    Fixture f;
    f.intent.mode = EditExportMode::OriginalStreams;
    f.snapshot.timeline.clips[1].gain_db = -6;
    const auto plan = f.prepare();
    EXPECT_FALSE(plan->ready());
    EXPECT_EQ(plan->video_path, EditVideoPath::Unsupported);
    EXPECT_EQ(plan->intent.mode, EditExportMode::OriginalStreams);
}

TEST(EditorExportPlan, IndependentAudioMoveBlocksOriginalRecipe) {
    Fixture f;
    f.intent.mode = EditExportMode::OriginalStreams;
    f.snapshot.timeline.clips[1].start_us = 100;
    EXPECT_FALSE(f.prepare()->ready());
}

TEST(EditorExportPlan, HdrCopyIndependentOfRender) {
    Fixture f;
    f.intent.mode = EditExportMode::OriginalStreams;
    f.snapshot.sources[0].metadata.render_color_supported = false;
    f.facts.video_render_available = false;
    ASSERT_TRUE(f.prepare()->ready());
    EXPECT_EQ(f.prepare()->video_path, EditVideoPath::Copy);
}

TEST(EditorExportPlan, Mp4IntentRetained) {
    Fixture f;
    f.intent.container = EditExportContainer::Mp4;
    f.intent.audio_profile = EditAudioProfile::AacLc192Stereo;
    const auto plan = f.prepare();
    EXPECT_FALSE(plan->ready());
    EXPECT_EQ(plan->intent.container, EditExportContainer::Mp4);
    EXPECT_EQ(plan->intent.audio_profile, EditAudioProfile::AacLc192Stereo);
}

TEST(EditorExportPlan, BoundaryConsentHash) {
    Fixture f;
    f.facts.deviations.push_back({1, 10, 4'000'000, 0, 4'000'000, true});
    auto plan = f.prepare();
    EditBoundaryConsent consent{plan->hash, plan->facts.deviations};
    ASSERT_TRUE(AcceptsBoundaryConsent(*plan, consent));
    f.snapshot.timeline.result_in_us = 1;
    EXPECT_FALSE(AcceptsBoundaryConsent(*f.prepare(), consent));
    f.snapshot.timeline.result_in_us = 0;
    f.snapshot.sources[0].metadata.content_sha256[0] = 2;
    EXPECT_FALSE(AcceptsBoundaryConsent(*f.prepare(), consent));
}

TEST(EditorExportPlan, MonitoringIsExcludedAndInputSnapshotIsFrozen) {
    Fixture f;
    f.snapshot.sources[0].monitor_selection.emplace();
    auto plan = f.prepare();
    EXPECT_FALSE(plan->snapshot.sources[0].monitor_selection.has_value());
    const auto hash = plan->hash;
    f.snapshot.sources[0].monitor_selection->overlap_confirmed = true;
    EXPECT_EQ(f.prepare()->hash, hash);
    f.snapshot.timeline.clips[1].gain_db = -12;
    EXPECT_EQ(plan->snapshot.timeline.clips[1].gain_db, 0);
    EXPECT_NE(f.prepare()->hash, hash);
}

TEST(EditorExportPlan, NoFalseWholeFileLossless) {
    Fixture f;
    const auto plan = f.prepare();
    EXPECT_EQ(plan->video_path, EditVideoPath::Copy);
    EXPECT_EQ(plan->audio_path, EditAudioPath::RenderStereo);
    EXPECT_NE(plan->audio_path, EditAudioPath::CopyOriginalStreams);
}

TEST(EditorExportPlan, AuthoredGainNeverUsesStereoCopy) {
    Fixture f;
    f.snapshot.timeline.clips[2].enabled = false;
    f.snapshot.timeline.clips[1].gain_db = -6;
    f.facts.stereo_copy_qualified = true;
    EXPECT_EQ(f.prepare()->audio_path, EditAudioPath::RenderStereo);
}

// Stacks `count` simultaneously visible video clips, each on its own track.
void StackVideo(Fixture& f, uint32_t count) {
    f.facts.video_copy_qualified = false;
    f.facts.video_render_available = true;
    auto& t = f.snapshot.timeline;
    for (uint32_t index = 1; index < count; ++index) {
        const uint64_t track = 100 + index;
        t.tracks.insert(t.tracks.begin(), {track, false});
        TimelineClip clip{200 + index, 1, 0, false, 0, 0, t.duration_us};
        clip.track = track;
        t.clips.push_back(clip);
    }
}

TEST(EditorExportPlan, VideoLayerAdmissionIsDecidedWhilePlanning) {
    Fixture admitted;
    StackVideo(admitted, kMaxTimelineVideoSources);
    const auto within = AssessTimelineVideoAdmission(admitted.snapshot, 0, admitted.snapshot.timeline.duration_us);
    EXPECT_EQ(within.peak_sources, kMaxTimelineVideoSources);
    EXPECT_TRUE(within.admitted);
    EXPECT_EQ(admitted.prepare()->video_path, EditVideoPath::Render);
    EXPECT_TRUE(admitted.prepare()->ready());

    Fixture excessive;
    StackVideo(excessive, kMaxTimelineVideoSources + 1);
    EXPECT_FALSE(AssessTimelineVideoAdmission(excessive.snapshot, 0, excessive.snapshot.timeline.duration_us).admitted);
    const auto refused = excessive.prepare();
    ASSERT_FALSE(refused->ready());
    EXPECT_TRUE(std::any_of(refused->issues.begin(), refused->issues.end(), [](const auto& issue) {
        return issue.code == EditExportIssueCode::ResourceLimitExceeded && issue.scope == EditExportIssueScope::Video;
    }));
    EXPECT_EQ(ClassifyEditExport(excessive.snapshot, {TimelineExportEligibility::RenderRequired, {}}, false).path,
              EditExportPath::Unsupported);

    // Clips that only touch end to start are never visible together.
    Fixture sequential;
    auto& t = sequential.snapshot.timeline;
    t.clips.front().enabled = false;
    for (uint32_t index = 1; index <= kMaxTimelineVideoSources + 1; ++index) {
        TimelineClip clip{300 + index, 1, 0, false, index * 100'000, 0, 100'000};
        clip.track = 1;
        t.clips.push_back(clip);
    }
    EXPECT_EQ(AssessTimelineVideoAdmission(sequential.snapshot, 0, t.duration_us).peak_sources, 1u);

    // A disabled layer or one outside the exported span costs nothing.
    excessive.snapshot.timeline.clips.back().enabled = false;
    EXPECT_TRUE(AssessTimelineVideoAdmission(excessive.snapshot, 0, excessive.snapshot.timeline.duration_us).admitted);
}

TEST(EditorExportPlan, VideoPixelBudgetBoundsLargeSources) {
    Fixture f;
    f.snapshot.sources.front().metadata.width = 7680;
    f.snapshot.sources.front().metadata.height = 4320;
    StackVideo(f, 2);
    const auto assessed = AssessTimelineVideoAdmission(f.snapshot, 0, f.snapshot.timeline.duration_us);
    EXPECT_EQ(assessed.peak_pixels, uint64_t{2} * 7680 * 4320);
    EXPECT_FALSE(assessed.admitted);
}
} // namespace
} // namespace exosnap::engine
