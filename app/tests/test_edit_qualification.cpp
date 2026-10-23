#include "models/EditWorkspace.h"
#include "quick/ExoSnap/Quick/QuickAutoEditHarness.h"

#include <algorithm>
#include <chrono>
#include <gtest/gtest.h>
#include <map>
#include <sstream>

namespace {
using namespace exosnap::edit;
using Clock = std::chrono::steady_clock;
constexpr Time kTwoHours = 7'200'000'000;
constexpr Time kFourHours = 14'400'000'000;
constexpr Time kOverlap = 250'000;
constexpr size_t kGroups = 1000;

::testing::AssertionResult Populate(Workspace& workspace, Time duration) {
    Asset source;
    source.path = L"component-structure.mkv";
    source.duration = 60'000'000;
    source.fps_num = 60;
    source.width = 2560;
    source.height = 1440;
    const auto asset = workspace.addAsset(std::move(source));
    if (workspace.apply(AddAssetGroup{asset, 0}, workspace.revision()).status != CommandStatus::Applied ||
        workspace.clips().empty())
        return ::testing::AssertionFailure() << "output format initialization failed";
    if (workspace.apply(LiftClips{{workspace.clips().front().id}}, workspace.revision()).status !=
        CommandStatus::Applied)
        return ::testing::AssertionFailure() << "format seed removal failed";

    auto clipboard = std::make_shared<ClipClipboard>();
    clipboard->assets.push_back(std::make_shared<const Asset>(*workspace.asset(asset)));
    clipboard->duration_us = duration;
    for (Id lane = 0; lane < 8; ++lane) {
        clipboard->tracks.push_back({100 + lane * 2, TrackType::Video});
        clipboard->tracks.push_back({101 + lane * 2, TrackType::Audio});
    }
    const auto step = duration / kGroups;
    for (Id group = 0; group < kGroups; ++group) {
        const auto lane = group / 125;
        const auto start = static_cast<Time>(group) * step;
        const auto length = step + (group < 100 ? kOverlap : 0);
        for (Id audio = 0; audio < 2; ++audio) {
            Clip clip;
            clip.id = 20'000 + group * 2 + audio;
            clip.asset = asset;
            clip.track = 100 + lane * 2 + audio;
            clip.group = 10'000 + group;
            clip.start = start;
            clip.source_out = length;
            clipboard->clips.push_back(clip);
            if (group < 100) {
                Transition junction;
                junction.id = 30'000 + group * 2 + audio;
                junction.outgoing = clip.id;
                junction.incoming = clip.id + 2;
                junction.track = clip.track;
                junction.duration = kOverlap;
                junction.in_us = start + step;
                junction.out_us = junction.in_us + kOverlap;
                junction.cut_us = junction.in_us + kOverlap / 2;
                junction.incoming_anchor_us = junction.in_us;
                clipboard->junctions.push_back(junction);
            }
        }
    }
    const auto result = workspace.apply(PasteClips{clipboard, 0, 0, false, false}, workspace.revision());
    if (result.status != CommandStatus::Applied)
        return ::testing::AssertionFailure()
               << "paste failed with error " << (result.error ? static_cast<int>(result.error->code) : -1);
    return ::testing::AssertionSuccess();
}

void CheckStructure(const Workspace& workspace, Time duration) {
    const auto snapshot = workspace.snapshot();
    ASSERT_EQ(snapshot->tracks.size(), 16u);
    ASSERT_EQ(snapshot->clips.size(), 2000u);
    ASSERT_EQ(snapshot->junctions.size(), 200u);
    ASSERT_EQ(snapshot->assets.size(), 1u);
    EXPECT_EQ(snapshot->output_fps, (RationalFps{60, 1}));
    EXPECT_EQ(SnapshotDuration(*snapshot), duration);
    std::map<Id, std::vector<const Clip*>> groups;
    for (const auto& clip : snapshot->clips) {
        EXPECT_GE(clip.source_in, 0);
        EXPECT_LE(clip.source_out, snapshot->assets.front()->duration);
        EXPECT_LE(clip.end(), duration);
        groups[clip.group].push_back(&clip);
    }
    ASSERT_EQ(groups.size(), kGroups);
    for (const auto& [id, members] : groups) {
        ASSERT_EQ(members.size(), 2u) << id;
        EXPECT_EQ(members[0]->start, members[1]->start);
        EXPECT_EQ(members[0]->source_in, members[1]->source_in);
        EXPECT_EQ(members[0]->source_out, members[1]->source_out);
        EXPECT_NE(members[0]->track, members[1]->track);
    }
    const auto projected = RenderTimeline(*snapshot);
    EXPECT_EQ(projected.duration_us, duration);
    EXPECT_EQ(projected.clips.size(), 2000u);
    EXPECT_EQ(projected.tracks.size(), 16u);
    EXPECT_EQ(projected.junctions.size(), 200u);
    for (const auto position : {Time{0}, duration / 2, duration - 1, duration}) {
        const auto active = std::count_if(projected.clips.begin(), projected.clips.end(), [&](const auto& clip) {
            return clip.start_us <= position && clip.start_us + clip.source_out_us - clip.source_in_us > position;
        });
        EXPECT_EQ(active, position == duration ? 0 : 2) << position;
        const auto evaluated = exosnap::engine::EvaluateTimeline(projected, position);
        EXPECT_TRUE(evaluated.error.empty()) << evaluated.error;
        EXPECT_EQ(evaluated.video.size(), position == duration ? 0u : 1u);
        EXPECT_EQ(evaluated.audio.size(), position == duration ? 0u : 1u);
    }
    const auto overlap = exosnap::engine::EvaluateTimeline(projected, duration / kGroups + kOverlap / 2);
    EXPECT_TRUE(overlap.error.empty()) << overlap.error;
    EXPECT_EQ(overlap.video.size(), 2u);
    EXPECT_EQ(overlap.audio.size(), 2u);
    for (const auto fps : {RationalFps{60, 1}, RationalFps{30'000, 1001}}) {
        const auto frame = fps.frameIndex(duration);
        ASSERT_TRUE(frame);
        const auto before = fps.frameTime(*frame);
        const auto after = fps.frameTime(*frame + 1);
        ASSERT_TRUE(before);
        ASSERT_TRUE(after);
        EXPECT_LE(*before, duration);
        EXPECT_GT(*after, duration);
    }
    const auto end_sample = AudioSampleGrid::sampleIndex(duration);
    ASSERT_TRUE(end_sample);
    EXPECT_EQ(AudioSampleGrid::sampleTime(*end_sample), duration);
}

int64_t ElapsedNs(Clock::time_point begin) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
}

std::vector<size_t> AudioClipIndices(const EditSnapshot& snapshot) {
    std::vector<size_t> result;
    for (size_t index = 0; index < snapshot.clips.size(); ++index) {
        const auto track = std::find_if(snapshot.tracks.begin(), snapshot.tracks.end(), [&](const auto& candidate) {
            return candidate.id == snapshot.clips[index].track;
        });
        if (track != snapshot.tracks.end() && track->type == TrackType::Audio)
            result.push_back(index);
    }
    return result;
}

std::string SamplesJson(const std::vector<int64_t>& samples) {
    auto sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&](size_t percent) { return sorted[(sorted.size() * percent + 99) / 100 - 1]; };
    std::ostringstream json;
    json << "{\"unit\":\"ns\",\"count\":" << samples.size() << ",\"p50\":" << percentile(50)
         << ",\"p95\":" << percentile(95) << ",\"maximum\":" << sorted.back();
    if (samples.size() >= 1000)
        json << ",\"p99\":" << percentile(99);
    json << ",\"samples\":[";
    for (size_t i = 0; i < samples.size(); ++i) {
        if (i)
            json << ',';
        json << samples[i];
    }
    return json.str() + "]}";
}
} // namespace

TEST(EditQualificationStructure, TwoHourModelPreservesLinkedGeometryAndPublishedSnapshots) {
    Workspace workspace;
    ASSERT_TRUE(Populate(workspace, kTwoHours));
    CheckStructure(workspace, kTwoHours);
    const auto before = workspace.snapshot();
    const auto audio = AudioClipIndices(*before);
    ASSERT_EQ(audio.size(), kGroups);
    const auto index = audio.front();
    const auto first = before->clips[index].id;
    ASSERT_EQ(workspace.apply(SetGain{GainTarget::Clip, first, -6}, workspace.revision()).status,
              CommandStatus::Applied);
    const auto after = workspace.snapshot();
    EXPECT_NE(after, before);
    EXPECT_EQ(before->clips[index].gain_db, 0);
    EXPECT_EQ(after->clips[index].gain_db, -6);
    EXPECT_EQ(after->revision, before->revision + 1);
    ASSERT_TRUE(workspace.undo());
    EXPECT_EQ(workspace.snapshot()->clips, before->clips);
    EXPECT_EQ(workspace.snapshot()->junctions, before->junctions);
    ASSERT_TRUE(workspace.redo());
    EXPECT_EQ(workspace.snapshot()->clips, after->clips);
    EXPECT_EQ(workspace.snapshot()->junctions, after->junctions);
}

TEST(EditQualificationStructure, FourHourModelUsesBoundedSourceSpansAndExactEndpointArithmetic) {
    Workspace workspace;
    ASSERT_TRUE(Populate(workspace, kFourHours));
    CheckStructure(workspace, kFourHours);
    workspace.seek(kFourHours);
    EXPECT_EQ(workspace.playhead(), kFourHours);
    workspace.seek(kFourHours + 1);
    EXPECT_EQ(workspace.playhead(), kFourHours);
    workspace.seek(kFourHours - 1);
    EXPECT_EQ(workspace.playhead(), kFourHours - 1);
}

TEST(EditQualificationCharacterization, ThreeRunsRetainRawSelectionLookupAndPublicationSamples) {
#ifdef NDEBUG
    RecordProperty("build_configuration", "Release");
#else
    RecordProperty("build_configuration", "Debug");
#endif
    RecordProperty("scope", "model-component; synthetic source mapping; no decoding, GUI feedback, or native endpoint");
    RecordProperty("clock", "std::chrono::steady_clock; ns; per-operation timer overhead included");
    RecordProperty("protocol",
                   "3 runs; each 1000 select, 1000 cached snapshot, 1000 seek; publication counts 67,67,66");
    for (size_t run = 0; run < 3; ++run) {
        SCOPED_TRACE(run);
        Workspace workspace;
        ASSERT_TRUE(Populate(workspace, kTwoHours));
        const auto snapshot = workspace.snapshot();
        const auto audio = AudioClipIndices(*snapshot);
        ASSERT_EQ(audio.size(), kGroups);
        const auto history_bytes = workspace.historyBytes();
        std::vector<int64_t> selection, cached_snapshot, seek, publication;
        selection.reserve(1000);
        cached_snapshot.reserve(1000);
        seek.reserve(1000);
        publication.reserve(67);
        for (size_t event = 0; event < 1000; ++event) {
            const auto index = (event * 37 + run * 101) % snapshot->clips.size();
            auto begin = Clock::now();
            workspace.select(snapshot->clips[index].id);
            selection.push_back(ElapsedNs(begin));
            ASSERT_EQ(workspace.selection(), snapshot->clips[index].id);
            begin = Clock::now();
            const auto current = workspace.snapshot();
            cached_snapshot.push_back(ElapsedNs(begin));
            ASSERT_EQ(current, snapshot);
            const auto position = static_cast<Time>(event) * kTwoHours / 999;
            begin = Clock::now();
            workspace.seek(position);
            seek.push_back(ElapsedNs(begin));
            ASSERT_EQ(workspace.playhead(), position);
        }
        EXPECT_EQ(workspace.revision(), snapshot->revision);
        EXPECT_EQ(workspace.historyBytes(), history_bytes);
        for (size_t edit = 0; edit < (run < 2 ? 67u : 66u); ++edit) {
            const auto index = audio[(edit * 37 + run * 101) % audio.size()];
            const auto gain = edit % 2 ? -3.0 : -6.0;
            const auto before = workspace.snapshot();
            const auto begin = Clock::now();
            const auto result =
                workspace.apply(SetGain{GainTarget::Clip, snapshot->clips[index].id, gain}, workspace.revision());
            const auto published = workspace.snapshot();
            publication.push_back(ElapsedNs(begin));
            ASSERT_EQ(result.status, CommandStatus::Applied);
            ASSERT_NE(before, published);
            EXPECT_EQ(before->clips[index].gain_db, 0);
            EXPECT_EQ(published->clips[index].gain_db, gain);
            EXPECT_EQ(published->clips.size(), 2000u);
            EXPECT_EQ(published->junctions.size(), 200u);
        }
        const auto key = "run_" + std::to_string(run + 1) + '_';
        RecordProperty(key + "selection", SamplesJson(selection));
        RecordProperty(key + "cached_snapshot", SamplesJson(cached_snapshot));
        RecordProperty(key + "seek", SamplesJson(seek));
        RecordProperty(key + "publication", SamplesJson(publication));
        EXPECT_EQ(snapshot->clips.front().gain_db, 0);
    }
}

TEST(EditQualificationMeasurement, InsufficientSamplesNeverPublishAQualificationP99) {
    for (const auto count : {size_t{0}, size_t{1}, size_t{999}}) {
        const auto result = exosnap::quick::AutoEditLatencySummary(std::vector<double>(count, 7.0));
        EXPECT_EQ(result.value("samples").toInt(), static_cast<int>(count));
        EXPECT_FALSE(result.contains("p99")) << count;
        EXPECT_EQ(result.value("minimumP99Samples").toInt(), 1000);
        EXPECT_EQ(result.value("p99Status").toString(), QStringLiteral("insufficient_samples"));
        if (!count) {
            EXPECT_FALSE(result.contains("p50"));
            EXPECT_FALSE(result.contains("p95"));
            EXPECT_FALSE(result.contains("maximum"));
        } else {
            EXPECT_EQ(result.value("p50").toDouble(), 7.0);
            EXPECT_EQ(result.value("p95").toDouble(), 7.0);
            EXPECT_EQ(result.value("maximum").toDouble(), 7.0);
        }
    }
}

TEST(EditQualificationMeasurement, SufficientSamplesRetainNearestRankAndWorstObservation) {
    std::vector<double> samples;
    for (size_t value = 1000; value > 0; --value)
        samples.push_back(static_cast<double>(value));
    const auto result = exosnap::quick::AutoEditLatencySummary(samples);
    EXPECT_EQ(samples.front(), 1000);
    EXPECT_EQ(result.value("samples").toInt(), 1000);
    EXPECT_EQ(result.value("p50").toDouble(), 500);
    EXPECT_EQ(result.value("p95").toDouble(), 950);
    EXPECT_EQ(result.value("p99").toDouble(), 990);
    EXPECT_EQ(result.value("maximum").toDouble(), 1000);
    EXPECT_EQ(result.value("p99Status").toString(), QStringLiteral("available"));
}

TEST(EditQualificationCharacterization, PublicationStagesAreMeasuredSeparatelyFromCachedAcquisition) {
#ifdef NDEBUG
    RecordProperty("build_configuration", "Release");
#else
    RecordProperty("build_configuration", "Debug");
#endif
    RecordProperty("scope", "model component; 30 operations per stage; no GUI/native qualification");
    RecordProperty("protocol", "3 fresh two-hour models; each 10 distinct clip gains; fixed permutation stride 37");
    for (size_t run = 0; run < 3; ++run) {
        Workspace workspace;
        ASSERT_TRUE(Populate(workspace, kTwoHours));
        const auto original = workspace.snapshot();
        const auto audio = AudioClipIndices(*original);
        ASSERT_EQ(audio.size(), kGroups);
        std::vector<int64_t> preview, command, snapshot, projection;
        for (size_t event = 0; event < 10; ++event) {
            const auto index = audio[(event * 37 + run * 101) % audio.size()];
            const auto before = workspace.snapshot();
            const SetGain gain{GainTarget::Clip, before->clips[index].id, -6};
            auto begin = Clock::now();
            const auto candidate = PreviewEdit(*before, gain);
            preview.push_back(ElapsedNs(begin));
            ASSERT_TRUE(candidate.valid);
            begin = Clock::now();
            const auto result = workspace.apply(gain, workspace.revision());
            command.push_back(ElapsedNs(begin));
            ASSERT_EQ(result.status, CommandStatus::Applied);
            begin = Clock::now();
            const auto published = workspace.snapshot();
            snapshot.push_back(ElapsedNs(begin));
            begin = Clock::now();
            const auto rendered = RenderTimeline(*published);
            projection.push_back(ElapsedNs(begin));
            EXPECT_EQ(rendered.clips.size(), 2000u);
            EXPECT_EQ(before->clips[index].gain_db, 0);
            EXPECT_EQ(published->clips[index].gain_db, -6);
        }
        const auto key = "run_" + std::to_string(run + 1) + '_';
        RecordProperty(key + "preview_validation", SamplesJson(preview));
        RecordProperty(key + "command", SamplesJson(command));
        RecordProperty(key + "snapshot", SamplesJson(snapshot));
        RecordProperty(key + "render_projection", SamplesJson(projection));
    }
}

TEST(EditQualificationDelta, ExactDifferencesUseBoundedIdentityMatchesAndPreserveAuthoredOrder) {
    struct Entity {
        Id id = 0;
        int value = 0;
        size_t* comparisons = nullptr;
        bool operator==(const Entity& other) const {
            ++*comparisons;
            return id == other.id && value == other.value;
        }
    };
    size_t comparisons = 0;
    std::vector<Entity> before;
    for (Id id = 1; id <= 2000; ++id)
        before.push_back({id, 0, &comparisons});
    auto after = before;
    after[0].value = 1;
    after[999].value = 2;
    after[1999].value = 3;
    std::reverse(after.begin(), after.end());
    const auto key = [](const Entity& entity) { return entity.id; };
    const auto difference = exosnap::edit::detail::EntityDifference(after, before, key);
    ASSERT_EQ(difference.size(), 3u);
    EXPECT_EQ(difference[0].id, 2000);
    EXPECT_EQ(difference[1].id, 1000);
    EXPECT_EQ(difference[2].id, 1);
    EXPECT_LE(comparisons, 2000u);
    comparisons = 0;
    EXPECT_TRUE(exosnap::edit::detail::EntityDifference(before, before, key).empty());
    EXPECT_LE(comparisons, 2000u);
    comparisons = 0;
    const std::vector<Entity> none;
    const auto removed = exosnap::edit::detail::EntityDifference(before, none, key);
    ASSERT_EQ(removed.size(), before.size());
    EXPECT_EQ(removed.front().id, 1);
    EXPECT_EQ(removed.back().id, 2000);
    EXPECT_EQ(comparisons, 0u);
}

TEST(EditQualificationDelta, GainAndRemovalReportExactIdsWithHistoryRoundTrips) {
    Workspace workspace;
    ASSERT_TRUE(Populate(workspace, kTwoHours));
    const auto before = workspace.snapshot();
    const auto audio = AudioClipIndices(*before);
    ASSERT_EQ(audio.size(), kGroups);
    for (const auto index : {audio.front(), audio[audio.size() / 2], audio.back()}) {
        const auto gain = workspace.apply(SetGain{GainTarget::Clip, before->clips[index].id, -6}, workspace.revision());
        ASSERT_EQ(gain.status, CommandStatus::Applied);
        EXPECT_EQ(gain.changed_ids, (std::vector<Id>{before->clips[index].id}));
        ASSERT_TRUE(workspace.undo());
        EXPECT_EQ(workspace.snapshot()->clips, before->clips);
        ASSERT_TRUE(workspace.redo());
        EXPECT_EQ(workspace.clip(before->clips[index].id)->gain_db, -6);
        ASSERT_TRUE(workspace.undo());
    }
    const auto target = before->clips.back().id;
    std::vector<Id> expected;
    const auto group = before->clips.back().group;
    for (const auto& clip : before->clips)
        if (clip.group == group)
            expected.push_back(clip.id);
    const auto removed = workspace.apply(LiftClips{{target}}, workspace.revision());
    ASSERT_EQ(removed.status, CommandStatus::Applied);
    EXPECT_EQ(removed.changed_ids, expected);
    ASSERT_TRUE(workspace.undo());
    EXPECT_EQ(workspace.snapshot()->clips, before->clips);
    EXPECT_EQ(workspace.snapshot()->junctions, before->junctions);
}
